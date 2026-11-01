// FORK MODIFICATION NOTICE (2026)
// Own invocation state and publish coherent generations with typed freshness.
// Validate without interning and record ordered symbol observations for replay.
// Snapshot symbol enumeration before lazy expression decoding can intern names.
// Defer global variable materialization while preserving assignment precedence.
// Changed by the Openmktr fork, maintained by Rihards Paps and
// Haralds Paps.
// Changed build execution, evaluation, or portability for this fork.
// Upstream material retains its Apache-2.0 terms. Fork modifications are
// covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
// at the repository root. Original notices below remain applicable.

// Copyright 2015 Google Inc. All rights reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// +build ignore

// #define ENABLE_TID_CHECK

#include "symtab.h"
#include "observation.h"
#include "session.h"

#ifdef ENABLE_TID_CHECK
#include <pthread.h>
#endif
#include <string.h>

#include <deque>
#include <unordered_map>

#include "incremental_eval.h"
#include "log.h"
#include "strutil.h"
#include "var.h"

struct SymbolData {
  SymbolData() : gv(Var::Undefined()) {}

  Var* gv;
  std::unique_ptr<std::function<Var*()>> restore;
};

struct SymbolBindings {
  std::vector<SymbolData> entries;
};
#define g_symbol_data (SessionService<SymbolBindings>().entries)

Var* Symbol::PeekGlobalVar() const {
  if (static_cast<size_t>(v_) >= g_symbol_data.size()) {
    return Var::Undefined();
  }
  if (g_symbol_data[v_].restore) {
    // Restoring a value may intern strings and grow the symbol-data vector.
    auto restore = std::move(g_symbol_data[v_].restore);
    Var* value = (*restore)();
    CHECK(value != nullptr);
    g_symbol_data[v_].gv = value;
  }
  return g_symbol_data[v_].gv;
}

Var* Symbol::GetGlobalVar() const {
  if (static_cast<size_t>(v_) >= g_symbol_data.size()) {
    g_symbol_data.resize(v_ + 1);
  }
  Var* v = PeekGlobalVar();
  if (v->Origin() == VarOrigin::ENVIRONMENT ||
      v->Origin() == VarOrigin::ENVIRONMENT_OVERRIDE) {
    Vars::add_used_env_vars(*this);
  }
  return v;
}

void Symbol::SetGlobalVar(Var* v, bool is_override, bool* readonly) const {
  if (static_cast<size_t>(v_) >= g_symbol_data.size()) {
    g_symbol_data.resize(v_ + 1);
  }
  Var* orig = PeekGlobalVar();
  if (orig->ReadOnly()) {
    if (readonly != nullptr)
      *readonly = true;
    else
      ERROR("*** cannot assign to readonly variable: %s", c_str());
    return;
  } else if (readonly != nullptr) {
    *readonly = false;
  }
  if (!is_override && (orig->Origin() == VarOrigin::OVERRIDE ||
                       (orig->Origin() == VarOrigin::ENVIRONMENT_OVERRIDE &&
                        v->Origin() != VarOrigin::COMMAND_LINE))) {
    return;
  }
  if (orig->Origin() == VarOrigin::COMMAND_LINE &&
      v->Origin() == VarOrigin::FILE) {
    return;
  }
  if (orig->Origin() == VarOrigin::AUTOMATIC) {
    // Keep GNU make's behavior: an assignment to an automatic-variable name
    // does not replace the automatic binding used while a recipe runs.
    delete v;
    return;
  }
  if (orig->IsDefined())
    delete orig;
  g_symbol_data[v_].gv = v;
}

void Symbol::SetLazyGlobalVar(VarOrigin origin,
                              std::function<Var*()> restore) const {
  if (static_cast<size_t>(v_) >= g_symbol_data.size())
    g_symbol_data.resize(v_ + 1);
  // Context restoration uses override assignment, just like SetGlobalVar.
  // Existing command-line and automatic definitions still retain precedence.
  Var* previous = PeekGlobalVar();
  if (previous->ReadOnly())
    ERROR("*** cannot assign to readonly variable: %s", c_str());
  if ((previous->Origin() == VarOrigin::COMMAND_LINE &&
       origin == VarOrigin::FILE) ||
      previous->Origin() == VarOrigin::AUTOMATIC)
    return;
  if (previous->IsDefined())
    delete previous;
  g_symbol_data[v_].gv = Var::Undefined();
  g_symbol_data[v_].restore =
      std::make_unique<std::function<Var*()>>(std::move(restore));
}

