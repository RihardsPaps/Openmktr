// FORK MODIFICATION NOTICE (2026)
// Own invocation state and publish coherent generations with typed freshness.
// Publish runtime context manifests for immutable per-build sharing.
// Reuse escaped paths and identical immutable prerequisite fragments.
// Factor long absolute build paths through graph-local Ninja variables.
// Recheck missing intermediate chains against bounded, shared consumer proofs.
// Bind every guarded file recipe to its last successful command identity.
// Factor source dependencies by shared consumers and settle checked script
// guards. Record unchanged guard success and preserve relevant runtime command
// changes. Defer generated-content recipes and track their serialized contexts.
// Expand dependent recipes after their prerequisites without retaining vector
// references. Keep freshness guards precise on shells with second-only test
// -nt. Require Ninja 1.13.2 for stable FIFO jobserver and interruption
// behavior. Track and validate incremental evaluation, include proofs and
// imported Ninja graphs. Seal structural stamp bytes and bind generated
// artifacts to acceptance. Carry generation/runtime load limits into Ninja and
// recursive recipes. Preserve selected graph controls at deferred recursive
// boundaries. Bound FIFO setup retries and report failures without removing
// existing paths. Use native load admission for every recipe, with configurable
// Ninja selection. Quote generated launcher paths for build directories
// containing shell syntax. Changed by the Openmktr fork, maintained by Rihards
// Paps and Haralds Paps. Changed build execution, evaluation, or portability
// for this fork. Validate recursive reuse, regeneration inputs, and shared
// recipe budgets. Low-resolution timestamp guards use Openmktr's internal stat
// helper. Reuse Ninja's default POSIX shell instead of spawning a second shell.
// Preserve runtime reservation ownership across generated environment files.
// Clean changed non-precious outputs before interrupted timestamp guards recur.
// Capture interruption cleanup state in the existing recipe supervisor.
// Serialize evaluated recipes without a second deferred-assignment expansion.
// Retain managed-workspace recipe changes without trusting timestamps alone.
// Preserve trailing background commands inside generated shell guards.
// Track workspace exports and identities of elided source-only recipes.
// Limit depfile inference to literal compiler commands; preserve quoted
// recipes. Reject known Ninja compatibility gaps before publishing a strict
// graph. Upstream material retains its Apache-2.0 terms. Fork modifications are
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

#include "ninja.h"
#include "generation.h"
#include "graph_import.h"
#include "graph_optimization.h"

#ifndef OMKTR_NINJA_EXECUTABLE
#define OMKTR_NINJA_EXECUTABLE "ninja"
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "command.h"
#include "dep.h"
#include "eval.h"
#include "exec.h"
#include "file_cache.h"
#include "fileutil.h"
#include "find.h"
#include "flags.h"
#include "func.h"
#include "io.h"
#include "log.h"
#include "stats.h"
#include "stringprintf.h"
#include "strutil.h"
#include "timeutil.h"
#include "var.h"
#include "version.h"

static std::string ShellQuote(std::string_view s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

static std::string EncodeShellScript(std::string_view script) {
  std::string encoded = "eval \"$(printf '%s\\n'";
  size_t start = 0;
  while (start <= script.size()) {
    const size_t end = script.find('\n', start);
    encoded += " " + ShellQuote(script.substr(start, end == std::string::npos
                                                         ? std::string::npos
                                                         : end - start));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return encoded + ")\"";
}

static bool EnsureDirectory(std::string_view path) {
  if (path.empty() || path == ".")
    return true;

  std::string current;
  current.reserve(path.size());
  if (path.front() == '/')
    current = "/";
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i < path.size() && path[i] != '/') {
      current += path[i];
      continue;
    }

    if (current.empty() || current == "/")
      continue;

    if (mkdir(current.c_str(), 0777) != 0 && errno != EEXIST)
      return false;

    struct stat st;
    if (stat(current.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      return false;

    if (i < path.size() && path[i] == '/')
      current += '/';
  }
  return true;
}

// Infer only a literal compiler invocation. Shell syntax, response files and
// unknown wrappers require explicit .KATI_DEPFILE metadata.
bool GetDepfileFromCommand(std::string* cmd, std::string* out) {
  std::vector<std::string_view> words;
  size_t start = 0;
  for (size_t i = 0; i <= cmd->size(); ++i) {
    const char c = i == cmd->size() ? ' ' : (*cmd)[i];
    if (c == ' ' || c == '\t') {
      if (i > start)
        words.emplace_back(cmd->data() + start, i - start);
      start = i + 1;
    } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') ||
                 std::string_view("_./+-=,:%").find(c) !=
                     std::string_view::npos)) {
      return false;
    }
  }
  if (words.empty())
    return false;
  size_t compiler = 0;
  if (Basename(words[0]) == "ccache" || Basename(words[0]) == "sccache")
    compiler = 1;
  if (compiler >= words.size())
    return false;
  const std::string_view name = Basename(words[compiler]);
  if (name != "clang" && name != "clang++" && name != "cc" && name != "c++" &&
      name != "gcc" && name != "g++")
    return false;

  bool compile = false;
  bool dependencies = false;
  std::string_view output;
  std::string_view depfile;
  for (size_t i = compiler + 1; i < words.size(); ++i) {
    const std::string_view word = words[i];
    if (word == "-c") {
      compile = true;
    } else if (word == "-MD" || word == "-MMD") {
      dependencies = true;
    } else if (word == "--" || word == "-E" || word == "-S" || word == "-M" ||
               word == "-MM" || HasPrefix(word, "-X") ||
               HasPrefix(word, "-Wp,")) {
      return false;
    } else if (HasPrefix(word, "-MF") || HasPrefix(word, "-o")) {
      const size_t length = HasPrefix(word, "-MF") ? 3 : 2;
      std::string_view value = word.substr(length);
      if (value.empty()) {
        if (++i == words.size() || words[i].front() == '-')
          return false;
        value = words[i];
      }
      if (length == 3)
        depfile = value;
      else
        output = value;
    } else if (word == "-I" || word == "-D" || word == "-U" ||
               word == "-include" || word == "-imacros" || word == "-isystem" ||
               word == "-iquote" || word == "-idirafter" ||
               word == "-isysroot" || word == "-target" || word == "-x" ||
               word == "-MT" || word == "-MQ") {
      // Option operands must never be mistaken for dependency flags.
      if (++i == words.size())
        return false;
    } else if (!word.empty() && word.front() == '-' && word != "-MP" &&
               word != "-g" && word != "-O0" && word != "-O1" &&
               word != "-O2" && word != "-O3" && word != "-Os" &&
               word != "-Oz" && !HasPrefix(word, "-std=") &&
               !HasPrefix(word, "-I") && !HasPrefix(word, "-D") &&
               !HasPrefix(word, "-U")) {
      return false;
    }
  }
  if (!compile || !dependencies || output.empty() || output == "-")
    return false;
  for (std::string_view word : words) {
    // Unpreprocessed assembly does not produce compiler dependencies.
    if (HasSuffix(word, ".s"))
      return false;
  }
  std::string detected = depfile.empty() ? std::string(StripExt(output)) + ".d"
                                         : std::string(depfile);
  if (detected == "-" || detected.front() == '-')
    return false;
  // Ninja consumes our copy; keep the original for Makefile include directives.
  *cmd = "( " + *cmd + " ) && cp " + ShellQuote(detected) + " " +
         ShellQuote(detected + ".tmp");
  *out = detected + ".tmp";
  return true;
}

struct NinjaNode {
  const DepNode* node;
  std::vector<Command> commands;
  int rule_id;
};

static bool IsShellIdentifier(std::string_view name) {
  if (name.empty())
    return false;

  unsigned char first = static_cast<unsigned char>(name[0]);
  if (!(isalpha(first) || first == '_'))
    return false;

  for (size_t i = 1; i < name.size(); i++) {
    unsigned char c = static_cast<unsigned char>(name[i]);
    if (!(isalnum(c) || c == '_'))
      return false;
  }

  return true;
}

