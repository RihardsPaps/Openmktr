// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "graph_import.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>

#include "command.h"
#include "expr.h"
#include "file.h"
#include "file_cache.h"
#include "fileutil.h"
#include "flags.h"
#include "graph_optimization.h"
#include "import_observer.h"
#include "log.h"
#include "session.h"
#include "stats.h"
#include "stmt.h"
#include "strutil.h"

namespace {
struct ImportValidationState {
  std::vector<std::string> plans;
};
bool IgnoredBuildEntry(std::string_view name) {
  return name == "." || name == ".." || name == ".git" || name == ".openmktr" ||
         name == ".imports" || HasPrefix(name, ".tmp_");
}
bool BuildMembership(const std::string& directory, std::string* membership) {
  DIR* handle = opendir(directory.c_str());
  if (!handle)
    return false;
  std::vector<std::string> entries;
  bool valid = true;
  errno = 0;
  while (dirent* entry = readdir(handle)) {
    const std::string name(entry->d_name);
    if (IgnoredBuildEntry(name))
      continue;
    struct stat metadata;
    if (lstat((directory + "/" + name).c_str(), &metadata) != 0) {
      valid = false;
      break;
    }
    if (S_ISDIR(metadata.st_mode) || HasSuffix(name, ".o") ||
        HasSuffix(name, ".cmd"))
      entries.push_back(std::to_string(metadata.st_mode & S_IFMT) + ":" + name);
    errno = 0;
  }
  valid &= errno == 0;
  closedir(handle);
  std::sort(entries.begin(), entries.end());
  membership->clear();
  for (const auto& entry : entries)
    AppendGraphString(entry, membership);
  return valid;
}
bool ValidateBuildTree(std::string_view view) {
  std::vector<std::string> paths, identities, memberships;
  while (!view.empty()) {
    std::string path, identity, membership;
    if (!ConsumeGraphString(&view, &path) ||
        !ConsumeGraphString(&view, &identity) ||
        !ConsumeGraphString(&view, &membership))
      return false;
    paths.push_back(std::move(path));
    identities.push_back(std::move(identity));
    memberships.push_back(std::move(membership));
  }
  const auto metadata = StatFiles(paths);
  for (size_t i = 0; i < paths.size(); ++i) {
    if (metadata[i].error || !S_ISDIR(metadata[i].metadata.st_mode))
      return false;
    if (metadata[i].Identity() == identities[i])
      continue;
    std::string current;
    if (!BuildMembership(paths[i], &current) || current != memberships[i])
      return false;
  }
  return true;
}
bool ResolveFileReadExpansions(std::string* value) {
  const std::string prefix = GetExecutablePath() + " --file-read ";
  size_t position = 0;
  while ((position = value->find("$(", position)) != std::string::npos) {
    const size_t end = value->find(')', position + 2);
    if (end == std::string::npos)
      break;
    const std::string_view expression(value->data() + position + 2,
                                      end - position - 2);
    if (!HasPrefix(expression, prefix)) {
      position = end + 1;
      continue;
    }
    const std::string_view path = expression.substr(prefix.size());
    if (path.empty() ||
        path.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTU"
                               "VWXYZ0123456789_./-") != std::string_view::npos)
      return false;
    std::ifstream input(std::string(path), std::ios::binary);
    if (!input)
      return false;
    std::string contents((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    if (!contents.empty() && contents.back() == '\n')
      contents.pop_back();
    value->replace(position, end - position + 1, contents);
    position += contents.size();
  }
  // Other dollar expressions and backticks are literal Make variable values
  // carried to the child. Only Openmktr's deferred file-read wrapper needs to
  // be resolved while importing the graph.
  return true;
}

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

const std::vector<std::string>& ImportValidationPlans() {
  return SessionService<ImportValidationState>().plans;
}

void AddImportValidationPlan(const std::string& path) {
  auto& plans = SessionService<ImportValidationState>().plans;
  if (std::find(plans.begin(), plans.end(), path) == plans.end())
    plans.push_back(path);
}

void KbuildDependencyIndex::Read(const std::string& directory,
                                 const std::string& source_directory) {
  objects.clear();
  covered_inputs.clear();
  directories.clear();
  complete = true;
  char resolved_source[4096];
  const std::string source = realpath(source_directory.c_str(), resolved_source)
                                 ? resolved_source
                                 : "";
  std::vector<std::string> pending{directory};
  while (!pending.empty()) {
    std::string parent = std::move(pending.back());
    pending.pop_back();
    std::string membership;
    const std::string identity = GetFileIdentity(parent);
    if (!BuildMembership(parent, &membership))
      complete = false;
    directories.emplace(parent, std::make_pair(identity, membership));
    DIR* handle = opendir(parent.c_str());
    if (!handle) {
      complete = false;
      continue;
    }
    while (dirent* entry = readdir(handle)) {
      const std::string name(entry->d_name);
      if (IgnoredBuildEntry(name))
        continue;
      const std::string path = parent + "/" + name;
      struct stat metadata;
      if (entry->d_type == DT_DIR) {
        pending.push_back(path);
        continue;
      }
      if (entry->d_type == DT_UNKNOWN) {
        if (lstat(path.c_str(), &metadata) != 0) {
          complete = false;
          continue;
        }
        if (S_ISDIR(metadata.st_mode)) {
          pending.push_back(path);
          continue;
        }
        if (!S_ISREG(metadata.st_mode) && !S_ISLNK(metadata.st_mode))
          continue;
      }
      if (entry->d_type == DT_LNK ||
          (entry->d_type == DT_UNKNOWN && S_ISLNK(metadata.st_mode))) {
        // Kbuild's source alias points outside the object tree. Any other
        // directory alias could hide owners and invalidates narrow coverage.
        struct stat target;
        char resolved[4096];
        if ((stat(path.c_str(), &target) == 0 && S_ISDIR(target.st_mode) &&
             (!realpath(path.c_str(), resolved) || source != resolved ||
              source == directory || name != "source")) ||
            HasSuffix(name, ".o") || HasSuffix(name, ".cmd"))
          complete = false;
        continue;
      }
      if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN) {
        if (HasSuffix(name, ".o") || HasSuffix(name, ".cmd"))
          complete = false;
        continue;
      }
      if (HasSuffix(name, ".o") &&
          access((parent + "/." + name + ".cmd").c_str(), R_OK) != 0)
        complete = false;
      if (!HasSuffix(name, ".cmd"))
        continue;
      std::ifstream input(path, std::ios::binary);
      if (!input) {
        complete = false;
        continue;
      }
      std::string line, logical;
      bool compiler_output = false;
      bool has_source = false;
      while (std::getline(input, line)) {
        const bool continued = !line.empty() && line.back() == '\\';
        if (continued)
          line.pop_back();
        logical += line;
        if (continued) {
          logical += ' ';
          continue;
        }
        const bool source = HasPrefix(logical, "source_");
        const bool dependencies = HasPrefix(logical, "deps_");
        compiler_output |= HasPrefix(logical, "savedcmd_") &&
                           logical.find(" -c ") != std::string::npos;
        has_source |= source;
        if (source || dependencies) {
          const size_t equal = logical.find(":=");
          if (equal == std::string::npos) {
            complete = false;
          } else {
            std::string target =
                logical.substr(source ? 7 : 5, equal - (source ? 7 : 5));
            while (!target.empty() &&
                   isspace(static_cast<unsigned char>(target.back())))
              target.pop_back();
            if (target.empty() ||
                target.find_first_of("$\\ \t") != std::string::npos) {
              complete = false;
            } else {
              if (target.front() != '/')
                target = directory + "/" + target;
              NormalizePath(&target);
              auto& inputs = objects[target];
              inputs.push_back(path);
              std::string values = logical.substr(equal + 2);
              size_t position;
              while ((position = values.find("$(wildcard ")) !=
                     std::string::npos) {
                const size_t end = values.find(')', position + 11);
                if (end == std::string::npos)
                  break;
                const std::string literal =
                    values.substr(position + 11, end - position - 11);
                values.replace(position, end - position + 1, literal);
              }
              if (values.find_first_of("$*?[]#") != std::string::npos) {
                complete = false;
              } else {
                std::string word;
                bool escaped = false;
                auto append = [&] {
                  if (word.empty())
                    return;
                  if (word.front() != '/')
                    word = directory + "/" + word;
                  NormalizePath(&word);
                  inputs.push_back(std::move(word));
                  word.clear();
                };
                for (char c : values) {
                  if (escaped) {
                    word += c;
                    escaped = false;
                  } else if (c == '\\') {
                    escaped = true;
                  } else if (isspace(static_cast<unsigned char>(c))) {
                    append();
                  } else {
                    word += c;
                  }
                }
                if (escaped)
                  complete = false;
                append();
              }
            }
          }
        }
        logical.clear();
      }
      if (!logical.empty() || input.bad())
        complete = false;
      if (compiler_output && !has_source)
        complete = false;
    }
    closedir(handle);
  }
  complete &= !objects.empty();
  for (auto& object : objects) {
    auto& inputs = object.second;
    std::sort(inputs.begin(), inputs.end());
    inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());
    covered_inputs.insert(inputs.begin(), inputs.end());
  }
}

