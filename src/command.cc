// FORK MODIFICATION NOTICE (2026)
// Changed by the GNU-free Kati fork, maintained by Rihards Paps and
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

#include "command.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include "dep.h"
#include "eval.h"
#include "exec.h"
#include "fileutil.h"
#include "flags.h"
#include "log.h"
#include "strutil.h"
#include "var.h"

extern char** environ;

namespace {

// Recursive make recipes sometimes pass a deferred make variable as a
// command-line assignment, for example 'CXX=$$(RAW_CXX_FOR_TARGET)'.  The
// recipe evaluator turns '$$' into a literal '$'; when the command is later
// run by Ninja or by Kati's executor, the child would otherwise receive an
// unresolved reference and install an empty command-line override.  Resolve
// deferred references occurring in quoted assignment values while the
// parent evaluator is still in scope.
static std::string ExpandDeferredAssignments(std::string_view input,
                                             Evaluator* ev) {
  std::string output;
  output.reserve(input.size());
  char quote = 0;

  for (size_t i = 0; i < input.size(); ++i) {
    char c = input[i];
    if (c == '\'' || c == '"' || c == '`') {
      if (quote == 0)
        quote = c;
      else if (quote == c)
        quote = 0;
      output += c;
      continue;
    }

    if (c == '$' && i + 1 < input.size() && input[i + 1] == '(' &&
        quote == '\'' && !output.empty() && output.back() == '=') {
      size_t end = i + 2;
      int depth = 1;
      while (end < input.size() && depth != 0) {
        if (input[end] == '(')
          ++depth;
        else if (input[end] == ')')
          --depth;
        ++end;
      }

      if (depth == 0) {
        const std::string name(input.substr(i + 2, end - i - 3));
        Var* var = ev->LookupVar(Intern(name));
        if (var != nullptr && var->IsDefined()) {
          output += var->Eval(ev);
          i = end - 1;
          continue;
        }
      }
    }

    output += c;
  }

  return output;
}

static std::string ShellQuoteCommand(std::string_view value) {
  std::string quoted = "'";
  for (char c : value) {
    if (c == '\'')
      quoted += "'\\''";
    else
      quoted += c;
  }
  quoted += "'";
  return quoted;
}

static bool IsShellIdentifierCommand(std::string_view name) {
  if (name.empty())
    return false;
  if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
    return false;
  for (size_t i = 1; i < name.size(); ++i) {
    if (!std::isalnum(static_cast<unsigned char>(name[i])) && name[i] != '_')
      return false;
  }
  return true;
}

// MAKEFLAGS and MAKEOVERRIDES are recursive-make transport channels.  They
// describe the invocation being constructed, not ordinary environment
// variables.  Copying their current process-environment values into a
// deferred recursive command freezes state from an earlier recursion level
// and can override assignments made by the recipe immediately before the
// child is started.
static bool IsRecursiveTransportVariable(std::string_view name) {
  return name == "MAKEFLAGS" || name == "MAKEOVERRIDES" || name == "MAKELEVEL";
}

// Recursive commands executed directly by Kati do not pass through the
// Ninja emitter.  Export the effective values at this command boundary so a
// child sees directory-local modifications just as it would under make.
static void ExportRecursiveEnvironment(Command* command, Evaluator* ev) {
  const bool recursive = IsRecursiveKatiCommand(command->cmd);

  std::string exports;
  std::unordered_set<std::string> emitted;
  auto emit = [&](Symbol name) {
    const std::string name_string(name.str());
    if (IsRecursiveTransportVariable(name_string))
      return;
    auto explicit_export = ev->exports().find(name);
    const Vars* scope = ev->current_scope();
    const bool scoped_export = scope != nullptr &&
                               scope->exported().exists(name) &&
                               scope->IsExported(name);
    if (explicit_export != ev->exports().end() && !explicit_export->second &&
        !scoped_export)
      return;
    Var* variable = ev->LookupVar(name);
    if (variable == nullptr || !variable->IsDefined() ||
        !IsShellIdentifierCommand(name_string) ||
        !emitted.insert(name_string).second)
      return;
    const std::string value = ev->EvalVar(name);
    if (g_flags.generate_ninja)
      exports +=
          "export " + name_string + "=" + ShellQuoteCommand(value) + "; ";
    else
      command->environment.push_back(name_string + "=" + value);
  };

  // GNU make exports command-line assignments to every directly executed
  // recipe. Ninja's deferred environment is materialized separately; adding
  // these prefixes to generated commands changes their shell semantics.
  if (!g_flags.generate_ninja) {
    for (std::string_view assignment : g_flags.cl_vars) {
      size_t equal = assignment.find('=');
      if (equal == std::string_view::npos || equal == 0)
        continue;
      std::string_view name = TrimSpace(assignment.substr(0, equal));
      if (!name.empty() &&
          (name.back() == ':' || name.back() == '+' || name.back() == '?'))
        name.remove_suffix(1);
      // GNU make treats SHELL specially on Unix: a command-line SHELL
      // selects the recipe interpreter but is not exported automatically.
      if (!name.empty() && name != "SHELL")
        emit(Intern(name));
    }
  }

  if (!recursive) {
    if (ev->current_scope() != nullptr) {
      for (Symbol name : ev->current_scope()->exported())
        if (ev->current_scope()->IsExported(name))
          emit(name);
    }
    if (g_flags.generate_ninja)
      command->cmd.insert(0, exports);
    return;
  }

  for (const auto& [name, is_exported] : ev->exports()) {
    if (is_exported)
      emit(name);
  }
  if (ev->current_scope() != nullptr) {
    for (Symbol name : ev->current_scope()->exported())
      if (ev->current_scope()->IsExported(name))
        emit(name);
  }

  for (char** p = environ; p != nullptr && *p != nullptr; ++p) {
    const char* equal = strchr(*p, '=');
    if (equal == nullptr || equal == *p)
      continue;
    const std::string_view name_view(*p, equal - *p);
    if (IsRecursiveTransportVariable(name_view))
      continue;
    const Symbol name = Intern(name_view);
    auto explicit_export = ev->exports().find(name);
    if (explicit_export != ev->exports().end() && !explicit_export->second)
      continue;
    // Environment-origin variables are exported to recursive makes even
    // when their value is unchanged.  Serialize them explicitly because a
    // deferred child may run behind a generated environment snapshot or
    // after another recursive command altered its shell environment.
    emit(name);
  }

  // Preserve the complete MAKEFLAGS value, including its command-line
  // assignments. Environment exports alone are insufficient: a recursive
  // makefile may assign CC=... itself, so command-line values such as
  // CC=clang must remain command-line overrides in the child.
  Var* makeflags = ev->LookupVar(Intern("MAKEFLAGS"));
  if (!g_flags.generate_ninja) {
    if (makeflags != nullptr && makeflags->IsDefined())
      command->environment.push_back("MAKEFLAGS=" + makeflags->Eval(ev));
    else
      command->unset_environment.push_back("MAKEFLAGS");
    command->unset_environment.push_back("MAKEOVERRIDES");
    return;
  }
  if (makeflags != nullptr && makeflags->IsDefined()) {
    exports = "export MAKEFLAGS=" + ShellQuoteCommand(makeflags->Eval(ev)) +
              "; unset MAKEOVERRIDES; " + exports;
  } else {
    exports = "unset MAKEFLAGS MAKEOVERRIDES; " + exports;
  }
  command->cmd.insert(0, exports);
}

// .EXPORT_ALL_VARIABLES makes every make variable whose name can be passed
// through a POSIX environment visible to every recipe.  Keep this at the
// recipe boundary rather than changing Kati's own process environment: the
// latter would leak target-local values between parallel recipes.
static void ExportAllVariables(Command* command, Evaluator* ev) {
  std::string exports;
  std::unordered_set<std::string> emitted;
  for (std::string_view name_view : GetSymbolNames(
           [](Var* var) { return var->IsDefined() && !var->Obsolete(); })) {
    if (name_view == "MAKELEVEL" || !IsShellIdentifierCommand(name_view) ||
        !emitted.insert(std::string(name_view)).second)
      continue;
    Symbol name = Intern(name_view);
    Var* variable = ev->LookupVar(name);
    if (variable == nullptr || !variable->IsDefined())
      continue;
    const std::string value = ev->EvalVar(name);
    if (g_flags.generate_ninja)
      exports += "export " + std::string(name_view) + "=" +
                 ShellQuoteCommand(value) + "; ";
    else
      command->environment.push_back(std::string(name_view) + "=" + value);
  }
  if (g_flags.generate_ninja)
    command->cmd.insert(0, exports);
}

class AutoVar : public Var {
 public:
  AutoVar() : Var(VarOrigin::AUTOMATIC, nullptr, Loc()) {}
  virtual const char* Flavor() const override { return "undefined"; }

