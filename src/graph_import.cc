// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "graph_import.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <set>

#include "command.h"
#include "expr.h"
#include "file.h"
#include "file_cache.h"
#include "fileutil.h"
#include "flags.h"
#include "stmt.h"
#include "strutil.h"

namespace {
bool StaticExpansion(std::string_view source) {
  for (size_t i = 0; i < source.size(); ++i) {
    if (source[i] != '$' || ++i == source.size())
      continue;
    if (source[i] != '(' && source[i] != '{')
      continue;
    const char closer = source[i] == '(' ? ')' : '}';
    const size_t end = source.find(closer, i + 1);
    if (end == std::string_view::npos)
      return false;
    std::string_view name = source.substr(i + 1, end - i - 1);
    if (name.empty() || name == "MAKE" || name == ".VARIABLES" ||
        name == ".KATI_SYMBOLS" || name == ".SHELLSTATUS" ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTU"
                               "VWXYZ0123456789_.-@%<^+?|") !=
            std::string_view::npos)
      return false;
    i = end;
  }
  return true;
}

bool Inspect(const std::string& name,
             std::set<std::string>* seen,
             std::set<std::string>* includes,
             std::set<std::string>* outputs) {
  if (seen->size() >= 1000 || !seen->insert(name).second)
    return false;
  const Makefile& file = MakefileCacheManager::Get().ReadMakefile(name);
  if (!file.Exists())
    return false;
  for (const Stmt* statement : file.stmts()) {
    if (const auto* assignment = dynamic_cast<const AssignStmt*>(statement)) {
      if (!assignment->lhs->IsLiteral() ||
          !StaticExpansion(assignment->orig_rhs))
        return false;
      const std::string_view lhs = assignment->lhs->GetLiteralValueUnsafe();
      if (lhs == "MAKE" || lhs == "MAKEFLAGS" || lhs == "MAKEOVERRIDES" ||
          lhs == "VPATH" || lhs == "SHELL" || lhs == ".SHELLFLAGS")
        return false;
    } else if (const auto* rule = dynamic_cast<const RuleStmt*>(statement)) {
      if (!rule->lhs->IsLiteral() || rule->rhs != nullptr ||
          rule->sep != RuleStmt::SEP_NULL)
        return false;
      std::string_view lhs = rule->lhs->GetLiteralValueUnsafe();
      if (lhs.find_first_of("%*?[\\=$") != std::string_view::npos ||
          lhs.find("::") != std::string_view::npos)
        return false;
      const size_t colon = lhs.find(':');
      if (colon == std::string_view::npos)
        return false;
      for (auto output : WordScanner(lhs.substr(0, colon))) {
        if (output == ".NOTPARALLEL" || output == ".WAIT" ||
            output == ".ONESHELL" || output == ".EXPORT_ALL_VARIABLES")
          return false;
        std::string absolute;
        AbsPath(output, &absolute);
        outputs->insert(absolute);
      }
    } else if (const auto* recipe =
                   dynamic_cast<const CommandStmt*>(statement)) {
      if (!StaticExpansion(recipe->orig))
        return false;
    } else if (const auto* include =
                   dynamic_cast<const IncludeStmt*>(statement)) {
      if (!include->expr->IsLiteral())
        return false;
      for (auto input : WordScanner(include->expr->GetLiteralValueUnsafe())) {
        includes->insert(std::string(input));
        if (!Inspect(std::string(input), seen, includes, outputs))
          return false;
      }
    } else if (const auto* exported =
                   dynamic_cast<const ExportStmt*>(statement)) {
      if (!exported->expr->IsLiteral())
        return false;
    } else {
      return false;
    }
  }
  return true;
}
}  // namespace

bool StaticImportSources(std::vector<std::string>* files) {
  // Environment and argv definitions can hide effects behind a plain $(VAR).
  // Require their expansions to obey the same preflight as source definitions.
  for (char** p = environ; p && *p; ++p)
    if (!StaticExpansion(*p))
      return false;
  if (getenv("MAKEFILES"))
    return false;
  for (auto value : g_flags.cl_vars)
    if (!StaticExpansion(value))
      return false;
  std::set<std::string> seen, includes, outputs;
  for (const char* name : g_flags.makefiles)
    if (strcmp(name, "-") == 0 || !Inspect(name, &seen, &includes, &outputs))
      return false;
  for (const auto& source : seen) {
    std::string absolute;
    AbsPath(source, &absolute);
    if (outputs.count(absolute))
      return false;
  }
  files->assign(seen.begin(), seen.end());
  return true;
}