std::string KbuildDependencyIndex::TreeProof() const {
  std::string bytes;
  AppendGraphString("kbuild-tree-v1", &bytes);
  for (const auto& directory : directories) {
    AppendGraphString(directory.first, &bytes);
    AppendGraphString(directory.second.first, &bytes);
    AppendGraphString(directory.second.second, &bytes);
  }
  return bytes;
}

bool KbuildDependencyIndex::Covers(const std::string& input) const {
  return complete && covered_inputs.count(input) != 0;
}

std::vector<std::string> KbuildDependencyIndex::Inputs(
    const std::string& directory) const {
  std::set<std::string> inputs;
  const std::string prefix = directory + "/";
  for (const auto& object : objects)
    if (HasPrefix(object.first, prefix))
      inputs.insert(object.second.begin(), object.second.end());
  return {inputs.begin(), inputs.end()};
}

std::string KbuildDependencyIndex::Encode() const {
  std::string bytes;
  AppendGraphString("kbuild-dependencies-v1", &bytes);
  AppendGraphString(complete ? "1" : "0", &bytes);
  for (const auto& object : objects) {
    AppendGraphString(object.first, &bytes);
    std::string inputs;
    for (const std::string& input : object.second)
      AppendGraphString(input, &inputs);
    AppendGraphString(inputs, &bytes);
  }
  return bytes;
}

