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

#include "dep.h"

#include <sys/stat.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "eval.h"
#include "expr.h"
#include "fileutil.h"
#include "flags.h"
#include "log.h"
#include "rule.h"
#include "stats.h"
#include "strutil.h"
#include "symtab.h"
#include "timeutil.h"
#include "var.h"

bool IsSuffixRule(Symbol output);

namespace {

// A target-specific += suffix starts with a separator so it can be joined to
// an inherited value. When that value expands to empty, GNU make omits the
// separator. Keep the inherited expression live until the consuming recipe.
class TargetAppendValue : public Value {
 public:
  TargetAppendValue(const Loc& loc, const Evaluable* base, Value* suffix)
      : Value(loc), base_(base), suffix_(suffix) {}

  void Eval(Evaluator* ev, std::string* out) const override {
    std::string prefix;
    if (base_)
      base_->Eval(ev, &prefix);
    std::string suffix;
    suffix_->Eval(ev, &suffix);
    if (prefix.empty() && !suffix.empty() && suffix.front() == ' ')
      suffix.erase(suffix.begin());
    *out += prefix;
    *out += suffix;
  }

  bool IsFunc(Evaluator* ev) const override {
    return (base_ && base_->IsFunc(ev)) || suffix_->IsFunc(ev);
  }

 protected:
  std::string DebugString_() const override { return "target append"; }

 private:
  const Evaluable* base_;
  Value* suffix_;
};

static constexpr const char* kDefaultSuffixes[] = {
    "out",  "a",      "ln",  "o",   "c",   "cc",   "C",   "cpp", "p",
    "f",    "F",      "m",   "r",   "y",   "l",    "ym",  "lm",  "s",
    "S",    "mod",    "sym", "def", "h",   "info", "dvi", "tex", "texinfo",
    "texi", "txinfo", "w",   "ch",  "web", "sh",   "elc", "el"};

static std::vector<std::unique_ptr<DepNode>> g_dep_node_pool;

static Symbol ReplaceSuffix(Symbol s, Symbol newsuf) {
  std::string r{StripExt(s.str())};
  r += '.';
  r += newsuf.str();
  return Intern(r);
}

// An implicit target pattern without a slash is matched against the file-name
// component. GNU make puts the target's directory in front of prerequisites
// whose stems were substituted by the rule.
static std::string_view ImplicitMatchTarget(Symbol output, Symbol pattern) {
  if (pattern.str().find('/') == std::string_view::npos)
    return Basename(output.str());
  return output.str();
}

static std::string SubstituteImplicitPrerequisite(Symbol output,
                                                  Symbol pattern,
                                                  std::string_view input) {
  Pattern pat(pattern.str());
  std::string result;
  std::string_view match_target = ImplicitMatchTarget(output, pattern);
  pat.AppendSubst(match_target, input, &result);
  if (input.find('%') != std::string_view::npos &&
      pattern.str().find('/') == std::string_view::npos) {
    std::string_view dir = Dirname(output.str());
    // An absolute prerequisite already names its location.  Prefixing the
    // target directory to it would turn, for example, /build/libc.so into
    // /build/subdir/build/libc.so.
    if (dir != "." && !dir.empty() && (result.empty() || result.front() != '/'))
      result = std::string(dir) + "/" + result;
  }
  return result;
}

// A wildcard in an implicit prerequisite is expanded after the stem has
// been substituted.  At parse time a name such as zstd/*/%.c cannot match
// anything, but zstd/*/debug.c can.
static std::vector<Symbol> ExpandImplicitPrerequisite(
    std::string_view substituted) {
  std::vector<Symbol> result;
  if (substituted.find_first_of("*?[") != std::string_view::npos) {
    for (const std::string& match : Glob(substituted))
      result.push_back(Intern(match));
  }
  if (result.empty())
    result.push_back(Intern(substituted));
  return result;
}

static bool IsDirectoryTarget(Symbol output) {
  return !output.str().empty() && output.str().back() == '/';
}

void ApplyOutputPattern(const Rule& r,
                        Symbol output,
                        const std::vector<Symbol>& inputs,
                        std::vector<Symbol>* out_inputs) {
  if (inputs.empty())
    return;
  if (r.is_suffix_rule) {
    for (Symbol input : inputs) {
      out_inputs->push_back(ReplaceSuffix(output, input));
    }
    return;
  }
  if (r.output_patterns.empty()) {
    copy(inputs.begin(), inputs.end(), back_inserter(*out_inputs));
    return;
  }
  CHECK(r.output_patterns.size() == 1);
  Symbol output_pattern = r.output_patterns[0];
  Pattern pat(output_pattern.str());
  std::string_view match_target = ImplicitMatchTarget(output, output_pattern);
  for (Symbol input : inputs) {
    std::string buf;
    pat.AppendSubst(match_target, input.str(), &buf);
    if (input.str().find('%') != std::string_view::npos &&
        output_pattern.str().find('/') == std::string_view::npos) {
      std::string_view dir = Dirname(output.str());
      if (dir != "." && !dir.empty() && (buf.empty() || buf.front() != '/'))
        buf = std::string(dir) + "/" + buf;
    }
    // Keep the lexical spelling through rule lookup and command expansion.
    // GNU make permits a rule to publish a target as `dir/../child`, and a
    // recursive rule may be registered under that exact spelling.  The graph
    // edge is canonicalized later, after the provider has been selected.
    // `.` is a real make prerequisite, while an empty prerequisite is not.
    if (buf.empty())
      buf = ".";
    std::vector<Symbol> expanded = ExpandImplicitPrerequisite(buf);
    out_inputs->insert(out_inputs->end(), expanded.begin(), expanded.end());
  }
}

class RuleTrie {
  struct Entry {
    Entry(const Rule* r, std::string_view s) : rule(r), suffix(s) {}
    const Rule* rule;
    std::string_view suffix;
  };

 public:
  RuleTrie() {}
  ~RuleTrie() {
    for (auto& p : children_)
      delete p.second;
  }

  void Add(std::string_view name, const Rule* rule, bool replace = true) {
    if (name.empty() || name[0] == '%') {
      // GNU make keeps distinct implicit rules in definition order, but a
      // later rule with the same target and prerequisites replaces the
      // earlier recipe.  Generated sysdep makefiles rely on the order: their
      // architecture-specific source rules precede the generic fallback.
      for (Entry& entry : rules_) {
        const Rule* previous = entry.rule;
        if (entry.suffix == name &&
            previous->output_patterns == rule->output_patterns &&
            previous->inputs == rule->inputs &&
            previous->order_only_inputs == rule->order_only_inputs &&
            previous->wait_groups == rule->wait_groups &&
            previous->secondary_expansion == rule->secondary_expansion &&
            previous->secondary_prerequisites ==
                rule->secondary_prerequisites) {
          if (replace)
            entry.rule = rule;
          return;
        }
      }
      rules_.push_back(Entry(rule, name));
      return;
    }
    const char c = name[0];
    auto p = children_.emplace(c, nullptr);
    if (p.second) {
      p.first->second = new RuleTrie();
    }
    p.first->second->Add(name.substr(1), rule, replace);
  }

  void Get(std::string_view name, std::vector<const Rule*>* rules) const {
    for (const Entry& ent : rules_) {
      if ((ent.suffix.empty() && name.empty()) ||
          HasSuffix(name, ent.suffix.substr(1))) {
        rules->push_back(ent.rule);
      }
    }
    if (name.empty())
      return;
    auto found = children_.find(name[0]);
    if (found != children_.end()) {
      found->second->Get(name.substr(1), rules);
    }
  }

  size_t size() const {
    size_t r = rules_.size();
    for (const auto& c : children_)
      r += c.second->size();
    return r;
  }

 private:
  std::vector<Entry> rules_;
  std::unordered_map<char, RuleTrie*> children_;
};

static std::vector<const Rule*> GetImplicitRuleCandidates(const RuleTrie* rules,
                                                          Symbol output) {
  std::vector<const Rule*> candidates;
  rules->Get(output.str(), &candidates);
  std::string_view basename = Basename(output.str());
  if (basename != output.str())
    rules->Get(basename, &candidates);

  std::unordered_set<const Rule*> seen;
  candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                  [&seen](const Rule* rule) {
                                    return !seen.insert(rule).second;
                                  }),
                   candidates.end());
  return candidates;
}

static bool HasRuleRecipe(const Rule* rule) {
  // GNU make distinguishes a rule with no recipe from an explicit empty
  // recipe (for example, "target: ;"). The latter still counts as a recipe
  // for pattern-rule selection and must not cancel a competing implicit rule.
  return !rule->cmds.empty();
}

static bool IsImplicitRuleCancellation(Symbol output,
                                       const std::vector<const Rule*>& rules) {
  for (const Rule* rule : rules) {
    const bool has_recipe = HasRuleRecipe(rule);
    if (has_recipe || !rule->inputs.empty() || !rule->order_only_inputs.empty())
      continue;

    for (Symbol output_pattern : rule->output_patterns) {
      if (IsDirectoryTarget(output) &&
          (output_pattern.str().empty() || output_pattern.str().back() != '/'))
        continue;
      Pattern pat(output_pattern.str());
      if (pat.Match(ImplicitMatchTarget(output, output_pattern)))
        return true;
    }
  }
  return false;
}

struct RuleMerger {
  std::vector<const Rule*> rules;
  std::vector<std::pair<Symbol, RuleMerger*>> implicit_outputs;
  std::vector<Symbol> validations;
  const Rule* primary_rule;
  const RuleMerger* parent;
  Symbol parent_sym;
  bool is_double_colon;

  RuleMerger()
      : primary_rule(nullptr), parent(nullptr), is_double_colon(false) {}

  void AddImplicitOutput(Symbol output, RuleMerger* merger) {
    implicit_outputs.push_back(std::make_pair(output, merger));
  }

  void AddValidation(Symbol validation) { validations.push_back(validation); }

  void SetImplicitOutput(Symbol output, Symbol p, const RuleMerger* merger) {
    if (!merger->primary_rule) {
      ERROR("*** implicit output `%s' on phony target `%s'", output.c_str(),
            p.c_str());
    }
    if (parent) {
      ERROR_LOC(merger->primary_rule->cmd_loc(),
                "*** implicit output `%s' of `%s' was already defined by `%s' "
                "at %s:%d",
                output.c_str(), p.c_str(), parent_sym.c_str(),
                LOCF(parent->primary_rule->cmd_loc()));
    }
    if (primary_rule) {
      ERROR_LOC(primary_rule->cmd_loc(),
                "*** implicit output `%s' may not have commands",
                output.c_str());
    }
    parent = merger;
    parent_sym = p;
  }

  void AddRule(Symbol output, const Rule* r) {
    if (rules.empty()) {
      is_double_colon = r->is_double_colon;
    } else if (is_double_colon != r->is_double_colon) {
      ERROR_LOC(r->loc, "*** target file `%s' has both : and :: entries.",
                output.c_str());
    }

    if (primary_rule && !r->cmds.empty() && !IsSuffixRule(output) &&
        !r->is_double_colon) {
      if (g_flags.werror_overriding_commands) {
        ERROR_LOC(r->cmd_loc(),
                  "*** overriding commands for target `%s', previously defined "
                  "at %s:%d",
                  output.c_str(), LOCF(primary_rule->cmd_loc()));
      } else {
        WARN_LOC(r->cmd_loc(), "warning: overriding commands for target `%s'",
                 output.c_str());
        WARN_LOC(primary_rule->cmd_loc(),
                 "warning: ignoring old commands for target `%s'",
                 output.c_str());
      }
      primary_rule = r;
    }
    if (!primary_rule && !r->cmds.empty()) {
      primary_rule = r;
    }

    rules.push_back(r);
  }

