// FORK MODIFICATION NOTICE (2026)
// Changed by the GNU-free Kati fork, maintained by Rihards Paps and
// Haralds Paps.
// Changed build execution, evaluation, or portability for this fork.
// Upstream material retains its Apache-2.0 terms. Fork modifications are
// covered by PolyForm Perimeter 1.0.1; see docs/LICENSING.md and NOTICE
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

#include "func.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <iterator>
#include <memory>
#include <sstream>
#include <unordered_map>

#include "eval.h"
#include "file_cache.h"
#include "fileutil.h"
#include "find.h"
#include "loc.h"
#include "log.h"
#include "parser.h"
#include "stats.h"
#include "stmt.h"
#include "strutil.h"
#include "symtab.h"
#include "var.h"

namespace {

// TODO: This code is very similar to
// NinjaGenerator::TranslateCommand. Factor them out.
void StripShellComment(std::string* cmd) {
  if (cmd->find('#') == std::string::npos)
    return;

  std::string res;
  bool prev_backslash = false;
  // Set space as an initial value so the leading comment will be
  // stripped out.
  char prev_char = ' ';
  char quote = 0;
  bool done = false;
  const char* in = cmd->c_str();
  for (; *in && !done; in++) {
    switch (*in) {
      case '#':
        if (quote == 0 && isspace(prev_char)) {
          while (in[1] && *in != '\n')
            in++;
          break;
        }
        [[fallthrough]];

      case '\'':
      case '"':
      case '`':
        if (quote) {
          if (quote == *in)
            quote = 0;
        } else if (!prev_backslash) {
          quote = *in;
        }
        res += *in;
        break;

      case '\\':
        res += '\\';
        break;

      default:
        res += *in;
    }

    if (*in == '\\') {
      prev_backslash = !prev_backslash;
    } else {
      prev_backslash = false;
    }

    prev_char = *in;
  }
  cmd->swap(res);
}

void PatsubstFunc(const std::vector<Value*>& args,
                  Evaluator* ev,
                  std::string* s) {
  const std::string&& pat_str = args[0]->Eval(ev);
  const std::string&& repl = args[1]->Eval(ev);
  const std::string&& str = args[2]->Eval(ev);
  WordWriter ww(s);
  Pattern pat(pat_str);
  for (std::string_view tok : WordScanner(str)) {
    ww.MaybeAddSeparator();
    pat.AppendSubst(tok, repl, s);
  }
}

void StripFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  const std::string&& str = args[0]->Eval(ev);
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(str)) {
    ww.Write(tok);
  }
}

void SubstFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  const std::string&& pat = args[0]->Eval(ev);
  const std::string&& repl = args[1]->Eval(ev);
  const std::string&& str = args[2]->Eval(ev);
  if (pat.empty()) {
    *s += str;
    *s += repl;
    return;
  }
  size_t index = 0;
  while (index < str.size()) {
    size_t found = str.find(pat, index);
    if (found == std::string::npos)
      break;
    s->append(std::string_view(str).substr(index, found - index));
    s->append(repl);
    index = found + pat.size();
  }
  s->append(std::string_view(str).substr(index));
}

void FindstringFunc(const std::vector<Value*>& args,
                    Evaluator* ev,
                    std::string* s) {
  const std::string&& find = args[0]->Eval(ev);
  const std::string&& in = args[1]->Eval(ev);
  if (in.find(find) != std::string::npos)
    s->append(find);
}

void FilterFunc(const std::vector<Value*>& args,
                Evaluator* ev,
                std::string* s) {
  const std::string&& pat_buf = args[0]->Eval(ev);
  const std::string&& text = args[1]->Eval(ev);
  std::vector<Pattern> pats;
  for (std::string_view pat : WordScanner(pat_buf)) {
    pats.push_back(Pattern(pat));
  }
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(text)) {
    for (const Pattern& pat : pats) {
      if (pat.Match(tok)) {
        ww.Write(tok);
        break;
      }
    }
  }
}

void FilterOutFunc(const std::vector<Value*>& args,
                   Evaluator* ev,
                   std::string* s) {
  const std::string&& pat_buf = args[0]->Eval(ev);
  const std::string&& text = args[1]->Eval(ev);
  std::vector<Pattern> pats;
  for (std::string_view pat : WordScanner(pat_buf)) {
    pats.push_back(Pattern(pat));
  }
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(text)) {
    bool matched = false;
    for (const Pattern& pat : pats) {
      if (pat.Match(tok)) {
        matched = true;
        break;
      }
    }
    if (!matched)
      ww.Write(tok);
  }
}

void SortFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  std::string list;
  args[0]->Eval(ev, &list);
  COLLECT_STATS("func sort time");
  // TODO(hamaji): Probably we could use a faster string-specific sort
  // algorithm.
  std::vector<std::string_view> toks;
  WordScanner(list).Split(&toks);
  stable_sort(toks.begin(), toks.end());
  WordWriter ww(s);
  std::string_view prev;
  for (std::string_view tok : toks) {
    if (prev != tok) {
      ww.Write(tok);
      prev = tok;
    }
  }
}

static int GetNumericValueForFunc(const std::string& buf) {
  std::string_view s = TrimLeftSpace(buf);
  char* end;
  long n = strtol(s.data(), &end, 10);
  if (n < 0 || n == LONG_MAX || s.data() + s.size() != end) {
    return -1;
  }
  return n;
}

void WordFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  const std::string&& n_str = args[0]->Eval(ev);
  int n = GetNumericValueForFunc(n_str);
  if (n < 0) {
    ev->Error(
        StringPrintf("*** non-numeric first argument to `word' function: '%s'.",
                     n_str.c_str()));
  }
  if (n == 0) {
    ev->Error("*** first argument to `word' function must be greater than 0.");
  }

  const std::string&& text = args[1]->Eval(ev);
  for (std::string_view tok : WordScanner(text)) {
    n--;
    if (n == 0) {
      s->append(tok);
      break;
    }
  }
}

