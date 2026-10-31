// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "recursive_cache.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "eval.h"
#include "expr.h"
#include "file_cache.h"
#include "fileutil.h"
#include "flags.h"
#include "rule.h"
#include "stats.h"
#include "stmt.h"
#include "strutil.h"
#include "timeutil.h"
#include "var.h"

extern char** environ;

namespace {
constexpr size_t kMaxCache = 64 * 1024 * 1024;
constexpr uint32_t kMaxEntries = 1000000;
constexpr const char* kCache = ".openmktr-recursive-cache";

bool Read(const std::string& path, std::string* data) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  const auto size = file.tellg();
  if (!file || size < 0 || size > static_cast<std::streamoff>(kMaxCache))
    return false;
  data->resize(static_cast<size_t>(size));
  file.seekg(0);
  return static_cast<bool>(file.read(data->data(), data->size()));
}

void PutInt(std::string* out, uint32_t value) {
  for (int i = 0; i < 4; ++i)
    out->push_back(static_cast<char>(value >> (8 * i)));
}
void Put(std::string* out, std::string_view value) {
  PutInt(out, value.size());
  out->append(value);
}
bool GetInt(std::string_view* in, uint32_t* value) {
  if (in->size() < 4)
    return false;
  *value = 0;
  for (int i = 0; i < 4; ++i)
    *value |= static_cast<uint32_t>(static_cast<unsigned char>((*in)[i]))
              << (8 * i);
  in->remove_prefix(4);
  return true;
}
bool Get(std::string_view* in, std::string_view* value) {
  uint32_t size;
  if (!GetInt(in, &size) || size > in->size())
    return false;
  *value = in->substr(0, size);
  in->remove_prefix(size);
  return true;
}
std::string Checksum(std::string_view data) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char byte : data) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return std::to_string(hash);
}
bool Identifier(std::string_view value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '_';
         });
}

bool SafeCommand(std::string_view source, Evaluator* ev) {
  for (size_t i = 0; i < source.size(); ++i) {
    if (source[i] != '$')
      continue;
    if (++i == source.size())
      return false;
    if (source[i] == '$' ||
        std::string_view("@<^+?*%|").find(source[i]) != std::string_view::npos)
      continue;
    std::string_view name;
    if (source[i] == '(' || source[i] == '{') {
      size_t end = source.find(source[i] == '(' ? ')' : '}', i + 1);
      if (end == std::string_view::npos)
        return false;
      name = source.substr(i + 1, end - i - 1);
      i = end;
    } else {
      name = source.substr(i, 1);
    }
    if (!Identifier(name) || name == "MAKE" || name == "MAKEFLAGS" ||
        name == "MAKEOVERRIDES" || name == "MAKELEVEL" ||
        name == "KATI_JOBSERVER_FIFO" || name == "KATI_JOBSERVER_RESERVED")
      return false;
    Var* value = ev->LookupVar(Intern(name));
    if (value->IsDefined() &&
        value->String().find('$') != std::string_view::npos)
      return false;
  }
  return true;
}