  virtual void AppendVar(Evaluator*, Value*) override { CHECK(false); }

  virtual std::string_view String() const override {
    // Automatic variables are computed values rather than makefile text, so
    // GNU make's raw-value query expands to the empty string.
    return "";
  }

  virtual std::string DebugString() const override {
    return std::string("AutoVar(") + sym_ + ")";
  }

  virtual bool IsFunc(Evaluator*) const override { return true; }

 protected:
  AutoVar(CommandEvaluator* ce, const char* sym) : ce_(ce), sym_(sym) {}
  virtual ~AutoVar() = default;

  CommandEvaluator* ce_;
  const char* sym_;
};

#define DECLARE_AUTO_VAR_CLASS(name)                                  \
  class name : public AutoVar {                                       \
   public:                                                            \
    name(CommandEvaluator* ce, const char* sym) : AutoVar(ce, sym) {} \
    virtual ~name() = default;                                        \
    virtual void Eval(Evaluator* ev, std::string* s) const override;  \
  }

DECLARE_AUTO_VAR_CLASS(AutoAtVar);
DECLARE_AUTO_VAR_CLASS(AutoLessVar);
DECLARE_AUTO_VAR_CLASS(AutoHatVar);
DECLARE_AUTO_VAR_CLASS(AutoPlusVar);
DECLARE_AUTO_VAR_CLASS(AutoStarVar);
DECLARE_AUTO_VAR_CLASS(AutoQuestionVar);
DECLARE_AUTO_VAR_CLASS(AutoPercentVar);
DECLARE_AUTO_VAR_CLASS(AutoPipeVar);

class AutoSuffixDVar : public AutoVar {
 public:
  AutoSuffixDVar(CommandEvaluator* ce, const char* sym, Var* wrapped)
      : AutoVar(ce, sym), wrapped_(wrapped) {}
  virtual ~AutoSuffixDVar() = default;
  virtual void Eval(Evaluator* ev, std::string* s) const override;