void WordlistFunc(const std::vector<Value*>& args,
                  Evaluator* ev,
                  std::string* s) {
  const std::string&& s_str = args[0]->Eval(ev);
  int si = GetNumericValueForFunc(s_str);
  if (si < 0) {
    ev->Error(StringPrintf(
        "*** non-numeric first argument to `wordlist' function: '%s'.",
        s_str.c_str()));
  }
  if (si == 0) {
    ev->Error(
        StringPrintf("*** invalid first argument to `wordlist' function: %s`",
                     s_str.c_str()));
  }

  const std::string&& e_str = args[1]->Eval(ev);
  int ei = GetNumericValueForFunc(e_str);
  if (ei < 0) {
    ev->Error(StringPrintf(
        "*** non-numeric second argument to `wordlist' function: '%s'.",
        e_str.c_str()));
  }

  const std::string&& text = args[2]->Eval(ev);
  int i = 0;
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(text)) {
    i++;
    if (si <= i && i <= ei) {
      ww.Write(tok);
    }
  }
}

void WordsFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  const std::string&& text = args[0]->Eval(ev);
  WordScanner ws(text);
  int n = 0;
  for (auto iter = ws.begin(); iter != ws.end(); ++iter)
    n++;
  char buf[32];
  sprintf(buf, "%d", n);
  *s += buf;
}

void FirstwordFunc(const std::vector<Value*>& args,
                   Evaluator* ev,
                   std::string* s) {
  const std::string&& text = args[0]->Eval(ev);
  WordScanner ws(text);
  auto begin = ws.begin();
  if (begin != ws.end()) {
    s->append(*begin);
  }
}

void LastwordFunc(const std::vector<Value*>& args,
                  Evaluator* ev,
                  std::string* s) {
  const std::string&& text = args[0]->Eval(ev);
  std::string_view last;
  for (std::string_view tok : WordScanner(text)) {
    last = tok;
  }
  s->append(last);
}

void JoinFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  const std::string&& list1 = args[0]->Eval(ev);
  const std::string&& list2 = args[1]->Eval(ev);
  WordScanner ws1(list1);
  WordScanner ws2(list2);
  WordWriter ww(s);
  WordScanner::Iterator iter1, iter2;
  for (iter1 = ws1.begin(), iter2 = ws2.begin();
       iter1 != ws1.end() && iter2 != ws2.end(); ++iter1, ++iter2) {
    ww.Write(*iter1);
    // Use append to not append extra ' '.
    s->append(*iter2);
  }
  for (; iter1 != ws1.end(); ++iter1)
    ww.Write(*iter1);
  for (; iter2 != ws2.end(); ++iter2)
    ww.Write(*iter2);
}

void WildcardFunc(const std::vector<Value*>& args,
                  Evaluator* ev,
                  std::string* s) {
  const std::string&& pat = args[0]->Eval(ev);
  COLLECT_STATS("func wildcard time");
  // Note GNU make does not delay the execution of $(wildcard) so we
  // do not need to check avoid_io here.
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(pat)) {
    const auto& files = Glob(tok);
    for (const std::string& file : files) {
      ww.Write(file);
    }
  }
}

void DirFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  const std::string&& text = args[0]->Eval(ev);
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(text)) {
    ww.Write(Dirname(tok));
    s->push_back('/');
  }
}

void NotdirFunc(const std::vector<Value*>& args,
                Evaluator* ev,
                std::string* s) {
  const std::string&& text = args[0]->Eval(ev);
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(text)) {
    if (tok == "/") {
      ww.Write(std::string_view(""));
    } else {
      ww.Write(Basename(tok));
    }
  }
}

void SuffixFunc(const std::vector<Value*>& args,
                Evaluator* ev,
                std::string* s) {
  const std::string&& text = args[0]->Eval(ev);
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(text)) {
    std::string_view suf = GetExt(tok);
    if (!suf.empty())
      ww.Write(suf);
  }
}

void BasenameFunc(const std::vector<Value*>& args,
                  Evaluator* ev,
                  std::string* s) {
  const std::string&& text = args[0]->Eval(ev);
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(text)) {
    ww.Write(StripExt(tok));
  }
}

void AddsuffixFunc(const std::vector<Value*>& args,
                   Evaluator* ev,
                   std::string* s) {
  const std::string&& suf = args[0]->Eval(ev);
  const std::string&& text = args[1]->Eval(ev);
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(text)) {
    ww.Write(tok);
    *s += suf;
  }
}

void AddprefixFunc(const std::vector<Value*>& args,
                   Evaluator* ev,
                   std::string* s) {
  const std::string&& pre = args[0]->Eval(ev);
  const std::string&& text = args[1]->Eval(ev);
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(text)) {
    ww.Write(pre);
    s->append(tok);
  }
}

void RealpathFunc(const std::vector<Value*>& args,
                  Evaluator* ev,
                  std::string* s) {
  const std::string&& text = args[0]->Eval(ev);
  if (ev->avoid_io()) {
    *s += "$(";
    *s += GetExecutablePath();
    *s += " --realpath ";
    *s += text;
    *s += " 2> /dev/null)";
    return;
  }

  WordWriter ww(s);
  std::string path;
  for (std::string_view tok : WordScanner(text)) {
    path.assign(tok);
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf))
      ww.Write(buf);
  }
}

void AbspathFunc(const std::vector<Value*>& args,
                 Evaluator* ev,
                 std::string* s) {
  const std::string&& text = args[0]->Eval(ev);
  WordWriter ww(s);
  std::string buf;
  for (std::string_view tok : WordScanner(text)) {
    AbsPath(tok, &buf);
    ww.Write(buf);
  }
}

void IfFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  const std::string&& cond = args[0]->Eval(ev);
  if (cond.empty()) {
    if (args.size() > 2)
      args[2]->Eval(ev, s);
  } else {
    args[1]->Eval(ev, s);
  }
}

void AndFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  std::string cond;
  for (Value* a : args) {
    cond = a->Eval(ev);
    if (cond.empty())
      return;
  }
  if (!cond.empty()) {
    *s += cond;
  }
}

void OrFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  for (Value* a : args) {
    const std::string&& cond = a->Eval(ev);
    if (!cond.empty()) {
      *s += cond;
      return;
    }
  }
}

void ValueFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  const std::string&& var_name = args[0]->Eval(ev);
  Var* var = ev->LookupVar(Intern(var_name));
  s->append(std::string(var->String()));
}