bool SafeFile(const Makefile& file,
              Evaluator* ev,
              const std::unordered_set<std::string>& files) {
  COLLECT_STATS("recursive cache eligibility time");
  if (!file.Exists())
    return false;
  for (const Stmt* stmt : file.stmts()) {
    if (const auto* assign = dynamic_cast<const AssignStmt*>(stmt)) {
      if (!assign->lhs->IsLiteral() || !assign->rhs->IsLiteral() ||
          assign->directive != AssignDirective::NONE || assign->is_final)
        return false;
      const auto name = assign->lhs->GetLiteralValueUnsafe();
      // Assigning inherited variables can change the exported recipe state.
      if (!Identifier(name) || getenv(std::string(name).c_str()) ||
          name == "SHELL" || name == "VPATH" || name == "MAKE" ||
          name == "MAKEFLAGS" || name == "MAKEOVERRIDES")
        return false;
    } else if (const auto* include = dynamic_cast<const IncludeStmt*>(stmt)) {
      if (!include->expr->IsLiteral() || !include->should_exist ||
          include->expr->GetLiteralValueUnsafe().find_first_of("*?[$\\") !=
              std::string_view::npos)
        return false;
      // Search-path resolution can change when a previously absent nearer
      // include appears. Until misses are tracked, require the direct spelling
      // to be present in the evaluated source set.
      for (std::string_view name :
           WordScanner(include->expr->GetLiteralValueUnsafe()))
        if (!files.count(std::string(name)))
          return false;
    } else if (const auto* rule = dynamic_cast<const RuleStmt*>(stmt)) {
      if (!rule->lhs->IsLiteral() || rule->sep != RuleStmt::SEP_NULL)
        return false;
      const auto source = rule->lhs->GetLiteralValueUnsafe();
      if (source.find_first_of("%*?[\\=$") != std::string_view::npos ||
          source.find("::") != std::string_view::npos)
        return false;
    } else if (const auto* command = dynamic_cast<const CommandStmt*>(stmt)) {
      if (!SafeCommand(command->orig, ev))
        return false;
    } else {
      return false;
    }
  }
  return true;
}

void Publish(const std::string& data) {
  char temporary[] = ".openmktr-recursive-cache-XXXXXX";
  int fd = mkstemp(temporary);
  if (fd < 0)
    return;
  size_t done = 0;
  while (done < data.size()) {
    ssize_t count =
        HANDLE_EINTR(write(fd, data.data() + done, data.size() - done));
    if (count <= 0)
      break;
    done += count;
  }
  const bool complete = close(fd) == 0 && done == data.size();
  if (complete)
    rename(temporary, kCache);
  unlink(temporary);
}
}  // namespace

RecursiveCache::RecursiveCache(const std::string& arguments) {
  const char* enabled = getenv("OMKTR_RECURSIVE_CACHE");
  if (!enabled || std::string_view(enabled) != "1" || g_flags.make_level < 1 ||
      g_flags.generate_ninja || g_flags.is_dry_run || g_flags.is_question ||
      g_flags.is_touch || g_flags.is_syntax_check_only ||
      g_flags.dump_include_graph || g_flags.dump_variable_assignment_trace ||
      g_flags.dump_kati_stamp || !g_flags.traced_variables_pattern.empty() ||
      g_flags.enable_debug || g_flags.warn_undefined_variables ||
      g_flags.enable_kati_warnings || g_flags.warn_implicit_rules ||
      g_flags.warn_suffix_rules || g_flags.warn_real_to_phony ||
      g_flags.warn_phony_looks_real || g_flags.warn_real_no_cmds_or_deps ||
      g_flags.warn_real_no_cmds || !g_flags.old_files.empty() ||
      !g_flags.what_if_files.empty() || getenv("MAKEFILES"))
    return;
  for (const char* name : g_flags.makefiles)
    if (std::string_view(name) == "-")
      return;
  for (std::string_view value : g_flags.cl_vars)
    if (value.find('$') != std::string_view::npos)
      return;
  // Never block a recursive invocation in the same directory behind its own
  // parent. Contention is a normal cache miss, not a build lock dependency.
  lock_ = open(".openmktr-recursive.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock_ < 0)
    return;
  struct flock lock = {};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  if (fcntl(lock_, F_SETLK, &lock) != 0) {
    close(lock_);
    lock_ = -1;
    return;
  }
  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd))) {
    close(lock_);
    lock_ = -1;
    return;
  }
  Put(&key_, "OMKTR-NOOP-2");
  Put(&key_, cwd);
  Put(&key_, arguments);
  Put(&key_, GetFileIdentity(g_flags.executable_path));
  for (const char* name : g_flags.makefiles)
    Put(&key_, name);
  std::vector<std::string> environment;
  for (char** entry = environ; *entry; ++entry) {
    const std::string_view value(*entry);
    if (value.substr(0, value.find('=')) == "KATI_JOBSERVER_FIFO" ||
        value.substr(0, value.find('=')) == "KATI_JOBSERVER_RESERVED")
      continue;
    environment.emplace_back(value);
  }
  std::sort(environment.begin(), environment.end());
  for (const auto& value : environment)
    Put(&key_, value);
}