 private:
  Var* wrapped_;
};

class AutoSuffixFVar : public AutoVar {
 public:
  AutoSuffixFVar(CommandEvaluator* ce, const char* sym, Var* wrapped)
      : AutoVar(ce, sym), wrapped_(wrapped) {}
  virtual ~AutoSuffixFVar() = default;
  virtual void Eval(Evaluator* ev, std::string* s) const override;

 private:
  Var* wrapped_;
};

void AutoAtVar::Eval(Evaluator*, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (n)
    *s += n->lexical_output.str();
}

static Symbol EffectivePrerequisite(Evaluator* ev,
                                    const DepNode* node,
                                    Symbol input) {
  if (g_flags.generate_ninja || Exists(input.str()))
    return input;
  auto find_provider = [ev,
                        input](const std::vector<NamedDepNode>& dependencies) {
    for (const NamedDepNode& dependency : dependencies) {
      const DepNode* child = dependency.second;
      if (!(dependency.first == input) && !(child->lexical_output == input))
        continue;
      if (child->is_phony ||
          child->vpath_discarded.load(std::memory_order_acquire))
        return input;
      if (child->vpath_provider.IsValid())
        return child->vpath_provider;
      if (!(child->output == input) && !child->has_rule)
        return child->output;
      const std::string resolved = ev->ResolveVpath(input);
      if (!resolved.empty())
        return Intern(resolved);
      return input;
    }
    return Symbol();
  };
  Symbol provider = find_provider(node->deps);
  if (!provider.IsValid())
    provider = find_provider(node->order_onlys);
  if (provider.IsValid())
    return provider;
  // Implicit suffix prerequisites can be materialized without a named
  // dependency edge. They still use their VPATH source for automatic vars.
  const std::string resolved = ev->ResolveVpath(input);
  if (!resolved.empty())
    return Intern(resolved);
  return input;
}

void AutoLessVar::Eval(Evaluator* ev, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (!n)
    return;
  auto& ai = n->actual_inputs;
  if (!ai.empty())
    *s += EffectivePrerequisite(ev, n, ai[0]).str();
}

void AutoHatVar::Eval(Evaluator* ev, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (!n)
    return;
  std::unordered_set<std::string_view> seen;
  WordWriter ww(s);
  for (Symbol ai : n->actual_inputs) {
    ai = EffectivePrerequisite(ev, n, ai);
    if (seen.insert(ai.str()).second)
      ww.Write(ai.str());
  }
}

void AutoPlusVar::Eval(Evaluator* ev, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (!n)
    return;
  WordWriter ww(s);
  for (Symbol ai : n->actual_inputs) {
    ai = EffectivePrerequisite(ev, n, ai);
    ww.Write(ai.str());
  }
}

void AutoStarVar::Eval(Evaluator*, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();

  if (!n || !n->output_pattern.IsValid())
    return;

  Pattern pat(n->output_pattern.str());
  s->append(pat.Stem(n->lexical_output.str()));
}

void AutoQuestionVar::Eval(Evaluator* ev, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (!n)
    return;
  std::unordered_set<std::string_view> seen;

  if (ev->avoid_io()) {
    // Check timestamps using the shell at the start of rule execution
    // instead.
    *s += "${KATI_NEW_INPUTS}";
    // Command-state files can outlive their output. GNU make does not
    // consider a saved command sufficient when the target itself is absent;
    // FORCE-only rules must still execute. Preserve that rule generically
    // instead of relying on a stale savedcmd value.
    if (!Exists(n->output.str()))
      *s += " kati_missing_output";
    if (!ce_->found_new_inputs()) {
      std::string def;

      WordWriter ww(&def);
      bool have_probe_inputs = false;
      ww.Write("KATI_NEW_INPUTS=$(find");
      // Command expansion happens before BuildPlan has populated n->deps.
      // Use the already materialized prerequisite list instead of the later
      // dependency-node list; otherwise secondary-expanded rules can produce
      // `find` with no paths, which makes find scan `.' and can overflow the
      // environment passed to the next command.
      for (Symbol ai : n->actual_inputs) {
        // Keep missing paths too: they may be generated by an earlier Ninja
        // edge before this recipe runs.  find's diagnostics are suppressed
        // below, so a not-yet-created prerequisite cannot fail the probe.
        if (seen.insert(ai.str()).second) {
          ww.Write(ai.str());
          have_probe_inputs = true;
        }
      }
      if (have_probe_inputs) {
        // Prerequisites may be directories.  Prune them so the probe checks
        // the prerequisite itself instead of recursively enumerating an
        // entire source tree.
        ww.Write("-prune");
        ww.Write("$(test -e");
        ww.Write(n->lexical_output.str());
        ww.Write("&& echo -newer");
        ww.Write(n->lexical_output.str());
        ww.Write(") 2>/dev/null || : ) && export KATI_NEW_INPUTS");
      } else {
        def = "KATI_NEW_INPUTS=; export KATI_NEW_INPUTS";
      }
      ev->add_delayed_output_command(def);
      ce_->set_found_new_inputs(true);
    }
  } else {
    WordWriter ww(s);
    // GNU make computes $? against the target timestamp observed before
    // updating prerequisites. A prerequisite recipe may also rewrite the
    // target as a side effect; restating it here would erase the original
    // newer-prerequisite list used by generated makefile refresh recipes.
    double target_age = ce_->target_age();
    for (Symbol ai : n->actual_inputs) {
      ai = EffectivePrerequisite(ev, n, ai);
      double input_age = GetTimestamp(ai.str());
      if (std::find(n->low_resolution_inputs.begin(),
                    n->low_resolution_inputs.end(),
                    ai) != n->low_resolution_inputs.end())
        input_age = std::floor(input_age);
      if (seen.insert(ai.str()).second && input_age > target_age) {
        ww.Write(ai.str());
      }
    }
  }
}

void AutoPercentVar::Eval(Evaluator*, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (!n)
    return;
  const std::string output = n->lexical_output.str();
  const size_t open = output.find('(');
  if (open != std::string::npos && output.back() == ')' &&
      open + 1 < output.size()) {
    s->append(output, open + 1, output.size() - open - 2);
  }
}

void AutoPipeVar::Eval(Evaluator* ev, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (!n)
    return;
  WordWriter ww(s);
  for (Symbol input : n->actual_order_only_inputs)
    ww.Write(EffectivePrerequisite(ev, n, input).str());
}

void AutoSuffixDVar::Eval(Evaluator* ev, std::string* s) const {
  std::string buf;
  wrapped_->Eval(ev, &buf);
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(buf)) {
    ww.Write(Dirname(tok));
  }
}