void EvalFunc(const std::vector<Value*>& args, Evaluator* ev, std::string*) {
  // TODO: eval leaks everything... for now.
  // const string text = args[0]->Eval(ev);
  ev->CheckStack();
  std::string* text = new std::string;
  args[0]->Eval(ev, text);
  if (ev->avoid_io()) {
    KATI_WARN_LOC(ev->loc(),
                  "*warning*: $(eval) in a recipe is not recommended: %s",
                  text->c_str());
  }
  std::vector<Stmt*> stmts;
  Parse(*text, ev->loc(), &stmts);
  for (Stmt* stmt : stmts) {
    stmt->Eval(ev);
    // delete stmt;
  }
}

// #define TEST_FIND_EMULATOR

// A hack for Android build. We need to evaluate things like $((3+4))
// when we emit ninja file, because the result of such expressions
// will be passed to other make functions.
// TODO: Maybe we should introduce a helper binary which evaluate
// make expressions at ninja-time.
static bool HasNoIoInShellScript(const std::string& cmd) {
  if (cmd.empty())
    return true;
  if (HasPrefix(cmd, "echo $((") && cmd[cmd.size() - 1] == ')')
    return true;
  return false;
}

static int ShellFuncImpl(const std::string& shell,
                         const std::string& shellflag,
                         const std::string& cmd,
                         const Loc& loc,
                         std::string* s,
                         FindCommand** fc) {
  LOG("ShellFunc: %s", cmd.c_str());

#ifdef TEST_FIND_EMULATOR
  bool need_check = false;
  string out2;
#endif
  if (FindEmulator::Get()) {
    *fc = new FindCommand();
    if ((*fc)->Parse(cmd)) {
#ifdef TEST_FIND_EMULATOR
      if (FindEmulator::Get()->HandleFind(cmd, **fc, loc, &out2)) {
        need_check = true;
      }
#else
      if (FindEmulator::Get()->HandleFind(cmd, **fc, loc, s)) {
        return 0;
      }
#endif
    }
    delete *fc;
    *fc = NULL;
  }

  COLLECT_STATS_WITH_SLOW_REPORT("func shell time", cmd.c_str());
  int status = RunCommand(shell, shellflag, cmd, RedirectStderr::NONE, s);
  FormatForCommandSubstitution(s);

#ifdef TEST_FIND_EMULATOR
  if (need_check) {
    if (*s != out2) {
      ERROR("FindEmulator is broken: %s\n%s\nvs\n%s", cmd.c_str(), s->c_str(),
            out2.c_str());
    }
  }
#endif

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return 1;
}

static std::vector<CommandResult*> g_command_results;

bool ShouldStoreCommandResult(std::string_view cmd) {
  // We really just want to ignore this one, or remove BUILD_DATETIME from
  // Android completely
  if (cmd == "date +%s")
    return false;

  Pattern pat(g_flags.ignore_dirty_pattern ? g_flags.ignore_dirty_pattern : "");
  Pattern nopat(
      g_flags.no_ignore_dirty_pattern ? g_flags.no_ignore_dirty_pattern : "");
  for (std::string_view tok : WordScanner(cmd)) {
    if (pat.Match(tok) && !nopat.Match(tok)) {
      return false;
    }
  }

  return true;
}

void ShellFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  std::string cmd = args[0]->Eval(ev);
  if (ev->avoid_io() && !HasNoIoInShellScript(cmd)) {
    if (TrimSpace(cmd) == "cat /dev/null") {
      return;
    }

    if (ev->eval_depth() > 0) {
      // A nested shell result is part of make-language evaluation.  It must
      // be available before the surrounding function or variable reference
      // can be expanded (for example, $(STAGE$(shell ...)_TFLAGS)).  A
      // deferred shell substitution would only be valid as a recipe token
      // and cannot determine the outer make construct.  Evaluate this class
      // of shell command while generating the graph and retain its result so
      // the normal Kati regeneration machinery can track it.
      std::string out;
      FindCommand* fc = NULL;
      int returnCode = ShellFuncImpl(ev->GetShell(), ev->GetShellFlag(), cmd,
                                     ev->loc(), &out, &fc);
      if (ShouldStoreCommandResult(cmd)) {
        CommandResult* cr = new CommandResult();
        cr->op = (fc == NULL) ? CommandOp::SHELL : CommandOp::FIND;
        cr->shell = ev->GetShell();
        cr->shellflag = ev->GetShellFlag();
        cr->cmd = cmd;
        cr->find.reset(fc);
        cr->result = out;
        cr->loc = ev->loc();
        g_command_results.push_back(cr);
      } else {
        delete fc;
      }
      *s += out;
      ShellStatusVar::SetValue(returnCode);
      return;
    }
    StripShellComment(&cmd);
    *s += "$(";
    *s += cmd;
    *s += ")";
    return;
  }

  std::string shell = ev->GetShell();
  std::string shellflag = ev->GetShellFlag();

  std::string out;
  FindCommand* fc = NULL;
  int returnCode = ShellFuncImpl(shell, shellflag, cmd, ev->loc(), &out, &fc);
  if (ShouldStoreCommandResult(cmd)) {
    CommandResult* cr = new CommandResult();
    cr->op = (fc == NULL) ? CommandOp::SHELL : CommandOp::FIND,
    cr->shell = shell;
    cr->shellflag = shellflag;
    cr->cmd = cmd;
    cr->find.reset(fc);
    cr->result = out;
    cr->loc = ev->loc();
    g_command_results.push_back(cr);
  }
  *s += out;
  ShellStatusVar::SetValue(returnCode);
}

void ShellFuncNoRerun(const std::vector<Value*>& args,
                      Evaluator* ev,
                      std::string* s) {
  std::string cmd = args[0]->Eval(ev);
  if (ev->avoid_io() && !HasNoIoInShellScript(cmd)) {
    // In the regular ShellFunc, if it sees a $(shell) inside of a rule when in
    // ninja mode, the shell command will just be written to the ninja file
    // instead of run directly by kati. So it already has the benefits of not
    // rerunning every time kati is invoked.
    ERROR_LOC(ev->loc(),
              "KATI_shell_no_rerun provides no benefit over regular $(shell) "
              "inside of a rule.",
              cmd.c_str());
    return;
  }

  std::string shell = ev->GetShell();
  std::string shellflag = ev->GetShellFlag();

  std::string out;
  FindCommand* fc = NULL;
  int returnCode = ShellFuncImpl(shell, shellflag, cmd, ev->loc(), &out, &fc);
  *s += out;
  ShellStatusVar::SetValue(returnCode);
}