bool ParseImportCommand(const Command& command,
                        std::vector<std::string>* argv) {
  if (command.echo || command.ignore_error ||
      (!command.shell.empty() && command.shell != "/bin/sh") ||
      (!command.shellflag.empty() && command.shellflag != "-c"))
    return false;
  // Ninja may defer exported shell/file expansions until the recipe runs.
  // Those strings are not resolved environment values for child discovery.
  for (const auto& value : command.environment)
    if (value.find_first_of("$`") != std::string::npos)
      return false;
  std::string word;
  char quote = 0;
  bool started = false;
  for (char c : command.display_cmd) {
    if (quote) {
      if (c == quote)
        quote = 0;
      else {
        if (quote == '"' && (c == '$' || c == '`' || c == '\\'))
          return false;
        word += c;
      }
    } else if (c == '\'' || c == '"') {
      quote = c;
      started = true;
    } else if (c == ' ' || c == '\t') {
      if (started) {
        argv->push_back(std::move(word));
        word.clear();
        started = false;
      }
    } else {
      if (std::string_view(";|&<>$`\\\n\r*?[](){}").find(c) !=
          std::string_view::npos)
        return false;
      word += c;
      started = true;
    }
  }
  if (quote)
    return false;
  if (started)
    argv->push_back(std::move(word));
  if (argv->empty() || argv->front() != GetExecutablePath())
    return false;
  // Import changes execution ownership; query/touch/dry-run and arbitrary
  // frontend options must keep the original recursive execution boundary.
  for (size_t i = 1; i < argv->size(); ++i) {
    const std::string& arg = (*argv)[i];
    if (arg == "-C" || arg == "-f") {
      if (++i == argv->size())
        return false;
    } else if (!arg.empty() && arg.front() == '-') {
      return false;
    }
  }
  return true;
}

int GenerateImportedGraph(const Command& command,
                          const std::vector<std::string>& argv,
                          const std::string& directory,
                          const std::string& gate) {
  // Discovery diagnostics cannot be printed speculatively: ownership or a
  // later sibling can still force ordinary recursion. A diagnostic-producing
  // child retains that original boundary and prints its messages once there.
  std::string temporary = directory + "/discovery.XXXXXX";
  int diagnostics = mkstemp(temporary.data());
  if (diagnostics < 0)
    return -1;
  unlink(temporary.c_str());
  pid_t child = fork();
  if (child < 0) {
    close(diagnostics);
    return -1;
  }
  if (child == 0) {
    if (dup2(diagnostics, STDOUT_FILENO) < 0 ||
        dup2(diagnostics, STDERR_FILENO) < 0)
      _exit(126);
    if (diagnostics > STDERR_FILENO)
      close(diagnostics);
    for (const auto& name : command.unset_environment)
      unsetenv(name.c_str());
    for (const auto& item : command.environment) {
      const size_t equal = item.find('=');
      if (equal != std::string::npos)
        setenv(item.substr(0, equal).c_str(), item.c_str() + equal + 1, 1);
    }
    setenv("MAKELEVEL", std::to_string(g_flags.make_level + 1).c_str(), 1);
    std::vector<std::string> arguments = argv;
    arguments.push_back("--ninja");
    arguments.push_back("--graph_optimizations=safe");
    for (const auto& feature : g_flags.disabled_graph_optimizations)
      arguments.push_back("--disable_graph_optimization=" + feature);
    arguments.push_back("--internal-import-root=" + directory);
    arguments.push_back("--internal-import-gate=" + gate);
    arguments.push_back("--ninja_dir=" + directory);
    std::vector<char*> pointers;
    for (auto& arg : arguments)
      pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    execv(pointers[0], pointers.data());
    _exit(127);
  }
  int status;
  const bool waited = HANDLE_EINTR(waitpid(child, &status, 0)) == child;
  struct stat st;
  const bool quiet = fstat(diagnostics, &st) == 0 && st.st_size == 0;
  close(diagnostics);
  if (!waited)
    return -1;
  if (!quiet)
    return 3;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