bool KbuildDependencyIndex::Decode(std::string_view bytes) {
  objects.clear();
  covered_inputs.clear();
  complete = false;
  std::string version, coverage;
  if (!ConsumeGraphString(&bytes, &version) ||
      version != "kbuild-dependencies-v1" ||
      !ConsumeGraphString(&bytes, &coverage) ||
      (coverage != "1" && coverage != "0"))
    return false;
  while (!bytes.empty()) {
    std::string object, encoded;
    if (!ConsumeGraphString(&bytes, &object) ||
        !ConsumeGraphString(&bytes, &encoded))
      return false;
    std::string_view inputs(encoded);
    auto& paths = objects[object];
    while (!inputs.empty()) {
      std::string path;
      if (!ConsumeGraphString(&inputs, &path))
        return false;
      paths.push_back(std::move(path));
    }
    if (!std::is_sorted(paths.begin(), paths.end()))
      return false;
    covered_inputs.insert(paths.begin(), paths.end());
  }
  complete = coverage == "1" && !objects.empty();
  return true;
}

bool ResolveImportedCommandEnvironment(Command* command) {
  for (std::string& value : command->environment) {
    const size_t equal = value.find('=');
    if (equal == std::string::npos)
      return false;
    std::string resolved = value.substr(equal + 1);
    if (!ResolveFileReadExpansions(&resolved))
      return false;
    value.resize(equal + 1);
    value += resolved;
  }
  return true;
}

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
  // Environment entries are passed directly to the child with setenv; shell
  // metacharacters in their values remain literal and need no quoting here.
  // Kbuild's recursive invocation can be line-wrapped by Make's command
  // expansion. A backslash-newline pair is shell whitespace; flatten it
  // before parsing argv while continuing to reject other shell escapes.
  std::string flattened;
  for (size_t i = 0; i < command.display_cmd.size(); ++i) {
    if (command.display_cmd[i] == '\\' && i + 1 < command.display_cmd.size() &&
        command.display_cmd[i + 1] == '\n') {
      ++i;
      while (i + 1 < command.display_cmd.size() &&
             (command.display_cmd[i + 1] == ' ' ||
              command.display_cmd[i + 1] == '\t'))
        ++i;
      if (!flattened.empty() && flattened.back() != ' ')
        flattened.push_back(' ');
    } else {
      flattened.push_back(command.display_cmd[i]);
    }
  }
  std::string word;
  char quote = 0;
  bool started = false;
  for (char c : flattened) {
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
    } else if (arg == "--no-print-directory") {
      // Recursive Kbuild commands commonly add this output-only flag. It has
      // no effect on graph semantics and can be preserved during discovery.
    } else if (!arg.empty() && arg.front() == '-') {
      return false;
    }
  }
  return true;
}