RecursiveCache::~RecursiveCache() {
  if (lock_ >= 0)
    close(lock_);
}

bool RecursiveCache::Reuse() {
  if (lock_ < 0)
    return false;
  eligible_sources_.clear();
  ScopedTimeReporter timer("recursive cache lookup time");
  if (!Read(kCache, &certificate_))
    return false;
  const std::string& data = certificate_;
  const size_t footer = data.rfind('\n');
  if (footer == std::string::npos || Checksum(std::string_view(data).substr(
                                         0, footer)) != data.substr(footer + 1))
    return false;
  std::string_view input(data.data(), footer), value;
  if (!Get(&input, &value) || value != key_)
    return false;
  uint32_t count;
  if (!GetInt(&input, &count) || count > kMaxEntries)
    return false;
  bool unchanged = true;
  std::unordered_map<std::string, std::string_view> sources;
  for (uint32_t i = 0; i < count; ++i) {
    std::string_view path, identity, contents;
    if (!Get(&input, &path) || !Get(&input, &identity) ||
        !Get(&input, &contents) || identity == "unreadable")
      return false;
    unchanged &= GetFileIdentity(path) == identity;
    if (!contents.empty()) {
      // An explicit kind marker distinguishes even an empty Makefile from a
      // graph-only record. Never infer eligibility from filesystem metadata.
      if (contents.front() != 'M' ||
          !sources.emplace(std::string(path), contents.substr(1)).second)
        return false;
    }
  }
  if (!GetInt(&input, &count) || count > kMaxEntries)
    return false;
  std::vector<std::string> roots;
  for (uint32_t i = 0; i < count; ++i) {
    if (!Get(&input, &value))
      return false;
    roots.emplace_back(value);
  }
  if (!input.empty())
    return false;
  // A dirty graph is still a cache miss. Its intact, key-matching certificate
  // can prove source eligibility later, but only after normal evaluation has
  // read exactly the same complete source set and Save rechecks those bytes.
  eligible_sources_ = std::move(sources);
  if (!unchanged)
    return false;
  for (const auto& [path, contents] : eligible_sources_) {
    std::string current;
    if (!Read(path, &current) || current != contents)
      return false;
  }
  if (g_flags.enable_stat_logs)
    fprintf(stderr, "Recursive cache hit\n");
  const char* verbose = getenv("KATI_VERBOSE");
  if (!g_flags.is_silent_mode && (!verbose || std::string_view(verbose) != "1"))
    for (const auto& root : roots)
      printf("kati: Nothing to be done for `%s'.\n", root.c_str());
  return true;
}