void VarVisibilityFunc(const std::vector<Value*>& args,
                       Evaluator* ev,
                       std::string*) {
  std::string arg = args[0]->Eval(ev);
  std::vector<std::string> prefixes;

  std::stringstream ss(args[1]->Eval(ev));
  std::string prefix;
  while (ss >> prefix) {
    if (HasPrefix(prefix, "/")) {
      ERROR_LOC(ev->loc(), "Visibility prefix should not start with /");
    }
    if (HasPrefix(prefix, "../")) {
      ERROR_LOC(ev->loc(), "Visibility prefix should not start with ../");
    }

    std::string normalizedPrefix = prefix;
    NormalizePath(&normalizedPrefix);
    if (prefix != normalizedPrefix) {
      ERROR_LOC(ev->loc(),
                "Visibility prefix %s is not normalized. Normalized prefix: %s",
                prefix.c_str(), normalizedPrefix.c_str());
    }

    // one visibility prefix cannot be the prefix of another visibility prefix
    for (std::vector<std::string>::iterator it = prefixes.begin();
         it != prefixes.end(); ++it) {
      if (HasPathPrefix(*it, prefix)) {
        ERROR_LOC(ev->loc(),
                  "Visibility prefix %s is the prefix of another visibility "
                  "prefix %s",
                  prefix.c_str(), it->c_str());
      } else if (HasPathPrefix(prefix, *it)) {
        ERROR_LOC(ev->loc(),
                  "Visibility prefix %s is the prefix of another visibility "
                  "prefix %s",
                  it->c_str(), prefix.c_str());
      }
    }

    prefixes.push_back(prefix);
  }

  Symbol sym = Intern(arg);
  Var* v = ev->PeekVar(sym);
  // If variable is not defined, create an empty variable.
  if (!v->IsDefined()) {
    v = new SimpleVar(VarOrigin::FILE, ev->CurrentFrame(), ev->loc());
    sym.SetGlobalVar(v, false, nullptr);
  }
  v->SetVisibilityPrefix(prefixes, sym.c_str());
}

void CallFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  static const Symbol tmpvar_names[] = {
      Intern("0"), Intern("1"), Intern("2"), Intern("3"), Intern("4"),
      Intern("5"), Intern("6"), Intern("7"), Intern("8"), Intern("9")};

  ev->CheckStack();
  const std::string&& func_name_buf = args[0]->Eval(ev);
  Symbol func_sym = Intern(TrimSpace(func_name_buf));
  Var* func = ev->LookupVar(func_sym);
  func->Used(ev, func_sym);
  if (!func->IsDefined()) {
    KATI_WARN_LOC(ev->loc(), "*warning*: undefined user function: %s",
                  func_sym.c_str());
  }
  std::vector<std::unique_ptr<SimpleVar>> av;
  for (size_t i = 1; i < args.size(); i++) {
    av.emplace_back(std::make_unique<SimpleVar>(
        args[i]->Eval(ev), VarOrigin::AUTOMATIC, nullptr, Loc()));
  }
  std::vector<std::unique_ptr<ScopedGlobalVar>> sv;
  // $(0) names the function being called. Forwarding wrappers use it to
  // select another function, and nested calls must restore the outer name.
  SimpleVar called_name(std::string(func_sym.str()), VarOrigin::AUTOMATIC,
                        nullptr, Loc());
  ScopedGlobalVar zero(tmpvar_names[0], &called_name);
  for (size_t i = 1;; i++) {
    std::string s;
    Symbol tmpvar_name_sym;
    if (i < sizeof(tmpvar_names) / sizeof(tmpvar_names[0])) {
      tmpvar_name_sym = tmpvar_names[i];
    } else {
      s = StringPrintf("%d", i);
      tmpvar_name_sym = Intern(s);
    }
    if (i < args.size()) {
      sv.emplace_back(new ScopedGlobalVar(tmpvar_name_sym, av[i - 1].get()));
    } else {
      // We need to blank further automatic vars
      Var* v = ev->LookupVar(tmpvar_name_sym);
      if (!v->IsDefined())
        break;
      if (v->Origin() != VarOrigin::AUTOMATIC)
        break;

      av.emplace_back(new SimpleVar("", VarOrigin::AUTOMATIC, nullptr, Loc()));
      sv.emplace_back(new ScopedGlobalVar(tmpvar_name_sym, av[i - 1].get()));
    }
  }

  ev->DecrementEvalDepth();

  {
    ScopedFrame frame(ev->Enter(FrameType::CALL, func_sym.str(), ev->loc()));
    func->Eval(ev, s);
  }

  ev->IncrementEvalDepth();
}

void ForeachFunc(const std::vector<Value*>& args,
                 Evaluator* ev,
                 std::string* s) {
  const std::string&& varname = args[0]->Eval(ev);
  const std::string&& list = args[1]->Eval(ev);
  ev->DecrementEvalDepth();
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(list)) {
    std::unique_ptr<SimpleVar> v(
        new SimpleVar(std::string(tok), VarOrigin::AUTOMATIC, nullptr, Loc()));
    ScopedGlobalVar sv(Intern(varname), v.get());
    ww.MaybeAddSeparator();
    args[2]->Eval(ev, s);
  }
  ev->IncrementEvalDepth();
}

void LetFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  // GNU make expands the variable list and the value list before binding
  // anything.  The final text is deliberately evaluated afterwards, while
  // the bindings are in scope.  Use the evaluator's current scope when one
  // exists so target-specific variables remain lexically isolated; top-level
  // evaluation falls back to the same global scope used by foreach.
  const std::string&& names = args[0]->Eval(ev);
  const std::string&& values = args[1]->Eval(ev);
  std::vector<Symbol> symbols;
  for (std::string_view name : WordScanner(names))
    symbols.push_back(Intern(name));

  std::vector<std::unique_ptr<SimpleVar>> vars;
  std::vector<std::unique_ptr<ScopedVar>> scoped;
  std::vector<std::unique_ptr<ScopedGlobalVar>> global;
  WordScanner value_words(values);
  auto value_it = value_words.begin();
  auto value_end = value_words.end();
  std::string remainder;
  for (size_t i = 0; i < symbols.size(); ++i) {
    std::string value;
    if (value_it != value_end) {
      if (i + 1 == symbols.size()) {
        // The final let variable receives all remaining words.
        value.assign(*value_it);
        ++value_it;
        while (value_it != value_end) {
          value += ' ';
          value += *value_it;
          ++value_it;
        }
      } else {
        value.assign(*value_it);
        ++value_it;
      }
    }
    vars.emplace_back(std::make_unique<SimpleVar>(value, VarOrigin::AUTOMATIC,
                                                  nullptr, Loc()));
    if (ev->current_scope()) {
      scoped.emplace_back(std::make_unique<ScopedVar>(
          ev->current_scope(), symbols[i], vars.back().get()));
    } else {
      global.emplace_back(
          std::make_unique<ScopedGlobalVar>(symbols[i], vars.back().get()));
    }
  }
  args[2]->Eval(ev, s);
}

void IntcmpFunc(const std::vector<Value*>& args,
                Evaluator* ev,
                std::string* s) {
  const std::string&& lhs_string = args[0]->Eval(ev);
  const std::string&& rhs_string = args[1]->Eval(ev);
  errno = 0;
  char* lhs_end = nullptr;
  char* rhs_end = nullptr;
  const long long lhs = strtoll(lhs_string.c_str(), &lhs_end, 10);
  const int lhs_errno = errno;
  errno = 0;
  const long long rhs = strtoll(rhs_string.c_str(), &rhs_end, 10);
  const int rhs_errno = errno;
  if (lhs_errno == ERANGE || rhs_errno == ERANGE ||
      lhs_end == lhs_string.c_str() || *lhs_end != '\0' ||
      rhs_end == rhs_string.c_str() || *rhs_end != '\0') {
    ev->Error(StringPrintf("*** intcmp arguments must be integers: %s, %s",
                           lhs_string.c_str(), rhs_string.c_str()));
    return;
  }
  size_t branch = lhs < rhs ? 2 : lhs == rhs ? 3 : 4;
  if (branch < args.size())
    args[branch]->Eval(ev, s);
}

void ForeachWithSepFunc(const std::vector<Value*>& args,
                        Evaluator* ev,
                        std::string* s) {
  const std::string&& varname = args[0]->Eval(ev);
  const std::string&& separator = args[1]->Eval(ev);
  const std::string&& list = args[2]->Eval(ev);
  ev->DecrementEvalDepth();
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(list)) {
    std::unique_ptr<SimpleVar> v(
        new SimpleVar(std::string(tok), VarOrigin::AUTOMATIC, nullptr, Loc()));
    ScopedGlobalVar sv(Intern(varname), v.get());
    ww.MaybeAddSeparator(separator);
    args[3]->Eval(ev, s);
  }
  ev->IncrementEvalDepth();
}

void OriginFunc(const std::vector<Value*>& args,
                Evaluator* ev,
                std::string* s) {
  const std::string&& var_name = args[0]->Eval(ev);
  Var* var = ev->LookupVar(Intern(var_name));
  *s += GetOriginStr(var->Origin());
}

void FlavorFunc(const std::vector<Value*>& args,
                Evaluator* ev,
                std::string* s) {
  const std::string&& var_name = args[0]->Eval(ev);
  Var* var = ev->LookupVar(Intern(var_name));
  *s += var->Flavor();
}

void InfoFunc(const std::vector<Value*>& args, Evaluator* ev, std::string*) {
  const std::string&& a = args[0]->Eval(ev);
  if (ev->avoid_io()) {
    ev->add_delayed_output_command(
        StringPrintf("echo -e \"%s\"", EchoEscape(a).c_str()));
    return;
  }
  printf("%s\n", a.c_str());
  fflush(stdout);
}

void WarningFunc(const std::vector<Value*>& args, Evaluator* ev, std::string*) {
  const std::string&& a = args[0]->Eval(ev);
  if (ev->avoid_io()) {
    ev->add_delayed_output_command(StringPrintf(
        "echo -e \"%s:%d: %s\" 2>&1", LOCF(ev->loc()), EchoEscape(a).c_str()));
    return;
  }
  WARN_LOC(ev->loc(), "%s", a.c_str());
}

void ErrorFunc(const std::vector<Value*>& args, Evaluator* ev, std::string*) {
  const std::string&& a = args[0]->Eval(ev);
  if (ev->avoid_io()) {
    ev->add_delayed_output_command(
        StringPrintf("echo -e \"%s:%d: *** %s.\" 2>&1 && false",
                     LOCF(ev->loc()), EchoEscape(a).c_str()));
    return;
  }
  ev->Error(StringPrintf("*** %s.", a.c_str()));
}

static void FileReadFunc_(Evaluator* ev,
                          const std::string& filename,
                          std::string* s,
                          bool rerun) {
  int fd = open(filename.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) {
      if (ShouldStoreCommandResult(filename)) {
        CommandResult* cr = new CommandResult();
        cr->op = CommandOp::READ_MISSING;
        cr->cmd = filename;
        cr->loc = ev->loc();
        g_command_results.push_back(cr);
      }
      return;
    } else {
      ev->Error("*** open failed.");
    }
  }

  struct stat st;
  if (fstat(fd, &st) < 0) {
    ev->Error("*** fstat failed.");
  }

  size_t len = st.st_size;
  std::string out;
  out.resize(len);
  ssize_t r = HANDLE_EINTR(read(fd, &out[0], len));
  if (r != static_cast<ssize_t>(len)) {
    ev->Error("*** read failed.");
  }

  if (close(fd) < 0) {
    ev->Error("*** close failed.");
  }

  if (out.back() == '\n') {
    out.pop_back();
  }

  if (rerun && ShouldStoreCommandResult(filename)) {
    CommandResult* cr = new CommandResult();
    cr->op = CommandOp::READ;
    cr->cmd = filename;
    cr->loc = ev->loc();
    g_command_results.push_back(cr);
  }
  *s += out;
}

