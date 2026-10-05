// FORK MODIFICATION NOTICE (2026)
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

#ifndef DEP_H_
#define DEP_H_

#include <atomic>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "loc.h"
#include "symtab.h"

class Evaluator;
class Rule;
class Value;
class Var;
class Vars;
class Frame;

typedef std::pair<Symbol, struct DepNode*> NamedDepNode;

struct DepNode {
  DepNode(Symbol output, bool is_phony, bool is_restat);
  std::string DebugString();

  Symbol output;
  // The spelling used by make while expanding automatic variables and
  // recursive recipes.  |output| is the canonical spelling used by Ninja
  // edges and filesystem checks; these are distinct when a rule names a
  // target through a parent-relative alias such as dir/../child.
  Symbol lexical_output;
  // Existing source-tree copy of a rule-bearing logical target. The rule and
  // its prerequisites remain in the graph until execution decides freshness.
  Symbol vpath_provider;
  // Direct execution sets this when a rule discards a previously usable
  // VPATH copy. Automatic variables must then retain the logical filename.
  mutable std::atomic<bool> vpath_discarded{false};
  std::vector<Value*> cmds;
  std::vector<NamedDepNode> deps;
  std::vector<NamedDepNode> order_onlys;
  std::vector<NamedDepNode> validations;
  bool has_rule;
  bool is_default_target;
  bool is_notparallel;
  bool is_phony;
  bool is_restat;
  bool delete_on_error;
  bool precious;
  bool intermediate;
  bool oneshell;
  bool ignore_errors;
  bool silent;
  bool export_all_variables;
  std::vector<Symbol> implicit_outputs;
  // Prerequisites contributed by the selected implicit rule. These are
  // distinct from explicit prerequisites merged onto the target and are
  // needed to identify files introduced by an implicit-rule chain.
  std::vector<Symbol> implicit_inputs;
  std::vector<Symbol> actual_inputs;
  std::vector<Symbol> actual_order_only_inputs;
  // Double-colon rules retain independent prerequisite freshness and recipe
  // groups. Their recipes must not be merged into one always-run sequence.
  std::vector<std::vector<Symbol>> double_colon_group_inputs;
  std::vector<bool> double_colon_group_has_prerequisites;
  std::vector<size_t> double_colon_group_for_cmd;
  std::vector<Symbol> low_resolution_inputs;
  std::vector<std::vector<Symbol>> wait_groups;
  std::vector<Symbol> actual_validations;
  Vars* rule_vars;
  Var* depfile_var;
  Var* ninja_pool_var;
  Var* tags_var;
  Symbol output_pattern;
  Loc loc;
};

void MakeDep(Evaluator* ev,
             const std::vector<const Rule*>& rules,
             const std::unordered_map<Symbol, Vars*>& rule_vars,
             const std::vector<Symbol>& targets,
             std::vector<NamedDepNode>* nodes,
             bool add_parent_directory_edges = true);

bool IsSpecialTarget(Symbol output);

#endif  // DEP_H_