void AutoSuffixFVar::Eval(Evaluator* ev, std::string* s) const {
  std::string buf;
  wrapped_->Eval(ev, &buf);
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(buf)) {
    ww.Write(Basename(tok));
  }
}

void ParseCommandPrefixes(std::string_view* s,
                          bool* echo,
                          bool* ignore_error,
                          bool* force_run) {
  *s = TrimLeftSpace(*s);
  while (true) {
    char c = s->empty() ? 0 : s->front();
    if (c == '@') {
      *echo = false;
    } else if (c == '-') {
      *ignore_error = true;
    } else if (c == '+') {
      *force_run = true;
    } else {
      break;
    }
    *s = TrimLeftSpace(s->substr(1));
  }
}

}  // namespace

CommandEvaluator::CommandEvaluator(Evaluator* ev)
    : ev_(ev), current_dep_node_(nullptr), found_new_inputs_(false) {
#define INSERT_AUTO_VAR(name, sym)                                      \
  do {                                                                  \
    Var* v = new name(this, sym);                                       \
    Intern(sym).SetGlobalVar(v);                                        \
    Intern(sym "D").SetGlobalVar(new AutoSuffixDVar(this, sym "D", v)); \
    Intern(sym "F").SetGlobalVar(new AutoSuffixFVar(this, sym "F", v)); \
  } while (0)
  INSERT_AUTO_VAR(AutoAtVar, "@");
  INSERT_AUTO_VAR(AutoLessVar, "<");
  INSERT_AUTO_VAR(AutoHatVar, "^");
  INSERT_AUTO_VAR(AutoPlusVar, "+");
  INSERT_AUTO_VAR(AutoStarVar, "*");
  INSERT_AUTO_VAR(AutoQuestionVar, "?");
  INSERT_AUTO_VAR(AutoPercentVar, "%");
  INSERT_AUTO_VAR(AutoPipeVar, "|");
}