void RecursiveCache::Save(const std::vector<NamedDepNode>& roots,
                          Evaluator* ev) {
  if (lock_ < 0 || MakefileCacheManager::Get().requires_evaluation())
    return;
  ScopedTimeReporter timer("recursive cache save time");
  // Remove a preceding certificate before considering a replacement.
  unlink(kCache);
  // VPATH can switch a plain prerequisite to a newly appearing local file.
  // Such absent search candidates are not part of a no-op certificate yet.
  if (ev->LookupVar(Intern("VPATH"))->IsDefined())
    return;
  std::unordered_set<std::string> files;
  MakefileCacheManager::Get().GetAllFilenames(&files);
  std::unordered_set<Symbol> outputs;
  for (const Rule* rule : ev->rules()) {
    if (rule->is_suffix_rule || !rule->output_patterns.empty() ||
        rule->secondary_expansion)
      return;
    for (Symbol output : rule->outputs) {
      if ((IsSpecialTarget(output) && output.str() != ".PHONY") ||
          !outputs.insert(output).second)
        return;
    }
  }
  std::unordered_map<const DepNode*, int> colors;
  std::function<bool(const DepNode*, size_t)> acyclic = [&](const DepNode* node,
                                                            size_t depth) {
    if (depth > 512 || colors[node] == 1)
      return false;
    if (colors[node] == 2)
      return true;
    colors[node] = 1;
    for (const auto& dep : node->deps)
      if (!acyclic(dep.second, depth + 1))
        return false;
    for (const auto& dep : node->order_onlys)
      if (!acyclic(dep.second, depth + 1))
        return false;
    colors[node] = 2;
    return true;
  };
  for (const auto& root : roots)
    if (!acyclic(root.second, 0))
      return;
  std::string data;
  Put(&data, key_);
  std::vector<std::string> records;
  const bool sources_proven =
      files.size() == eligible_sources_.size() &&
      std::all_of(files.begin(), files.end(), [&](const std::string& name) {
        const auto prior = eligible_sources_.find(name);
        return prior != eligible_sources_.end() &&
               prior->second ==
                   MakefileCacheManager::Get().ReadMakefile(name).buf();
      });
  if (sources_proven && g_flags.enable_stat_logs)
    fprintf(stderr, "Recursive cache eligibility reused\n");
  for (const auto& name : files) {
    const auto& file = MakefileCacheManager::Get().ReadMakefile(name);
    std::string current;
    if ((!sources_proven && !SafeFile(file, ev, files)) ||
        !Read(name, &current) || current != file.buf())
      return;
    std::string record;
    Put(&record, name);
    Put(&record, GetFileIdentity(name));
    Put(&record, "M" + current);
    records.push_back(std::move(record));
  }
  std::unordered_set<const DepNode*> root_nodes, visited;
  std::vector<const DepNode*> pending;
  for (const auto& root : roots) {
    root_nodes.insert(root.second);
    pending.push_back(root.second);
  }
  while (!pending.empty()) {
    const DepNode* node = pending.back();
    pending.pop_back();
    if (!visited.insert(node).second)
      continue;
    if (node->rule_vars || node->vpath_provider.IsValid() ||
        node->intermediate || node->oneshell || node->export_all_variables ||
        node->is_notparallel || !node->DoubleColon().inputs.empty() ||
        !node->validations.empty() || !node->wait_groups.empty() ||
        !node->implicit_outputs.empty() || !node->implicit_inputs.empty() ||
        node->output_pattern.IsValid() ||
        !node->low_resolution_inputs.empty() || node->depfile_var)
      return;
    const bool root = root_nodes.count(node) != 0;
    struct stat st;
    const bool exists = stat(node->output.c_str(), &st) == 0;
    if ((exists && !S_ISREG(st.st_mode)) ||
        (node->is_phony && (!root || !node->cmds.empty())) ||
        (!exists && !(root && node->cmds.empty())))
      return;
    const double timestamp = GetTimestamp(node->output.str());
    for (const auto& dep : node->deps) {
      // A missing/phony recipe-less prerequisite can propagate an infinite
      // logical timestamp through aggregate nodes. Never certify that graph.
      if (dep.second->is_phony || GetTimestamp(dep.first.str()) < 0 ||
          (dep.second->has_rule && dep.second->cmds.empty()) ||
          (!node->cmds.empty() && GetTimestamp(dep.first.str()) > timestamp))
        return;
      pending.push_back(dep.second);
    }
    for (const auto& dep : node->order_onlys)
      pending.push_back(dep.second);
    std::string record;
    Put(&record, node->output.str());
    Put(&record, GetFileIdentity(node->output.str()));
    Put(&record, "");
    records.push_back(std::move(record));
  }
  PutInt(&data, records.size());
  for (const auto& record : records)
    data += record;
  PutInt(&data, roots.size());
  for (const auto& root : roots)
    Put(&data, root.first.str());
  if (data.size() > kMaxCache - 64)
    return;
  data += '\n' + Checksum(data);
  Publish(data);
}
