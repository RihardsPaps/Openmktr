// FORK MODIFICATION NOTICE (2026)
// Pack recipe booleans together to avoid padding every expanded command.
// Own invocation state and publish coherent generations with typed freshness.
// Capture actual direct bootstrap reads for later deferred recipe identities.
// Expose private recipe snapshot and runtime evaluation helpers.
// Prepare sealed, build-owned runtime contexts for inherited recipe helpers.
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

#ifndef COMMAND_H_
#define COMMAND_H_

#include <memory>
#include <vector>

#include "fileutil.h"
#include "freshness.h"
#include "symtab.h"

struct DepNode;
class Evaluator;
struct Command;

class RuntimeRecipeReadCapture {
 public:
  RuntimeRecipeReadCapture(const Command& command, Evaluator* evaluator);
  ~RuntimeRecipeReadCapture();
  void Finish(const std::string& owner);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

bool MakeRuntimeRecipe(const DepNode& node,
                       Evaluator* evaluator,
                       const std::string& directory,
                       std::vector<Command>* commands);
int RunRuntimeRecipe(const char* path);
std::string RuntimeContextManifestPath(const std::string& graph);
void PrepareRuntimeContexts(const std::string& manifest);
void CommitRuntimeRecipeReads(const std::string& state_path,
                              const std::string& state);

struct Command {
  explicit Command(Symbol o)
      : output(o), echo(true), ignore_error(false), force_no_subshell(false) {}
  Symbol output;
  std::string cmd;
  std::string display_cmd;
  std::string runtime_sidecar;
  std::string runtime_context;
  std::string runtime_identity;
  std::string shell;
  std::string shellflag;
  std::vector<std::string> environment;
  std::vector<std::string> unset_environment;
  bool echo;
  bool verbose = false;
  bool force_run = false;
  bool ignore_error;
  bool force_no_subshell;
  bool runtime_freshness_only = false;
  size_t double_colon_group = static_cast<size_t>(-1);
};

class CommandEvaluator {
 public:
  explicit CommandEvaluator(Evaluator* ev);
  ~CommandEvaluator();
  std::vector<Command> Eval(const DepNode& n);
  std::vector<Command> Eval(const DepNode& n, FileStamp target_age);
  std::vector<Command> Eval(const DepNode& n,
                            FileStamp target_age,
                            size_t double_colon_group);
  const DepNode* current_dep_node() const { return current_dep_node_; }
  FileStamp target_age() const { return target_age_; }
  Evaluator* evaluator() const { return ev_; }
  bool found_new_inputs() const { return found_new_inputs_; }
  void set_found_new_inputs(bool val) { found_new_inputs_ = val; }

 private:
  Evaluator* ev_;
  const DepNode* current_dep_node_;
  FileStamp target_age_;
  bool found_new_inputs_;
  FileObservationCache observations_;
  uint64_t filesystem_epoch_ = static_cast<uint64_t>(-1);
};

#endif  // COMMAND_H_