  void FillDepNodeFromRule(Symbol output,
                           const Rule* r,
                           DepNode* n,
                           bool implicit_rule = false) const {
    if (is_double_colon)
      copy(r->cmds.begin(), r->cmds.end(), back_inserter(n->cmds));

    if (!r->secondary_expansion) {
      ApplyOutputPattern(*r, output, r->inputs, &n->actual_inputs);
      ApplyOutputPattern(*r, output, r->order_only_inputs,
                         &n->actual_order_only_inputs);
      if (implicit_rule) {
        std::vector<Symbol> chain_inputs;
        auto collect_chain_inputs = [&](const std::vector<Symbol>& inputs) {
          for (Symbol input : inputs) {
            // A literal prerequisite is explicitly attached to the implicit
            // rule; it is not a file introduced while searching an implicit
            // rule chain. Suffix rules encode their substitution without a
            // '%' token, so all their prerequisites participate.
            if (r->is_suffix_rule || input.str().find('%') != std::string::npos)
              chain_inputs.push_back(input);
          }
        };
        collect_chain_inputs(r->inputs);
        collect_chain_inputs(r->order_only_inputs);
        ApplyOutputPattern(*r, output, chain_inputs, &n->implicit_inputs);
      }
    }

    if (r->output_patterns.size() >= 1) {
      CHECK(r->output_patterns.size() == 1);
      n->output_pattern = r->output_patterns[0];
    }
    if (r->has_wait) {
      n->wait_groups.clear();
      for (const std::vector<Symbol>& group : r->wait_groups) {
        std::vector<Symbol> expanded_group;
        ApplyOutputPattern(*r, output, group, &expanded_group);
        n->wait_groups.push_back(std::move(expanded_group));
      }
    }
  }

  void FillDepNodeLoc(const Rule* r, DepNode* n) const {
    n->loc = r->loc;
    if (!r->cmds.empty() && r->cmd_lineno)
      n->loc.lineno = r->cmd_lineno;
  }

  void FillDepNode(Symbol output,
                   const Rule* pattern_rule,
                   DepNode* n,
                   bool include_implicit_outputs = true) const {
    if (primary_rule) {
      FillDepNodeFromRule(output, primary_rule, n);

      if (pattern_rule && pattern_rule->output_patterns.size() == 1) {
        n->output_pattern = pattern_rule->output_patterns[0];
      }

      FillDepNodeLoc(primary_rule, n);
      n->cmds = primary_rule->cmds;
    } else if (pattern_rule) {
      FillDepNodeFromRule(output, pattern_rule, n, true);
      FillDepNodeLoc(pattern_rule, n);
      n->cmds = pattern_rule->cmds;
    }

    for (const Rule* r : rules) {
      if (r == primary_rule)
        continue;
      FillDepNodeFromRule(output, r, n);
      if (n->loc.filename == NULL)
        n->loc = r->loc;
    }

    if (is_double_colon) {
      n->cmds.clear();
      auto add_group = [&](const Rule* rule) {
        if (!rule)
          return;
        const size_t group = n->double_colon_group_inputs.size();
        n->double_colon_group_inputs.emplace_back();
        std::vector<Symbol> first_inputs;
        auto has_first_pass_input = [&](const std::vector<Symbol>& inputs) {
          bool any = false;
          for (Symbol input : inputs) {
            if (!rule->secondary_expansion ||
                input.str().find('$') == std::string_view::npos) {
              any = true;
              if (&inputs == &rule->inputs)
                first_inputs.push_back(input);
            }
          }
          return any;
        };
        const bool normal = has_first_pass_input(rule->inputs);
        const bool order_only = has_first_pass_input(rule->order_only_inputs);
        n->double_colon_group_has_prerequisites.push_back(normal || order_only);
        ApplyOutputPattern(*rule, output, first_inputs,
                           &n->double_colon_group_inputs.back());
        const Rule* recipe = rule;
        if (rule->cmds.empty() && pattern_rule) {
          recipe = pattern_rule;
          ApplyOutputPattern(*pattern_rule, output, pattern_rule->inputs,
                             &n->double_colon_group_inputs.back());
          if (!pattern_rule->inputs.empty() ||
              !pattern_rule->order_only_inputs.empty())
            n->double_colon_group_has_prerequisites.back() = true;
        }
        n->cmds.insert(n->cmds.end(), recipe->cmds.begin(), recipe->cmds.end());
        n->double_colon_group_for_cmd.insert(
            n->double_colon_group_for_cmd.end(), recipe->cmds.size(), group);
      };
      for (const Rule* rule : rules)
        add_group(rule);
    }

    SymbolSet all_outputs = SymbolSet();
    all_outputs.insert(output);

    if (include_implicit_outputs) {
      for (auto& implicit_output : implicit_outputs) {
        n->implicit_outputs.push_back(implicit_output.first);
        all_outputs.insert(implicit_output.first);
      }
    }

    for (auto& validation : validations) {
      n->actual_validations.push_back(validation);
    }
  }

  void FillSecondaryInputs(Symbol output,
                           const Rule* pattern_rule,
                           DepNode* n,
                           Evaluator* ev) const {
    if (primary_rule)
      FillSecondaryInputsForRule(output, primary_rule, n, ev);
    if (pattern_rule)
      FillSecondaryInputsForRule(output, pattern_rule, n, ev, true);
    for (const Rule* r : rules) {
      if (r != primary_rule)
        FillSecondaryInputsForRule(output, r, n, ev);
    }
  }

  void FillOneSecondaryInput(Symbol output,
                             const Rule* r,
                             DepNode* n,
                             Evaluator* ev,
                             bool implicit_rule = false) const {
    FillSecondaryInputsForRule(output, r, n, ev, implicit_rule);
  }

  void FillSecondaryFirstInputs(Symbol output,
                                const Rule* pattern_rule,
                                DepNode* n) const {
    auto add_first_pass = [output, n](const Rule* r) {
      if (!r || !r->secondary_expansion)
        return;
      std::vector<Symbol> first_inputs;
      std::vector<Symbol> first_order_only;
      for (Symbol input : r->inputs) {
        // Escaped references survive the first pass as literal '$' text and
        // must not be visible to automatic variables until the second pass.
        if (input.str().find('$') == std::string::npos)
          first_inputs.push_back(input);
      }
      for (Symbol input : r->order_only_inputs) {
        if (input.str().find('$') == std::string::npos)
          first_order_only.push_back(input);
      }
      ApplyOutputPattern(*r, output, first_inputs, &n->actual_inputs);
      ApplyOutputPattern(*r, output, first_order_only,
                         &n->actual_order_only_inputs);
    };
    if (primary_rule)
      add_first_pass(primary_rule);
    add_first_pass(pattern_rule);
    for (const Rule* r : rules) {
      if (r != primary_rule)
        add_first_pass(r);
    }
  }

 private:
  void FillSecondaryInputsForRule(Symbol output,
                                  const Rule* r,
                                  DepNode* n,
                                  Evaluator* ev,
                                  bool implicit_rule = false) const {
    if (!r || !r->secondary_expansion)
      return;
    std::vector<Symbol> inputs;
    std::vector<Symbol> order_only_inputs;
    std::vector<std::vector<Symbol>> wait_groups;
    r->ParseSecondaryInputs(ev, &inputs, &order_only_inputs, &wait_groups);
    ApplyOutputPattern(*r, output, inputs, &n->actual_inputs);
    ApplyOutputPattern(*r, output, order_only_inputs,
                       &n->actual_order_only_inputs);
    if (implicit_rule) {
      std::vector<Symbol> chain_inputs;
      auto collect_chain_inputs = [&](const std::vector<Symbol>& prereqs) {
        for (Symbol input : prereqs) {
          if (r->is_suffix_rule || input.str().find('%') != std::string::npos)
            chain_inputs.push_back(input);
        }
      };
      collect_chain_inputs(inputs);
      collect_chain_inputs(order_only_inputs);
      ApplyOutputPattern(*r, output, chain_inputs, &n->implicit_inputs);
    }
    if (!wait_groups.empty()) {
      n->wait_groups.clear();
      for (const std::vector<Symbol>& group : wait_groups) {
        std::vector<Symbol> expanded_group;
        ApplyOutputPattern(*r, output, group, &expanded_group);
        n->wait_groups.push_back(std::move(expanded_group));
      }
    }
  }

 public:
};

}  // namespace

bool IsSuffixRule(Symbol output) {
  // GNU make suffix rules begin with a dot (for example, .c.o).  A
  // normal target such as lib.a is not a suffix rule merely because it
  // contains a dot.
  if (output.empty() || output.str()[0] != '.')
    return false;
  const std::string_view rest = std::string_view(output.str()).substr(1);
  size_t dot_index = rest.find('.');
  // A declared source suffix may itself contain a dot, as in .test.bin.log.
  // The active suffix list decides which dot separates the two suffixes.
  return dot_index != std::string_view::npos;
}

DepNode::DepNode(Symbol o, bool p, bool r)
    : output(o),
      lexical_output(o),
      has_rule(false),
      is_default_target(false),
      is_notparallel(false),
      is_phony(p),
      is_restat(r),
      delete_on_error(false),
      precious(false),
      intermediate(false),
      oneshell(false),
      ignore_errors(false),
      silent(false),
      export_all_variables(false),
      rule_vars(NULL),
      depfile_var(NULL),
      ninja_pool_var(NULL),
      tags_var(NULL) {}

class DepBuilder {
 public:
  DepBuilder(Evaluator* ev,
             const std::vector<const Rule*>& rules,
             const std::unordered_map<Symbol, Vars*>& rule_vars,
             bool add_parent_directory_edges)
      : ev_(ev),
        rule_vars_(rule_vars),
        add_parent_directory_edges_(add_parent_directory_edges),
        implicit_rules_(new RuleTrie()),
        depfile_var_name_(Intern(".KATI_DEPFILE")),
        implicit_outputs_var_name_(Intern(".KATI_IMPLICIT_OUTPUTS")),
        ninja_pool_var_name_(Intern(".KATI_NINJA_POOL")),
        validations_var_name_(Intern(".KATI_VALIDATIONS")),
        tags_var_name_(Intern(".KATI_TAGS")) {
    ScopedTimeReporter tr("make dep (populate)");
    ReadSuffixList(rules);
    PopulateRules(rules);
    // TODO?
    // LOG_STAT("%zu variables", ev->mutable_vars()->size());
    LOG_STAT("%zu explicit rules", rules_.size());
    LOG_STAT("%zu implicit rules", implicit_rules_->size());
    LOG_STAT("%zu suffix rules", suffix_rules_.size());

    HandleSpecialTargets();
  }

  void ReadSuffixList(const std::vector<const Rule*>& rules) {
    for (const Rule* rule : rules) {
      bool is_suffix_list = false;
      for (Symbol output : rule->outputs) {
        if (output == Intern(".SUFFIXES")) {
          is_suffix_list = true;
          break;
        }
      }
      if (!is_suffix_list)
        continue;

      // GNU make treats an empty .SUFFIXES rule as a reset. Nonempty rules
      // add suffixes to the active list, preserving the order in which the
      // makefile declares them.
      if (!suffixes_specified_ && !rule->inputs.empty() &&
          !g_flags.no_builtin_rules) {
        for (const char* suffix : kDefaultSuffixes) {
          active_suffixes_.insert(suffix);
          dotted_suffixes_.insert(suffix);
          active_suffix_order_.emplace_back(suffix);
        }
      }
      if (rule->inputs.empty()) {
        active_suffixes_.clear();
        active_suffix_order_.clear();
        dotted_suffixes_.clear();
        dotless_suffixes_.clear();
      } else {
        for (Symbol suffix : rule->inputs) {
          std::string name = suffix.str();
          if (!name.empty() && name.front() == '.') {
            name.erase(name.begin());
            dotted_suffixes_.insert(name);
          } else {
            dotless_suffixes_.insert(name);
          }
          if (active_suffixes_.insert(name).second)
            active_suffix_order_.push_back(name);
        }
      }
      suffixes_specified_ = true;
    }
  }

  size_t FindSuffixRuleBoundary(Symbol output) const {
    if (!IsSuffixRule(output))
      return std::string_view::npos;

    const std::string_view rest = std::string_view(output.str()).substr(1);
    // These are GNU make's standard suffixes.  A dotted hidden file such as
    // .kconfig.d is an ordinary target unless its suffixes are active; it
    // must not be discarded as a suffix rule merely because it contains two
    // dots.
    auto is_default_suffix = [&](const std::string& suffix) {
      for (const char* candidate : kDefaultSuffixes)
        if (suffix == candidate)
          return true;
      return false;
    };
    for (size_t dot = rest.find('.'); dot != std::string_view::npos;
         dot = rest.find('.', dot + 1)) {
      const std::string input_suffix(rest.substr(0, dot));
      const std::string output_suffix(rest.substr(dot + 1));
      const bool input_active = suffixes_specified_
                                    ? active_suffixes_.count(input_suffix) != 0
                                    : is_default_suffix(input_suffix);
      const bool output_active =
          suffixes_specified_ ? active_suffixes_.count(output_suffix) != 0
                              : is_default_suffix(output_suffix);
      if (input_active && output_active)
        return dot;
    }
    return std::string_view::npos;
  }