class NinjaGenerator {
 public:
  NinjaGenerator(Evaluator* ev, double start_time)
      : ce_(ev),
        ev_(ev),
        rule_id_(0),
        shell_(EscapeNinja(ev->GetShell())),
        shell_flags_(EscapeNinja(ev->GetShellFlag())),
        kati_binary_(GetExecutablePath()),
        start_time_(start_time),
        default_target_(NULL) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != nullptr)
      working_dir_ = cwd;
    else
      PERROR("getcwd failed while generating Ninja graph");
    if (g_flags.GraphOptimizationEnabled("compact")) {
      const size_t slash = working_dir_.rfind('/');
      const std::string prefix = working_dir_.substr(0, slash + 1);
      // Conservative plain values avoid Ninja whitespace/escape ambiguities.
      if (prefix.size() > 32 &&
          prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ"
                                   "RSTUVWXYZ0123456789/_.-") ==
              std::string::npos) {
        path_prefix_ = prefix;
        path_variable_ = "omktr_path_" + GraphDigest(prefix);
      }
    }
    ev_->set_avoid_io(true);
  }

  ~NinjaGenerator() { ev_->set_avoid_io(false); }

  void Generate(const std::vector<NamedDepNode>& nodes,
                const std::string& orig_args) {
    if (g_flags.ninja_dir && !EnsureDirectory(g_flags.ninja_dir))
      ERROR("*** cannot create Ninja output directory: %s", g_flags.ninja_dir);
    // Finish recipe expansion before creating new filesystem entries that
    // evaluation-time wildcard/find queries could otherwise observe.
    PopulateNinjaNodes(nodes);
    GenerationTransaction generation;
    generation_id_ = std::string(Basename(generation.directory()));
    // Raw Ninja readers retain the parsed graph but cannot hold our bundle
    // lease. Its recipe proofs therefore reference immutable stable objects.
    intermediate_states_path_ =
        GetFilename(".kati_intermediate_states-") + generation_id_;
    for (const NinjaNode& nn : nodes_)
      if (nn.node->intermediate)
        intermediate_proofs_[nn.node] = IntermediateProof(nn.node);
    if (g_flags.import_root) {
      if (!cycle_edges_.empty())
        throw BuildExit{3};
      for (const NinjaNode& nn : nodes_) {
        const DepNode& node = *nn.node;
        struct stat st;
        if (node.is_notparallel || node.vpath_provider.IsValid() ||
            node.output_pattern.IsValid() || !node.implicit_inputs.empty() ||
            !node.implicit_outputs.empty() || !node.validations.empty() ||
            !node.double_colon_group_for_cmd.empty() ||
            !node.low_resolution_inputs.empty() || !node.wait_groups.empty() ||
            node.intermediate || node.precious || node.ignore_errors ||
            std::any_of(
                nn.commands.begin(), nn.commands.end(),
                [](const Command& command) { return command.ignore_error; }) ||
            (stat(node.output.c_str(), &st) == 0 && S_ISDIR(st.st_mode)))
          throw BuildExit{3};
      }
    }
    PrepareImports();
    // Command expansion is complete. Capture exports once before serializing
    // the graph so every recipe refers to this immutable environment snapshot.
    GenerateShell();
    GenerateNinja();
    std::unordered_set<std::string> runtime_contexts;
    for (const auto& node : nodes_)
      for (const auto& command : node.commands)
        if (!command.runtime_context.empty()) {
          std::string absolute;
          AbsPath(command.runtime_context, &absolute);
          runtime_contexts.insert(std::move(absolute));
        }
    std::vector<std::string> ordered_contexts(runtime_contexts.begin(),
                                              runtime_contexts.end());
    std::sort(ordered_contexts.begin(), ordered_contexts.end());
    std::string context_manifest, previous_context_manifest;
    AppendGraphString("runtime-context-manifest-v1", &context_manifest);
    for (const std::string& context : ordered_contexts)
      AppendGraphString(context, &context_manifest);
    const std::string context_manifest_path =
        RuntimeContextManifestPath(GetNinjaFilename());
    if (!ordered_contexts.empty() ||
        access(context_manifest_path.c_str(), F_OK) == 0) {
      if ((!ReadGraphCache(context_manifest_path, &previous_context_manifest) ||
           previous_context_manifest != context_manifest) &&
          !WriteGraphCache(context_manifest_path, context_manifest))
        ERROR("cannot publish runtime context manifest");
      MakefileCacheManager::Get().AddGeneratedGraphDep(context_manifest_path);
    }
    // Sealing changes object metadata. Observe it only after the durability
    // barriers, so the generated dependency identities certify the final files.
    generation.SyncObjects();
    GenerateStamp(orig_args);
    generation.Commit();
    if (g_flags.import_root) {
      std::string manifest;
      AppendGraphString("jobs", &manifest);
      AppendGraphString(std::to_string(g_flags.num_jobs), &manifest);
      auto record = [&](const char* kind, Symbol name) {
        std::string absolute;
        AbsPath(name.str(), &absolute);
        AppendGraphString(kind, &manifest);
        AppendGraphString(absolute, &manifest);
      };
      for (const NinjaNode& nn : nodes_)
        if (!IsSpecialTarget(nn.node->output))
          record("output", nn.node->output);
      for (const auto& root : nodes)
        record("goal", root.first);
      std::unordered_set<std::string> files;
      MakefileCacheManager::Get().GetAllFilenames(&files);
      for (const auto& file : files)
        if (!MakefileCacheManager::Get().IsGeneratedGraphDep(file))
          record("source", Intern(file));
      if (!WriteGraphCache(std::string(g_flags.import_root) + "/manifest",
                           manifest))
        ERROR("cannot publish imported graph manifest");
    }
  }

  static std::string GetStampTempFilename() {
    return GetFilename(".kati_stamp%s.tmp");
  }

  static std::string GetFilename(const char* fmt) {
    return GenerationPath(
        g_flags.ninja_dir ? g_flags.ninja_dir : ".",
        StringPrintf(fmt, g_flags.ninja_suffix ? g_flags.ninja_suffix : ""));
  }

 private:
  void PopulateNinjaNodes(const std::vector<NamedDepNode>& nodes) {
    ScopedTimeReporter tr("ninja gen (eval)");
    for (auto const& [symbol, depnode] : nodes) {
      PopulateNinjaNode(*depnode);
    }
    for (NinjaNode& nn : nodes_)
      nn.rule_id = nn.commands.empty() ? -1 : rule_id_++;
    for (const NinjaNode& nn : nodes_) {
      for (const auto& edge : nn.node->deps)
        if (edge.second->intermediate)
          intermediate_parents_[edge.second].push_back(nn.node);
      for (const auto* edges : {&nn.node->order_onlys, &nn.node->validations})
        for (const auto& edge : *edges)
          if (edge.second->intermediate)
            intermediate_required_.insert(edge.second);
    }
  }

  std::string IntermediateProof(const DepNode* node) {
    auto consumer_proof = [&](const DepNode* root, const std::string& path,
                              size_t size) {
      auto previous = intermediate_consumer_sizes_.find(root);
      if (previous == intermediate_consumer_sizes_.end() ||
          size < previous->second) {
        intermediate_consumer_sizes_[root] = size;
        intermediate_consumer_proofs_[root] = path;
      }
    };
    if (!node->intermediate || intermediate_required_.count(node) ||
        !g_flags.what_if_files.empty())
      return "";
    std::vector<const DepNode*> pending{node}, roots;
    std::unordered_set<const DepNode*> seen;
    while (!pending.empty()) {
      const DepNode* child = pending.back();
      pending.pop_back();
      if (!seen.insert(child).second)
        continue;
      auto parents = intermediate_parents_.find(child);
      if (intermediate_required_.count(child) ||
          parents == intermediate_parents_.end())
        return "";
      for (const DepNode* parent : parents->second) {
        if (parent->intermediate)
          pending.push_back(parent);
        else
          roots.push_back(parent);
      }
    }
    if (roots.empty())
      return "";
    std::sort(roots.begin(), roots.end(),
              [](const DepNode* a, const DepNode* b) {
                return a->output.str() < b->output.str();
              });
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    std::string root_names, records;
    for (const DepNode* root : roots)
      AppendGraphString(root->output.str(), &root_names);
    auto cached = intermediate_root_proofs_.find(root_names);
    if (cached != intermediate_root_proofs_.end())
      return cached->second;
    for (const auto& cached : intermediate_closures_) {
      if (std::all_of(
              roots.begin(), roots.end(),
              [&](const DepNode* root) { return cached.nodes.count(root); }) &&
          std::all_of(cached.roots.begin(), cached.roots.end(),
                      [&](const DepNode* root) {
                        return std::find(roots.begin(), roots.end(), root) !=
                               roots.end();
                      })) {
        intermediate_root_proofs_[root_names] = cached.path;
        for (const DepNode* root : roots)
          consumer_proof(root, cached.path, cached.nodes.size());
        return cached.path;
      }
    }
    const std::unordered_set<const DepNode*> root_nodes(roots.begin(),
                                                        roots.end());
    std::unordered_set<const DepNode*> redundant_roots;
    pending = roots;
    seen.clear();
    while (!pending.empty()) {
      const DepNode* entry = pending.back();
      pending.pop_back();
      if (!seen.insert(entry).second)
        continue;
      // Optional pruning must not duplicate a large compiler dependency graph.
      if (seen.size() > 131072 || records.size() > 64 * 1024 * 1024)
        return "";
      std::string record, flags, inputs, order_only;
      AppendGraphString(entry->output.str(), &record);
      flags += entry->intermediate ? '1' : '0';
      flags +=
          entry->is_phony || !entry->validations.empty() ||
                  std::find(entry->double_colon_group_has_prerequisites.begin(),
                            entry->double_colon_group_has_prerequisites.end(),
                            false) !=
                      entry->double_colon_group_has_prerequisites.end()
              ? '1'
              : '0';
      AppendGraphString(flags, &record);
      for (const auto& edge : entry->deps) {
        AppendGraphString(edge.first.str(), &inputs);
        pending.push_back(edge.second);
        if (root_nodes.count(edge.second))
          redundant_roots.insert(edge.second);
      }
      // Order-only inputs retain their original Ninja scheduling edges, but
      // cannot make this consumer stale. Traversing their library subgraphs
      // here would defeat bounded pruning of small missing intermediates.
      AppendGraphString(inputs, &record);
      AppendGraphString(order_only, &record);
      AppendGraphString(record, &records);
    }
    // A root already reached below another root adds no freshness condition.
    // Canonicalize it away so large shared image/header closures have one
    // immutable proof instead of a separate copy per intermediate consumer.
    const std::vector<const DepNode*> consumers = roots;
    roots.erase(std::remove_if(roots.begin(), roots.end(),
                               [&](const DepNode* root) {
                                 return redundant_roots.count(root);
                               }),
                roots.end());
    if (roots.empty())
      return "";
    std::string canonical_roots;
    for (const DepNode* root : roots)
      AppendGraphString(root->output.str(), &canonical_roots);
    std::string proof;
    AppendGraphString("intermediate-proof-v2", &proof);
    AppendGraphString(canonical_roots, &proof);
    AppendGraphString(records, &proof);
    AppendGraphString(intermediate_states_path_, &proof);
    const std::string path =
        GetFilename(".kati_intermediates-") + GraphDigest(proof);
    std::string previous;
    if ((!ReadGraphCache(path, &previous) || previous != proof) &&
        !WriteGraphCache(path, proof))
      ERROR("cannot write intermediate freshness proof");
    MakefileCacheManager::Get().AddGeneratedGraphDep(path);
    intermediate_proof_nodes_.insert(seen.begin(), seen.end());
    intermediate_root_proofs_[root_names] = path;
    intermediate_root_proofs_[canonical_roots] = path;
    for (const DepNode* root : consumers)
      consumer_proof(root, path, seen.size());
    intermediate_closures_.push_back({roots, std::move(seen), path});
    return path;
  }

  void PopulateNinjaNode(const DepNode& node) {
    // GNU make does not execute a rule for an existing target when it has no
    // prerequisites. Ninja treats a zero-input edge as always dirty, which
    // would incorrectly run guard recipes such as Kbuild's "missing .config"
    // diagnostic even when the file is present. FORCE and other explicit
    // prerequisites still enter the graph normally.
    if (done_.exists(node.output)) {
      return;
    }
    done_.insert(node.output);
    active_nodes_.insert(node.output.str());
    ScopedFrame frame(ce_.evaluator()->Enter(
        FrameType::NINJA, node.lexical_output.str(), node.loc));

    // A hack to exclude out phony target in Android. If this exists,
    // "ninja -t clean" tries to remove this directory and fails.
    if (g_flags.detect_android_echo && node.output.str() == "out")
      active_nodes_.erase(node.output.str());
    if (g_flags.detect_android_echo && node.output.str() == "out")
      return;

    // This node is a leaf node
    if (!node.has_rule && !node.is_phony) {
      active_nodes_.erase(node.output.str());
      return;
    }

    const size_t node_index = nodes_.size();
    nodes_.emplace_back().node = &node;

    for (auto const& [symbol, depnode] : node.deps) {
      MarkCycleEdge(node.output, symbol);
      PopulateNinjaNode(*depnode);
    }
    for (auto const& [symbol, depnode] : node.order_onlys) {
      MarkCycleEdge(node.output, symbol);
      PopulateNinjaNode(*depnode);
    }
    for (auto const& [symbol, depnode] : node.validations) {
      MarkCycleEdge(node.output, symbol);
      PopulateNinjaNode(*depnode);
    }
    if (!MakeRuntimeRecipe(node, ce_.evaluator(),
                           g_flags.ninja_dir ? g_flags.ninja_dir : ".",
                           &nodes_[node_index].commands))
      nodes_[node_index].commands = ce_.Eval(node);
    for (const Command& command : nodes_[node_index].commands) {
      if (!command.runtime_sidecar.empty())
        MakefileCacheManager::Get().AddGeneratedGraphDep(
            command.runtime_sidecar);
      if (!command.runtime_context.empty())
        MakefileCacheManager::Get().AddGeneratedGraphDep(
            command.runtime_context);
    }
    active_nodes_.erase(node.output.str());
  }

  std::string CycleEdgeKey(Symbol from, Symbol to) const {
    return std::string(from.str()) + "\n" + std::string(to.str());
  }

  void MarkCycleEdge(Symbol from, Symbol to) {
    if (active_nodes_.find(to.str()) != active_nodes_.end())
      cycle_edges_.insert(CycleEdgeKey(from, to));
  }

  bool IsCycleEdge(Symbol from, Symbol to) const {
    return cycle_edges_.find(CycleEdgeKey(from, to)) != cycle_edges_.end();
  }

  std::string_view TranslateCommand(const char* in,
                                    std::string* cmd_buf,
                                    bool preserve_newlines = false) {
    const size_t orig_size = cmd_buf->size();
    bool prev_backslash = false;
    // Set space as an initial value so the leading comment will be
    // stripped out.
    char prev_char = ' ';
    char quote = 0;
    for (; *in; in++) {
      switch (*in) {
        case '#':
          if (quote == 0 && !prev_backslash &&
              isspace(static_cast<unsigned char>(prev_char))) {
            while (in[1] && *in != '\n')
              in++;
          } else {
            *cmd_buf += *in;
          }
          break;

        case '\'':
        case '"':
        case '`':
          if (quote) {
            if (quote == *in && (quote == '\'' || !prev_backslash))
              quote = 0;
          } else if (!prev_backslash) {
            quote = *in;
          }
          *cmd_buf += *in;
          break;

        case '$':
          // CommandEvaluator has resolved deferred assignment overrides.
          // Any remaining dollars are recipe data, including $(pound) in
          // saved compiler commands. Only escape them for Ninja here.
          *cmd_buf += "$$";
          break;

        case '\n':
          if (prev_backslash) {
            cmd_buf->resize(cmd_buf->size() - 1);
          } else if (preserve_newlines) {
            // Ninja rule commands are single logical lines.  Preserve the
            // one-shell execution context with a command separator instead
            // of flattening lines into whitespace or emitting invalid Ninja
            // syntax.
            *cmd_buf += "; ";
          } else {
            *cmd_buf += ' ';
          }
          break;

        case '\\':
          *cmd_buf += '\\';
          break;

        default:
          *cmd_buf += *in;
      }

      if (*in == '\\') {
        prev_backslash = !prev_backslash;
      } else {
        prev_backslash = false;
      }

      prev_char = *in;
    }

    if (prev_backslash) {
      cmd_buf->resize(cmd_buf->size() - 1);
    }

    while (cmd_buf->size() > orig_size) {
      char c = (*cmd_buf)[cmd_buf->size() - 1];
      if (c == ';') {
        size_t backslashes = 0;
        for (size_t i = cmd_buf->size() - 1;
             i > orig_size && (*cmd_buf)[i - 1] == '\\'; --i)
          ++backslashes;
        // An escaped semicolon is an argument (e.g. find -exec ... \;),
        // not a redundant shell command separator.
        if (backslashes % 2 != 0)
          break;
      } else if (!isspace(static_cast<unsigned char>(c))) {
        break;
      }
      cmd_buf->resize(cmd_buf->size() - 1);
    }

    // A background list already ends with a shell separator. Finish it with
    // a builtin no-op so guards and recipe joins can append their own ';' or
    // '&&' without producing invalid '&;' or '& &&' syntax. Its exit status
    // remains zero, as it was for the asynchronous list.
    if (quote == 0 && cmd_buf->size() > orig_size && cmd_buf->back() == '&') {
      size_t end = cmd_buf->size() - 1;
      size_t backslashes = 0;
      while (end > orig_size && (*cmd_buf)[end - 1] == '\\') {
        ++backslashes;
        --end;
      }
      if (backslashes % 2 == 0 &&
          (end == orig_size || (*cmd_buf)[end - 1] != '&'))
        *cmd_buf += " :";
    }

    return std::string_view(cmd_buf->data() + orig_size,
                            cmd_buf->size() - orig_size);
  }

  bool IsOutputMkdir(const char* name, std::string_view cmd) {
    if (!HasPrefix(cmd, "mkdir -p ")) {
      return false;
    }
    cmd = cmd.substr(9, cmd.size());
    if (!cmd.empty() && cmd.back() == '/') {
      cmd.remove_suffix(1);
    }

    // The command may create the directory target itself (for example,
    // FFmpeg's libavcodec/ rule).  Only elide mkdir for a different output
    // located inside the directory.
    std::string_view output(name);
    while (!output.empty() && output.back() == '/')
      output.remove_suffix(1);
    if (cmd == output)
      return false;

    std::string_view dir = Dirname(name);
    if (cmd == dir) {
      return true;
    }
    return false;
  }

  bool GetDescriptionFromCommand(std::string_view cmd, std::string* out) {
    if (!HasPrefix(cmd, "echo ")) {
      return false;
    }
    cmd = cmd.substr(5, cmd.size());

    bool prev_backslash = false;
    char quote = 0;
    std::string out_buf;

    // Strip outer quotes, and fail if it is not a single echo command
    for (std::string_view::iterator in = cmd.begin(); in != cmd.end(); in++) {
      if (prev_backslash) {
        prev_backslash = false;
        out_buf += *in;
      } else if (*in == '\\') {
        prev_backslash = true;
        out_buf += *in;
      } else if (quote) {
        if (*in == quote) {
          quote = 0;
        } else {
          out_buf += *in;
        }
      } else {
        switch (*in) {
          case '\'':
          case '"':
          case '`':
            quote = *in;
            break;

          case '<':
          case '>':
          case '&':
          case '|':
          case ';':
            return false;

          default:
            out_buf += *in;
        }
      }
    }

    *out = out_buf;
    return true;
  }

  void GenShellScript(const char* name,
                      const std::vector<Command>& commands,
                      std::string* cmd_buf,
                      std::string* description,
                      bool preserve_newlines = false) {
    bool got_descritpion = false;
    auto command_count = commands.size();
    for (const Command& c : commands) {
      size_t cmd_begin = cmd_buf->size();

      if (!cmd_buf->empty()) {
        *cmd_buf += " && ";
      }

      std::string encoded_script;
      if (preserve_newlines)
        encoded_script = EncodeShellScript(c.cmd);
      const char* in =
          preserve_newlines ? encoded_script.c_str() : c.cmd.c_str();
      while (isspace(*in))
        in++;

      bool needs_subshell =
          (command_count > 1 || c.ignore_error) && !c.force_no_subshell;

      if (needs_subshell)
        *cmd_buf += '(';

      std::string_view translated =
          TranslateCommand(in, cmd_buf, preserve_newlines);
      if (g_flags.detect_android_echo && !got_descritpion && !c.echo &&
          GetDescriptionFromCommand(translated, description)) {
        got_descritpion = true;
        translated = std::string_view();
      } else if (IsOutputMkdir(name, translated) && !c.echo && cmd_begin == 0) {
        translated = std::string_view();
      }
      if (translated.empty()) {
        cmd_buf->resize(cmd_begin);
        command_count -= 1;
        continue;
      }

      if (c.ignore_error) {
        *cmd_buf += " ; true";
      }

      if (needs_subshell)
        *cmd_buf += " )";
    }
  }

  bool GetDepfile(const DepNode* node,
                  std::string* cmd_buf,
                  std::string* depfile) {
    if (node->depfile_var) {
      node->depfile_var->Eval(ev_, depfile);
      return true;
    }
    if (!g_flags.detect_depfiles)
      return false;

    return GetDepfileFromCommand(cmd_buf, depfile);
  }

  void EmitDepfile(const NinjaNode& nn,
                   std::string* cmd_buf,
                   std::ostream& out) {
    const DepNode* node = nn.node;
    std::string depfile;
    if (!GetDepfile(node, cmd_buf, &depfile))
      return;
    out << " depfile = " << EscapeNinja(depfile) << "\n";
    out << " deps = gcc\n";
  }

  void EmitNode(const NinjaNode& nn, std::ostream& out) {
    const DepNode* node = nn.node;
    if (node->is_default_target) {
      std::unique_lock<std::mutex> lock(mu_);
      default_target_ = node;
    }
    if (TryImport(nn, out))
      return;
    std::string edge_command, edge_depfile, script_path;
    auto proof = intermediate_proofs_.find(node);
    const std::string intermediate_proof =
        proof == intermediate_proofs_.end() ? "" : proof->second;
    const auto consumer = intermediate_consumer_proofs_.find(node);
    const std::string consumer_proof =
        consumer == intermediate_consumer_proofs_.end() ? "" : consumer->second;
    const std::string proof_dependency =
        intermediate_proof.empty() ? consumer_proof : intermediate_proof;
    // Ninja needs a concrete local output for a rule-bearing VPATH target.
    // Materialize a local link to the source only while its ordinary
    // prerequisites are older. This keeps the rule's edges reachable and
    // gives parent recipes a usable logical filename. The link follows later
    // source updates without copying stale contents into the build tree.
    std::vector<Command> commands = nn.commands;
    // A no-recipe VPATH target is an alias for an existing source file, not
    // a request to copy that file into the build tree.  Materializing a local
    // symlink changes include lookup for headers with sibling includes (the
    // build-tree alias shadows the source-tree header directory).  A Ninja
    // phony edge to the provider preserves VPATH lookup without changing the
    // filesystem view seen by compiler commands.
    const bool source_only_rule =
        commands.empty() || (commands.size() == 1 && !commands.front().echo &&
                             commands.front().cmd == ":");
    const bool vpath_source_alias =
        node->vpath_provider.IsValid() && !node->is_phony && source_only_rule;
    if (vpath_source_alias)
      commands.clear();
    const bool vpath_output = node->vpath_provider.IsValid() &&
                              !node->is_phony && !vpath_source_alias;
    if (vpath_output && commands.empty()) {
      Command materialize(node->output);
      materialize.cmd = ":";
      materialize.echo = false;
      commands.push_back(std::move(materialize));
    }

    // Make distinguishes "doc" from "doc/", but Ninja normalizes both to
    // the same path.  If the directory already exists and "doc" is another
    // target, the directory recipe is unnecessary and would emit a duplicate
    // Ninja output.  Its prerequisite edges are omitted for the same reason.
    if (IsExistingDirectoryAlias(node->output))
      return;

    // An existing source file may match a silent dummy rule such as FFmpeg's
    // "%.h:; @:". It is an input, not a generated output: declaring it as an
    // output would make "ninja -t clean" delete the source file.
    struct stat existing_output;
    const bool elided_source_recipe =
        !node->is_phony && node->deps.empty() && node->order_onlys.empty() &&
        node->validations.empty() && commands.size() == 1 &&
        !commands.front().echo && commands.front().cmd == ":" &&
        stat(node->output.c_str(), &existing_output) == 0 &&
        S_ISREG(existing_output.st_mode);
    const char* workspace_build = getenv("OMKTR_WORKSPACE_BUILD");
    const bool managed_workspace = workspace_build && *workspace_build;
    if (elided_source_recipe && !managed_workspace)
      return;

    std::string rule_name = "phony";
    // Ninja owns the build-time job budget. Do not impose a second fixed
    // pool from Kati's graph-generation -j value; ninja.sh exports the
    // runtime value to recursive Kati processes.
    bool use_local_pool = false;
    if (IsSpecialTarget(node->output)) {
      return;
    }
    if (g_flags.enable_debug) {
      out << "# " << (node->loc.filename ? node->loc.filename : "(null)") << ':'
          << node->loc.lineno << "\n";
    }
    if (!commands.empty()) {
      std::string description = "build $out";
      std::string cmd_buf;
      const std::string command_shell =
          commands.front().shell.empty() ? shell_
                                         : EscapeNinja(commands.front().shell);
      const std::string command_shellflag =
          commands.front().shellflag.empty()
              ? shell_flags_
              : EscapeNinja(commands.front().shellflag);
      GenShellScript(node->output.c_str(), commands, &cmd_buf, &description,
                     node->oneshell);
      std::string depfile;
      const bool has_depfile = GetDepfile(node, &cmd_buf, &depfile);
      if (has_depfile && g_flags.import_root) {
        std::string absolute;
        AbsPath(depfile, &absolute);
        depfile = std::move(absolute);
      }
      // Recipe translation may remove every command.  Keep the Ninja rule a
      // valid shell no-op, including when the leaf-output guard is added.
      if (cmd_buf.empty())
        cmd_buf = ":";
      // env.sh alone is invisible to Ninja's command hash. Repeat its stable
      // global exports here so an environment change schedules this edge and
      // makes its full recipe identity differ. Keep this after depfile
      // inference, which expects a literal compiler command, and before the
      // target-specific export prefixes already present in cmd_buf.
      if (managed_workspace)
        cmd_buf = EscapeNinja(workspace_environment_) + cmd_buf;
      if (!intermediate_proof_nodes_.empty())
        cmd_buf = "unset KATI_NINJA_GOALS KATI_NINJA_DEFAULT_GOALS; " + cmd_buf;
      std::string recipe_state;
      if (!node->is_phony) {
        if (workspace_recipe_dir_.empty()) {
          workspace_recipe_dir_ =
              GetFilename(managed_workspace ? ".recipes%s" : ".kati_recipes%s");
          if (!EnsureDirectory(workspace_recipe_dir_))
            ERROR("cannot create workspace recipe directory: %s",
                  workspace_recipe_dir_.c_str());
        }
        uint64_t key = UINT64_C(14695981039346656037);
        for (unsigned char c : node->output.str()) {
          key ^= c;
          key *= UINT64_C(1099511628211);
        }
        std::ostringstream name;
        name << workspace_recipe_dir_ << '/' << std::hex << key;
        recipe_state = name.str();
        size_t collision = 0;
        while (!recipe_state_paths_.insert(recipe_state).second)
          recipe_state = name.str() + "." + std::to_string(++collision);
        if (recipe_state.front() != '/')
          recipe_state = working_dir_ + "/" + recipe_state;
        // Full bytes, including the output name, are compared at execution.
        // Filename hashes only select storage; collisions can cause extra
        // rebuilds across graph reordering, never identity equality.
        std::string identity = "OMKTR_RECIPE1\n" + node->output.str();
        std::string command_identity = cmd_buf;
        if (!commands.front().runtime_sidecar.empty()) {
          command_identity = GetExecutablePath();
          AppendGraphString(commands.front().runtime_identity,
                            &command_identity);
          if (managed_workspace)
            AppendGraphString(workspace_environment_, &command_identity);
        }
        for (const std::string& part :
             {command_shell, command_shellflag, has_depfile ? depfile : "",
              command_identity}) {
          identity += '\0';
          identity += part;
        }
        recipe_state = PublishNinjaRecipeState(recipe_state, identity);
        if (intermediate_proof_nodes_.count(node))
          intermediate_recipe_states_[node->output.str()] = recipe_state;
        if (elided_source_recipe) {
          // The existing file satisfies this no-op rule without execution.
          // Retain that baseline so replacing ':' with a real recipe can
          // invalidate it, while keeping the source out of Ninja's outputs.
          WriteNinjaRecipeState(
              NinjaRecipeStateOwner(recipe_state) + ".success", identity);
          return;
        }
      }
      const std::string recipe_unchanged =
          recipe_state.empty() ? ""
          : has_depfile        ? "false && "
                               : "[ \"$${KATI_RECIPE_CHANGED:-0}\" != 1 ] && ";
      // Make's declared prerequisite list omits Ninja's depfile inputs. Never
      // let it suppress a scheduled compiler recipe in a managed workspace.
      // Keep commands stable before/after first output creation and restore.
      const bool make_freshness_guard = recipe_state.empty() || !has_depfile;
      // BusyBox test -nt compares whole seconds. Use the native stat helper
      // once for the complete input set, preserving Ninja's subsecond changes.
      auto inputs_current = [&](Symbol output) {
        std::string condition = ShellQuote(GetExecutablePath()) +
                                " --internal-low-resolution-up-to-date " +
                                ShellQuote(output.str()) + " " +
                                std::to_string(node->deps.size()) + " 0";
        for (const NamedDepNode& dep : node->deps)
          condition += " " + ShellQuote(dep.first.str());
        // A changed context sidecar can schedule a current consumer even when
        // Ninja restats its deliberately absent intermediate inputs. Reuse
        // the same complete freshness proof rather than recreating them.
        if (!consumer_proof.empty())
          condition = "{ " + condition + " || " +
                      ShellQuote(GetExecutablePath()) +
                      " --internal-intermediate-consumer-current " +
                      ShellQuote(output.str()) + " " +
                      ShellQuote(consumer_proof) + "; }";
        return condition;
      };
      // Internal cleanup helpers are ordinary commands, not recursive makes.
      // Classify the actual recipe before adding those executable references.
      use_local_pool = commands.front().runtime_sidecar.empty() &&
                       IsRecursiveKatiCommand(cmd_buf);
      // The helper already supervises the recipe process. Record the output
      // there after admission instead of launching another copy of the tool
      // and a shell subshell/trap wrapper for every file recipe. An absolute
      // path also works when the recipe itself changes its working directory.
      std::string cleanup_output;
      if (!node->is_phony && !node->precious && cmd_buf != ":") {
        cleanup_output = std::string(node->output.str());
        if (cleanup_output.front() != '/')
          cleanup_output = working_dir_ + "/" + cleanup_output;
      }
      if (vpath_output) {
        const std::string output = ShellQuote(node->output.str());
        const std::string provider = ShellQuote(node->vpath_provider.str());
        std::string current = "if " + recipe_unchanged + "{ [ ! -e " + output +
                              " ] || [ -L " + output + " ]; } && [ -e " +
                              provider + " ]";
        std::string local_current = "if " + recipe_unchanged + "[ -e " +
                                    output + " ] && [ ! -L " + output + " ]";
        for (const NamedDepNode& dep : node->deps) {
          // A prerequisite-only rule retains its provider even when a
          // prerequisite is newer: there is no recipe to replace it.
          if (node->cmds.empty())
            break;
          if (dep.second->is_phony) {
            current += " && false";
            local_current += " && false";
            break;
          }
        }
        if (!node->cmds.empty() && !node->deps.empty()) {
          current += " && " + inputs_current(node->vpath_provider);
          local_current += " && " + inputs_current(node->output);
        }
        const std::string link_target =
            node->vpath_provider.str().front() == '/'
                ? std::string(node->vpath_provider.str())
                : working_dir_ + "/" + node->vpath_provider.str();
        std::string materialize = "rm -f " + output + " && ln -s " +
                                  ShellQuote(link_target) + " " + output;
        const std::string_view parent = Dirname(node->output.str());
        if (!parent.empty() && parent != ".")
          materialize = "mkdir -p " + ShellQuote(parent) + " && " + materialize;
        cmd_buf = local_current + "; then :; el" + current + "; then " +
                  materialize + "; else " + "if [ -L " + output +
                  " ]; then rm -f " + output + "; fi; " + cmd_buf + "; fi";
      }
      // Ninja has no build-log entry for files that predate a newly emitted
      // graph, and runs their recipes even when make would find them current.
      // OpenSSL's configdata.pm recipe deliberately fails after reconfiguring,
      // so the first Ninja run must honor the ordinary timestamp fast path.
      // Phony and validation prerequisites retain their usual Ninja behavior.
      const bool has_phony_input = std::any_of(
          node->deps.begin(), node->deps.end(),
          [](const NamedDepNode& dep) { return dep.second->is_phony; });
      if (!intermediate_proof.empty()) {
        cmd_buf = "if " + recipe_unchanged + ShellQuote(GetExecutablePath()) +
                  " --internal-intermediate-current " +
                  ShellQuote(node->output.str()) + " " +
                  ShellQuote(intermediate_proof) + "; then exit 0; fi; " +
                  cmd_buf;
      }
      // Order-only prerequisites may run, but they do not make an existing
      // output stale. Ninja has no prior build-log entry for such an output.
      if (make_freshness_guard && !vpath_output && !node->is_phony &&
          node->deps.empty() && node->validations.empty()) {
        cmd_buf = "if " + recipe_unchanged + "[ -e " +
                  ShellQuote(node->output.str()) + " ]; then :; else " +
                  cmd_buf + "; fi";
      } else if (make_freshness_guard && !vpath_output && !node->is_phony &&
                 !node->deps.empty() && !has_phony_input &&
                 node->validations.empty()) {
        const std::string current =
            "if " + recipe_unchanged + inputs_current(node->output);
        cmd_buf = current + "; then :; else " + cmd_buf + "; fi";
      }
      // Recursive sub-builds remain serialized; recipe exports are prepared by
      // CommandEvaluator for both direct and Ninja execution.
      // The generated environment snapshot is part of Kati's graph output,
      // but users may invoke Ninja directly instead of using ninja.sh.  Load
      // that snapshot inside every recipe so exported make variables have the
      // same visibility in both entry paths.  The guard keeps graphs usable
      // with callers that intentionally provide their own environment.
      // GetFilename() may return an absolute path when --ninja_dir is
      // absolute.  Prefixing every result with "./" changes that path into
      // a relative one and makes the generated environment silently vanish.
      // Keep relative paths relative to the Ninja working directory, while
      // preserving absolute paths verbatim.
      const std::string quoted_env_script = ShellQuote(env_snapshot_);
      cmd_buf = "if [ -r " + quoted_env_script + " ]; then . " +
                quoted_env_script + "; fi; " + cmd_buf;
      // GNU make runs each recipe in the directory in which its makefile was
      // read. Ninja runs all recipes from the graph's invocation directory,
      // so recursive graphs otherwise resolve relative paths incorrectly.
      // Preserve make's directory semantics for every emitted recipe,
      // including the graph's environment snapshot.
      cmd_buf = "cd " + ShellQuote(working_dir_) + " && " + cmd_buf;

      // Ninja compares nanosecond timestamps, while GNU make treats
      // prerequisites named by .LOW_RESOLUTION_TIME as second-resolution
      // timestamps.  Ninja may therefore schedule this edge for a change
      // that GNU make would ignore.  Recheck the complete normal-prerequisite
      // set at recipe time and skip only when the edge is stale solely due to
      // a low-resolution prerequisite within the same second.
      const std::string output = ShellQuote(node->output.str());
      if (make_freshness_guard && !node->low_resolution_inputs.empty()) {
        std::string guard =
            "if " + recipe_unchanged + ShellQuote(GetExecutablePath()) +
            " --internal-low-resolution-up-to-date " + output + " " +
            std::to_string(node->actual_inputs.size()) + " " +
            std::to_string(node->low_resolution_inputs.size());
        for (Symbol input : node->actual_inputs)
          guard += " " + ShellQuote(input.str());
        for (Symbol input : node->low_resolution_inputs)
          guard += " " + ShellQuote(input.str());
        guard += "; then exit 0; fi; ";
        cmd_buf = guard + cmd_buf;
      }

      if (node->delete_on_error && !node->precious) {
        cmd_buf = "( " + cmd_buf +
                  " ); kati_status=$$?; "
                  "if [ $$kati_status -ne 0 ] && [ -e " +
                  output + " ] && [ ! -d " + output + " ]; then rm -f " +
                  output + "; fi; exit $$kati_status";
      }
      // Every ordinary Ninja recipe participates in the same budget as
      // recursive executors. A recursive boundary holds no token itself.
      // The helper forwards one live reservation to indirect child builds.
      const bool token_recipe =
          !use_local_pool &&
          !(commands.size() == 1 && commands.front().cmd == ":");
      const bool helper_recipe = token_recipe || use_local_pool ||
                                 !cleanup_output.empty() ||
                                 !recipe_state.empty();
      const char* recipe_helper = token_recipe
                                      ? " --internal-run-recipe "
                                      : " --internal-run-recursive-recipe ";
      const bool recipe_in_rsp = cmd_buf.size() > 100 * 1000;
      const std::string cleanup_arg =
          recipe_state.empty()
              ? (cleanup_output.empty() ? "" : " " + ShellQuote(cleanup_output))
              : " " + ShellQuote(cleanup_output) + " " +
                    ShellQuote(recipe_state);
      if (cmd_buf.size() > 4096 && helper_recipe) {
        // Store the unexpanded script once, outside Ninja's in-memory graph.
        // cmd_buf contains Ninja escaping; the interpreter consumes shell
        // bytes.
        std::string script;
        for (size_t i = 0; i < cmd_buf.size(); ++i) {
          if (cmd_buf[i] == '$' && i + 1 < cmd_buf.size() &&
              cmd_buf[i + 1] == '$')
            ++i;
          script += cmd_buf[i];
        }
        script_path = PublishGraphObject(
            GetFilename(".kati_script-") + GraphDigest(script), script);
        MakefileCacheManager::Get().AddGeneratedGraphDep(script_path);
        cmd_buf = EscapeNinja(ShellQuote(GetExecutablePath())) +
                  (token_recipe ? " --internal-run-recipe-file "
                                : " --internal-run-recursive-recipe-file ") +
                  ShellQuote(command_shell) + " " +
                  ShellQuote(command_shellflag) + " " +
                  EscapeNinja(ShellQuote(script_path)) +
                  EscapeNinja(cleanup_arg);
      }
      std::string helper_prefix;
      if (helper_recipe && !recipe_in_rsp && script_path.empty()) {
        helper_prefix = "cd " + ShellQuote(working_dir_) + " && " +
                        EscapeNinja(ShellQuote(GetExecutablePath())) +
                        recipe_helper + ShellQuote(command_shell) + " " +
                        ShellQuote(command_shellflag) + " ";
        cmd_buf =
            helper_prefix + ShellQuote(cmd_buf) + EscapeNinja(cleanup_arg);
      }
      struct stat compact_output;
      const bool special_output =
          node->precious || node->output == Intern(".") ||
          (stat(node->output.c_str(), &compact_output) == 0 &&
           S_ISDIR(compact_output.st_mode));
      const bool compact = !special_output &&
                           g_flags.GraphOptimizationEnabled("compact") &&
                           !recipe_in_rsp && cmd_buf.size() <= 100 * 1000;
      if (compact) {
        if (helper_recipe) {
          edge_command = cmd_buf.substr(helper_prefix.size());
        } else {
          edge_command = command_shell == "/bin/sh" && command_shellflag == "-c"
                             ? cmd_buf
                             : command_shell + " " + command_shellflag + " " +
                                   ShellQuote(cmd_buf);
        }

        edge_depfile = depfile;

        // Keep the native launcher in the shared rule. The edge retains the
        // opaque quoted script and cleanup arguments, so expansion produces
        // exactly the same command as the ordinary emitter.
        cmd_buf = helper_prefix + "$omktr_command";
        if (has_depfile)
          depfile = "$omktr_depfile";
      }
      std::string rule_key = description;
      rule_key += '\0';
      rule_key += cmd_buf;
      rule_key += '\0';
      rule_key += command_shell;
      rule_key += '\0';
      rule_key += command_shellflag;
      rule_key += '\0';
      rule_key += has_depfile ? depfile : "";
      rule_key += '\0';
      const bool effective_restat =
          node->is_restat ||
          (((!commands.front().runtime_sidecar.empty() &&
             !commands.front().runtime_freshness_only) ||
            !script_path.empty() || !proof_dependency.empty()) &&
           !node->is_phony && !has_phony_input && make_freshness_guard);
      rule_key += effective_restat ? '1' : '0';
      rule_key += node->delete_on_error ? '1' : '0';
      rule_key += node->precious ? '1' : '0';
      rule_key += g_flags.emit_sandbox_disabled ? '1' : '0';
      if (cmd_buf.size() > 100 * 1000) {
        // Large recipes source a response file named after this output, so
        // they cannot share a Ninja rule with another output.
        rule_key += node->output.str();
      }

      auto existing_rule = rules_.find(rule_key);
      if (existing_rule != rules_.end()) {
        rule_name = existing_rule->second;
      } else {
        rule_name = StringPrintf("rule%d", rule_id_++);
        rules_.emplace(std::move(rule_key), rule_name);
        out << "rule " << rule_name << "\n";
        out << " description = " << description << "\n";
        if (has_depfile)
          out << " depfile = " << (compact ? depfile : EscapeNinja(depfile))
              << "\n deps = gcc\n";

        // It seems Linux is OK with ~130kB and Mac's limit is ~250kB.
        // TODO: Find this number automatically.
        if (script_path.empty() &&
            (recipe_in_rsp || cmd_buf.size() > 100 * 1000)) {
          out << " rspfile = $out.rsp\n";
          out << " rspfile_content = " << cmd_buf << "\n";
          std::string rspfile = node->output.str() + ".rsp";
          if (g_flags.import_root) {
            std::string absolute;
            AbsPath(rspfile, &absolute);
            rspfile = std::move(absolute);
          }
          if (rspfile.front() != '/')
            rspfile = "./" + rspfile;
          const std::string source_rspfile = ". " + ShellQuote(rspfile);
          if (helper_recipe && recipe_in_rsp) {
            out << " command = " << EscapeNinja(ShellQuote(GetExecutablePath()))
                << recipe_helper << ShellQuote(command_shell) << ' '
                << ShellQuote(command_shellflag) << ' '
                << ShellQuote(source_rspfile) << EscapeNinja(cleanup_arg)
                << "\n";
          } else {
            out << " command = " << (helper_recipe ? "/bin/sh" : command_shell)
                << ' ' << (helper_recipe ? "-c" : command_shellflag) << ' '
                << ShellQuote(source_rspfile) << "\n";
          }
        } else if (compact || helper_recipe ||
                   (command_shell == "/bin/sh" && command_shellflag == "-c")) {
          // Ninja already runs commands through /bin/sh -c on POSIX hosts.
          // A second identical shell adds a process launch to every recipe.
          // cmd_buf is already escaped for Ninja; let its shell parse the
          // complete recipe once, including assignments and nested shells.
          out << " command = " << cmd_buf << "\n";
        } else {
          // Keep the recipe opaque to the shell that Ninja uses to launch the
          // command. The recipe itself may contain another shell invocation
          // and shell variables assigned earlier in that recipe (for example,
          // r=...; export r; ...). ShellQuote preserves the command until the
          // requested shell parses it.
          out << " command = " << command_shell << ' ' << command_shellflag
              << ' ' << ShellQuote(cmd_buf) << "\n";
        }
        if (effective_restat)
          out << " restat = 1\n";
        if (g_flags.emit_sandbox_disabled)
          out << " sandbox_disabled = true\n";
      }
    }

    EmitBuild(nn, rule_name, use_local_pool || node->is_notparallel, out,
              edge_command, edge_depfile, script_path, proof_dependency);
  }

  std::string EscapeNinja(std::string_view s) const {
    if (s.find_first_of("$: ") == std::string::npos)
      return std::string(s);
    std::string r;
    for (char c : s) {
      switch (c) {
        case '$':
        case ':':
        case ' ':
          r += '$';
          [[fallthrough]];
        default:
          r += c;
      }
    }
    return r;
  }

  const std::string& EscapeBuildTarget(Symbol s) const {
    auto cached = escaped_targets_.find(s);
    if (cached != escaped_targets_.end())
      return cached->second;
    std::string absolute;
    std::string_view path = s.str();
    if (g_flags.import_root) {
      AbsPath(path, &absolute);
      path = absolute;
    }
    std::string escaped =
        !path_prefix_.empty() && HasPrefix(path, path_prefix_)
            ? "${" + path_variable_ + "}" +
                  EscapeNinja(path.substr(path_prefix_.size()))
            : EscapeNinja(path);
    return escaped_targets_.emplace(s, std::move(escaped)).first->second;
  }

  bool TryImport(const NinjaNode& nn, std::ostream& out) {
    const DepNode& node = *nn.node;
    if (!preparing_imports_) {
      auto found = prepared_imports_.find(node.output);
      if (found == prepared_imports_.end())
        return false;
      out << found->second;
      return true;
    }
    if (!g_flags.GraphOptimizationEnabled("recursive") || g_flags.import_root ||
        g_flags.is_dry_run || nn.commands.size() != 1 || !node.is_phony ||
        g_flags.is_touch || g_flags.is_question || g_flags.keep_going ||
        g_flags.load_limit_set || g_flags.detect_depfiles || node.depfile_var ||
        node.ninja_pool_var || node.is_notparallel || node.precious ||
        node.delete_on_error || !node.validations.empty() ||
        !node.implicit_outputs.empty() || !node.wait_groups.empty() ||
        !node.double_colon_group_for_cmd.empty())
      return false;
    if (node.rule_vars &&
        node.rule_vars->find(Intern("MAKEFLAGS")) != node.rule_vars->end())
      return false;
    std::vector<std::string> arguments;
    if (!ParseImportCommand(nn.commands.front(), &arguments))
      return false;
    std::string directory = GetFilename(".imports%s") + "/" + generation_id_ +
                            "/" + GraphDigest(node.output.str());
    std::string absolute_directory;
    AbsPath(directory, &absolute_directory);
    directory = std::move(absolute_directory);
    const std::string directory_base = directory;
    size_t collision = 0;
    while (!import_directories_.insert(directory).second)
      directory = directory_base + "." + std::to_string(++collision);
    if (!EnsureDirectory(directory))
      return false;
    const std::string gate = directory + "/entry";
    if (GenerateImportedGraph(nn.commands.front(), arguments, directory,
                              gate) != 0)
      return false;
    std::string bytes;
    if (!ReadGraphCache(directory + "/manifest", &bytes))
      return false;
    std::string_view view(bytes);
    std::vector<std::string> outputs, sources, goals;
    bool matching_budget = false;
    while (!view.empty()) {
      std::string kind, path;
      if (!ConsumeGraphString(&view, &kind) ||
          !ConsumeGraphString(&view, &path))
        return false;
      if (kind == "output")
        outputs.push_back(path);
      else if (kind == "source")
        sources.push_back(path);
      else if (kind == "goal")
        goals.push_back(path);
      else if (kind == "jobs") {
        if (matching_budget || path != std::to_string(g_flags.num_jobs))
          return false;
        matching_budget = true;
      } else
        return false;
    }
    if (goals.empty() || !matching_budget)
      return false;
    auto unambiguous_output = [](const std::string& path) {
      const size_t slash = path.rfind('/');
      const std::string parent = slash == 0 ? "/" : path.substr(0, slash);
      char resolved[PATH_MAX];
      if (slash == std::string::npos || !realpath(parent.c_str(), resolved) ||
          parent != resolved)
        return false;
      struct stat st;
      if (lstat(path.c_str(), &st) < 0)
        return errno == ENOENT;
      return !S_ISLNK(st.st_mode) && (!S_ISREG(st.st_mode) || st.st_nlink == 1);
    };
    std::unordered_set<std::string> claimed = imported_outputs_;
    for (const NinjaNode& other : nodes_) {
      std::string absolute;
      AbsPath(other.node->output.str(), &absolute);
      if (!unambiguous_output(absolute))
        return false;
      claimed.insert(absolute);
    }
    for (const auto& output : outputs)
      if (!unambiguous_output(output) || !claimed.insert(output).second)
        return false;
    std::vector<const DepNode*> pending;
    for (const auto& input : node.deps)
      pending.push_back(input.second);
    for (const auto& input : node.order_onlys)
      pending.push_back(input.second);
    std::unordered_set<const DepNode*> visited;
    while (!pending.empty()) {
      const DepNode* input = pending.back();
      pending.pop_back();
      if (!visited.insert(input).second)
        continue;
      std::string absolute;
      AbsPath(input->output.str(), &absolute);
      if (std::find(outputs.begin(), outputs.end(), absolute) != outputs.end())
        return false;
      for (const auto& dependency : input->deps)
        pending.push_back(dependency.second);
      for (const auto& dependency : input->order_onlys)
        pending.push_back(dependency.second);
    }
    for (const auto& output : outputs)
      imported_outputs_.insert(output);
    out << "build " << EscapeNinja(gate) << ": phony";
    for (const auto& input : node.deps)
      out << ' ' << EscapeBuildTarget(input.first);
    if (!node.order_onlys.empty()) {
      out << " ||";
      for (const auto& input : node.order_onlys)
        out << ' ' << EscapeBuildTarget(input.first);
    }
    out << "\nsubninja " << EscapeNinja(directory + "/build.ninja") << "\n";
    out << "build " << EscapeBuildTarget(node.output) << ": phony";
    for (const auto& goal : goals)
      out << ' ' << EscapeNinja(goal);
    out << '\n';
    for (const auto& source : sources)
      import_inputs_.push_back(source);
    import_artifacts_.push_back(directory + "/build.ninja");
    import_artifacts_.push_back(directory + "/env.sh");
    import_artifacts_.push_back(directory + "/ninja.sh");
    import_artifacts_.push_back(directory + "/manifest");
    return true;
  }

  void PrepareImports() {
    if (!g_flags.GraphOptimizationEnabled("recursive") || g_flags.import_root ||
        !cycle_edges_.empty() ||
        MakefileCacheManager::Get().has_observable_evaluation())
      return;
    for (const CommandResult* command : GetShellCommandResults())
      if (command->op == CommandOp::SHELL || command->op == CommandOp::WRITE ||
          command->op == CommandOp::APPEND)
        return;
    preparing_imports_ = true;
    bool valid = true;
    for (const NinjaNode& nn : nodes_) {
      bool recursive = false;
      for (const Command& command : nn.commands)
        recursive |= IsRecursiveKatiCommand(command.cmd);
      if (!recursive)
        continue;
      std::ostringstream fragment;
      if (!TryImport(nn, fragment)) {
        valid = false;
        break;
      }
      prepared_imports_[nn.node->output] = fragment.str();
    }
    preparing_imports_ = false;
    // Commit ownership changes together. An opaque sibling could otherwise
    // race an imported graph that owns the same files behind that boundary.
    if (!valid) {
      prepared_imports_.clear();
      imported_outputs_.clear();
      LOG_STAT("recursive import: conservative graph-wide fallback");
    } else {
      for (const auto& child : prepared_imports_)
        LOG_STAT("imported child graph: %s", child.first.c_str());
      for (const auto& input : import_inputs_)
        MakefileCacheManager::Get().AddExtraFileDep(input);
      for (const auto& artifact : import_artifacts_)
        MakefileCacheManager::Get().AddGeneratedGraphDep(artifact);
    }
  }

  bool IsExistingDirectoryAlias(Symbol dependency) const {
    std::string_view path = dependency.str();
    if (path.empty() || path.back() != '/')
      return false;

    // Ninja canonicalizes a trailing slash, so an existing directory such
    // as "doc/" would resolve to the unrelated make target "doc".  Make
    // keeps those names distinct.  The directory already exists, so this
    // edge cannot create it and must not point at the other make target.
    while (!path.empty() && path.back() == '/')
      path.remove_suffix(1);
    struct stat st;
    return !path.empty() && done_.exists(Intern(path)) &&
           stat(dependency.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }

  void EmitBuild(const NinjaNode& nn,
                 const std::string& rule_name,
                 bool use_local_pool,
                 std::ostream& out,
                 const std::string& edge_command,
                 const std::string& edge_depfile,
                 const std::string& script_path,
                 const std::string& intermediate_proof) {
    const DepNode* node = nn.node;
    struct stat output_stat;
    const bool directory_output =
        node->output == Intern(".") ||
        (stat(node->output.str().c_str(), &output_stat) == 0 &&
         S_ISDIR(output_stat.st_mode));
    std::string directory_marker;
    if (directory_output) {
      directory_marker = "__kati_directory_";
      for (char c : node->output.str())
        directory_marker += c == '/' ? '_' : c;
    }
    const bool precious_output =
        node->precious && !node->is_phony && rule_name != "phony";
    if (precious_output) {
      // Ninja's own abort cleanup must never own a .PRECIOUS file. Keep its
      // command reachable through a synthetic output; the recipe freshness
      // guard still tests the real file and prevents needless compilation.
      directory_marker = "__kati_precious_";
      for (unsigned char c : node->output.str()) {
        directory_marker += "0123456789abcdef"[c >> 4];
        directory_marker += "0123456789abcdef"[c & 15];
      }
    }
    std::string target = EscapeBuildTarget(directory_output || precious_output
                                               ? Intern(directory_marker)
                                               : node->output);
    out << "build " << target;
    if (!node->implicit_outputs.empty()) {
      out << " |";
      for (Symbol output : node->implicit_outputs) {
        out << " " << EscapeBuildTarget(output);
      }
    }
    out << ": " << rule_name;
    std::vector<Symbol> order_onlys;
    if (node->is_phony && !g_flags.use_ninja_phony_output) {
      out << ' ' << EscapeBuildTarget(Intern("_kati_always_build_"));
    }
    out << NormalDependencyFragment(node);
    if (node->vpath_provider.IsValid() && !node->is_phony)
      out << " " << EscapeBuildTarget(node->vpath_provider);
    bool has_order_only = false;
    bool has_implicit_inputs = false;
    auto implicit_input = [&](const std::string& path) {
      if (!has_implicit_inputs)
        out << " |";
      has_implicit_inputs = true;
      out << " " << EscapeBuildTarget(Intern(path));
    };
    for (const Command& command : nn.commands) {
      if (!command.runtime_sidecar.empty() && !command.runtime_freshness_only) {
        implicit_input(command.runtime_sidecar);
        implicit_input(command.runtime_context);
      }
    }
    if (!script_path.empty())
      implicit_input(script_path);
    if (!intermediate_proof.empty())
      implicit_input(intermediate_proof);
    if (!intermediate_proof.empty())
      implicit_input(intermediate_states_path_);
    for (auto const& d : node->order_onlys) {
      if (IsCycleEdge(node->output, d.first) ||
          IsExistingDirectoryAlias(d.first))
        continue;
      if (!has_order_only)
        out << " ||";
      has_order_only = true;
      out << " " << EscapeBuildTarget(d.first).c_str();
    }
    for (const auto& command : nn.commands) {
      if (!command.runtime_freshness_only)
        continue;
      // Content-selected paths already participate in the command hash. Their
      // creation time is not a Make prerequisite timestamp.
      if (!has_order_only)
        out << " ||";
      has_order_only = true;
      out << " " << EscapeBuildTarget(Intern(command.runtime_sidecar)) << " "
          << EscapeBuildTarget(Intern(command.runtime_context));
    }
    if (!nn.commands.empty() && !env_snapshot_.empty()) {
      if (!has_order_only)
        out << " ||";
      has_order_only = true;
      out << " " << EscapeBuildTarget(Intern(env_snapshot_));
    }
    if (g_flags.import_gate && rule_name != "phony") {
      if (!has_order_only)
        out << " ||";
      out << ' ' << EscapeNinja(g_flags.import_gate);
    }
    bool has_validation = false;
    for (auto const& d : node->validations) {
      if (IsCycleEdge(node->output, d.first) ||
          IsExistingDirectoryAlias(d.first))
        continue;
      if (!has_validation)
        out << " |@";
      has_validation = true;
      out << " " << EscapeBuildTarget(d.first).c_str();
    }

    out << "\n";

    // Ninja removes an edge's declared outputs when the edge is interrupted.
    // A recursive make target can legitimately be an existing directory, so
    // declaring that directory as the command output makes interruption try
    // to unlink it (for example, remove(.)). Use a synthetic marker for the
    // command and retain the logical directory as a phony alias.
    if (directory_output || precious_output) {
      out << "build " << EscapeBuildTarget(node->output) << ": phony "
          << EscapeBuildTarget(Intern(directory_marker)) << "\n";
    }

    std::string pool;
    if (node->ninja_pool_var) {
      node->ninja_pool_var->Eval(ev_, &pool);
    }

    if (pool != "") {
      if (pool != "none") {
        out << " pool = " << pool << "\n";
      }
    } else if (g_flags.default_pool && rule_name != "phony") {
      out << " pool = " << g_flags.default_pool << "\n";
    } else if (use_local_pool) {
      out << " pool = local_pool\n";
    }
    if (node->is_phony && g_flags.use_ninja_phony_output) {
      out << " phony_output = true\n";
    }
    if (node->tags_var) {
      std::string tags;
      node->tags_var->Eval(ev_, &tags);
      if (!tags.empty()) {
        out << " tags = " << tags << "\n";
      }
    }
    if (!edge_command.empty()) {
      out << " omktr_command = " << edge_command << "\n";

      if (!edge_depfile.empty())
        out << " omktr_depfile = " << EscapeNinja(edge_depfile) << "\n";
    }
  }

  const std::string& NormalDependencyFragment(const DepNode* node) {
    size_t hash = node->deps.size();
    bool cacheable = cycle_edges_.empty();
    for (const auto& edge : node->deps) {
      hash ^= std::hash<Symbol>()(edge.first) + size_t(0x9e3779b9) +
              (hash << 6) + (hash >> 2);
      const std::string_view path = edge.first.str();
      // Directory-alias filtering observes the live filesystem. A nested
      // shell in a tag expression can change it between emitted edges.
      if (!path.empty() && path.back() == '/')
        cacheable = false;
    }
    // Cycle filtering is output-specific. Ordinary immutable sequences can
    // share their emitted text after an exact comparison, never on hash alone.
    if (cacheable) {
      auto found = dependency_fragments_.find(hash);
      if (found != dependency_fragments_.end())
        for (const auto& entry : found->second)
          if (*entry.first == node->deps)
            return entry.second;
    }
    std::string text;
    std::unordered_set<Symbol> emitted;
    for (const auto& edge : node->deps) {
      if (IsCycleEdge(node->output, edge.first) ||
          IsExistingDirectoryAlias(edge.first))
        continue;
      auto group = source_dependency_groups_.find(edge.first);
      const Symbol name =
          group == source_dependency_groups_.end() ? edge.first : group->second;
      if (emitted.insert(name).second) {
        text += ' ';
        text += EscapeBuildTarget(name);
      }
    }
    if (cacheable && fragment_bytes_ + text.size() <= 32 * 1024 * 1024) {
      fragment_bytes_ += text.size();
      auto& entries = dependency_fragments_[hash];
      entries.emplace_back(&node->deps, std::move(text));
      return entries.back().second;
    }
    uncached_fragment_ = std::move(text);
    return uncached_fragment_;
  }

  static std::string GetEnvScriptFilename() { return GetFilename("env%s.sh"); }

  void PrintStats() const {
    size_t dependency_edges = 0;
    size_t recipe_nodes = 0;
    size_t recipe_commands = 0;
    size_t recursive_edges = 0;
    size_t pool_annotations = 0;
    size_t max_dependencies = 0;
    size_t phony_nodes = 0;
    size_t leaf_recipes = 0;
    size_t critical_path = 0;
    std::unordered_map<Symbol, size_t> depths;
    std::unordered_set<Symbol> visiting;

    auto get_depth = [&](const auto& self, const DepNode* node) -> size_t {
      auto cached = depths.find(node->output);
      if (cached != depths.end())
        return cached->second;
      // A circular make dependency is invalid for a build path, but Kati can
      // still represent it while diagnosing the graph. Break the diagnostic
      // cycle without affecting the generated Ninja graph.
      if (!visiting.insert(node->output).second)
        return 0;
      size_t depth = 1;
      for (const auto& dependency : node->deps)
        depth = std::max(depth, 1 + self(self, dependency.second));
      for (const auto& dependency : node->order_onlys)
        depth = std::max(depth, 1 + self(self, dependency.second));
      for (const auto& dependency : node->validations)
        depth = std::max(depth, 1 + self(self, dependency.second));
      visiting.erase(node->output);
      depths.emplace(node->output, depth);
      return depth;
    };

    for (const NinjaNode& ninja_node : nodes_) {
      const DepNode* node = ninja_node.node;
      const size_t dependencies = node->deps.size() + node->order_onlys.size() +
                                  node->validations.size();
      dependency_edges += dependencies;
      if (dependencies > max_dependencies)
        max_dependencies = dependencies;
      if (node->is_phony)
        ++phony_nodes;
      if (node->ninja_pool_var != nullptr)
        ++pool_annotations;
      if (!ninja_node.commands.empty())
        ++recipe_nodes;
      if (!ninja_node.commands.empty() && dependencies == 0)
        ++leaf_recipes;
      critical_path = std::max(critical_path, get_depth(get_depth, node));
      for (const Command& command : ninja_node.commands) {
        ++recipe_commands;
        if (command.cmd.find("KATI_DEPTH=") != std::string::npos) {
          ++recursive_edges;
        }
      }
    }

    fprintf(stderr,
            "kati: Ninja graph stats: nodes=%zu recipes=%zu commands=%zu "
            "dependencies=%zu phony=%zu recursive=%zu pools=%zu "
            "max_dependencies=%zu leaf_recipes=%zu critical_path=%zu\n",
            nodes_.size(), recipe_nodes, recipe_commands, dependency_edges,
            phony_nodes, recursive_edges, pool_annotations, max_dependencies,
            leaf_recipes, critical_path);
    if (recursive_edges != 0) {
      fprintf(stderr,
              "kati: Ninja scheduling note: recursive commands hide their "
              "child graph from Ninja; inspect child Kati scheduling too.\n");
    }
  }

  void EmitSourceDependencyGroups(std::ostream& out) {
    // Factor headers with exactly the same consumers. This preserves every
    // dependency without relying on textual order in compiler depfiles.
    std::unordered_map<Symbol, std::vector<size_t>> consumers;
    for (size_t i = 0; i < nodes_.size(); ++i) {
      const DepNode* node = nodes_[i].node;
      for (const auto& edge : node->deps) {
        const DepNode* input = edge.second;
        if (input->is_phony || !input->cmds.empty() || !input->deps.empty() ||
            !input->order_onlys.empty() || !input->validations.empty() ||
            IsCycleEdge(node->output, edge.first) ||
            IsExistingDirectoryAlias(edge.first))
          continue;
        auto& parents = consumers[edge.first];
        if (parents.empty() || parents.back() != i)
          parents.push_back(i);
      }
    }
    std::map<std::vector<size_t>, std::vector<Symbol>> groups;
    for (auto& entry : consumers)
      if (entry.second.size() > 1)
        groups[std::move(entry.second)].push_back(entry.first);
    std::unordered_set<Symbol> group_names;
    for (auto& group : groups) {
      auto& inputs = group.second;
      if (inputs.size() < 2)
        continue;
      std::sort(inputs.begin(), inputs.end(),
                [](Symbol a, Symbol b) { return a.str() < b.str(); });
      std::string key;
      for (Symbol input : inputs)
        key += " " + EscapeBuildTarget(input);
      std::string name = GetFilename(".kati_deps-") + GraphDigest(key);
      while (done_.exists(Intern(name)) || group_names.count(Intern(name)))
        name += "_";
      const size_t users = group.first.size();
      if (key.size() * users <=
          key.size() + (users + 1) * (name.size() + 1) + 20)
        continue;
      Symbol symbol = Intern(name);
      group_names.insert(symbol);
      out << "build " << EscapeBuildTarget(symbol) << ": phony" << key << "\n";
      for (Symbol input : inputs)
        source_dependency_groups_.emplace(input, symbol);
    }
  }

  void GenerateNinja() {
    ScopedTimeReporter tr("ninja gen (emit)");
    const std::string filename = GetNinjaFilename();
    const std::string temporary = filename + ".tmp";
    std::ofstream out(temporary, std::ios::binary);
    if (!out)
      PERROR("fopen(build.ninja) failed");

    out << "# Generated by kati " << kGitVersion << "\n\n"
        << "ninja_required_version = 1.13.2\n\n";

    if (!path_prefix_.empty())
      out << path_variable_ << " = " << path_prefix_ << "\n\n";

    // Recursive make boundaries are intentionally serialized.  Their
    // internal dependency graphs are opaque to this Ninja graph, so Ninja
    // cannot otherwise know about shared outputs produced by sibling
    // recursive invocations.
    if (!g_flags.import_root)
      out << "pool local_pool\n depth = 1\n\n";

    if (!used_envs_.empty()) {
      out << "# Environment variables used:\n";
      for (const auto& [key, value] : used_envs_) {
        out << "# " << key << "=" << value << '\n';
      }
      out << '\n';
    }

    if (!g_flags.no_ninja_prelude) {
      if (g_flags.ninja_dir) {
        out << "builddir = " << g_flags.ninja_dir << "\n\n";
      }

      if (!g_flags.use_ninja_phony_output) {
        out << "build " << EscapeBuildTarget(Intern("_kati_always_build_"))
            << ": phony\n\n";
      }
    }

    if (!g_flags.generate_empty_ninja) {
      SymbolSet outputs;
      for (const auto& node : nodes_) {
        outputs.insert(node.node->output);
        for (Symbol output : node.node->implicit_outputs)
          outputs.insert(output);
      }
      SymbolSet providers;
      for (const auto& node : nodes_) {
        Symbol provider = node.node->vpath_provider;
        if (!provider.IsValid() || node.node->is_phony ||
            outputs.exists(provider) || providers.exists(provider))
          continue;
        providers.insert(provider);
        // A provider is optional once a recipe has produced a local file.
        // A phony leaf preserves its real timestamp while it exists and
        // lets the output's runtime guard handle disappearance safely.
        out << "build " << EscapeBuildTarget(provider) << ": phony\n\n";
      }
      EmitSourceDependencyGroups(out);
      for (const auto& node : nodes_) {
        EmitNode(node, out);
      }
      if (!intermediate_proof_nodes_.empty()) {
        std::string states, previous;
        AppendGraphString("intermediate-states-v1", &states);
        for (const auto& state : intermediate_recipe_states_) {
          AppendGraphString(state.first, &states);
          AppendGraphString(state.second, &states);
        }
        const std::string path = intermediate_states_path_;
        if (PublishGraphObject(path, states) != path)
          ERROR("conflicting intermediate recipe state object");
        MakefileCacheManager::Get().AddGeneratedGraphDep(path);
      }

      std::string default_targets;
      if (g_flags.targets.empty() || g_flags.gen_all_targets) {
        CHECK(default_target_);
        default_targets = EscapeBuildTarget(default_target_->output);
      } else {
        for (Symbol s : g_flags.targets) {
          if (!default_targets.empty())
            default_targets += ' ';
          default_targets += EscapeBuildTarget(s);
        }
      }

      std::vector<Symbol> intermediate_outputs;
      std::unordered_set<Symbol> seen_intermediates;
      for (const NinjaNode& ninja_node : nodes_) {
        const DepNode* node = ninja_node.node;
        if (node->intermediate &&
            seen_intermediates.insert(node->output).second)
          intermediate_outputs.push_back(node->output);
      }
      if (!intermediate_outputs.empty()) {
        const std::string cleanup_target = ".kati_intermediate_cleanup";
        out << "\nrule kati_intermediate_cleanup\n"
            << " command = " << shell_ << ' ' << shell_flags_ << ' ';
        std::string cleanup_command = "rm -f --";
        for (Symbol output : intermediate_outputs) {
          cleanup_command += " ";
          cleanup_command += ShellQuote(output.str());
        }
        cleanup_command += " && touch \"$out\"";
        out << ShellQuote(cleanup_command) << "\n"
            << "build " << cleanup_target << ": kati_intermediate_cleanup | "
            << default_targets << "\n";
        default_targets = cleanup_target;
      }
      if (!g_flags.import_root)
        out << "\n" << "default " << default_targets << '\n';
    }

    if (g_flags.ninja_stats)
      PrintStats();

    SymbolSet used_env_vars(Vars::used_env_vars());
    // PATH changes $(shell).
    used_env_vars.insert(Intern("PATH"));
    for (Symbol e : used_env_vars) {
      // MFLAGS is regenerated from this invocation's options, not inherited
      // input. Comparing it with the caller's environment forces false regen.
      if (e.str() == "MFLAGS")
        continue;
      std::string_view val(getenv(e.c_str()));
      used_envs_.emplace(e.str(), std::string(val));
    }
    out.close();
    if (!out)
      PERROR("write(build.ninja) failed");
    if (rename(temporary.c_str(), filename.c_str()) != 0)
      PERROR("rename(build.ninja) failed");
  }

  void GenerateShell() {
    const std::string env_filename = GetEnvScriptFilename();
    const std::string env_temporary = env_filename + ".tmp";
    FILE* fp = fopen(env_temporary.c_str(), "wb");
    if (fp == NULL)
      PERROR("fopen(env.sh) failed");

    fprintf(fp, "#!/bin/sh\n");
    fprintf(fp, "# Generated by kati %s\n", kGitVersion);
    fprintf(fp, "\n");

    std::unordered_set<std::string> emitted_exports;
    std::map<std::string, std::string> workspace_exports;
    emitted_exports.insert("MAKELEVEL");
    fprintf(fp, "export MAKELEVEL=%d\n", g_flags.make_level + 1);
    fprintf(fp, "export KATI_GRAPH_SETTINGS=%s\n",
            ShellQuote(g_flags.GraphOptimizationTransport()).c_str());
    for (const auto& [symbol, is_exported] : ev_->exports()) {
      if (symbol.str() == "MAKELEVEL" ||
          symbol.str() == "KATI_GRAPH_SETTINGS" ||
          symbol.str() == "KATI_JOBSERVER_RESERVED" ||
          symbol.str() == "KATI_RUNTIME_CONTEXT_FD" ||
          symbol.str() == "KATI_NINJA_LOAD_LIMIT" ||
          symbol.str() == "KATI_RECIPE_CHANGED" ||
          symbol.str() == "KATI_NINJA_GOALS" ||
          symbol.str() == "KATI_NINJA_DEFAULT_GOALS" ||
          symbol.str() == "KATI_NINJA_LOAD_STATE")
        continue;
      if (!IsShellIdentifier(symbol.str())) {
        continue;
      }

      emitted_exports.insert(symbol.str());

      if (is_exported) {
        std::string val = ev_->EvalVar(symbol);
        const std::string quoted_symbol = ShellQuote(symbol.str());
        const std::string quoted_val = ShellQuote(val);
        fprintf(fp, "export %s=%s\n", quoted_symbol.c_str(),
                quoted_val.c_str());
        if (symbol.str() != "MAKE" && symbol.str() != "MAKEFLAGS" &&
            symbol.str() != "MAKEOVERRIDES")
          workspace_exports[symbol.str()] =
              "export " + quoted_symbol + "=" + quoted_val + "; ";
      } else {
        const std::string quoted_symbol = ShellQuote(symbol.str());
        fprintf(fp, "unset %s\n", quoted_symbol.c_str());
        if (symbol.str() != "MAKE" && symbol.str() != "MAKEFLAGS" &&
            symbol.str() != "MAKEOVERRIDES")
          workspace_exports[symbol.str()] = "unset " + quoted_symbol + "; ";
      }
    }
    workspace_environment_.clear();
    for (const auto& [name, command] : workspace_exports)
      workspace_environment_ += command;
    if (workspace_environment_.find('\n') != std::string::npos)
      workspace_environment_ = EncodeShellScript(workspace_environment_) + "; ";

    // GNU make always propagates these special recursive-make variables.
    // They are not consistently represented as ordinary user exports in
    // Kati's symbol table: Kbuild's explicit `export MAKE`, for example, can
    // be lost while the bootstrap variable is still defined.  Omitting them
    // from a deferred Ninja recipe either makes quoted "$MAKE" fail or loses
    // command-line assignments (for example LLVM=1).  Keep MAKE separate from
    // its arguments: it must remain one exact executable pathname, while
    // MAKEFLAGS carries the recursive invocation state.
    for (const char* special : {"MAKE", "MAKEFLAGS", "MAKEOVERRIDES"}) {
      const Symbol symbol = Intern(special);
      Var* value = ev_->LookupVar(symbol);
      if (value == nullptr || !value->IsDefined())
        continue;

      // An ordinary export above already emitted the value.  MAKE is still
      // emitted here when the export table did not retain Kbuild's special
      // export declaration.
      if (emitted_exports.count(symbol.str()))
        continue;

      const std::string val = ev_->EvalVar(symbol);
      const std::string quoted_symbol = ShellQuote(symbol.str());
      const std::string quoted_val = ShellQuote(val);
      fprintf(fp, "export %s=%s\n", quoted_symbol.c_str(), quoted_val.c_str());
    }

    if (fclose(fp) != 0)
      PERROR("fclose(env.sh) failed");
    if (rename(env_temporary.c_str(), env_filename.c_str()) != 0)
      PERROR("rename(env.sh) failed");

    std::ifstream environment(env_filename, std::ios::binary);
    std::string env_bytes((std::istreambuf_iterator<char>(environment)), {});
    const std::string object_directory = GetFilename(".omktr-objects");
    if (!EnsureDirectory(object_directory))
      ERROR("cannot create environment object directory");
    const std::string snapshot = PublishGraphObject(
        object_directory + "/env-" + GraphDigest(env_bytes), env_bytes, true);
    AbsPath(snapshot, &env_snapshot_);
    MakefileCacheManager::Get().AddGeneratedGraphDep(env_snapshot_);

    const std::string launcher_filename = GetNinjaShellScriptFilename();
    const std::string launcher_temporary = launcher_filename + ".tmp";
    fp = fopen(launcher_temporary.c_str(), "wb");
    if (fp == NULL)
      PERROR("fopen(ninja.sh) failed");

    fprintf(fp, "#!/bin/sh\n");
    const std::string generation_directory =
        std::string(Dirname(launcher_filename));
    fprintf(fp,
            "if [ \"${OMKTR_PINNED_GENERATION:-}\" != %s ]; then\n"
            "  exec %s --internal-pin-generation %s %s \"$@\"\nfi\n"
            "unset OMKTR_PINNED_GENERATION\n",
            ShellQuote(generation_directory).c_str(),
            ShellQuote(GetExecutablePath()).c_str(),
            ShellQuote(generation_directory).c_str(),
            ShellQuote(std::string(Basename(launcher_filename))).c_str());

    fprintf(fp, "# Generated by kati %s\n", kGitVersion);
    fprintf(fp, "\n");

    const std::string env_reference =
        !env_filename.empty() && env_filename.front() == '/'
            ? env_filename
            : "./" + env_filename;
    fprintf(fp, ". %s\n", ShellQuote(env_reference).c_str());

    // Build-time parallelism belongs to Ninja, not graph generation.
    // Propagate the runtime -j value to recursive Kati commands without
    // requiring callers to know about Kati's internal scheduling metadata.
    // Match Ninja's normal host-parallel default for recursive Kati work.
    // An explicit runtime -j option still overrides this value below.
    fprintf(fp, "kati_jobs=\"${KATI_JOBS:-}\"\n");
    fprintf(fp, "kati_jobs_explicit=0\n");
    fprintf(fp, "if [ -n \"$kati_jobs\" ]; then kati_jobs_explicit=1; fi\n");
    fprintf(fp, "default_kati_jobs=%d\n",
            g_flags.num_cpus > 0 ? g_flags.num_cpus : 1);
    fprintf(fp, "ninja_jobs=\"\"\n");
    fprintf(fp, "prev=\"\"\n");
    fprintf(fp, "for arg in \"$@\"; do\n");
    fprintf(fp, "  case \"$arg\" in\n");
    fprintf(fp, "    --) break ;;\n");
    fprintf(fp, "    -j[0-9]*) ninja_jobs=\"${arg#-j}\"; prev=\"\" ;;\n");
    fprintf(fp, "    -j=*) ninja_jobs=\"${arg#-j=}\"; prev=\"\" ;;\n");
    fprintf(fp, "    -j) prev=\"-j\" ;;\n");
    fprintf(fp,
            "    [0-9]*) if [ \"$prev\" = \"-j\" ]; then ninja_jobs=\"$arg\"; "
            "fi; prev=\"\" ;;\n");
    fprintf(fp, "    *) prev=\"\" ;;\n");
    fprintf(fp, "  esac\n");
    fprintf(fp, "done\n");
    fprintf(fp, "if [ \"$kati_jobs_explicit\" -eq 0 ]; then\n");
    fprintf(fp, "  kati_jobs=\"${ninja_jobs:-$default_kati_jobs}\"\n");
    fprintf(fp, "fi\n");
    fprintf(
        fp,
        "if [ -n \"$kati_jobs\" ]; then export KATI_JOBS=\"$kati_jobs\"; fi\n");

    // Recursive Kati processes are invisible to Ninja's scheduler. Give them
    // one shared byte-token FIFO so nested Kati executors consume the same
    // runtime budget instead of multiplying it at every recursion level.
    fprintf(fp, "kati_fifo=\"\"\n");
    fprintf(fp, "if [ -z \"${KATI_JOBSERVER_FIFO:-}\" ]; then\n");
    fprintf(fp,
            "  case \"$kati_jobs\" in ''|0|*[!0-9]*) kati_jobs=1; export "
            "KATI_JOBS=1 ;; esac\n");
    fprintf(fp, "  kati_fifo_base=\"${TMPDIR:-/tmp}/kati-jobserver-$$\"\n");
    fprintf(fp, "  kati_fifo=\"$kati_fifo_base\"\n");
    fprintf(fp, "  kati_fifo_i=0\n");
    fprintf(fp,
            "  while ! kati_fifo_error=$(umask 077 && mkfifo \"$kati_fifo\" "
            "2>&1); do\n");
    fprintf(fp,
            "    if { [ ! -e \"$kati_fifo\" ] && [ ! -L \"$kati_fifo\" ]; } || "
            "[ \"$kati_fifo_i\" -ge 1023 ]; then\n"
            "      printf 'omktr: cannot create jobserver FIFO %%s: %%s\\n' "
            "\"$kati_fifo\" \"$kati_fifo_error\" >&2\n"
            "      exit 1\n"
            "    fi\n");
    fprintf(fp, "    kati_fifo_i=$((kati_fifo_i + 1))\n");
    fprintf(fp, "    kati_fifo=\"$kati_fifo_base-$kati_fifo_i\"\n");
    fprintf(fp, "  done\n");
    fprintf(fp, "  exec 9<>\"$kati_fifo\"\n");
    fprintf(fp, "  kati_i=0\n");
    fprintf(fp,
            "  while [ \"$kati_i\" -lt \"$kati_jobs\" ]; do printf x >&9; "
            "kati_i=$((kati_i + 1)); done\n");
    fprintf(fp, "  export KATI_JOBSERVER_FIFO=\"$kati_fifo\"\n");
    fprintf(fp,
            "  trap 'if [ -n \"${kati_fifo:-}\" ]; then rm -f \"$kati_fifo\"; "
            "fi' EXIT HUP INT TERM\n");
    fprintf(fp, "fi\n");

    // Keep Ninja as a child so the wrapper can cleanly terminate it before
    // removing the shared FIFO.  An exec-only wrapper cannot reap or signal
    // Ninja's recursive command tree when the wrapper receives an interrupt;
    // that can leave recursive Kati processes alive and corrupt the next
    // incremental attempt by racing the same output directory.
    fprintf(fp, "kati_ninja_pid=\"\"\n");
    fprintf(fp, "kati_cleanup() {\n");
    fprintf(fp, "  trap '' HUP INT TERM\n");
    fprintf(fp,
            "  if [ -n \"$kati_ninja_pid\" ]; then kill -TERM "
            "\"$kati_ninja_pid\" 2>/dev/null || :; fi\n");
    fprintf(fp,
            "  if [ -n \"$kati_ninja_pid\" ]; then wait "
            "\"$kati_ninja_pid\" 2>/dev/null || :; fi\n");
    fprintf(fp,
            "  if [ -n \"${kati_fifo:-}\" ]; then rm -f \"$kati_fifo\"; fi\n");
    fprintf(fp, "}\n");
    fprintf(fp, "trap 'kati_cleanup; exit 129' HUP INT TERM\n");
    // A reservation belongs to one parent recipe. Separate Ninja helper
    // processes cannot share the executor's atomic reservation owner, so
    // serialize this child Ninja while borrowing that single slot. Other
    // recursive executors may still use free FIFO tokens normally.
    std::string default_goals;
    auto append_goal = [&](Symbol goal) {
      default_goals += std::to_string(goal.str().size()) + ":" + goal.str();
    };
    if (g_flags.targets.empty() || g_flags.gen_all_targets) {
      if (default_target_)
        append_goal(default_target_->output);
    } else {
      for (Symbol goal : g_flags.targets)
        append_goal(goal);
    }
    if (!intermediate_proof_nodes_.empty())
      fprintf(fp, "export KATI_NINJA_DEFAULT_GOALS=%s\n",
              ShellQuote(default_goals).c_str());
    else
      fprintf(fp, "unset KATI_NINJA_DEFAULT_GOALS\n");
    // A caller may replace the public graph with a wrapper that declares
    // pools and includes the generated graph under another name. Preserve
    // that interface without following a concurrently switched public alias.
    const std::string public_graph =
        std::string(g_flags.ninja_dir ? g_flags.ninja_dir : ".") + "/build" +
        (g_flags.ninja_suffix ? g_flags.ninja_suffix : "") + ".ninja";
    fprintf(
        fp,
        "kati_graph=%s\nif [ ! -L %s ] && [ -f %s ]; then kati_graph=%s; fi\n",
        ShellQuote(GetNinjaFilename()).c_str(),
        ShellQuote(public_graph).c_str(), ShellQuote(public_graph).c_str(),
        ShellQuote(public_graph).c_str());
    fprintf(
        fp, "%s --internal-run-ninja %s %s -f \"$kati_graph\" ",
        ShellQuote(GetExecutablePath()).c_str(),
        ShellQuote(g_flags.load_limit < 0 ? std::string()
                                          : g_flags.LoadLimitOption().substr(2))
            .c_str(),
        ShellQuote(OMKTR_NINJA_EXECUTABLE).c_str());
    if (g_flags.remote_num_jobs > 0) {
      fprintf(fp, "-j%d ", g_flags.remote_num_jobs);
    }
    fprintf(fp, "\"$@\" &\n");
    fprintf(fp, "kati_ninja_pid=$!\n");
    fprintf(fp, "wait \"$kati_ninja_pid\"\n");
    fprintf(fp, "kati_status=$?\n");
    fprintf(fp, "kati_ninja_pid=\"\"\n");
    fprintf(fp, "trap - HUP INT TERM\n");
    fprintf(fp, "if [ -n \"$kati_fifo\" ]; then rm -f \"$kati_fifo\"; fi\n");
    fprintf(fp, "exit $kati_status\n");

    if (fclose(fp) != 0)
      PERROR("fclose(ninja.sh) failed");

    if (chmod(launcher_temporary.c_str(), 0755) != 0)
      PERROR("chmod ninja.sh failed");
    if (rename(launcher_temporary.c_str(), launcher_filename.c_str()) != 0)
      PERROR("rename(ninja.sh) failed");
  }

  void GenerateStamp(const std::string& orig_args) {
    std::unordered_set<std::string> makefiles;
    MakefileCacheManager::Get().GetAllFilenames(&makefiles);
    int evaluation = (MakefileCacheManager::Get().requires_evaluation() ||
                      HasInconsistentGlobObservations())
                         ? 1
                         : 0;
    // Required effects are correctness boundaries in every profile, including
    // off. Checking them speculatively and then evaluating would run them
    // twice.
    for (const CommandResult* command : GetShellCommandResults())
      if (command->op == CommandOp::SHELL || command->op == CommandOp::WRITE ||
          command->op == CommandOp::APPEND)
        evaluation = 1;
    if (!evaluation && MakefileCacheManager::Get().includes_certified()) {
      std::string proof;
      AppendGraphString(g_flags.GraphOptimizationIdentity(), &proof);
      AppendGraphString(orig_args, &proof);
      auto record = [&](const std::string& path) {
        AppendGraphString(path, &proof);
        AppendGraphString(GetFileIdentity(path), &proof);
      };
      record(GetNinjaFilename());
      record(GetEnvScriptFilename());
      for (const auto& path : makefiles)
        record(path);
      evaluation =
          WriteGraphCache(GetNinjaStampFilename() + ".includes", proof) ? 2 : 1;
    }
    std::string temporary = GetStampTempFilename() + ".XXXXXX";
    const int descriptor = mkstemp(temporary.data());
    if (descriptor < 0) {
      LOG_STAT("stamp cache publication failed: %s", strerror(errno));
      return;
    }
    FILE* fp = fdopen(descriptor, "wb+");
    if (!fp) {
      close(descriptor);
      unlink(temporary.c_str());
      LOG_STAT("stamp cache publication failed: cannot open stream");
      return;
    }

    size_t r = fwrite(&start_time_, sizeof(start_time_), 1, fp);
    CHECK(r == 1);
    CHECK(fwrite("KAT4", 1, 4, fp) == 4);
    DumpInt(fp, evaluation);
    // Generation artifacts certify acceptance, but are not Makefile inputs.
    // Keep their records separate so stamp inspection retains that contract.
    DumpInt(fp, 3);
    for (const std::string& artifact :
         {GetNinjaFilename(), GetNinjaShellScriptFilename(),
          GetEnvScriptFilename()}) {
      DumpString(fp, artifact);
      DumpString(fp, GetFileIdentity(artifact));
    }
    DumpInt(fp, makefiles.size() + 1);
    DumpString(fp, kati_binary_);
    DumpString(fp, GetFileIdentity(kati_binary_));
    for (const std::string& makefile : makefiles) {
      DumpString(fp, makefile);
      DumpString(fp, (MakefileCacheManager::Get().IsGeneratedGraphDep(makefile)
                          ? "generated:"
                          : "") +
                         GetFileIdentity(makefile));
    }

    DumpInt(fp, Evaluator::used_undefined_vars().size());
    for (Symbol v : Evaluator::used_undefined_vars()) {
      DumpString(fp, v.str());
    }
    DumpInt(fp, used_envs_.size());
    for (const auto& [key, value] : used_envs_) {
      DumpString(fp, key);
      DumpString(fp, value);
    }

    auto& globs = GetAllGlobCache();
    DumpInt(fp, globs.size());
    for (const auto& [key, files] : globs) {
      DumpString(fp, key);
#if 0
      std::unordered_set<std::string> dirs;
      GetReadDirs(p.first, files, &dirs);
      DumpInt(fp, dirs.size());
      for (const std::string& dir : dirs) {
        DumpString(fp, dir);
      }
#endif
      DumpInt(fp, files.size());
      for (const std::string& file : files) {
        DumpString(fp, file);
      }
    }

    const std::vector<CommandResult*>& crs = GetShellCommandResults();
    DumpInt(fp, crs.size());
    for (CommandResult* cr : crs) {
      DumpInt(fp, static_cast<int>(cr->op));
      DumpString(fp, cr->shell);
      DumpString(fp, cr->shellflag);
      DumpString(fp, cr->cmd);
      DumpString(fp, cr->result);
      DumpString(fp, cr->loc.filename ? cr->loc.filename : "");
      DumpInt(fp, cr->loc.lineno);

      if (cr->op == CommandOp::FIND) {
        std::vector<std::string> missing_dirs;
        for (std::string_view fd : cr->find->finddirs) {
          const std::string& d = ConcatDir(cr->find->chdir, fd);
          if (!Exists(d))
            missing_dirs.push_back(d);
        }
        DumpInt(fp, missing_dirs.size());
        for (const std::string& d : missing_dirs) {
          DumpString(fp, d);
        }

        DumpInt(fp, cr->find->found_files->size());
        for (std::string_view s : *cr->find->found_files) {
          DumpString(fp, ConcatDir(cr->find->chdir, s));
        }

        DumpInt(fp, cr->find->read_dirs->size());
        for (std::string_view s : *cr->find->read_dirs) {
          DumpString(fp, ConcatDir(cr->find->chdir, s));
        }
      }
    }

    DumpString(fp, orig_args);

    if (!SealGraphStamp(fp)) {
      fclose(fp);
      unlink(temporary.c_str());
      unlink(GetNinjaStampFilename().c_str());
      LOG_STAT("stamp cache not published: oversized or incomplete record");
      return;
    }
    if (fclose(fp) != 0)
      PERROR("fclose(kati_stamp) failed");
    if (rename(temporary.c_str(), GetNinjaStampFilename().c_str()) != 0) {
      LOG_STAT("stamp cache publication failed: %s", strerror(errno));
      unlink(temporary.c_str());
    }
  }

  CommandEvaluator ce_;
  Evaluator* ev_;
  SymbolSet done_;
  std::unordered_set<std::string> recipe_state_paths_;
  std::unordered_set<std::string> imported_outputs_;
  std::unordered_set<std::string> import_directories_;
  bool preparing_imports_ = false;
  std::unordered_map<Symbol, std::string> prepared_imports_;
  std::vector<std::string> import_inputs_;
  std::vector<std::string> import_artifacts_;
  std::string workspace_recipe_dir_;
  std::string workspace_environment_;
  std::string env_snapshot_;
  std::string generation_id_;
  std::unordered_set<std::string> active_nodes_;
  std::unordered_set<std::string> cycle_edges_;
  int rule_id_;
  std::unordered_map<std::string, std::string> rules_;
  const std::string shell_;
  const std::string shell_flags_;
  std::map<std::string, std::string> used_envs_;
  const std::string kati_binary_;
  std::string working_dir_;
  std::string path_prefix_;
  std::string path_variable_;
  mutable std::unordered_map<Symbol, std::string> escaped_targets_;
  std::unordered_map<
      size_t,
      std::vector<std::pair<const std::vector<NamedDepNode>*, std::string>>>
      dependency_fragments_;
  size_t fragment_bytes_ = 0;
  std::string uncached_fragment_;
  const double start_time_;
  std::unordered_map<Symbol, Symbol> source_dependency_groups_;
  std::unordered_map<const DepNode*, std::vector<const DepNode*>>
      intermediate_parents_;
  std::unordered_set<const DepNode*> intermediate_required_;
  std::unordered_map<const DepNode*, std::string> intermediate_proofs_;
  std::unordered_map<std::string, std::string> intermediate_root_proofs_;
  struct IntermediateClosure {
    std::vector<const DepNode*> roots;
    std::unordered_set<const DepNode*> nodes;
    std::string path;
  };
  std::vector<IntermediateClosure> intermediate_closures_;
  std::unordered_map<const DepNode*, std::string> intermediate_consumer_proofs_;
  std::unordered_map<const DepNode*, size_t> intermediate_consumer_sizes_;
  std::unordered_set<const DepNode*> intermediate_proof_nodes_;
  std::map<std::string, std::string> intermediate_recipe_states_;
  std::string intermediate_states_path_;
  std::vector<NinjaNode> nodes_;

  std::mutex mu_;
  const DepNode* default_target_;
};

std::string GetNinjaFilename() {
  return NinjaGenerator::GetFilename("build%s.ninja");
}

std::string GetNinjaShellScriptFilename() {
  return NinjaGenerator::GetFilename("ninja%s.sh");
}

std::string GetEnvScriptFilename() {
  return NinjaGenerator::GetFilename("env%s.sh");
}

std::string GetNinjaStampFilename() {
  return NinjaGenerator::GetFilename(".kati_stamp%s");
}

void GenerateNinja(const std::vector<NamedDepNode>& nodes,
                   Evaluator* ev,
                   const std::string& orig_args,
                   double start_time) {
  NinjaGenerator ng(ev, start_time);
  ng.Generate(nodes, orig_args);
}