static void FileWriteFunc_(Evaluator* ev,
                           const std::string& filename,
                           bool append,
                           std::string text,
                           bool rerun) {
  FILE* f = fopen(filename.c_str(), append ? "ab" : "wb");
  if (f == NULL) {
    ev->Error(StringPrintf("*** fopen %s failed: %s.", filename.c_str(),
                           strerror(errno)));
  }

  if (fwrite(&text[0], text.size(), 1, f) != 1) {
    ev->Error("*** fwrite failed.");
  }

  if (fclose(f) != 0) {
    ev->Error("*** fclose failed.");
  }

  if (rerun && ShouldStoreCommandResult(filename)) {
    CommandResult* cr = new CommandResult();
    cr->op = CommandOp::WRITE;
    cr->cmd = filename;
    cr->result = text;
    cr->loc = ev->loc();
    g_command_results.push_back(cr);
  }
}

void FileFunc_(const std::vector<Value*>& args,
               Evaluator* ev,
               std::string* s,
               bool rerun) {
  std::string arg = args[0]->Eval(ev);
  std::string_view filename = TrimSpace(arg);

  if (filename.size() <= 1) {
    ev->Error("*** Missing filename");
  }

  if (filename[0] == '<') {
    filename = TrimLeftSpace(filename.substr(1));
    if (!filename.size()) {
      ev->Error("*** Missing filename");
    }
    if (args.size() > 1) {
      ev->Error("*** invalid argument");
    }

    if (ev->avoid_io()) {
      std::string filename_str(filename);

      if (rerun && ShouldStoreCommandResult(filename_str)) {
        CommandResult* cr = new CommandResult();
        cr->op =
            Exists(filename_str) ? CommandOp::READ : CommandOp::READ_MISSING;
        cr->cmd = filename_str;
        cr->loc = ev->loc();
        g_command_results.push_back(cr);
      }

      std::string executable = GetExecutablePath();
      EscapeShell(&executable);
      EscapeShell(&filename_str);

      *s += "$(";
      *s += executable;
      *s += " --file-read ";
      *s += filename_str;
      *s += ")";
      return;
    }

    FileReadFunc_(ev, std::string(filename), s, rerun);
  } else if (filename[0] == '>') {
    if (ev->avoid_io()) {
      // A recipe-time $(file >name,text) must run when the Ninja edge runs,
      // after automatic variables such as $@ and $^ have been expanded.
      // Materialize it as a shell printf instead of writing during graph
      // generation.
      bool append = filename.size() > 1 && filename[1] == '>';
      filename = filename.substr(append ? 2 : 1);
      filename = TrimLeftSpace(filename);
      if (filename.empty()) {
        ev->Error("*** Missing filename");
      }

      std::string text;
      if (args.size() > 1) {
        text = args[1]->Eval(ev);
        if (text.empty() || text.back() != '\n')
          text.push_back('\n');
      }

      std::string escaped_filename(filename);
      std::string escaped_text;
      for (char c : text) {
        if (c == '\\') {
          escaped_text += "\\\\";
        } else if (c == '\n') {
          escaped_text += "\\n";
        } else {
          escaped_text += c;
        }
      }
      EscapeShell(&escaped_filename);
      EscapeShell(&escaped_text);
      *s += "printf '%b' \"";
      *s += escaped_text;
      *s += "\" ";
      *s += append ? ">> \"" : "> \"";
      *s += escaped_filename;
      *s += "\"";
      return;
    }

    bool append = false;
    if (filename[1] == '>') {
      append = true;
      filename = filename.substr(2);
    } else {
      filename = filename.substr(1);
    }
    filename = TrimLeftSpace(filename);
    if (!filename.size()) {
      ev->Error("*** Missing filename");
    }

    std::string text;
    if (args.size() > 1) {
      text = args[1]->Eval(ev);
      if (text.size() == 0 || text.back() != '\n') {
        text.push_back('\n');
      }
    }

    FileWriteFunc_(ev, std::string(filename), append, text, rerun);
  } else {
    ev->Error(StringPrintf("*** Invalid file operation: %s.  Stop.",
                           std::string(filename).c_str()));
  }
}

void FileFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  FileFunc_(args, ev, s, true);
}

void FileFuncNoRerun(const std::vector<Value*>& args,
                     Evaluator* ev,
                     std::string* s) {
  FileFunc_(args, ev, s, false);
}

void DeprecatedVarFunc(const std::vector<Value*>& args,
                       Evaluator* ev,
                       std::string*) {
  std::string vars_str = args[0]->Eval(ev);
  std::string msg;

  if (args.size() == 2) {
    msg = ". " + args[1]->Eval(ev);
  }

  if (ev->avoid_io()) {
    ev->Error("*** $(KATI_deprecated_var ...) is not supported in rules.");
  }

  for (std::string_view var : WordScanner(vars_str)) {
    Symbol sym = Intern(var);
    Var* v = ev->PeekVar(sym);
    if (!v->IsDefined()) {
      v = new SimpleVar(VarOrigin::FILE, ev->CurrentFrame(), ev->loc());
      sym.SetGlobalVar(v, false, nullptr);
    }

    if (v->Deprecated()) {
      ev->Error(
          StringPrintf("*** Cannot call KATI_deprecated_var on already "
                       "deprecated variable: %s.",
                       sym.c_str()));
    } else if (v->Obsolete()) {
      ev->Error(
          StringPrintf("*** Cannot call KATI_deprecated_var on already "
                       "obsolete variable: %s.",
                       sym.c_str()));
    }

    v->SetDeprecated(msg);
  }
}

void ObsoleteVarFunc(const std::vector<Value*>& args,
                     Evaluator* ev,
                     std::string*) {
  std::string vars_str = args[0]->Eval(ev);
  std::string msg;

  if (args.size() == 2) {
    msg = ". " + args[1]->Eval(ev);
  }

  if (ev->avoid_io()) {
    ev->Error("*** $(KATI_obsolete_var ...) is not supported in rules.");
  }

  for (std::string_view var : WordScanner(vars_str)) {
    Symbol sym = Intern(var);
    Var* v = ev->PeekVar(sym);
    if (!v->IsDefined()) {
      v = new SimpleVar(VarOrigin::FILE, ev->CurrentFrame(), ev->loc());
      sym.SetGlobalVar(v, false, nullptr);
    }

    if (v->Deprecated()) {
      ev->Error(
          StringPrintf("*** Cannot call KATI_obsolete_var on already "
                       "deprecated variable: %s.",
                       sym.c_str()));
    } else if (v->Obsolete()) {
      ev->Error(StringPrintf(
          "*** Cannot call KATI_obsolete_var on already obsolete variable: %s.",
          sym.c_str()));
    }

    v->SetObsolete(msg);
  }
}

