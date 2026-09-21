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

#include <algorithm>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>

#include "eval.h"
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

static std::vector<std::unique_ptr<DepNode>> g_dep_node_pool;

static Symbol ReplaceSuffix(Symbol s, Symbol newsuf) {
  std::string r{StripExt(s.str())};
  r += '.';
  r += newsuf.str();
  return Intern(r);
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
  Pattern pat(r.output_patterns[0].str());
  for (Symbol input : inputs) {
    std::string buf;
    pat.AppendSubst(output.str(), input.str(), &buf);
    // Keep the lexical spelling through rule lookup and command expansion.
    // GNU make permits a rule to publish a target as `dir/../child`, and a
    // recursive rule may be registered under that exact spelling.  The graph
    // edge is canonicalized later, after the provider has been selected.
    // `.` is a real make prerequisite, while an empty prerequisite is not.
    if (buf.empty())
      buf = ".";
    out_inputs->push_back(Intern(buf));
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

  void Add(std::string_view name, const Rule* rule) {
    if (name.empty() || name[0] == '%') {
      rules_.push_back(Entry(rule, name));
      return;
    }
    const char c = name[0];
    auto p = children_.emplace(c, nullptr);
    if (p.second) {
      p.first->second = new RuleTrie();
    }
    p.first->second->Add(name.substr(1), rule);
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

  void FillDepNodeFromRule(Symbol output, const Rule* r, DepNode* n) const {
    if (is_double_colon)
      copy(r->cmds.begin(), r->cmds.end(), back_inserter(n->cmds));

    if (!r->secondary_expansion) {
      ApplyOutputPattern(*r, output, r->inputs, &n->actual_inputs);
      ApplyOutputPattern(*r, output, r->order_only_inputs,
                         &n->actual_order_only_inputs);
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
      FillDepNodeFromRule(output, pattern_rule, n);
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
      FillSecondaryInputsForRule(output, pattern_rule, n, ev);
    for (const Rule* r : rules) {
      if (r != primary_rule)
        FillSecondaryInputsForRule(output, r, n, ev);
    }
  }

  void FillOneSecondaryInput(Symbol output,
                             const Rule* r,
                             DepNode* n,
                             Evaluator* ev) const {
    FillSecondaryInputsForRule(output, r, n, ev);
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
                                  Evaluator* ev) const {
    if (!r || !r->secondary_expansion)
      return;
    std::vector<Symbol> inputs;
    std::vector<Symbol> order_only_inputs;
    std::vector<std::vector<Symbol>> wait_groups;
    r->ParseSecondaryInputs(ev, &inputs, &order_only_inputs, &wait_groups);
    ApplyOutputPattern(*r, output, inputs, &n->actual_inputs);
    ApplyOutputPattern(*r, output, order_only_inputs,
                       &n->actual_order_only_inputs);
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
  // If there is only a single dot or a third dot, this is not a suffix rule.
  if (dot_index == std::string::npos ||
      rest.substr(dot_index + 1).find('.') != std::string::npos)
    return false;
  return true;
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
             const std::unordered_map<Symbol, Vars*>& rule_vars)
      : ev_(ev),
        rule_vars_(rule_vars),
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
      if (rule->inputs.empty()) {
        active_suffixes_.clear();
      } else {
        for (Symbol suffix : rule->inputs) {
          std::string name = suffix.str();
          if (!name.empty() && name.front() == '.')
            name.erase(name.begin());
          active_suffixes_.insert(std::move(name));  // active GNU suffix
        }
      }
      suffixes_specified_ = true;
    }
  }

  bool IsActiveSuffixRule(Symbol output) const {
    if (!IsSuffixRule(output))
      return false;

    const std::string_view rest = std::string_view(output.str()).substr(1);
    const size_t dot = rest.find('.');
    if (dot == std::string_view::npos)
      return false;
    const std::string input_suffix(rest.substr(0, dot));
    const std::string output_suffix(rest.substr(dot + 1));

    if (suffixes_specified_)
      return active_suffixes_.count(input_suffix) != 0 &&
             active_suffixes_.count(output_suffix) != 0;

    // These are GNU make's standard suffixes.  A dotted hidden file such as
    // .kconfig.d is an ordinary target unless its suffixes are active; it
    // must not be discarded as a suffix rule merely because it contains two
    // dots.
    static constexpr const char* kDefaultSuffixes[] = {
        "out", "a", "ln", "o", "c", "cc", "C", "cpp", "p", "f",
        "F",   "m", "r",  "y", "l", "ym", "lm", "s", "S", "mod",
        "sym", "def", "h", "info", "dvi", "tex", "texinfo", "texi",
        "txinfo", "w", "ch", "web", "sh", "elc", "el"};
    auto is_default_suffix = [&](const std::string& suffix) {
      for (const char* candidate : kDefaultSuffixes)
        if (suffix == candidate)
          return true;
      return false;
    };
    return is_default_suffix(input_suffix) &&
           is_default_suffix(output_suffix);
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
    static const char* kUnsupportedBuiltinTargets[] = {".NOTPARALLEL",
                                                       NULL};
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
      if (rules_.find(input) != rules_.end() || phony_.exists(input) ||
          (stat(input.str().c_str(), &st) == 0 && S_ISDIR(st.st_mode)))
        continue;
      std::string resolved = ev_->ResolveVpath(input);
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
    for (auto& p : suffix_rules_) {
      reverse(p.second.begin(), p.second.end());
    }
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

  bool PopulateSuffixRule(const Rule* rule, Symbol output) {
    if (!IsActiveSuffixRule(output))
      return false;

    if (g_flags.werror_suffix_rules) {
      ERROR_LOC(rule->loc, "*** suffix rules are obsolete: %s", output.c_str());
    } else if (g_flags.warn_suffix_rules) {
      WARN_LOC(rule->loc, "warning: suffix rules are deprecated: %s",
               output.c_str());
    }

    const std::string_view rest = std::string_view(output.str()).substr(1);
    size_t dot_index = rest.find('.');

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
    suffix_rules_[output_suffix].push_back(r);
    return true;
  }

  void PopulateExplicitRule(const Rule* rule) {
    for (Symbol output : rule->outputs) {
      if (!first_rule_.IsValid() && !IsSpecialTarget(output) &&
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
    if (Exists(output))
      return true;

    std::vector<const Rule*> irules;
    implicit_rules_->Get(output.str(), &irules);

    for (auto iter = irules.rbegin(); iter != irules.rend(); ++iter) {
      const Rule* rule = *iter;

      // GNU make does not allow an implicit rule to be used more than once
      // in a single implicit-rule chain.  This also prevents pathological
      // chains such as foo_shipped_shipped_shipped...
      if (rule == current_rule || used_rules->find(rule) != used_rules->end())
        continue;

      Symbol matched;
    for (Symbol output_pattern : rule->output_patterns) {
      Pattern pat(output_pattern.str());
      if (pat.Match(output.str())) {
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
      if (rule->secondary_expansion && input.str().find('$') !=
                                              std::string_view::npos)
        continue;
      std::string buf;
      pat.AppendSubst(output.str(), input.str(), &buf);

      if (!CanBuildImplicit(Intern(buf), rule, used_rules)) {
        ok = false;
        break;
      }
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
      Pattern pat(output_pattern.str());
      if (pat.Match(output.str())) {
        bool ok = true;
        for (Symbol input : rule->inputs) {
          if (rule->secondary_expansion && input.str().find('$') !=
                                                  std::string_view::npos)
            continue;
          std::string buf;
          pat.AppendSubst(output.str(), input.str(), &buf);

          Symbol prerequisite = Intern(buf);
          if (!Exists(prerequisite)) {
            std::unordered_set<const Rule*> used_rules;
            used_rules.insert(rule);

            if (!CanBuildImplicit(prerequisite, rule, &used_rules)) {
              ok = false;
              break;
            }
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
    Vars* result = vars;

    auto found = rule_vars_.find(output);
    if (found != rule_vars_.end()) {
      if (result == NULL) {
        result = found->second;
      } else {
        result = new Vars(*found->second);
        for (auto p : *vars) {
          (*result)[p.first] = p.second;
        }
      }
    }

    std::string_view target = output.str();
    if (HasPrefix(target, "./"))
      target.remove_prefix(2);

    for (const auto& p : rule_vars_) {
      std::string_view key = p.first.str();

      if (key.find('%') == std::string_view::npos)
        continue;

      Pattern pat(key);
      if (!pat.Match(target))
        continue;

      if (result == NULL) {
        result = p.second;
      } else {
        Vars* merged = new Vars(*p.second);
        for (auto v : *result) {
          (*merged)[v.first] = v.second;
        }
        result = merged;
      }
    }

    return result;
  }

  bool PickRule(Symbol output,
                DepNode* n,
                const RuleMerger** out_rule_merger,
                std::shared_ptr<Rule>* pattern_rule,
                Vars** out_var) {
    const RuleMerger* rule_merger = LookupRuleMerger(output);
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

    std::vector<const Rule*> irules;
    implicit_rules_->Get(output.str(), &irules);

    for (auto iter = irules.rbegin(); iter != irules.rend(); ++iter) {
      bool picked = CanPickImplicitRule(*iter, output, n, pattern_rule);

      if (!picked)
        continue;
      CHECK((*pattern_rule)->output_patterns.size() == 1);
      vars = MergeImplicitRuleVars(output, vars);

      *out_var = vars;
      if (rule_merger) {
        return true;
      }
      return true;
    }

    std::string_view output_suffix = GetExt(output.str());
    if (output_suffix.empty() || output_suffix.front() != '.')
      return rule_merger != nullptr;
    output_suffix = output_suffix.substr(1);

    SuffixRuleMap::const_iterator found = suffix_rules_.find(output_suffix);
    if (found == suffix_rules_.end())
      return rule_merger != nullptr;

    for (const std::shared_ptr<Rule>& irule : found->second) {
      CHECK(irule->inputs.size() == 1);
      Symbol input = ReplaceSuffix(output, irule->inputs[0]);
      if (!Exists(input)) {
        std::unordered_set<const Rule*> used_rules;
        used_rules.insert(irule.get());
        if (!CanBuildImplicit(input, irule.get(), &used_rules))
          continue;
      }

      *pattern_rule = irule;
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

  DepNode* BuildPlan(Symbol output, Symbol needed_by) {
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
                      !secondary_all_ && !secondary_.exists(n->lexical_output) &&
                      !n->precious;
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
      // Rule matching must see the lexical spelling first.  Pattern rules
      // commonly use the requesting directory's $(obj)/$(src) relationship;
      // canonicalizing a parent-relative prerequisite before this lookup can
      // incorrectly fall through to a generic built-in rule.
      if (PickRule(output, n, &lexical_merger, &lexical_pattern,
                   &lexical_vars)) {
        // Keep the selected rule and its lexical prerequisites.  The output
        // name is canonicalized after rule expansion below.
      } else {
      n->is_phony = true;
      DepNode* canonical_node = BuildPlan(canonical, output);
      // A parent-relative spelling of an ordinary source file does not need
      // a graph alias.  Keeping the alias would emit, for example,
      // `out/../inputs/x: phony inputs/x`; Ninja canonicalizes both names and
      // turns that harmless make spelling into a phony self-cycle.  There is
      // no recipe or phony semantics to preserve for a rule-less real file,
      // so use its canonical provider directly.
      if (!canonical_node->has_rule && !canonical_node->is_phony &&
          canonical_node->deps.empty() &&
          canonical_node->order_onlys.empty() &&
          canonical_node->validations.empty()) {
        return canonical_node;
      }
      // The canonical node may have been discovered earlier through a
      // different prerequisite path.  Its recipe still needs the active
      // target-specific scope from this lexical alias (for example flags
      // attached by the directory that references an external source).
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
      n->deps.push_back({canonical, canonical_node});
      return n;
      }
    }

    const RuleMerger* rule_merger = nullptr;
    std::shared_ptr<Rule> pattern_rule;
    Vars* vars;
    bool picked = PickRule(output, n, &rule_merger, &pattern_rule, &vars);

    if (!picked) {
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
      if (!PickRule(output, n, &rule_merger, &pattern_rule, &vars))
        return n;
    }

    if (rule_merger)
      rule_merger->FillDepNode(output, pattern_rule.get(), n);
    else
      RuleMerger().FillDepNode(output, pattern_rule.get(), n);

    // GNU make treats files introduced only by an implicit-rule chain as
    // intermediate files.  They are removed after a successful build unless
    // the makefile explicitly protects them with .SECONDARY or .PRECIOUS.
    // The dependency graph already records whether this node was reached as
    // a prerequisite (needed_by) and whether an implicit/pattern rule was
    // selected (pattern_rule), so use those generic facts instead of
    // guessing from target names or adding project-specific rules.
    if (!normalized_output.empty() && normalized_output != output.str())
      n->output = Intern(normalized_output);

    if (pattern_rule) {
      implicit_nodes_.insert(n->output);
      implicit_nodes_.insert(n->lexical_output);
    }

    // An implicit target directly required by an explicit rule is a requested
    // result, not an intermediate.  Only implicit links below another
    // implicit link are automatic intermediates in GNU make's chain model.
    if (!needed_by.empty() && pattern_rule &&
        implicit_nodes_.exists(needed_by) && !n->precious &&
        !secondary_all_ && !secondary_.exists(n->lexical_output))
      n->intermediate = true;

    // A target-specific rule can provide the commands while a suffix rule
    // provides the source relationship.  GNU make still defines $* for this
    // case; preserve that stem for command expansion even though the suffix
    // rule was not copied into pattern_rule because the explicit rule won.
    if (!n->output_pattern.IsValid() && !suffix_rules_.empty()) {
      std::string_view suffix = GetExt(output.str());
      if (!suffix.empty() && suffix.front() == '.' &&
          [&] {
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
                "out", "a", "ln", "o", "c", "cc", "C", "cpp", "p",
                "f",   "F", "m",  "r", "y", "l", "ym", "lm", "s",
                "S",   "mod", "sym", "def", "h", "info", "dvi", "tex",
                "texinfo", "texi", "txinfo", "w", "ch", "web", "sh",
                "elc", "el"};
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
            old_var->Eval(ev_, s.get());
            if (!s->empty())
              *s += ' ';
            new_var->Eval(ev_, s.get());
            new_var =
                new SimpleVar(*s, old_var->Origin(), frame.Current(), n->loc);
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
      if (archive_open != std::string::npos &&
          output_name.back() == ')' && archive_open + 1 < output_name.size()) {
        archive_member = output_name.substr(archive_open + 1,
                                             output_name.size() - archive_open - 2);
      }
      const std::pair<const char*, std::string> automatic[] = {
          {"@", n->lexical_output.str()}, {"%", archive_member}, {"<", first},
          {"^", unique},          {"+", all},  {"*", stem}};
      for (const auto& item : automatic) {
        secondary_auto_vars.emplace_back(new ScopedVar(
            cur_rule_vars_.get(), Intern(item.first),
            new SimpleVar(item.second, VarOrigin::AUTOMATIC, frame.Current(),
                          n->loc)));
      }
      active_merger->FillOneSecondaryInput(n->lexical_output, secondary_rule,
                                           n, ev_);
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

    for (Symbol input : n->actual_inputs) {
      DepNode* c = BuildPlan(input, output);
      Symbol graph_input = canonical_symbol(input);
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
    for (Symbol input : n->actual_order_only_inputs) {
      DepNode* c = BuildPlan(input, output);
      Symbol graph_input = canonical_symbol(input);
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
        std::string name = ".kati_wait/" + n->output.str() + "/" +
                           std::to_string(number);
        std::replace(name.begin(), name.end(), '/', '_');
        DepNode* stamp =
            g_dep_node_pool.emplace_back(std::make_unique<DepNode>(
                                             Intern(name), false, false))
                .get();
        stamp->has_rule = true;
        stamp->loc = n->loc;
        stamp->cmds.push_back(
            Value::NewLiteral(Intern("touch " + name).str()));
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
  std::unique_ptr<Vars> cur_rule_vars_;

  std::unique_ptr<RuleTrie> implicit_rules_;
  typedef std::unordered_map<std::string_view,
                             std::vector<std::shared_ptr<Rule>>>
      SuffixRuleMap;
  SuffixRuleMap suffix_rules_;
  bool suffixes_specified_ = false;
  std::unordered_set<std::string> active_suffixes_;

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
             std::vector<NamedDepNode>* nodes) {
  DepBuilder db(ev, rules, rule_vars);
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
      ".DEFAULT",          ".DELETE_ON_ERROR", ".EXPORT_ALL_VARIABLES",
      ".EXTRA_PREREQS",    ".IGNORE",          ".INTERMEDIATE",
      ".LOW_RESOLUTION_TIME", ".NOTPARALLEL",  ".ONESHELL",
      ".PHONY",            ".POSIX",           ".PRECIOUS",
      ".SECONDARY",        ".SECONDEXPANSION", ".SILENT",
      ".SUFFIXES",         ".WAIT",
  };
  if (std::find(std::begin(kSpecialTargets), std::end(kSpecialTargets), name) !=
      std::end(kSpecialTargets))
    return true;

  return false;
}