ScopedGlobalVar::ScopedGlobalVar(Symbol name, Var* var)
    : name_(name), orig_(NULL) {
  orig_ = name.GetGlobalVar();
  g_symbol_data[name_.val()].gv = var;
}

ScopedGlobalVar::~ScopedGlobalVar() {
  g_symbol_data[name_.val()].gv = orig_;
}

class Symtab {
 public:
  Symtab() {
#ifdef ENABLE_TID_CHECK
    tid_ = pthread_self();
#endif

    Symbol s = InternImpl("");
    CHECK(s.v_ == 0);

    char b[2];
    b[1] = 0;
    for (int i = 1; i < 256; i++) {
      b[0] = i;
      s = InternImpl(b);
      CHECK(s.val() == i);
    }

    CHECK(InternImpl("SHELL") == kShellSym);
    CHECK(InternImpl(".SHELLFLAGS") == kShellFlagsSym);
    InternImpl(".SHELLSTATUS")
        .SetGlobalVar(new ShellStatusVar(), false, nullptr);
    CHECK(InternImpl(".KATI_ALLOW_RULES") == kAllowRulesSym);
    CHECK(InternImpl(".KATI_READONLY") == kKatiReadonlySym);
    InternImpl(".VARIABLES")
        .SetGlobalVar(new VariableNamesVar(".VARIABLES", true));
    InternImpl(".KATI_SYMBOLS")
        .SetGlobalVar(new VariableNamesVar(".KATI_SYMBOLS", false));
  }
  const std::vector<std::string*>& symbols() const { return symbols_; }

  Symbol InternImpl(std::string_view s) {
    auto found = symtab_.find(s);
    if (found != symtab_.end()) {
      return found->second;
    }
    // deque keeps string addresses stable while allocating in blocks rather
    // than once for every symbol. The public symbol index still uses pointers.
    storage_.emplace_back(s.data(), s.size());
    symbols_.push_back(&storage_.back());
    Symbol sym = Symbol(symtab_.size());
    bool ok = symtab_.emplace(*symbols_.back(), sym).second;
    CHECK(ok);
    return sym;
  }

  Symbol Intern(std::string_view s) {
#ifdef ENABLE_TID_CHECK
    if (tid_ != pthread_self())
      abort();
#endif

    if (s.size() <= 1) {
      return Symbol(s.empty() ? 0 : (unsigned char)s[0]);
    }
    return InternImpl(s);
  }

  std::vector<std::string_view> GetSymbolNames(
      std::function<bool(Var*)> const& filter) {
    std::vector<std::string_view> result;
    // Filters may decode a lazy runtime expression and intern more strings.
    // Take a snapshot so that rehashing cannot invalidate this traversal.
    std::vector<std::pair<std::string_view, Symbol>> entries(symtab_.begin(),
                                                             symtab_.end());
    for (auto entry : entries) {
      Var* var = entry.second.PeekGlobalVar();
      // The symbol table contains all interned strings, not just variables
      // which have been defined.
      if (!var->IsDefined()) {
        continue;
      }
      if (filter(var)) {
        result.push_back(entry.first);
      }
    }
    return result;
  }

  Symbol Find(std::string_view name) const {
    auto found = symtab_.find(name);
    return found == symtab_.end() ? Symbol() : found->second;
  }

 private:
  std::unordered_map<std::string_view, Symbol> symtab_;
  std::deque<std::string> storage_;
  std::vector<std::string*> symbols_;
#ifdef ENABLE_TID_CHECK
  pthread_t tid_;
#endif
};

#define g_symtab (SessionService<Symtab>())
const std::vector<std::string*>& SessionSymbols() {
  return g_symtab.symbols();
}

Symbol Intern(std::string_view s) {
  RecordEvaluationIntern(s);
  if (ObservationLog::Active())
    ObservationLog::Record({ObservationKind::Intern, {std::string(s)}});
  return g_symtab.Intern(s);
}

Symbol FindSymbol(std::string_view s) {
  return g_symtab.Find(s);
}

std::string JoinSymbols(const std::vector<Symbol>& syms, const char* sep) {
  std::vector<std::string> strs;
  strs.reserve(syms.size());
  for (Symbol s : syms) {
    strs.push_back(s.str());
  }
  return JoinStrings(strs, sep);
}

std::vector<std::string_view> GetSymbolNames(
    std::function<bool(Var*)> const& filter) {
  return g_symtab.GetSymbolNames(filter);
}
