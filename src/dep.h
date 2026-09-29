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