static int GenerateImportedGraphAttempt(const Command& command,
                                        const std::vector<std::string>& argv,
                                        const std::string& directory,
                                        const std::string& gate,
                                        bool observed,
                                        bool allow_dynamic) {
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
    if (allow_dynamic)
      unsetenv("KATI_IMPORT_STATIC_ONLY");
    else
      setenv("KATI_IMPORT_STATIC_ONLY", "1", 1);
    if (observed) {
      setenv("KATI_IMPORT_OBSERVED", std::to_string(getppid()).c_str(), 1);
    }
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
    arguments.push_back("--regen");
    arguments.push_back(g_flags.GraphOptimizationEnabled("recursive")
                            ? "--graph_optimizations=aggressive"
                            : "--graph_optimizations=safe");
    if (g_flags.use_ninja_phony_output)
      arguments.push_back("--use_ninja_phony_output");
    for (const auto& feature : g_flags.disabled_graph_optimizations)
      arguments.push_back("--disable_graph_optimization=" + feature);
    arguments.push_back("--internal-import-root=" + directory);
    arguments.push_back("--internal-import-gate=" + gate);
    arguments.push_back("--ninja_dir=" + directory);
    char current_directory[4096];
    if (!getcwd(current_directory, sizeof(current_directory)))
      _exit(3);
    std::string context, encoded_arguments, encoded_environment;
    AppendGraphString("import-validation-context-v1", &context);
    AppendGraphString(current_directory, &context);
    for (const auto& argument : arguments)
      AppendGraphString(argument, &encoded_arguments);
    AppendGraphString(encoded_arguments, &context);
    for (char** entry = environ; entry && *entry; ++entry)
      if (!HasPrefix(*entry, "KATI_IMPORT_"))
        AppendGraphString(*entry, &encoded_environment);
    AppendGraphString(encoded_environment, &context);
    if (!WriteGraphCache(directory + "/validation-context", context))
      _exit(3);
    if (observed && !BeginImportObservation())
      _exit(3);
    std::vector<char*> pointers;
    for (auto& arg : arguments)
      pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    execv(pointers[0], pointers.data());
    _exit(127);
  }
  int status = 0;
  const int observed_status = observed ? ObserveImport(child, directory) : 3;
  const bool waited =
      observed || HANDLE_EINTR(waitpid(child, &status, 0)) == child;
  struct stat st;
  const bool quiet = fstat(diagnostics, &st) == 0 && st.st_size == 0;
  LOG_STAT("import discovery: observed=%d status=%d quiet=%d", observed,
           observed            ? observed_status
           : WIFEXITED(status) ? WEXITSTATUS(status)
                               : -1,
           quiet);
  if (!quiet) {
    char message[512];
    const ssize_t size = pread(diagnostics, message, sizeof(message) - 1, 0);
    if (size > 0) {
      message[size] = 0;
      LOG_STAT("import discovery diagnostic: %s", message);
    }
  }
  close(diagnostics);
  if (!waited)
    return -1;
  if (!quiet)
    return 3;
  return observed            ? observed_status
         : WIFEXITED(status) ? WEXITSTATUS(status)
                             : -1;
}