CommandEvaluator::~CommandEvaluator() {
  // Automatic variables are installed in the process-global symbol table,
  // but their implementations point back to this evaluator.  Clear those
  // bindings before a restarted makefile evaluation can observe a stale
  // command evaluator through $@, $^, $<, and friends.
  static const char* kAutomaticSymbols[] = {"@", "<", "^", "+",
                                            "*", "?", "%", "|"};
  for (const char* sym : kAutomaticSymbols) {
    Intern(sym).SetGlobalVar(Var::Undefined());
    Intern(StringPrintf("%sD", sym)).SetGlobalVar(Var::Undefined());
    Intern(StringPrintf("%sF", sym)).SetGlobalVar(Var::Undefined());
  }
}

std::vector<Command> CommandEvaluator::Eval(const DepNode& n) {
  return Eval(n, GetTimestamp(n.output.str()));
}

std::vector<Command> CommandEvaluator::Eval(const DepNode& n,
                                            double target_age) {
  return Eval(n, target_age, static_cast<size_t>(-1));
}

std::vector<Command> CommandEvaluator::Eval(const DepNode& n,
                                            double target_age,
                                            size_t double_colon_group) {
  std::vector<Command> result;
  target_age_ = target_age;
  ev_->set_loc(n.loc);
  ev_->set_current_scope(n.rule_vars);
  ev_->SetEvaluatingCommand(true);
  current_dep_node_ = &n;
  found_new_inputs_ = false;
  // Test harnesses may clear V while checking GNU make's printed recipes.
  // Keep an invocation-wide command display switch that survives recursive
  // makes without adding a command-line variable override to MAKEFLAGS.
  const char* kati_verbose = std::getenv("KATI_VERBOSE");
  const bool environment_verbose =
      kati_verbose != nullptr && std::string_view(kati_verbose) == "1";
  // Silent recipes do not need a display mode. Avoid expanding an otherwise
  // unused V definition for them (it may be recursive or have side effects).
  auto is_verbose = [&](bool echo) {
    if (!echo)
      return false;
    Var* verbosity = ev_->LookupVar(Intern("V"));
    // V is a display extension, not a make variable used by this recipe.
    // Expanding a deferred definition here can run shell functions or fail.
    return environment_verbose ||
           (verbosity->IsDefined() && verbosity->String() == "1");
  };
  if (n.oneshell) {
    std::string script;
    for (size_t i = 0; i < n.cmds.size(); ++i) {
      if (double_colon_group != static_cast<size_t>(-1) &&
          (i >= n.double_colon_group_for_cmd.size() ||
           n.double_colon_group_for_cmd[i] != double_colon_group))
        continue;
      Value* v = n.cmds[i];
      ev_->set_loc(v->Location());
      if (!script.empty())
        script += '\n';
      std::string line = ExpandDeferredAssignments(v->Eval(ev_), ev_);
      if (!script.empty()) {
        std::string_view remaining = line;
        bool ignored_echo = true;
        bool ignored_error = false;
        bool ignored_force_run = false;
        ParseCommandPrefixes(&remaining, &ignored_echo, &ignored_error,
                             &ignored_force_run);
        line = std::string(remaining);
      }
      script += line;
    }
    std::string_view cmds = script;
    bool echo = !g_flags.is_silent_mode;
    bool ignore_error = false;
    bool force_run = false;
    ParseCommandPrefixes(&cmds, &echo, &ignore_error, &force_run);
    cmds = TrimLeftSpace(cmds);
    if (!cmds.empty()) {
      Command& command = result.emplace_back(n.output);
      if (double_colon_group != static_cast<size_t>(-1))
        command.double_colon_group = double_colon_group;
      command.cmd = std::string(cmds);
      command.display_cmd = command.cmd;
      command.verbose = is_verbose(echo && !n.silent);
      command.force_run = force_run;
      // Generated Ninja recipes source env.sh, which is only a snapshot of
      // the process environment taken during graph generation.  Recursive
      // make commands must receive the effective exported make variables at
      // this recipe boundary as well; otherwise makefile assignments made
      // after the snapshot are lost in the child.
      ExportRecursiveEnvironment(&command, ev_);
      if (n.export_all_variables)
        ExportAllVariables(&command, ev_);
      command.echo = echo && !n.silent;
      command.ignore_error = ignore_error || n.ignore_errors;
    }
  } else {
    for (size_t command_index = 0; command_index < n.cmds.size();
         ++command_index) {
      if (double_colon_group != static_cast<size_t>(-1) &&
          (command_index >= n.double_colon_group_for_cmd.size() ||
           n.double_colon_group_for_cmd[command_index] != double_colon_group))
        continue;
      Value* v = n.cmds[command_index];
      ev_->set_loc(v->Location());
      const std::string cmds_buf = ExpandDeferredAssignments(v->Eval(ev_), ev_);
      std::string_view cmds = cmds_buf;
      bool global_echo = !g_flags.is_silent_mode;
      bool global_ignore_error = false;
      bool global_force_run = false;
      ParseCommandPrefixes(&cmds, &global_echo, &global_ignore_error,
                           &global_force_run);
      if (cmds == "")
        continue;
      while (true) {
        size_t lf_cnt;
        size_t index = FindEndOfLine(cmds, 0, &lf_cnt);
        if (index == cmds.size())
          index = std::string::npos;
        std::string_view cmd = TrimLeftSpace(cmds.substr(0, index));
        cmds = cmds.substr(index + 1);

        bool echo = global_echo;
        bool ignore_error = global_ignore_error;
        bool force_run = global_force_run;
        ParseCommandPrefixes(&cmd, &echo, &ignore_error, &force_run);

        if (!cmd.empty()) {
          Command& command = result.emplace_back(n.output);
          if (command_index < n.double_colon_group_for_cmd.size())
            command.double_colon_group =
                n.double_colon_group_for_cmd[command_index];
          command.cmd = std::string(cmd);
          command.display_cmd = command.cmd;
          command.verbose = is_verbose(echo && !n.silent);
          command.force_run = force_run;
          // See the oneshell case above: recursive commands need the current
          // makefile export state even when Ninja defers their execution.
          ExportRecursiveEnvironment(&command, ev_);
          if (n.export_all_variables)
            ExportAllVariables(&command, ev_);
          command.echo = echo && !n.silent;
          command.ignore_error = ignore_error || n.ignore_errors;
        }
        if (index == std::string::npos)
          break;
      }
      continue;
    }
  }

  // SHELL and .SHELLFLAGS may be target-specific. Capture their effective
  // values while the target-specific scope is active so direct execution and
  // generated Ninja rules cannot accidentally use the top-level shell.
  const std::string command_shell = ev_->GetShell();
  const std::string command_shellflag = ev_->GetShellFlag();
  for (Command& command : result) {
    command.shell = command_shell;
    command.shellflag = command_shellflag;
  }

  if (!ev_->delayed_output_commands().empty()) {
    std::vector<Command> output_commands;
    for (const std::string& cmd : ev_->delayed_output_commands()) {
      Command& c = output_commands.emplace_back(n.output);
      c.cmd = cmd;
      c.echo = false;
      c.ignore_error = false;
      c.force_no_subshell = true;
    }
    // Prepend |output_commands|.
    result.swap(output_commands);
    copy(output_commands.begin(), output_commands.end(), back_inserter(result));
    ev_->clear_delayed_output_commands();
  }

  ev_->set_current_scope(NULL);
  ev_->SetEvaluatingCommand(false);

  return result;
}