  bool IsActiveSuffixRule(Symbol output) const {
    const std::string_view name = output.str();
    // A single suffix rule, for example .pre:, maps a dotless target to
    // the same name with .pre appended.  Automake's X.Org locale files use
    // this form with source files located through VPATH.
    if (name.size() > 1 && name.front() == '.' &&
        name.find('.', 1) == std::string_view::npos) {
      const std::string suffix(name.substr(1));
      if (suffixes_specified_) {
        if (active_suffixes_.count(suffix) != 0)
          return true;
      } else if (!g_flags.no_builtin_rules) {
        for (const char* candidate : kDefaultSuffixes)
          if (suffix == candidate)
            return true;
      }
    }
    if (FindSuffixRuleBoundary(output) != std::string_view::npos)
      return true;
    return FindDotlessSuffixRuleBoundary(output).first !=
           std::string_view::npos;
  }

  std::pair<size_t, bool> FindDotlessSuffixRuleBoundary(Symbol output) const {
    const std::string_view name = output.str();
    // A dotted source suffix may lead to a dotless destination suffix.
    // Info-ZIP uses .c_.o to make zipfile_.o from zipfile.c.
    for (const std::string& input : dotted_suffixes_) {
      const std::string source = "." + input;
      if (HasPrefix(name, source) &&
          dotless_suffixes_.count(std::string(name.substr(source.size()))) != 0)
        return {source.size(), false};
    }
    for (const std::string& input : dotless_suffixes_) {
      if (input.empty() || !HasPrefix(name, input))
        continue;
      const std::string_view remainder = name.substr(input.size());
      if (dotless_suffixes_.count(std::string(remainder)) != 0)
        return {input.size(), false};
      if (!remainder.empty() && remainder.front() == '.' &&
          dotted_suffixes_.count(std::string(remainder.substr(1))) != 0)
        return {input.size(), true};
    }
    return {std::string_view::npos, false};
  }

  void HandleSpecialTargets() {
    Loc loc;
    std::vector<Symbol> targets;

    auto normalize_special_target = [](Symbol target) {
      const std::string_view trimmed = TrimLeadingCurdir(target.str());
      const bool current_directory = IsCurrentDirectoryPath(target.str());
      std::string normalized(trimmed);
      NormalizeMakePath(&normalized);
      // NormalizePath represents the current directory as an empty string,
      // but "." is a real make target and must remain addressable.
      if (normalized.empty() && current_directory)
        normalized = ".";
      return Intern(normalized);
    };

    if (GetRuleInputs(Intern(".PHONY"), &targets, &loc)) {
      for (Symbol t : targets)
        phony_.insert(normalize_special_target(t));
    }
    if (GetRuleInputs(Intern(".KATI_RESTAT"), &targets, &loc)) {
      for (Symbol t : targets)
        restat_.insert(normalize_special_target(t));
    }
    if (GetRuleInputs(Intern(".SUFFIXES"), &targets, &loc)) {
      // The ordered rule scan in ReadSuffixList already applied these
      // declarations before suffix rules were populated.
    }

    // With no prerequisites GNU make makes the entire invocation serial.
    // Preserve that property in direct Kati execution and emitted Ninja
    // graphs so recursive makefiles cannot race on shared outputs.
    if (GetRuleInputs(Intern(".NOTPARALLEL"), &targets, &loc)) {
      if (targets.empty()) {
        notparallel_ = true;
      } else {
        for (Symbol target : targets)
          notparallel_targets_.insert(normalize_special_target(target));
      }
    }

    // Carry GNU make's delete-on-error policy to each generated recipe.  The
    // Ninja wrapper can then remove only the declared output after a genuine
    // recipe failure; no target-specific or project-specific rule is needed.
    delete_on_error_ =
        GetRuleInputs(Intern(".DELETE_ON_ERROR"), &targets, &loc);
    if (GetRuleInputs(Intern(".PRECIOUS"), &targets, &loc)) {
      for (Symbol target : targets)
        precious_.insert(normalize_special_target(target));
    }
    if (GetRuleInputs(Intern(".INTERMEDIATE"), &targets, &loc)) {
      for (Symbol target : targets)
        intermediate_.insert(normalize_special_target(target));
    }
    if (GetRuleInputs(Intern(".SECONDARY"), &targets, &loc)) {
      if (targets.empty())
        secondary_all_ = true;
      for (Symbol target : targets)
        secondary_.insert(normalize_special_target(target));
    }
    oneshell_ = GetRuleInputs(Intern(".ONESHELL"), &targets, &loc);
    export_all_variables_ =
        GetRuleInputs(Intern(".EXPORT_ALL_VARIABLES"), &targets, &loc);
    GetRuleInputs(Intern(".EXTRA_PREREQS"), &extra_prereqs_, &loc);
    if (GetRuleInputs(Intern(".LOW_RESOLUTION_TIME"), &targets, &loc)) {
      for (Symbol target : targets)
        low_resolution_.insert(normalize_special_target(target));
    }

    if (GetRuleInputs(Intern(".IGNORE"), &targets, &loc)) {
      if (targets.empty()) {
        ignore_all_ = true;
      } else {
        for (Symbol target : targets)
          ignored_.insert(normalize_special_target(target));
      }
    }

    if (GetRuleInputs(Intern(".SILENT"), &targets, &loc)) {
      if (targets.empty()) {
        silent_all_ = true;
      } else {
        for (Symbol target : targets)
          silent_.insert(normalize_special_target(target));
      }
    }

    // Ninja does not perform make's intermediate-file cleanup pass.  These
    // targets therefore have no additional action to translate: generated
    // outputs remain available for incremental builds, and a failed command
    // is reported by Ninja without deleting a previously-created output.
    // Treat the cleanup-control targets as recognized no-ops instead of
    // emitting a warning for every recursive makefile that uses them.
    // .DELETE_ON_ERROR is likewise intentionally ignored in --ninja mode.
    static const char* kUnsupportedBuiltinTargets[] = {".NOTPARALLEL", NULL};
    for (const char** p = kUnsupportedBuiltinTargets; *p; p++) {
      if (std::string_view(*p) == ".NOTPARALLEL" &&
          (notparallel_ || notparallel_targets_.size() != 0))
        continue;
      if (GetRuleInputs(Intern(*p), &targets, &loc)) {
        WARN_LOC(loc, "kati doesn't support %s", *p);
      }
    }
  }

  ~DepBuilder() {}

  void Build(std::vector<Symbol> targets, std::vector<NamedDepNode>* nodes) {
    if (targets.empty() && !g_flags.gen_all_targets) {
      const std::string default_goal = ev_->EvalVar(Intern(".DEFAULT_GOAL"));
      std::vector<std::string_view> goals;
      WordScanner(default_goal).Split(&goals);
      if (goals.size() > 1)
        ERROR("*** .DEFAULT_GOAL contains more than one target.");
      if (!goals.empty())
        first_rule_ = Intern(TrimLeadingCurdir(goals[0]));
    }
    if (!first_rule_.IsValid() && targets.empty()) {
      ERROR("*** No targets.");
    }

    if (!g_flags.gen_all_targets && targets.empty()) {
      targets.push_back(first_rule_);
    }
    if (g_flags.gen_all_targets) {
      SymbolSet non_root_targets;
      for (const auto& p : rules_) {
        if (IsSpecialTarget(p.first) || IsActiveSuffixRule(p.first))
          continue;
        for (const Rule* r : p.second.rules) {
          for (Symbol t : r->inputs)
            non_root_targets.insert(t);
          for (Symbol t : r->order_only_inputs)
            non_root_targets.insert(t);
        }
      }

      for (const auto& p : rules_) {
        Symbol t = p.first;
        if (!non_root_targets.exists(t) && !IsSpecialTarget(t) &&
            !IsActiveSuffixRule(t)) {
          targets.push_back(p.first);
        }
      }
    }

    // TODO: LogStats?

    for (Symbol target : targets) {
      cur_rule_vars_.reset(new Vars);
      ev_->set_current_scope(cur_rule_vars_.get());
      DepNode* n = BuildPlan(target, Intern(""));
      nodes->push_back({target, n});
      ev_->set_current_scope(NULL);
      cur_rule_vars_.reset(NULL);
    }
  }

 private:
  bool Exists(Symbol target) {
    return (rules_.find(target) != rules_.end()) || phony_.exists(target) ||
           ::Exists(target.str()) || !ev_->ResolveVpath(target).empty();
  }