void DeprecateExportFunc(const std::vector<Value*>& args,
                         Evaluator* ev,
                         std::string*) {
  std::string msg = ". " + args[0]->Eval(ev);

  if (ev->avoid_io()) {
    ev->Error("*** $(KATI_deprecate_export) is not supported in rules.");
  }

  if (ev->ExportObsolete()) {
    ev->Error("*** Export is already obsolete.");
  } else if (ev->ExportDeprecated()) {
    ev->Error("*** Export is already deprecated.");
  }

  ev->SetExportDeprecated(msg);
}

void ObsoleteExportFunc(const std::vector<Value*>& args,
                        Evaluator* ev,
                        std::string*) {
  std::string msg = ". " + args[0]->Eval(ev);

  if (ev->avoid_io()) {
    ev->Error("*** $(KATI_obsolete_export) is not supported in rules.");
  }

  if (ev->ExportObsolete()) {
    ev->Error("*** Export is already obsolete.");
  }

  ev->SetExportObsolete(msg);
}

void ProfileFunc(const std::vector<Value*>& args, Evaluator* ev, std::string*) {
  for (auto arg : args) {
    std::string files = arg->Eval(ev);
    for (std::string_view file : WordScanner(files)) {
      ev->ProfileMakefile(file);
    }
  }
}

void VariableLocationFunc(const std::vector<Value*>& args,
                          Evaluator* ev,
                          std::string* s) {
  std::string arg = args[0]->Eval(ev);
  WordWriter ww(s);
  for (std::string_view var : WordScanner(arg)) {
    Symbol sym = Intern(var);
    Var* v = ev->PeekVar(sym);
    const Loc& loc = v->Location();
    ww.Write(loc.filename ? loc.filename : "<unknown>");
    s->append(":");
    s->append(std::to_string(loc.lineno > 0 ? loc.lineno : 0));
  }
}

void ExtraFileDepsFunc(const std::vector<Value*>& args,
                       Evaluator* ev,
                       std::string*) {
  for (auto arg : args) {
    std::string files = arg->Eval(ev);
    for (std::string_view file : WordScanner(files)) {
      if (!Exists(file)) {
        std::string errorMsg = "*** file does not exist: ";
        errorMsg += file;
        ev->Error(errorMsg);
      }
      MakefileCacheManager::Get().AddExtraFileDep(file);
    }
  }
}

// A dependency-free, deliberately pure subset of the GNU Make Guile
// function. GNU Make substitutes the value of the last Guile expression;
// implementing only expressions with no external runtime or side effects
// keeps graph generation deterministic and does not embed Guile.
struct GuileExpr {
  bool list = false;
  bool string = false;
  std::string atom;
  std::vector<GuileExpr> items;
};

class GuileReader {
 public:
  explicit GuileReader(std::string_view input) : input_(input) {}

  GuileExpr Read(Evaluator* ev) {
    SkipSpace();
    if (pos_ == input_.size())
      Fail(ev, "empty expression");
    GuileExpr result = ReadExpr(ev);
    SkipSpace();
    if (pos_ != input_.size())
      Fail(ev, "trailing expressions are not supported");
    return result;
  }

 private:
  [[noreturn]] void Fail(Evaluator* ev, std::string_view reason) {
    ev->Error(
        StringPrintf("*** guile: unsupported or invalid pure expression (%s)",
                     std::string(reason).c_str()));
    abort();
  }

  void SkipSpace() {
    while (pos_ < input_.size() &&
           isspace(static_cast<unsigned char>(input_[pos_])))
      ++pos_;
  }

  GuileExpr ReadExpr(Evaluator* ev) {
    SkipSpace();
    if (pos_ == input_.size())
      Fail(ev, "unexpected end of expression");
    if (input_[pos_] == '(')
      return ReadList(ev);
    if (input_[pos_] == '"')
      return ReadString(ev);

    GuileExpr result;
    result.atom = ReadAtom(ev);
    return result;
  }

  GuileExpr ReadList(Evaluator* ev) {
    GuileExpr result;
    result.list = true;
    ++pos_;
    while (true) {
      SkipSpace();
      if (pos_ == input_.size())
        Fail(ev, "missing closing parenthesis");
      if (input_[pos_] == ')') {
        ++pos_;
        return result;
      }
      result.items.push_back(ReadExpr(ev));
    }
  }

  GuileExpr ReadString(Evaluator* ev) {
    GuileExpr result;
    result.string = true;
    ++pos_;
    while (pos_ < input_.size()) {
      char c = input_[pos_++];
      if (c == '"')
        return result;
      if (c == '\\') {
        if (pos_ == input_.size())
          Fail(ev, "unterminated string escape");
        char escaped = input_[pos_++];
        switch (escaped) {
          case 'n':
            result.atom += '\n';
            break;
          case 'r':
            result.atom += '\r';
            break;
          case 't':
            result.atom += '\t';
            break;
          default:
            result.atom += escaped;
            break;
        }
      } else {
        result.atom += c;
      }
    }
    Fail(ev, "unterminated string");
  }

  std::string ReadAtom(Evaluator* ev) {
    const size_t start = pos_;
    while (pos_ < input_.size() &&
           !isspace(static_cast<unsigned char>(input_[pos_])) &&
           input_[pos_] != '(' && input_[pos_] != ')')
      ++pos_;
    if (start == pos_)
      Fail(ev, "unexpected token");
    return std::string(input_.substr(start, pos_ - start));
  }

  std::string_view input_;
  size_t pos_ = 0;
};