int GenerateImportedGraph(const Command& command,
                          const std::vector<std::string>& argv,
                          const std::string& directory,
                          const std::string& gate,
                          bool dynamic,
                          bool allow_dynamic) {
  if (dynamic && !allow_dynamic)
    return 3;
  if (ImportObservationActive())
    return GenerateImportedGraphAttempt(command, argv, directory, gate, false,
                                        allow_dynamic);
  if (dynamic)
    return GenerateImportedGraphAttempt(command, argv, directory, gate, true,
                                        allow_dynamic);
  const int status = GenerateImportedGraphAttempt(command, argv, directory,
                                                  gate, false, allow_dynamic);
  if (status != 3 || !g_flags.use_ninja_phony_output || !allow_dynamic)
    return status;
  return GenerateImportedGraphAttempt(command, argv, directory, gate, true,
                                      allow_dynamic);
}

bool ValidateImportedGraph(const std::string& path) {
  COLLECT_STATS("import validation");
  std::string bytes, version, directory, arguments, environment;
  if (!ReadGraphCache(path, &bytes))
    return false;
  std::string_view view(bytes);
  if (!ConsumeGraphString(&view, &version))
    return false;
  if (version == "kbuild-tree-v1")
    return ValidateBuildTree(view);
  if (version != "import-validation-context-v1" ||
      !ConsumeGraphString(&view, &directory) ||
      !ConsumeGraphString(&view, &arguments) ||
      !ConsumeGraphString(&view, &environment) || !view.empty())
    return false;
  std::vector<std::string> argv, env;
  auto decode = [](std::string_view values, std::vector<std::string>* output) {
    while (!values.empty()) {
      std::string value;
      if (!ConsumeGraphString(&values, &value))
        return false;
      output->push_back(std::move(value));
    }
    return true;
  };
  if (!decode(arguments, &argv) || argv.empty() || !decode(environment, &env))
    return false;
  const std::string cache = std::string(Dirname(path));
  std::string temporary = cache + "/validation.XXXXXX";
  const int diagnostics = mkstemp(temporary.data());
  if (diagnostics < 0)
    return false;
  unlink(temporary.c_str());
  const bool inherited_observer = ImportObservationActive();
  const std::string observer =
      inherited_observer ? getenv("KATI_IMPORT_OBSERVED") : "";
  const pid_t child = fork();
  if (child < 0) {
    close(diagnostics);
    return false;
  }
  if (!child) {
    if (dup2(diagnostics, STDOUT_FILENO) < 0 ||
        dup2(diagnostics, STDERR_FILENO) < 0 || chdir(directory.c_str()) != 0)
      _exit(3);
    if (diagnostics > STDERR_FILENO)
      close(diagnostics);
    clearenv();
    for (const auto& value : env) {
      const size_t equal = value.find('=');
      if (equal == std::string::npos)
        _exit(3);
      setenv(value.substr(0, equal).c_str(), value.c_str() + equal + 1, 1);
    }
    setenv("KATI_IMPORT_VALIDATE_ONLY", "1", 1);
    setenv("KATI_IMPORT_OBSERVED",
           inherited_observer ? observer.c_str()
                              : std::to_string(getppid()).c_str(),
           1);
    if (std::find(argv.begin(), argv.end(), "--regen") == argv.end())
      argv.push_back("--regen");
    if (!inherited_observer && !BeginImportObservation())
      _exit(3);
    std::vector<char*> pointers;
    for (auto& value : argv)
      pointers.push_back(value.data());
    pointers.push_back(nullptr);
    execv(pointers[0], pointers.data());
    _exit(3);
  }
  int status;
  const int result = inherited_observer
                         ? (HANDLE_EINTR(waitpid(child, &status, 0)) == child &&
                                    WIFEXITED(status)
                                ? WEXITSTATUS(status)
                                : 3)
                         : ObserveImport(child, cache);
  struct stat metadata;
  const bool quiet =
      fstat(diagnostics, &metadata) == 0 && metadata.st_size == 0;
  if (result != 0 || !quiet) {
    char message[512];
    const ssize_t size = pread(diagnostics, message, sizeof(message) - 1, 0);
    if (size > 0) {
      message[size] = 0;
      LOG_STAT("import validation diagnostic (%d): %s", result, message);
      if (getenv("KATI_IMPORT_VALIDATE_ONLY"))
        fprintf(stderr, "nested import validation (%d): %s", result, message);
    } else {
      LOG_STAT("import validation failed without diagnostics (%d)", result);
    }
  }
  close(diagnostics);
  return result == 0 && quiet;
}