  void ResolveVpathInputs(std::vector<Symbol>* inputs) {
    for (Symbol& input : *inputs) {
      // A directory can be both a VPATH-visible source directory and a
      // recursive output target. Preserve the target-side spelling when Kati
      // already has a rule for it; otherwise VPATH would replace it with the
      // source path and bypass the recursive child build.
      struct stat st;
      if (phony_.exists(input) ||
          (stat(input.str().c_str(), &st) == 0 && S_ISDIR(st.st_mode)))
        continue;
      auto found_rule = rules_.find(input);
      std::string resolved = ev_->ResolveVpath(input);
      if (found_rule != rules_.end()) {
        // Keep rule-bearing targets in the graph. Their implicit, order-only,
        // and double-colon dependencies must still be visited even when a
        // VPATH copy is currently usable.
        continue;
      }
      // A matching implicit rule may create this target in the build tree.
      // Replacing it with its source-tree provider would make that rule write
      // back into the source tree and bypass the intended target path.
      if (!GetImplicitRuleCandidates(implicit_rules_.get(), input).empty())
        continue;
      if (!resolved.empty()) {
        // A VPATH match can itself be a directory in the source tree.  Keep
        // the logical target name in that case: it may be supplied by a
        // recursive rule in the output tree.
        if (stat(resolved.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
          continue;
        input = Intern(resolved);
      }
    }
  }

  bool GetRuleInputs(Symbol s, std::vector<Symbol>* o, Loc* l) {
    auto found = rules_.find(s);
    if (found == rules_.end())
      return false;

    o->clear();
    CHECK(!found->second.rules.empty());
    *l = found->second.rules.front()->loc;
    for (const Rule* r : found->second.rules) {
      for (Symbol i : r->inputs)
        o->push_back(i);
    }
    return true;
  }

  void PopulateRules(const std::vector<const Rule*>& rules) {
    for (const Rule* rule : rules) {
      if (rule->outputs.empty()) {
        PopulateImplicitRule(rule);
      } else {
        PopulateExplicitRule(rule);
      }
    }
    // GNU make tries suffix conversions in .SUFFIXES order, independent of
    // where their recipes appear in the makefile.
    auto suffix_rank = [this](Symbol suffix) {
      const std::string_view name = suffix.str();
      if (suffixes_specified_) {
        auto it = std::find(active_suffix_order_.begin(),
                            active_suffix_order_.end(), name);
        return static_cast<size_t>(it - active_suffix_order_.begin());
      }
      for (size_t i = 0; i < std::size(kDefaultSuffixes); ++i)
        if (name == kDefaultSuffixes[i])
          return i;
      return std::size(kDefaultSuffixes);
    };
    for (auto& entry : suffix_rules_)
      std::stable_sort(entry.second.begin(), entry.second.end(),
                       [&](const auto& a, const auto& b) {
                         return suffix_rank(a->inputs.front()) <
                                suffix_rank(b->inputs.front());
                       });
    auto dotless_rank = [&](const auto& rule) {
      std::string_view input = rule->inputs.front().str();
      input.remove_prefix(1);  // Remove the implicit-rule percent.
      if (!input.empty() && input.front() == '.')
        input.remove_prefix(1);
      return suffix_rank(Intern(input));
    };
    std::stable_sort(dotless_suffix_rules_.begin(), dotless_suffix_rules_.end(),
                     [&](const auto& a, const auto& b) {
                       return dotless_rank(a) < dotless_rank(b);
                     });
    for (const auto& rule : dotless_suffix_rules_)
      // A suffix conversion is a fallback. An equivalent explicit pattern
      // already in the trie keeps its recipe or cancellation.
      implicit_rules_->Add(rule->output_patterns.front().str(), rule.get(),
                           false);
    for (auto& p : rules_) {
      auto vars = LookupRuleVars(p.first);
      if (!vars) {
        continue;
      }
      auto var = vars->Lookup(implicit_outputs_var_name_);
      if (var->IsDefined()) {
        std::string implicit_outputs;
        var->Eval(ev_, &implicit_outputs);

        for (std::string_view output : WordScanner(implicit_outputs)) {
          Symbol sym = Intern(TrimLeadingCurdir(output));
          rules_[sym].SetImplicitOutput(sym, p.first, &p.second);
          p.second.AddImplicitOutput(sym, &rules_[sym]);
        }
      }

      var = vars->Lookup(validations_var_name_);
      if (var->IsDefined()) {
        std::string validations;
        var->Eval(ev_, &validations);

        for (std::string_view validation : WordScanner(validations)) {
          Symbol sym = Intern(TrimLeadingCurdir(validation));
          p.second.AddValidation(sym);
        }
      }
    }
  }

  void StoreDotlessSuffixRule(std::shared_ptr<Rule> rule) {
    // Repeating the same suffix conversion replaces its recipe without
    // changing the conversion's position in .SUFFIXES precedence.
    for (std::shared_ptr<Rule>& candidate : dotless_suffix_rules_) {
      if (candidate->output_patterns == rule->output_patterns &&
          candidate->inputs == rule->inputs &&
          candidate->order_only_inputs == rule->order_only_inputs) {
        candidate = std::move(rule);
        return;
      }
    }
    dotless_suffix_rules_.push_back(std::move(rule));
  }

  bool PopulateSuffixRule(const Rule* rule, Symbol output) {
    if (!IsActiveSuffixRule(output))
      return false;

    const auto dotless_boundary = FindDotlessSuffixRuleBoundary(output);
    if (dotless_boundary.first != std::string_view::npos) {
      const std::string_view name = output.str();
      const std::string input_suffix(name.substr(0, dotless_boundary.first));
      const std::string output_suffix(name.substr(dotless_boundary.first));
      std::shared_ptr<Rule> converted = std::make_shared<Rule>(*rule);
      converted->inputs = {Intern("%" + input_suffix)};
      converted->outputs.clear();
      converted->output_patterns = {Intern("%" + output_suffix)};
      StoreDotlessSuffixRule(std::move(converted));
      return true;
    }

    const std::string_view output_name = output.str();
    if (output_name.size() > 1 && output_name.front() == '.' &&
        output_name.find('.', 1) == std::string_view::npos) {
      std::shared_ptr<Rule> converted = std::make_shared<Rule>(*rule);
      converted->inputs = {Intern("%" + std::string(output_name))};
      converted->outputs.clear();
      converted->output_patterns = {Intern("%")};
      StoreDotlessSuffixRule(std::move(converted));
      return true;
    }

    if (g_flags.werror_suffix_rules) {
      ERROR_LOC(rule->loc, "*** suffix rules are obsolete: %s", output.c_str());
    } else if (g_flags.warn_suffix_rules) {
      WARN_LOC(rule->loc, "warning: suffix rules are deprecated: %s",
               output.c_str());
    }

    const std::string_view rest = std::string_view(output.str()).substr(1);
    size_t dot_index = FindSuffixRuleBoundary(output);

    std::string_view input_suffix = rest.substr(0, dot_index);
    std::string_view output_suffix = rest.substr(dot_index + 1);
    if (suffixes_specified_ &&
        (active_suffixes_.find(std::string(input_suffix)) ==
             active_suffixes_.end() ||
         active_suffixes_.find(std::string(output_suffix)) ==
             active_suffixes_.end())) {
      return true;
    }
    std::shared_ptr<Rule> r = std::make_shared<Rule>(*rule);
    r->inputs.clear();
    r->inputs.push_back(Intern(input_suffix));
    std::string output_pattern("%.");
    output_pattern.append(output_suffix);
    r->output_patterns.push_back(Intern(output_pattern));
    r->is_suffix_rule = true;
    auto& candidates = suffix_rules_[output_suffix];
    // GNU make tries distinct suffix rules in definition order. A later
    // definition of the same source/destination pair replaces its recipe
    // without moving it behind unrelated candidates.
    for (std::shared_ptr<Rule>& candidate : candidates) {
      if (candidate->inputs == r->inputs) {
        candidate = r;
        return true;
      }
    }
    candidates.push_back(r);
    return true;
  }

  void PopulateExplicitRule(const Rule* rule) {
    // A file named as a prerequisite anywhere in an explicit rule is not
    // merely an implicit-chain intermediate.  Automake, for example, lists
    // generated lexer.c in distdir prerequisites even when the current goal
    // is an executable; GNU make retains that generated source after build.
    for (Symbol input : rule->inputs)
      explicit_prerequisites_.insert(Intern(TrimLeadingCurdir(input.str())));
    for (Symbol input : rule->order_only_inputs)
      explicit_prerequisites_.insert(Intern(TrimLeadingCurdir(input.str())));
    for (Symbol output : rule->outputs) {
      // Built-in suffix rules precede the user's makefile, but must never
      // become its default goal.  An empty .SUFFIXES declaration can make
      // IsActiveSuffixRule false for .c.o, so exclude bootstrap rules here.
      const bool builtin_rule =
          rule->loc.filename != nullptr &&
          std::string_view(rule->loc.filename) == "*bootstrap*";
      if (!first_rule_.IsValid() && !builtin_rule && !IsSpecialTarget(output) &&
          !IsActiveSuffixRule(output)) {
        first_rule_ = output;
      }
      rules_[output].AddRule(output, rule);

      if (output == Intern(".DEFAULT"))
        default_rule_ = std::make_shared<Rule>(*rule);

      PopulateSuffixRule(rule, output);
    }
  }

  static bool IsIgnorableImplicitRule(const Rule* rule) {
    // As kati doesn't have RCS/SCCS related default rules, we can
    // safely ignore suppression for them.
    if (rule->inputs.size() != 1)
      return false;
    if (!rule->order_only_inputs.empty())
      return false;
    if (!rule->cmds.empty())
      return false;
    const std::string& i = rule->inputs[0].str();
    return (i == "RCS/%,v" || i == "RCS/%" || i == "%,v" || i == "s.%" ||
            i == "SCCS/s.%");
  }

  void PopulateImplicitRule(const Rule* rule) {
    for (Symbol output_pattern : rule->output_patterns) {
      if (output_pattern.str() != "%" || !IsIgnorableImplicitRule(rule)) {
        if (g_flags.werror_implicit_rules) {
          ERROR_LOC(rule->loc, "*** implicit rules are obsolete: %s",
                    output_pattern.c_str());
        } else if (g_flags.warn_implicit_rules) {
          WARN_LOC(rule->loc, "warning: implicit rules are deprecated: %s",
                   output_pattern.c_str());
        }

        implicit_rules_->Add(output_pattern.str(), rule);
      }
    }
  }

  const RuleMerger* LookupRuleMerger(Symbol o) {
    auto found = rules_.find(o);
    if (found != rules_.end()) {
      return &found->second;
    }
    // Explicit rules may have been recorded using a lexical path containing
    // parent components while the dependency graph requests the canonical
    // spelling.  Both spellings identify the same target and must expose the
    // same recipe and rule metadata.
    std::string canonical(o.str());
    NormalizeMakePath(&canonical);
    if (!canonical.empty() && canonical != o.str()) {
      found = rules_.find(Intern(canonical));
      if (found != rules_.end())
        return &found->second;
    }
    return nullptr;
  }

  Vars* LookupRuleVars(Symbol o) {
    auto found = rule_vars_.find(o);

    if (found != rule_vars_.end())
      return found->second;
    std::string canonical(o.str());
    NormalizeMakePath(&canonical);
    if (!canonical.empty() && canonical != o.str()) {
      found = rule_vars_.find(Intern(canonical));
      if (found != rule_vars_.end())
        return found->second;
    }
    return nullptr;
  }

  bool CanBuildImplicit(Symbol output,
                        const Rule* current_rule,
                        std::unordered_set<const Rule*>* used_rules) {
    // Phony targets are always considered buildable and GNU make does not
    // search implicit rules to remake them.
    if (Exists(output) || phony_.exists(output))
      return true;

    std::vector<const Rule*> irules =
        GetImplicitRuleCandidates(implicit_rules_.get(), output);

    // A cancellation rule prevents any implicit-rule chain from making this
    // target, including while checking a candidate's prerequisites.
    if (IsImplicitRuleCancellation(output, irules))
      return false;

    for (auto iter = irules.rbegin(); iter != irules.rend(); ++iter) {
      const Rule* rule = *iter;
      if (!HasRuleRecipe(rule) && rule->output_patterns.size() == 1 &&
          rule->output_patterns.front() == Intern("%"))
        continue;

      // GNU make does not allow an implicit rule to be used more than once
      // in a single implicit-rule chain.  This also prevents pathological
      // chains such as foo_shipped_shipped_shipped...
      if (rule == current_rule || used_rules->find(rule) != used_rules->end())
        continue;

      Symbol matched;
      for (Symbol output_pattern : rule->output_patterns) {
        if (IsDirectoryTarget(output) && (output_pattern.str().empty() ||
                                          output_pattern.str().back() != '/'))
          continue;
        Pattern pat(output_pattern.str());
        if (pat.Match(ImplicitMatchTarget(output, output_pattern))) {
          matched = output_pattern;
          break;
        }
      }

      if (!matched.IsValid())
        continue;

      used_rules->insert(rule);

      bool ok = true;
      Pattern pat(matched.str());
      for (Symbol input : rule->inputs) {
        if (rule->secondary_expansion &&
            input.str().find('$') != std::string_view::npos)
          continue;
        std::string buf;
        buf = SubstituteImplicitPrerequisite(output, matched, input.str());

        for (Symbol prerequisite : ExpandImplicitPrerequisite(buf)) {
          if (!CanBuildImplicit(prerequisite, rule, used_rules)) {
            ok = false;
            break;
          }
        }
        if (!ok)
          break;
      }

      used_rules->erase(rule);

      if (ok)
        return true;
    }

    // Suffix rules participate in GNU make's implicit-rule chains too.  For
    // example, a missing foo.o may be made through .c.o after foo.c is made
    // through .l.c.  The old implementation only accepted an existing
    // suffix prerequisite, which rejected valid chained rules such as this.
    std::string_view output_suffix = GetExt(output.str());
    if (!output_suffix.empty() && output_suffix.front() == '.') {
      output_suffix = output_suffix.substr(1);
      auto found = suffix_rules_.find(std::string(output_suffix));
      if (found != suffix_rules_.end()) {
        for (const std::shared_ptr<Rule>& rule : found->second) {
          if (rule.get() == current_rule ||
              used_rules->find(rule.get()) != used_rules->end() ||
              rule->inputs.size() != 1)
            continue;
          Symbol input = ReplaceSuffix(output, rule->inputs[0]);
          if (Exists(input))
            return true;
          used_rules->insert(rule.get());
          bool ok = CanBuildImplicit(input, rule.get(), used_rules);
          used_rules->erase(rule.get());
          if (ok)
            return true;
        }
      }
    }

    return false;
  }

  bool CanPickImplicitRule(const Rule* rule,
                           Symbol output,
                           DepNode* n,
                           std::shared_ptr<Rule>* out_rule) {
    Symbol matched;
    for (Symbol output_pattern : rule->output_patterns) {
      if (IsDirectoryTarget(output) &&
          (output_pattern.str().empty() || output_pattern.str().back() != '/'))
        continue;
      Pattern pat(output_pattern.str());
      if (pat.Match(ImplicitMatchTarget(output, output_pattern))) {
        bool ok = true;
        for (Symbol input : rule->inputs) {
          if (rule->secondary_expansion &&
              input.str().find('$') != std::string_view::npos)
            continue;
          std::string buf;
          buf = SubstituteImplicitPrerequisite(output, output_pattern,
                                               input.str());

          for (Symbol prerequisite : ExpandImplicitPrerequisite(buf)) {
            if (prerequisite == output && !Exists(prerequisite)) {
              ok = false;
              break;
            }
            if (!Exists(prerequisite)) {
              std::unordered_set<const Rule*> used_rules;
              used_rules.insert(rule);

              if (!CanBuildImplicit(prerequisite, rule, &used_rules)) {
                ok = false;
                break;
              }
            }
          }
          if (!ok)
            break;
        }

        // A pattern rule whose order-only prerequisite expands to its own
        // target is not a usable implicit candidate. GNU make tries the
        // next pattern (often a directory-stamp rule) instead.
        if (ok) {
          for (Symbol input : rule->order_only_inputs) {
            std::string buf = SubstituteImplicitPrerequisite(
                output, output_pattern, input.str());
            for (Symbol prerequisite : ExpandImplicitPrerequisite(buf)) {
              if (prerequisite == output && !Exists(prerequisite)) {
                ok = false;
                break;
              }
            }
            if (!ok)
              break;
          }
        }

        if (ok) {
          matched = output_pattern;
          break;
        }
      }
    }
    if (!matched.IsValid())
      return false;

    // Recipe-less pattern rules can still provide prerequisite relationships
    // (GNU make uses these, for example, to route a directory stamp target
    // through a recursive subdirectory target).  Keep treating a rule with
    // no prerequisites as a cancellation rule.  When a recipe-less rule has
    // prerequisites, however, prefer an equally specific or more specific
    // suffix rule that can actually build the target.  This preserves the
    // usual suffix-rule fallback for rules such as %.o: %.m.
    const bool has_recipe = HasRuleRecipe(rule);
    if (!has_recipe) {
      if (rule->inputs.empty() && rule->order_only_inputs.empty())
        return false;

      const size_t pattern_stem_size =
          Pattern(matched.str())
              .Stem(ImplicitMatchTarget(output, matched))
              .size();
      std::string_view suffix = GetExt(output.str());
      if (!suffix.empty() && suffix.front() == '.') {
        auto found = suffix_rules_.find(std::string(suffix.substr(1)));
        if (found != suffix_rules_.end()) {
          for (const std::shared_ptr<Rule>& suffix_rule : found->second) {
            if (suffix_rule->cmds.empty() || suffix_rule->inputs.size() != 1)
              continue;
            Symbol input = ReplaceSuffix(output, suffix_rule->inputs[0]);
            bool usable = Exists(input);
            if (!usable) {
              std::unordered_set<const Rule*> used_rules;
              used_rules.insert(suffix_rule.get());
              usable = CanBuildImplicit(input, suffix_rule.get(), &used_rules);
            }
            if (usable &&
                pattern_stem_size >= output.str().size() - suffix.size())
              return false;
          }
        }
      }
    }

    *out_rule = std::make_shared<Rule>(*rule);
    if ((*out_rule)->output_patterns.size() > 1) {
      // We should mark all other output patterns as used.
      Pattern pat(matched.str());
      for (Symbol output_pattern : rule->output_patterns) {
        if (output_pattern == matched)
          continue;
        std::string buf;
        pat.AppendSubst(output.str(), output_pattern.str(), &buf);
        done_[Intern(buf)] = n;
      }
      (*out_rule)->output_patterns.clear();
      (*out_rule)->output_patterns.push_back(matched);
    }

    return true;
  }

  Vars* MergeImplicitRuleVars(Symbol output, Vars* vars) {
    auto exact = rule_vars_.find(output);
    if (exact != rule_vars_.end() && exact->second != vars) {
      if (!vars) {
        vars = exact->second;
      } else {
        Vars* combined = new Vars(*exact->second);
        for (const auto& entry : *vars)
          (*combined)[entry.first] = entry.second;
        vars = combined;
      }
    }
    std::string_view target = output.str();
    if (HasPrefix(target, "./"))
      target.remove_prefix(2);
    std::vector<std::pair<Symbol, Vars*>> patterns;
    for (const auto& p : rule_vars_) {
      std::string_view key = p.first.str();
      if (key.find('%') == std::string_view::npos)
        continue;
      Pattern pat(key);
      if (!pat.Match(target))
        continue;
      patterns.push_back(p);
    }
    if (patterns.empty())
      return vars;
    // Broad patterns form the base for narrower patterns and explicit
    // target assignments. Appends contribute only their local suffix.
    std::unordered_map<Symbol, size_t> definition_order;
    const auto& ordered_vars = ev_->rule_vars_order();
    for (size_t i = 0; i < ordered_vars.size(); ++i)
      definition_order[ordered_vars[i]] = i;
    std::sort(
        patterns.begin(), patterns.end(),
        [target, &definition_order](const auto& a, const auto& b) {
          const size_t a_stem = Pattern(a.first.str()).Stem(target).size();
          const size_t b_stem = Pattern(b.first.str()).Stem(target).size();
          return a_stem != b_stem ? a_stem > b_stem
                                  : definition_order.at(a.first) <
                                        definition_order.at(b.first);
        });
    Vars* result = new Vars;
    auto merge = [&](Vars* additions) {
      if (!additions)
        return;
      result->MergeExportStateFrom(*additions);
      for (const auto& entry : *additions) {
        Var* value = entry.second;
        if (value->op() == AssignOp::QUESTION_EQ &&
            ev_->LookupVar(entry.first)->IsDefined())
          continue;
        auto previous = result->find(entry.first);
        if (previous != result->end() && value->TargetAppend()) {
          Var* base = previous->second;
          Value* base_value = base->TargetAppend();
          if (!base_value) {
            if (const auto* recursive = dynamic_cast<const RecursiveVar*>(base))
              base_value = recursive->v_;
            else
              base_value = Value::NewLiteral(Intern(base->String()).str());
          }
          Value* combined = new TargetAppendValue(value->Location(), base_value,
                                                  value->TargetAppend());
          // Evaluating a SimpleVar here would freeze references before the
          // target's own variable scope is installed. Resolve the complete
          // append at BuildPlan/recipe time instead.
          value = new RecursiveVar(combined, value->Origin(),
                                   value->Definition(), value->Location(),
                                   std::string_view("*pattern append*"));
          if (base->TargetAppend()) {
            value->SetAssignOp(AssignOp::PLUS_EQ);
            value->SetTargetAppend(combined);
          }
        }
        (*result)[entry.first] = value;
      }
    };
    for (const auto& pattern : patterns)
      merge(pattern.second);
    merge(vars);
    return result;
  }

  bool PickRule(Symbol output,
                DepNode* n,
                const RuleMerger** out_rule_merger,
                std::shared_ptr<Rule>* pattern_rule,
                Vars** out_var,
                const std::unordered_set<const Rule*>* implicit_rules_used,
                const Rule** out_selected_implicit_rule) {
    const RuleMerger* rule_merger = LookupRuleMerger(output);
    *out_selected_implicit_rule = nullptr;
    // Pattern-specific variables apply to explicit targets as well as to
    // implicit-rule results.  In particular, a pattern assignment such as
    // "foo.%: CXXFLAGS += -frtti" must be visible when foo.o has an
    // ordinary explicit compilation rule.  Previously this merge happened
    // only after selecting an implicit rule, so the target inherited
    // -fno-rtti but lost the required -frtti.
    Vars* vars = MergeImplicitRuleVars(output, LookupRuleVars(output));
    *out_rule_merger = rule_merger;
    *out_var = vars;
    if (rule_merger && rule_merger->primary_rule) {
      for (auto implicit_output : rule_merger->implicit_outputs) {
        vars = MergeImplicitRuleVars(implicit_output.first, vars);
      }
      *out_var = vars;
      return true;
    }

    // GNU make skips implicit-rule search for phony targets. This matters
    // when a phony target with prerequisites also matches a recursive
    // pattern such as install-%: install-%-nosubdir.
    if (phony_.exists(output))
      return rule_merger != nullptr;

    std::vector<const Rule*> irules =
        GetImplicitRuleCandidates(implicit_rules_.get(), output);

    if (IsImplicitRuleCancellation(output, irules))
      return rule_merger != nullptr;

    // GNU make tries the implicit rule with the shortest stem first.  Trie
    // lookup groups rules by literal prefixes/suffixes, which is useful for
    // finding candidates but does not by itself preserve that priority.
    std::stable_sort(
        irules.begin(), irules.end(), [output](const Rule* a, const Rule* b) {
          auto stem_size = [output](const Rule* rule) {
            size_t result = std::string_view::npos;
            for (Symbol pattern : rule->output_patterns) {
              Pattern pat(pattern.str());
              std::string_view target = ImplicitMatchTarget(output, pattern);
              if (pat.Match(target)) {
                // GNU make includes the target's directory in the stem of a
                // pattern without a slash. It is stripped only for matching.
                // Otherwise git-% can outrank bin-wrappers/% for a target in
                // bin-wrappers even though the latter is more specific.
                const size_t directory_size =
                    output.str().size() - target.size();
                result =
                    std::min(result, pat.Stem(target).size() + directory_size);
              }
            }
            return result;
          };
          return stem_size(a) < stem_size(b);
        });

    std::vector<std::pair<const Rule*, std::shared_ptr<Rule>>>
        prerequisite_only_rules;
    for (auto iter = irules.begin(); iter != irules.end(); ++iter) {
      // A recipe-less catch-all cannot supply the missing link recipe.
      // Preserve narrower prerequisite-only routing patterns.
      if (!HasRuleRecipe(*iter) && (*iter)->output_patterns.size() == 1 &&
          (*iter)->output_patterns.front() == Intern("%"))
        continue;
      if (implicit_rules_used != nullptr &&
          implicit_rules_used->find(*iter) != implicit_rules_used->end())
        continue;
      bool picked = CanPickImplicitRule(*iter, output, n, pattern_rule);

      if (!picked)
        continue;
      if (!HasRuleRecipe(*iter)) {
        prerequisite_only_rules.emplace_back(*iter, *pattern_rule);
        continue;
      }

      // GNU make lets a recipe-less pattern rule contribute prerequisites
      // alongside the recipe-bearing implicit rule selected for the same
      // target.  This is used for generated intermediates (for example,
      // compiling a generated .c file to .o) and must remain an ordinary
      // implicit-rule relationship, not a target-specific special case.
      for (const auto& deferred_rule : prerequisite_only_rules) {
        const std::shared_ptr<Rule>& prereq_rule = deferred_rule.second;
        if (prereq_rule->secondary_expansion)
          continue;
        std::vector<Symbol> expanded_inputs;
        std::vector<Symbol> expanded_order_only_inputs;
        ApplyOutputPattern(*prereq_rule, output, prereq_rule->inputs,
                           &expanded_inputs);
        ApplyOutputPattern(*prereq_rule, output, prereq_rule->order_only_inputs,
                           &expanded_order_only_inputs);
        pattern_rule->get()->inputs.insert(pattern_rule->get()->inputs.end(),
                                           expanded_inputs.begin(),
                                           expanded_inputs.end());
        pattern_rule->get()->order_only_inputs.insert(
            pattern_rule->get()->order_only_inputs.end(),
            expanded_order_only_inputs.begin(),
            expanded_order_only_inputs.end());
      }
      *out_selected_implicit_rule = *iter;
      CHECK((*pattern_rule)->output_patterns.size() == 1);
      *out_var = vars;
      if (rule_merger) {
        return true;
      }
      return true;
    }

    // A prerequisite-only pattern is still a useful implicit rule when no
    // recipe-bearing alternative applies.  Preserve it as the fallback, as
    // GNU make does for directory-routing and suffix-rule precedence.
    if (!prerequisite_only_rules.empty()) {
      *out_selected_implicit_rule = prerequisite_only_rules.front().first;
      *pattern_rule = prerequisite_only_rules.front().second;
      *out_var = vars;
      return true;
    }

    // GNU make's built-in object linker is a fallback behind user pattern
    // rules.  In particular a user `%:` last-resort recipe must win even
    // though its output can also be routed through `%.o`.  Materialize the
    // linker here only for an explicit executable rule that already names
    // its matching object, as Kbuild's host-program rules do.
    if (!g_flags.no_builtin_rules &&
        (!rule_merger || !rule_merger->primary_rule)) {
      const Symbol object = Intern(std::string(output.str()) + ".o");
      bool names_object = false;
      if (rule_merger) {
        for (const Rule* r : rule_merger->rules) {
          if (std::find(r->inputs.begin(), r->inputs.end(), object) !=
              r->inputs.end()) {
            names_object = true;
            break;
          }
        }
      }
      if (names_object &&
          (!suffixes_specified_ || active_suffixes_.count("o")) &&
          std::none_of(irules.begin(), irules.end(), [](const Rule* rule) {
            return rule->output_patterns == std::vector<Symbol>{Intern("%")} &&
                   rule->inputs == std::vector<Symbol>{Intern("%.o")} &&
                   rule->order_only_inputs.empty() && !HasRuleRecipe(rule);
          })) {
        auto link = std::make_shared<Rule>();
        link->output_patterns.push_back(Intern("%"));
        link->inputs.push_back(Intern("%.o"));
        link->loc = Loc("*bootstrap*", 0);
        Loc recipe_loc = link->loc;
        link->cmds.push_back(
            ParseExpr(&recipe_loc, "$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@",
                      ParseExprOpt::COMMAND));
        *pattern_rule = link;
        *out_selected_implicit_rule = link.get();
        return true;
      }
      // A recipe-less explicit executable rule can likewise name its C
      // source directly. GNU make's built-in link recipe supplies the
      // command while retaining all explicitly listed prerequisites.
      const Symbol source = Intern(std::string(output.str()) + ".c");
      bool names_source = !rule_merger;
      if (rule_merger) {
        for (const Rule* r : rule_merger->rules) {
          if (std::find(r->inputs.begin(), r->inputs.end(), source) !=
              r->inputs.end()) {
            names_source = true;
            break;
          }
        }
      }
      if (names_source && Exists(source) &&
          (!suffixes_specified_ || active_suffixes_.count("c"))) {
        auto link = std::make_shared<Rule>();
        link->output_patterns.push_back(Intern("%"));
        link->inputs.push_back(Intern("%.c"));
        link->loc = Loc("*bootstrap*", 0);
        Loc recipe_loc = link->loc;
        link->cmds.push_back(
            ParseExpr(&recipe_loc, "$(LINK.c) $^ $(LOADLIBES) $(LDLIBS) -o $@",
                      ParseExprOpt::COMMAND));
        *pattern_rule = link;
        *out_selected_implicit_rule = link.get();
        return true;
      }
    }

    std::string_view output_suffix = GetExt(output.str());
    if (output_suffix.empty() || output_suffix.front() != '.')
      return rule_merger != nullptr;
    output_suffix = output_suffix.substr(1);

    SuffixRuleMap::const_iterator found = suffix_rules_.find(output_suffix);
    if (found == suffix_rules_.end())
      return rule_merger != nullptr;

    for (const std::shared_ptr<Rule>& irule : found->second) {
      if (implicit_rules_used != nullptr &&
          implicit_rules_used->find(irule.get()) != implicit_rules_used->end())
        continue;
      CHECK(irule->inputs.size() == 1);
      Symbol input = ReplaceSuffix(output, irule->inputs[0]);
      if (!Exists(input)) {
        std::unordered_set<const Rule*> used_rules;
        used_rules.insert(irule.get());
        if (!CanBuildImplicit(input, irule.get(), &used_rules))
          continue;
      }

      *pattern_rule = irule;
      *out_selected_implicit_rule = irule.get();
      if (rule_merger != nullptr)
        return true;
      if (vars) {
        CHECK(irule->outputs.size() == 1);
        vars = MergeImplicitRuleVars(irule->outputs[0], vars);
        *out_var = vars;
      }
      return true;
    }

    return rule_merger != nullptr;
  }

  DepNode* BuildPlan(
      Symbol output,
      Symbol needed_by,
      const std::unordered_set<const Rule*>* implicit_rules_used = nullptr) {
    LOG("BuildPlan: %s for %s", output.c_str(), needed_by.c_str());

    auto found = done_.find(output);
    if (found != done_.end()) {
      return found->second;
    }

    DepNode* n = g_dep_node_pool
                     .emplace_back(std::make_unique<DepNode>(
                         output, phony_.exists(output), restat_.exists(output)))
                     .get();

    done_[output] = n;
    n->is_notparallel = notparallel_;
    n->delete_on_error = delete_on_error_;
    n->precious = precious_.exists(n->lexical_output);
    n->intermediate = intermediate_.exists(n->lexical_output) &&
                      !secondary_all_ &&
                      !secondary_.exists(n->lexical_output) && !n->precious;
    n->oneshell = oneshell_;
    n->ignore_errors = ignore_all_ || ignored_.exists(n->lexical_output);
    n->silent = silent_all_ || silent_.exists(n->lexical_output);
    n->export_all_variables = export_all_variables_;

    // GNU make permits lexical path components such as "dir/../file" in
    // target names. Resolve the graph through one canonical provider while
    // retaining the requested spelling as an alias.
    std::string normalized_output = output.str();
    NormalizeMakePath(&normalized_output);
    if (!normalized_output.empty() && normalized_output != output.str()) {
      Symbol canonical = Intern(normalized_output);
      const RuleMerger* lexical_merger = nullptr;
      std::shared_ptr<Rule> lexical_pattern;
      Vars* lexical_vars = nullptr;
      const Rule* lexical_selected_implicit_rule = nullptr;
      // Rule matching must see the lexical spelling first.  Pattern rules
      // commonly use the requesting directory's $(obj)/$(src) relationship;
      // canonicalizing a parent-relative prerequisite before this lookup can
      // incorrectly fall through to a generic built-in rule.
      if (PickRule(output, n, &lexical_merger, &lexical_pattern, &lexical_vars,
                   implicit_rules_used, &lexical_selected_implicit_rule)) {
        // Keep the selected rule and its lexical prerequisites.  The output
        // name is canonicalized after rule expansion below.
      } else {
        n->is_phony = true;
        DepNode* canonical_node =
            BuildPlan(canonical, output, implicit_rules_used);
        // The canonical node may have been discovered earlier through a
        // different prerequisite path.  Preserve the active target-specific
        // scope before choosing whether to retain a lexical alias.
        Vars* inherited_vars = cur_rule_vars_.get();
        auto parent = done_.find(needed_by);
        if (parent != done_.end() && parent->second->rule_vars != nullptr)
          inherited_vars = parent->second->rule_vars;
        if (inherited_vars != nullptr && !inherited_vars->empty()) {
          Vars* merged = new Vars;
          if (canonical_node->rule_vars != nullptr) {
            for (const auto& var : *canonical_node->rule_vars)
              merged->insert(var);
          }
          for (const auto& var : *inherited_vars)
            (*merged)[var.first] = var.second;
          canonical_node->rule_vars = merged;
        }

        // Ninja canonicalizes parent-relative paths.  Multiple lexical
        // spellings of an existing source file must share one graph node,
        // even if a depfile gives that source an empty rule of its own.
        struct stat canonical_stat;
        const bool existing_file =
            stat(canonical.c_str(), &canonical_stat) == 0 &&
            S_ISREG(canonical_stat.st_mode);
        const bool plain_source = !canonical_node->has_rule &&
                                  !canonical_node->is_phony &&
                                  canonical_node->deps.empty() &&
                                  canonical_node->order_onlys.empty() &&
                                  canonical_node->validations.empty();
        if (existing_file || plain_source) {
          done_[output] = canonical_node;
          return canonical_node;
        }
        n->deps.push_back({canonical, canonical_node});
        return n;
      }
    }

    const RuleMerger* rule_merger = nullptr;
    std::shared_ptr<Rule> pattern_rule;
    Vars* vars;
    const Rule* selected_implicit_rule = nullptr;
    bool picked = PickRule(output, n, &rule_merger, &pattern_rule, &vars,
                           implicit_rules_used, &selected_implicit_rule);

    if (!picked) {
      // A requested target may already exist in a VPATH directory.  This is
      // also true for recursive make invocations that name the file directly
      // (for example, Automake's "make m4.1" in an out-of-tree doc build).
      // Resolve it to the existing provider before treating it as missing.
      std::string vpath_output = ev_->ResolveVpath(output);
      if (!vpath_output.empty() && vpath_output != output.str()) {
        DepNode* provider =
            BuildPlan(Intern(vpath_output), output, implicit_rules_used);
        done_[output] = provider;
        return provider;
      }
      if (default_rule_) {
        n->cmds = default_rule_->cmds;
        n->loc = default_rule_->loc;
        if (!n->cmds.empty() && default_rule_->cmd_lineno)
          n->loc.lineno = default_rule_->cmd_lineno;
        n->has_rule = true;
      }
      return n;
    }
    if (rule_merger && rule_merger->parent) {
      // Resolve an implicit output to its provider before expanding its
      // dependencies.  This is important for recursive build directories:
      // the provider carries the actual producer rule, while the implicit
      // name is only an alias exposed by the makefile.
      output = rule_merger->parent_sym;
      done_[output] = n;
      n->output = output;
      if (!PickRule(output, n, &rule_merger, &pattern_rule, &vars,
                    implicit_rules_used, &selected_implicit_rule))
        return n;
    }

    if (rule_merger)
      rule_merger->FillDepNode(output, pattern_rule.get(), n);
    else
      RuleMerger().FillDepNode(output, pattern_rule.get(), n);
    if (rule_merger || pattern_rule) {
      const std::string provider = ev_->ResolveVpath(n->output);
      if (!provider.empty() && provider != n->output.str())
        n->vpath_provider = Intern(provider);
    }

    if (!normalized_output.empty() && normalized_output != output.str())
      n->output = Intern(normalized_output);

    // Retain the prerequisites from the selected implicit rule separately
    // from explicit rules merged onto this target. Only a prerequisite
    // supplied by an implicit rule can be an intermediate link in an
    // implicit-rule chain.
    n->implicit_inputs.clear();
    if (pattern_rule && !pattern_rule->secondary_expansion) {
      std::vector<Symbol> chain_inputs;
      auto collect_chain_inputs = [&](const std::vector<Symbol>& inputs) {
        for (Symbol input : inputs) {
          if (pattern_rule->is_suffix_rule ||
              input.str().find('%') != std::string::npos)
            chain_inputs.push_back(input);
        }
      };
      collect_chain_inputs(pattern_rule->inputs);
      collect_chain_inputs(pattern_rule->order_only_inputs);
      ApplyOutputPattern(*pattern_rule, n->lexical_output, chain_inputs,
                         &n->implicit_inputs);
    }

    if (pattern_rule) {
      implicit_nodes_.insert(n->output);
      implicit_nodes_.insert(n->lexical_output);
    }

    // A generated target in an implicit-rule chain is intermediate. The
    // recipe-bearing catch-all `%:` rule is GNU make's last-resort rule and
    // its outputs are intermediate even when an explicit target names them
    // directly as prerequisites. Existing files found through a fallback
    // pattern rule are sources, not generated intermediates.
    const auto parent = done_.find(needed_by);
    const bool implicit_chain_input =
        parent != done_.end() &&
        std::find(parent->second->implicit_inputs.begin(),
                  parent->second->implicit_inputs.end(),
                  n->lexical_output) != parent->second->implicit_inputs.end();
    const bool last_resort_rule =
        pattern_rule && pattern_rule->output_patterns.size() == 1 &&
        pattern_rule->output_patterns.front() == Intern("%") &&
        pattern_rule->inputs.empty() && pattern_rule->order_only_inputs.empty();
    if (!needed_by.empty() && pattern_rule &&
        ((implicit_chain_input && implicit_nodes_.exists(needed_by) &&
          !explicit_prerequisites_.exists(n->lexical_output)) ||
         last_resort_rule) &&
        !::Exists(n->output.str()) && !n->precious && !secondary_all_ &&
        !secondary_.exists(n->lexical_output))
      n->intermediate = true;

    // A target-specific rule can provide the commands while a suffix rule
    // provides the source relationship.  GNU make still defines $* for this
    // case; preserve that stem for command expansion even though the suffix
    // rule was not copied into pattern_rule because the explicit rule won.
    if (!n->output_pattern.IsValid() && !suffix_rules_.empty()) {
      std::string_view suffix = GetExt(output.str());
      if (!suffix.empty() && suffix.front() == '.' && [&] {
            const std::string name(suffix.substr(1));
            if (suffixes_specified_)
              return active_suffixes_.find(name) != active_suffixes_.end();
            if (suffix_rules_.find(name) != suffix_rules_.end())
              return true;
            // GNU make also defines $* for explicit targets whose suffix is
            // in its built-in suffix list, even when no conversion rule is
            // selected. Keep that automatic-variable behavior without
            // importing or depending on GNU make itself.
            static constexpr const char* kDefaultSuffixes[] = {
                "out",    "a",  "ln",   "o",   "c",   "cc",      "C",
                "cpp",    "p",  "f",    "F",   "m",   "r",       "y",
                "l",      "ym", "lm",   "s",   "S",   "mod",     "sym",
                "def",    "h",  "info", "dvi", "tex", "texinfo", "texi",
                "txinfo", "w",  "ch",   "web", "sh",  "elc",     "el"};
            for (const char* default_suffix : kDefaultSuffixes)
              if (name == default_suffix)
                return true;
            return false;
          }()) {
        std::string pattern("%");
        pattern.append(suffix);
        n->output_pattern = Intern(pattern);
      }
    }

    std::vector<std::unique_ptr<ScopedVar>> sv;
    ScopedFrame frame(
        ev_->Enter(FrameType::DEPENDENCY, n->lexical_output.str(), n->loc));

    if (vars) {
      for (const auto& p : *vars) {
        Symbol name = p.first;
        Var* var = p.second;
        CHECK(var);
        Var* new_var = var;
        if (var->op() == AssignOp::PLUS_EQ) {
          Var* old_var = ev_->LookupVar(name);
          if (old_var->IsDefined()) {
            // TODO: This would be incorrect and has a leak.
            std::shared_ptr<std::string> s = std::make_shared<std::string>();
            if (var->TargetAppend()) {
              Value* appended = new TargetAppendValue(var->Location(), old_var,
                                                      var->TargetAppend());
              new_var =
                  new RecursiveVar(appended, old_var->Origin(), frame.Current(),
                                   n->loc, std::string_view("*target append*"));
            } else {
              old_var->Eval(ev_, s.get());
              if (!s->empty())
                *s += ' ';
              new_var->Eval(ev_, s.get());
              new_var =
                  new SimpleVar(*s, old_var->Origin(), frame.Current(), n->loc);
            }
          } else if (var->TargetAppend()) {
            Value* appended = new TargetAppendValue(var->Location(), nullptr,
                                                    var->TargetAppend());
            new_var =
                new RecursiveVar(appended, var->Origin(), frame.Current(),
                                 n->loc, std::string_view("*target append*"));
          }
        } else if (var->op() == AssignOp::QUESTION_EQ) {
          Var* old_var = ev_->LookupVar(name);
          if (old_var->IsDefined()) {
            continue;
          }
        }

        if (name == depfile_var_name_) {
          n->depfile_var = new_var;
        } else if (name == implicit_outputs_var_name_) {
        } else if (name == validations_var_name_) {
        } else if (name == ninja_pool_var_name_) {
          n->ninja_pool_var = new_var;
        } else if (name == tags_var_name_) {
          n->tags_var = new_var;
        } else {
          sv.emplace_back(new ScopedVar(cur_rule_vars_.get(), name, new_var));
        }
      }
    }

    if (rule_merger) {
      rule_merger->FillSecondaryFirstInputs(n->lexical_output,
                                            pattern_rule.get(), n);
    } else {
      RuleMerger().FillSecondaryFirstInputs(n->lexical_output,
                                            pattern_rule.get(), n);
    }

    // Install automatic variables separately for each rule's second pass.
    // Later rules must see prerequisites added by earlier rules; this is
    // observable through the difference between $^ (unique) and $+ (all).
    std::vector<std::unique_ptr<ScopedVar>> secondary_auto_vars;
    RuleMerger empty_merger;
    const RuleMerger* active_merger = rule_merger ? rule_merger : &empty_merger;
    auto expand_secondary_rule = [&](const Rule* secondary_rule) {
      if (!secondary_rule || !secondary_rule->secondary_expansion)
        return;
      secondary_auto_vars.clear();
      std::string all;
      std::string unique;
      SymbolSet seen;
      for (Symbol input : n->actual_inputs) {
        if (!all.empty())
          all += ' ';
        all += input.str();
        if (!seen.exists(input)) {
          seen.insert(input);
          if (!unique.empty())
            unique += ' ';
          unique += input.str();
        }
      }
      const std::string first = n->actual_inputs.empty()
                                    ? std::string()
                                    : n->actual_inputs.front().str();
      std::string stem;
      if (n->output_pattern.IsValid())
        stem = Pattern(n->output_pattern.str()).Stem(n->lexical_output.str());
      std::string archive_member;
      const std::string output_name = n->lexical_output.str();
      const size_t archive_open = output_name.find('(');
      if (archive_open != std::string::npos && output_name.back() == ')' &&
          archive_open + 1 < output_name.size()) {
        archive_member = output_name.substr(
            archive_open + 1, output_name.size() - archive_open - 2);
      }
      const std::pair<const char*, std::string> automatic[] = {
          {"@", n->lexical_output.str()},
          {"%", archive_member},
          {"<", first},
          {"^", unique},
          {"+", all},
          {"*", stem}};
      for (const auto& item : automatic) {
        secondary_auto_vars.emplace_back(
            new ScopedVar(cur_rule_vars_.get(), Intern(item.first),
                          new SimpleVar(item.second, VarOrigin::AUTOMATIC,
                                        frame.Current(), n->loc)));
      }
      const size_t prior_inputs = n->actual_inputs.size();
      const size_t prior_order_only = n->actual_order_only_inputs.size();
      active_merger->FillOneSecondaryInput(
          n->lexical_output, secondary_rule, n, ev_,
          secondary_rule == pattern_rule.get());
      if (!n->double_colon_group_inputs.empty()) {
        const auto found_rule =
            std::find(active_merger->rules.begin(), active_merger->rules.end(),
                      secondary_rule);
        if (found_rule != active_merger->rules.end()) {
          const size_t group = found_rule - active_merger->rules.begin();
          auto& inputs = n->double_colon_group_inputs[group];
          inputs.insert(inputs.end(), n->actual_inputs.begin() + prior_inputs,
                        n->actual_inputs.end());
          if (!inputs.empty() ||
              n->actual_order_only_inputs.size() > prior_order_only)
            n->double_colon_group_has_prerequisites[group] = true;
        }
      }
    };

    std::unordered_set<const Rule*> expanded_rules;
    auto expand_once = [&](const Rule* secondary_rule) {
      if (!secondary_rule || !expanded_rules.insert(secondary_rule).second)
        return;
      expand_secondary_rule(secondary_rule);
    };
    expand_once(active_merger->primary_rule);
    expand_once(pattern_rule.get());
    for (const Rule* r : active_merger->rules)
      expand_once(r);

    // Secondary expansion may discover a prerequisite after first-pass
    // prerequisites have already been appended to actual_inputs. When the
    // rule has .WAIT groups, those groups preserve the original lexical
    // order, which is also the order GNU make uses for $<, $^, and $+.
    // Reorder only the inputs represented by the groups; unrelated merged
    // inputs retain their existing order.
    if (!n->wait_groups.empty()) {
      std::vector<Symbol> ordered_inputs;
      std::unordered_set<Symbol> present(n->actual_inputs.begin(),
                                         n->actual_inputs.end());
      std::unordered_set<Symbol> ordered;
      for (const std::vector<Symbol>& group : n->wait_groups) {
        for (Symbol input : group) {
          if (present.count(input) != 0 && ordered.insert(input).second)
            ordered_inputs.push_back(input);
        }
      }
      for (Symbol input : n->actual_inputs) {
        if (ordered.insert(input).second)
          ordered_inputs.push_back(input);
      }
      n->actual_inputs.swap(ordered_inputs);
    }

    // Automatic variables installed for secondary expansion are scoped to
    // that expansion only.  Do not let the last secondary-pass values mask
    // the command evaluator's automatic variables later in this build plan.
    secondary_auto_vars.clear();

    // Resolve VPATH only after all second-expansion passes have produced the
    // real prerequisites.
    ResolveVpathInputs(&n->actual_inputs);
    ResolveVpathInputs(&n->actual_order_only_inputs);
    for (auto& group : n->double_colon_group_inputs)
      ResolveVpathInputs(&group);

    if (g_flags.warn_phony_looks_real && n->is_phony &&
        output.str().find('/') != std::string::npos) {
      if (g_flags.werror_phony_looks_real) {
        ERROR_LOC(
            n->loc,
            "*** PHONY target \"%s\" looks like a real file (contains a \"/\")",
            output.c_str());
      } else {
        WARN_LOC(n->loc,
                 "warning: PHONY target \"%s\" looks like a real file "
                 "(contains a \"/\")",
                 output.c_str());
      }
    }

    if (!g_flags.writable.empty() && !n->is_phony) {
      bool found = false;
      for (const auto& w : g_flags.writable) {
        if (HasPrefix(output.str(), w)) {
          found = true;
          break;
        }
      }
      if (!found) {
        if (g_flags.werror_writable) {
          ERROR_LOC(n->loc, "*** writing to readonly directory: \"%s\"",
                    output.c_str());
        } else {
          WARN_LOC(n->loc, "warning: writing to readonly directory: \"%s\"",
                   output.c_str());
        }
      }
    }

    if (cur_rule_vars_->empty()) {
      n->rule_vars = NULL;
    } else {
      n->rule_vars = new Vars;
      *n->rule_vars = *cur_rule_vars_;
      // Variable values are installed into the active scope one by one above,
      // but target-specific export attributes are scope metadata rather than
      // variables. Preserve the metadata from the rule scope when materializing
      // the dependency node used by command and Ninja evaluation.
      if (vars != nullptr)
        n->rule_vars->MergeExportStateFrom(*vars);
    }

    for (Symbol output : n->implicit_outputs) {
      done_[output] = n;

      if (g_flags.warn_phony_looks_real && n->is_phony &&
          output.str().find('/') != std::string::npos) {
        if (g_flags.werror_phony_looks_real) {
          ERROR_LOC(n->loc,
                    "*** PHONY target \"%s\" looks like a real file (contains "
                    "a \"/\")",
                    output.c_str());
        } else {
          WARN_LOC(n->loc,
                   "warning: PHONY target \"%s\" looks like a real file "
                   "(contains a \"/\")",
                   output.c_str());
        }
      }

      if (!g_flags.writable.empty() && !n->is_phony) {
        bool found = false;
        for (const auto& w : g_flags.writable) {
          if (HasPrefix(output.str(), w)) {
            found = true;
            break;
          }
        }
        if (!found) {
          if (g_flags.werror_writable) {
            ERROR_LOC(n->loc, "*** writing to readonly directory: \"%s\"",
                      output.c_str());
          } else {
            WARN_LOC(n->loc, "warning: writing to readonly directory: \"%s\"",
                     output.c_str());
          }
        }
      }
    }

    auto inherit_rule_vars = [](DepNode* child, const DepNode* parent) {
      if (parent->rule_vars == nullptr || parent->rule_vars->empty())
        return;
      Vars* merged = new Vars;
      if (child->rule_vars != nullptr) {
        *merged = *child->rule_vars;
        for (const auto& var : *child->rule_vars)
          merged->insert(var);
      }
      for (const auto& var : *parent->rule_vars) {
        if (merged->find(var.first) == merged->end())
          (*merged)[var.first] = var.second;
      }
      merged->MergeExportStateFrom(*parent->rule_vars);
      child->rule_vars = merged;
    };

    // Preserve the makefile spelling while resolving prerequisites.  GNU make
    // selects implicit rules before collapsing path aliases; for example a
    // prerequisite written as arch/x86/kvm/../../../virt/kvm/kvm_main.o can
    // match arch/x86/kvm/%.o even though its canonical graph path is
    // virt/kvm/kvm_main.o.  BuildPlan therefore receives the lexical symbol,
    // while the dependency edge uses the canonical symbol consumed by Ninja.
    auto canonical_symbol = [](Symbol input) {
      std::string path(input.str());
      NormalizeMakePath(&path);
      if (path.empty())
        path = ".";
      return Intern(path);
    };

    auto build_prerequisite = [&](Symbol input) {
      const bool is_implicit_chain_input =
          std::find(n->implicit_inputs.begin(), n->implicit_inputs.end(),
                    input) != n->implicit_inputs.end();
      if (!is_implicit_chain_input || selected_implicit_rule == nullptr)
        return BuildPlan(input, output);

      std::unordered_set<const Rule*> used_rules;
      if (implicit_rules_used != nullptr)
        used_rules = *implicit_rules_used;
      used_rules.insert(selected_implicit_rule);
      return BuildPlan(input, output, &used_rules);
    };

    for (Symbol& input : n->actual_inputs) {
      DepNode* c = build_prerequisite(input);
      Symbol graph_input = canonical_symbol(input);
      const std::string provider = ev_->ResolveVpath(input);
      if (!provider.empty() && c->output == Intern(provider) &&
          !(c->output == input) && !c->has_rule) {
        graph_input = canonical_symbol(c->output);
        if (g_flags.generate_ninja)
          input = c->output;
      }
      inherit_rule_vars(c, n);
      n->deps.push_back({graph_input, c});
      if (low_resolution_.exists(input))
        n->low_resolution_inputs.push_back(graph_input);

      bool is_phony = c->is_phony;
      if (!is_phony && !c->has_rule && g_flags.top_level_phony) {
        is_phony = input.str().find('/') == std::string::npos;
      }
      if (!n->is_phony && is_phony) {
        if (g_flags.werror_real_to_phony) {
          ERROR_LOC(n->loc,
                    "*** real file \"%s\" depends on PHONY target \"%s\"",
                    output.c_str(), input.c_str());
        } else if (g_flags.warn_real_to_phony) {
          WARN_LOC(n->loc,
                   "warning: real file \"%s\" depends on PHONY target \"%s\"",
                   output.c_str(), input.c_str());
        }
      }
    }

    // GNU make adds .EXTRA_PREREQS to the graph of every target, but keeps
    // them out of the automatic prerequisite variables.  Keep that semantic
    // boundary here: actual_inputs remains the source for $^/$+/$<, while
    // this separate edge participates only in scheduling and freshness.
    for (Symbol input : extra_prereqs_) {
      if (input == n->output)
        continue;
      DepNode* c = BuildPlan(input, output);
      Symbol graph_input = canonical_symbol(input);
      inherit_rule_vars(c, n);
      if (std::find_if(n->deps.begin(), n->deps.end(),
                       [graph_input](const NamedDepNode& dep) {
                         return dep.first == graph_input;
                       }) == n->deps.end())
        n->deps.push_back({graph_input, c});
    }

    // GNU make gives a target-specific .NOTPARALLEL declaration the same
    // ordering effect as inserting .WAIT between that target's prerequisites.
    // Keep unrelated targets parallel and preserve automatic variables by
    // changing only the graph edges, not actual_inputs.
    if (notparallel_targets_.exists(n->lexical_output) &&
        n->wait_groups.empty()) {
      for (Symbol input : n->actual_inputs)
        n->wait_groups.push_back({input});
      if (!n->actual_order_only_inputs.empty())
        n->wait_groups.push_back(n->actual_order_only_inputs);
    }

    // Order-only prerequisites participate in graph ordering just like
    // ordinary prerequisites, while remaining excluded from automatic
    // prerequisite variables.  Add their graph edges before constructing
    // .WAIT barriers so a barrier can reference either kind of prerequisite.
    for (Symbol& input : n->actual_order_only_inputs) {
      DepNode* c = build_prerequisite(input);
      Symbol graph_input = canonical_symbol(input);
      const std::string provider = ev_->ResolveVpath(input);
      if (!provider.empty() && c->output == Intern(provider) &&
          !(c->output == input) && !c->has_rule) {
        graph_input = canonical_symbol(c->output);
        if (g_flags.generate_ninja)
          input = c->output;
      }
      inherit_rule_vars(c, n);
      n->order_onlys.push_back({graph_input, c});
    }

    // Represent each .WAIT boundary with a generated stamp.  Prerequisites
    // after the boundary depend on a stamp built from all earlier groups;
    // prerequisites within the same group remain parallel.  The original
    // actual_inputs are deliberately untouched, so automatic variables keep
    // GNU make's prerequisite values.
    if (n->wait_groups.size() > 1) {
      const std::vector<NamedDepNode> original_deps = n->deps;
      const std::vector<NamedDepNode> original_order_onlys = n->order_onlys;
      auto find_original = [&](Symbol input) -> NamedDepNode* {
        const Symbol graph_input = canonical_symbol(input);
        for (const NamedDepNode& dep : original_deps) {
          if (dep.first == graph_input)
            return const_cast<NamedDepNode*>(&dep);
        }
        for (const NamedDepNode& dep : original_order_onlys) {
          if (dep.first == graph_input)
            return const_cast<NamedDepNode*>(&dep);
        }
        return nullptr;
      };
      size_t wait_stamp_number = 0;
      auto make_wait_stamp = [&](size_t number,
                                 const std::vector<NamedDepNode>& deps) {
        // Encode every byte, so paths such as a/b and a_b cannot share a
        // barrier. Split long encodings to stay within filename limits.
        static constexpr char hex[] = "0123456789abcdef";
        std::string name = ".kati_wait_";
        size_t encoded = 0;
        for (unsigned char byte : n->output.str()) {
          if (encoded && encoded % 100 == 0)
            name += '/';
          name += hex[byte >> 4];
          name += hex[byte & 15];
          encoded += 2;
        }
        name += "_" + std::to_string(number);
        DepNode* stamp = g_dep_node_pool
                             .emplace_back(std::make_unique<DepNode>(
                                 Intern(name), false, false))
                             .get();
        stamp->has_rule = true;
        stamp->loc = n->loc;
        std::string command = "touch " + name;
        if (encoded > 100)
          command = "mkdir -p " + std::string(Dirname(name)) + " && " + command;
        stamp->cmds.push_back(Value::NewLiteral(Intern(command).str()));
        stamp->deps = deps;
        return stamp;
      };

      std::vector<NamedDepNode> previous;
      for (size_t group = 0; group + 1 < n->wait_groups.size(); ++group) {
        for (Symbol input : n->wait_groups[group]) {
          NamedDepNode* dep = find_original(input);
          if (dep != nullptr)
            previous.push_back(*dep);
        }
        if (previous.empty())
          continue;
        DepNode* barrier = make_wait_stamp(wait_stamp_number++, previous);
        for (Symbol input : n->wait_groups[group + 1]) {
          NamedDepNode* dep = find_original(input);
          if (dep == nullptr)
            continue;
          // Attach the barrier to the later prerequisite itself. A wrapper
          // with both edges would leave those edges siblings in Ninja and
          // would not establish an ordering relationship.
          dep->second->deps.push_back({barrier->output, barrier});
        }
      }
    }

    for (Symbol validation : n->actual_validations) {
      if (!g_flags.use_ninja_validations) {
        ERROR_LOC(
            n->loc,
            ".KATI_VALIDATIONS not allowed without --use_ninja_validations");
      }
      DepNode* c = BuildPlan(validation, output);
      n->validations.push_back({canonical_symbol(validation), c});
    }

    // Glibc and other projects commonly define a generic `%/` rule for
    // creating directories and rely on make to build a target's parent
    // directory first. Ninja's writer supplies directory edges for file
    // outputs, but direct Kati execution must resolve that make rule too.
    if (add_parent_directory_edges_ && !g_flags.generate_ninja &&
        !n->is_phony) {
      const std::string target_path = n->output.str();
      const size_t slash = target_path.find_last_of('/');
      if (slash != std::string::npos && slash + 1 < target_path.size()) {
        const std::string parent_path = target_path.substr(0, slash + 1);
        struct stat parent_stat;
        const bool local_parent_exists =
            stat(parent_path.c_str(), &parent_stat) == 0 &&
            S_ISDIR(parent_stat.st_mode);
        Symbol parent_symbol = Intern(parent_path);
        const std::string vpath_parent = ev_->ResolveVpath(parent_symbol);
        const bool vpath_parent_exists =
            !vpath_parent.empty() &&
            stat(vpath_parent.c_str(), &parent_stat) == 0 &&
            S_ISDIR(parent_stat.st_mode);
        if (!local_parent_exists && !vpath_parent_exists) {
          const bool already_ordered =
              std::any_of(n->order_onlys.begin(), n->order_onlys.end(),
                          [parent_symbol](const NamedDepNode& dep) {
                            return dep.first == parent_symbol;
                          });
          if (!already_ordered) {
            DepNode* parent = BuildPlan(parent_symbol, n->output);
            // Only add a dependency when make has a way to build the missing
            // directory. Some recipes create their own output directory (for
            // example Automake's .deps/.dirstamp recipe); inventing a hard
            // dependency on an unruled directory makes Kati reject a build
            // that make accepts.
            if (parent->has_rule || parent->is_phony)
              n->order_onlys.push_back({parent_symbol, parent});
          }
        }
      }
    }

    // Block on werror_writable/werror_phony_looks_real, because otherwise we
    // can't rely on is_phony being valid for this check.
    if (!n->is_phony && n->cmds.empty() && g_flags.werror_writable &&
        g_flags.werror_phony_looks_real) {
      if (n->deps.empty() && n->order_onlys.empty()) {
        if (g_flags.werror_real_no_cmds_or_deps) {
          ERROR_LOC(
              n->loc,
              "*** target \"%s\" has no commands or deps that could create it",
              output.c_str());
        } else if (g_flags.warn_real_no_cmds_or_deps) {
          WARN_LOC(n->loc,
                   "warning: target \"%s\" has no commands or deps that could "
                   "create it",
                   output.c_str());
        }
      } else {
        if (n->actual_inputs.size() == 1) {
          if (g_flags.werror_real_no_cmds) {
            ERROR_LOC(n->loc,
                      "*** target \"%s\" has no commands. Should \"%s\" be "
                      "using .KATI_IMPLICIT_OUTPUTS?",
                      output.c_str(), n->actual_inputs[0].c_str());
          } else if (g_flags.warn_real_no_cmds) {
            WARN_LOC(n->loc,
                     "warning: target \"%s\" has no commands. Should \"%s\" be "
                     "using .KATI_IMPLICIT_OUTPUTS?",
                     output.c_str(), n->actual_inputs[0].c_str());
          }
        } else {
          if (g_flags.werror_real_no_cmds) {
            ERROR_LOC(
                n->loc,
                "*** target \"%s\" has no commands that could create output "
                "file. Is a dependency missing .KATI_IMPLICIT_OUTPUTS?",
                output.c_str());
          } else if (g_flags.warn_real_no_cmds) {
            WARN_LOC(
                n->loc,
                "warning: target \"%s\" has no commands that could create "
                "output file. Is a dependency missing .KATI_IMPLICIT_OUTPUTS?",
                output.c_str());
          }
        }
      }
    }

    n->has_rule = true;
    n->is_default_target = first_rule_ == output;
    return n;
  }

  Evaluator* ev_;
  struct TargetComp {
    bool operator()(const Symbol& lhs, const Symbol& rhs) const {
      return lhs.str() < rhs.str();
    }
  };
  std::map<Symbol, RuleMerger, TargetComp> rules_;
  const std::unordered_map<Symbol, Vars*>& rule_vars_;
  bool add_parent_directory_edges_;
  std::unique_ptr<Vars> cur_rule_vars_;

  std::unique_ptr<RuleTrie> implicit_rules_;
  typedef std::unordered_map<std::string_view,
                             std::vector<std::shared_ptr<Rule>>>
      SuffixRuleMap;
  SuffixRuleMap suffix_rules_;
  bool suffixes_specified_ = false;
  std::unordered_set<std::string> active_suffixes_;
  std::vector<std::string> active_suffix_order_;
  std::unordered_set<std::string> dotted_suffixes_;
  std::unordered_set<std::string> dotless_suffixes_;
  std::vector<std::shared_ptr<Rule>> dotless_suffix_rules_;

  Symbol first_rule_;
  std::shared_ptr<Rule> default_rule_;
  std::unordered_map<Symbol, DepNode*> done_;
  SymbolSet phony_;
  SymbolSet restat_;
  Symbol depfile_var_name_;
  Symbol implicit_outputs_var_name_;
  Symbol ninja_pool_var_name_;
  Symbol validations_var_name_;
  Symbol tags_var_name_;
  bool notparallel_ = false;
  SymbolSet notparallel_targets_;
  bool delete_on_error_ = false;
  SymbolSet precious_;
  SymbolSet intermediate_;
  SymbolSet explicit_prerequisites_;
  SymbolSet implicit_nodes_;
  SymbolSet secondary_;
  bool secondary_all_ = false;
  bool oneshell_ = false;
  bool ignore_all_ = false;
  SymbolSet ignored_;
  bool silent_all_ = false;
  SymbolSet silent_;
  bool export_all_variables_ = false;
  std::vector<Symbol> extra_prereqs_;
  SymbolSet low_resolution_;
};

void MakeDep(Evaluator* ev,
             const std::vector<const Rule*>& rules,
             const std::unordered_map<Symbol, Vars*>& rule_vars,
             const std::vector<Symbol>& targets,
             std::vector<NamedDepNode>* nodes,
             bool add_parent_directory_edges) {
  DepBuilder db(ev, rules, rule_vars, add_parent_directory_edges);
  ScopedTimeReporter tr("make dep (build)");
  db.Build(targets, nodes);
}

bool IsSpecialTarget(Symbol output) {
  const std::string_view name = output.str();
  if (name == "." || name.size() < 2 || name[0] != '.')
    return false;

  // A leading dot does not by itself make a target special: build systems
  // routinely use ordinary hidden stamp files such as .binman_stamp. Only
  // GNU make's special declarations belong here; suffix-rule recognition is
  // handled separately with the active suffix list.
  static constexpr std::string_view kSpecialTargets[] = {
      ".DEFAULT",
      ".DELETE_ON_ERROR",
      ".EXPORT_ALL_VARIABLES",
      ".EXTRA_PREREQS",
      ".IGNORE",
      ".INTERMEDIATE",
      ".LOW_RESOLUTION_TIME",
      ".NOTPARALLEL",
      ".ONESHELL",
      ".PHONY",
      ".POSIX",
      ".PRECIOUS",
      ".SECONDARY",
      ".SECONDEXPANSION",
      ".SILENT",
      ".SUFFIXES",
      ".WAIT",
  };
  if (std::find(std::begin(kSpecialTargets), std::end(kSpecialTargets), name) !=
      std::end(kSpecialTargets))
    return true;

  return false;
}