static std::string EvalGuileExpr(const GuileExpr& expr, Evaluator* ev) {
  if (!expr.list)
    return expr.atom;

  if (expr.items.empty())
    ev->Error("*** guile: empty lists are not supported");
  const std::string& function = expr.items[0].atom;

  if (function == "quote") {
    if (expr.items.size() != 2)
      ev->Error("*** guile: quote expects one argument");
    const GuileExpr& quoted = expr.items[1];
    if (quoted.list)
      ev->Error("*** guile: quoted lists are not supported");
    return quoted.atom;
  }

  if (function == "begin") {
    std::string result;
    for (size_t i = 1; i < expr.items.size(); ++i)
      result = EvalGuileExpr(expr.items[i], ev);
    return result;
  }

  if (function == "string-append") {
    std::string result;
    for (size_t i = 1; i < expr.items.size(); ++i)
      result += EvalGuileExpr(expr.items[i], ev);
    return result;
  }

  if (function == "if") {
    if (expr.items.size() != 3 && expr.items.size() != 4)
      ev->Error("*** guile: if expects two or three arguments");
    const std::string condition = EvalGuileExpr(expr.items[1], ev);
    if (!condition.empty() && condition != "#f")
      return EvalGuileExpr(expr.items[2], ev);
    return expr.items.size() == 4 ? EvalGuileExpr(expr.items[3], ev) : "";
  }

  ev->Error(
      StringPrintf("*** guile: procedure `%s' is unsupported without Guile",
                   function.c_str()));
  return "";
}

void GuileFunc(const std::vector<Value*>& args, Evaluator* ev, std::string* s) {
  const std::string body = args[0]->Eval(ev);
  GuileReader reader(body);
  *s = EvalGuileExpr(reader.Read(ev), ev);
}

#define ENTRY(name, args...) \
  {                          \
    name, {                  \
      name, args             \
    }                        \
  }

static const std::unordered_map<std::string_view, FuncInfo> g_func_info_map = {

    ENTRY("patsubst", &PatsubstFunc, 3, 3, false, false),
    ENTRY("strip", &StripFunc, 1, 1, false, false),
    ENTRY("subst", &SubstFunc, 3, 3, false, false),
    ENTRY("findstring", &FindstringFunc, 2, 2, false, false),
    ENTRY("filter", &FilterFunc, 2, 2, false, false),
    ENTRY("filter-out", &FilterOutFunc, 2, 2, false, false),
    ENTRY("sort", &SortFunc, 1, 1, false, false),
    ENTRY("word", &WordFunc, 2, 2, false, false),
    ENTRY("wordlist", &WordlistFunc, 3, 3, false, false),
    ENTRY("words", &WordsFunc, 1, 1, false, false),
    ENTRY("firstword", &FirstwordFunc, 1, 1, false, false),
    ENTRY("lastword", &LastwordFunc, 1, 1, false, false),

    ENTRY("join", &JoinFunc, 2, 2, false, false),
    ENTRY("wildcard", &WildcardFunc, 1, 1, false, false),
    ENTRY("dir", &DirFunc, 1, 1, false, false),
    ENTRY("notdir", &NotdirFunc, 1, 1, false, false),
    ENTRY("suffix", &SuffixFunc, 1, 1, false, false),
    ENTRY("basename", &BasenameFunc, 1, 1, false, false),
    ENTRY("addsuffix", &AddsuffixFunc, 2, 2, false, false),
    ENTRY("addprefix", &AddprefixFunc, 2, 2, false, false),
    ENTRY("realpath", &RealpathFunc, 1, 1, false, false),
    ENTRY("abspath", &AbspathFunc, 1, 1, false, false),

    ENTRY("if", &IfFunc, 3, 2, false, true),
    ENTRY("and", &AndFunc, 0, 0, true, false),
    ENTRY("or", &OrFunc, 0, 0, true, false),

    ENTRY("value", &ValueFunc, 1, 1, false, false),
    ENTRY("eval", &EvalFunc, 1, 1, false, false),
    ENTRY("shell", &ShellFunc, 1, 1, false, false),
    ENTRY("call", &CallFunc, 0, 0, false, false),
    ENTRY("foreach", &ForeachFunc, 3, 3, false, false),
    ENTRY("let", &LetFunc, 3, 3, false, false),
    ENTRY("intcmp", &IntcmpFunc, 5, 2, false, false),
    ENTRY("guile", &GuileFunc, 1, 1, false, false),

    ENTRY("origin", &OriginFunc, 1, 1, false, false),
    ENTRY("flavor", &FlavorFunc, 1, 1, false, false),

    ENTRY("info", &InfoFunc, 1, 1, false, false),
    ENTRY("warning", &WarningFunc, 1, 1, false, false),
    ENTRY("error", &ErrorFunc, 1, 1, false, false),

    ENTRY("file", &FileFunc, 2, 1, false, false),

    /* Kati custom extension functions */
    ENTRY("KATI_deprecated_var", &DeprecatedVarFunc, 2, 1, false, false),
    ENTRY("KATI_obsolete_var", &ObsoleteVarFunc, 2, 1, false, false),
    ENTRY("KATI_deprecate_export", &DeprecateExportFunc, 1, 1, false, false),
    ENTRY("KATI_obsolete_export", &ObsoleteExportFunc, 1, 1, false, false),

    ENTRY("KATI_profile_makefile", &ProfileFunc, 0, 0, false, false),
    ENTRY("KATI_variable_location", &VariableLocationFunc, 1, 1, false, false),

    ENTRY("KATI_extra_file_deps", &ExtraFileDepsFunc, 0, 0, false, false),
    ENTRY("KATI_shell_no_rerun", &ShellFuncNoRerun, 1, 1, false, false),
    ENTRY("KATI_foreach_sep", &ForeachWithSepFunc, 4, 4, false, false),
    ENTRY("KATI_file_no_rerun", &FileFuncNoRerun, 2, 1, false, false),
    ENTRY("KATI_visibility_prefix", &VarVisibilityFunc, 2, 1, false, false),
};

}  // namespace

const FuncInfo* GetFuncInfo(std::string_view name) {
  auto found = g_func_info_map.find(name);
  if (found == g_func_info_map.end())
    return nullptr;
  return &found->second;
}

const std::vector<CommandResult*>& GetShellCommandResults() {
  return g_command_results;
}
