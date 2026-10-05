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

// +build ignore

#include "rule.h"

#include <cctype>

#include "eval.h"
#include "expr.h"
#include "fileutil.h"
#include "log.h"
#include "parser.h"
#include "stringprintf.h"
#include "strutil.h"
#include "symtab.h"

namespace {

// Escaped references survive the first expansion as make expressions. Their
// argument whitespace must not split the expression into file names.
std::vector<std::string_view> PrerequisiteWords(std::string_view text,
                                                bool deferred) {
  std::vector<std::string_view> words;
  if (!deferred) {
    for (std::string_view word : WordScanner(text))
      words.push_back(word);
    return words;
  }
  std::vector<char> closing;
  size_t start = std::string_view::npos;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (closing.empty() && isspace(static_cast<unsigned char>(c))) {
      if (start != std::string_view::npos)
        words.push_back(text.substr(start, i - start));
      start = std::string_view::npos;
      continue;
    }
    if (start == std::string_view::npos)
      start = i;
    if (c == '$' && i + 1 < text.size() &&
        (text[i + 1] == '(' || text[i + 1] == '{')) {
      closing.push_back(text[++i] == '(' ? ')' : '}');
    } else if (!closing.empty()) {
      if (c == '(' || c == '{')
        closing.push_back(c == '(' ? ')' : '}');
      else if (c == closing.back())
        closing.pop_back();
    }
  }
  if (start != std::string_view::npos)
    words.push_back(text.substr(start));
  return words;
}

}  // namespace

Rule::Rule()
    : is_double_colon(false),
      is_suffix_rule(false),
      secondary_expansion(false),
      has_wait(false),
      cmd_lineno(0) {}

void Rule::ParseSecondaryInputs(
    Evaluator* ev,
    std::vector<Symbol>* inputs_out,
    std::vector<Symbol>* order_only_inputs_out,
    std::vector<std::vector<Symbol>>* wait_groups_out) const {
  if (!secondary_expansion)
    return;

  // The first pass performed by EvalRule has already expanded ordinary
  // references and converted escaped $$ references to literal '$' text.
  // Parsing that result again is the second expansion pass.  Use a temporary
  // Rule so path normalization, wildcard handling, and the order-only '|'
  // separator remain identical to ordinary prerequisites.
  Loc secondary_loc = loc;
  std::string_view prerequisites = secondary_prerequisites;
  const size_t static_pattern_colon = prerequisites.find(':');
  if (static_pattern_colon != std::string_view::npos)
    prerequisites = prerequisites.substr(static_pattern_colon + 1);
  bool is_order_only = false;
  bool saw_wait = false;
  std::vector<Symbol> wait_group;
  auto finish_group = [&]() {
    if (wait_groups_out)
      wait_groups_out->push_back(std::move(wait_group));
    wait_group.clear();
  };
  for (std::string_view input : PrerequisiteWords(prerequisites, true)) {
    if (input == "|") {
      is_order_only = true;
      continue;
    }
    // Ordinary prerequisites were already retained from the first pass.
    // Only escaped expressions need the second pass; parsing every word here
    // would duplicate ordinary prerequisites and corrupt $+/dependency order.
    if (input == ".WAIT") {
      saw_wait = true;
      finish_group();
      continue;
    }
    if (input.find('$') == std::string_view::npos) {
      wait_group.push_back(Intern(std::string(input)));
      continue;
    }
    std::string expanded;
    const std::string expression(input);
    auto cached = secondary_exprs.find(expression);
    if (cached == secondary_exprs.end()) {
      Value* parsed_expr = ParseExpr(&secondary_loc, input);
      cached = secondary_exprs.emplace(expression, parsed_expr).first;
    }
    cached->second->Eval(ev, &expanded);
    Rule parsed;
    parsed.ParseInputs(expanded);
    if (parsed.has_wait) {
      saw_wait = true;
      for (const std::vector<Symbol>& group : parsed.wait_groups) {
        wait_group.insert(wait_group.end(), group.begin(), group.end());
        finish_group();
      }
    } else {
      wait_group.insert(wait_group.end(), parsed.inputs.begin(),
                        parsed.inputs.end());
      wait_group.insert(wait_group.end(), parsed.order_only_inputs.begin(),
                        parsed.order_only_inputs.end());
    }
    if (is_order_only) {
      order_only_inputs_out->insert(order_only_inputs_out->end(),
                                    parsed.inputs.begin(), parsed.inputs.end());
      order_only_inputs_out->insert(order_only_inputs_out->end(),
                                    parsed.order_only_inputs.begin(),
                                    parsed.order_only_inputs.end());
    } else {
      inputs_out->insert(inputs_out->end(), parsed.inputs.begin(),
                         parsed.inputs.end());
      order_only_inputs_out->insert(order_only_inputs_out->end(),
                                    parsed.order_only_inputs.begin(),
                                    parsed.order_only_inputs.end());
    }
  }
  if (saw_wait)
    finish_group();
}

void Rule::ParseInputs(const std::string_view& inputs_str) {
  bool is_order_only = false;
  std::vector<Symbol> wait_group;
  auto add_input = [&](Symbol input) {
    (is_order_only ? order_only_inputs : inputs).push_back(input);
    wait_group.push_back(input);
  };
  for (auto const& input : PrerequisiteWords(inputs_str, secondary_expansion)) {
    if (input == "|") {
      is_order_only = true;
      continue;
    }
    if (input == ".WAIT") {
      has_wait = true;
      wait_groups.push_back(std::move(wait_group));
      wait_group.clear();
      continue;
    }

    const std::string_view trimmed_input = TrimLeadingCurdir(input);
    const bool current_directory = IsCurrentDirectoryPath(input);
    std::string trimmed(trimmed_input);
    // Keep parent-relative names intact.  GNU make uses the spelling of a
    // prerequisite when matching it against pattern rules; collapsing
    // "obj/../../../src/foo.o" to "src/foo.o" can change which rule applies.
    // In particular, it would turn an output-relative prerequisite into a
    // built-in .c.o fallback.  Ordinary paths are still canonicalized.
    if (!(secondary_expansion && input.find('$') != std::string_view::npos) &&
        trimmed.find("../") == std::string::npos && trimmed != "..")
      NormalizeMakePath(&trimmed);
    // Keep a standalone current-directory prerequisite distinct from an
    // empty prerequisite. Recursive makefiles may use '.' as the dependency
    // that triggers the recursive build of the output directory.
    if (trimmed.empty() && current_directory)
      trimmed = ".";
    const bool has_wildcard = trimmed.find_first_of("*?[") != std::string::npos;

    if (has_wildcard) {
      const auto& files = Glob(trimmed);

      if (!files.empty()) {
        for (const std::string& file : files) {
          std::string normalized_file(TrimLeadingCurdir(file));
          if (normalized_file.find("../") == std::string::npos &&
              normalized_file != "..")
            NormalizePath(&normalized_file);
          Symbol input_sym = Intern(normalized_file);
          add_input(input_sym);
        }
        continue;
      }
    }

    Symbol input_sym = Intern(trimmed);
    add_input(input_sym);
  }
  if (has_wait)
    wait_groups.push_back(std::move(wait_group));
}

void Rule::ParsePrerequisites(const std::string_view& line,
                              size_t separator_pos,
                              const RuleStmt* rule_stmt) {
  // line is either
  //    prerequisites [ ; command ]
  // or
  //    target-prerequisites : prereq-patterns [ ; command ]
  // First, separate command. At this point separator_pos should point to ';'
  // unless null.
  std::string_view prereq_string = line;
  if (separator_pos != std::string::npos) {
    CHECK(line[separator_pos] == ';');
    // This command came from an expanded rule line (for example, a rule
    // emitted by a variable reference). Parse it as a make expression so
    // escaped references such as $$(compile-command.c) receive their normal
    // second expansion when the rule is executed.
    Loc command_loc = rule_stmt->loc();
    // ParseExpr stores string_views into its input in literal nodes. The
    // expanded rule line is temporary, so intern the command before parsing
    // it to keep those views valid for the lifetime of the rule.
    std::string_view command =
        Intern(TrimLeftSpace(line.substr(separator_pos + 1))).str();
    cmds.push_back(ParseExpr(&command_loc, command, ParseExprOpt::COMMAND));
    prereq_string = line.substr(0, separator_pos);
  }

  if ((separator_pos = prereq_string.find(':')) == std::string::npos) {
    // Simple prerequisites
    ParseInputs(prereq_string);
    return;
  }

  // Static pattern rule.
  if (!output_patterns.empty()) {
    ERROR_LOC(loc, "*** mixed implicit and normal rules: deprecated syntax");
  }

  // Empty static patterns should not produce rules, but need to eat the
  // commands So return a rule with no outputs nor output_patterns
  if (outputs.empty()) {
    return;
  }

  std::string_view target_prereq = prereq_string.substr(0, separator_pos);
  std::string_view prereq_patterns = prereq_string.substr(separator_pos + 1);

  for (std::string_view target_pattern : WordScanner(target_prereq)) {
    std::string normalized_target_pattern(TrimLeadingCurdir(target_pattern));
    NormalizePath(&normalized_target_pattern);
    for (Symbol target : outputs) {
      if (!Pattern(normalized_target_pattern).Match(target.str())) {
        WARN_LOC(loc, "target `%s' doesn't match the target pattern",
                 target.c_str());
      }
    }
    output_patterns.push_back(Intern(normalized_target_pattern));
  }

  if (output_patterns.empty()) {
    ERROR_LOC(loc, "*** missing target pattern.");
  }
  if (output_patterns.size() > 1) {
    ERROR_LOC(loc, "*** multiple target patterns.");
  }
  if (!IsPatternRule(output_patterns[0].str())) {
    ERROR_LOC(loc, "*** target pattern contains no '%%'.");
  }
  ParseInputs(prereq_patterns);
}

std::string Rule::DebugString() const {
  std::vector<std::string> v;
  v.push_back(StringPrintf("outputs=[%s]", JoinSymbols(outputs, ",").c_str()));
  v.push_back(StringPrintf("inputs=[%s]", JoinSymbols(inputs, ",").c_str()));
  if (!order_only_inputs.empty()) {
    v.push_back(StringPrintf("order_only_inputs=[%s]",
                             JoinSymbols(order_only_inputs, ",").c_str()));
  }
  if (!output_patterns.empty()) {
    v.push_back(StringPrintf("output_patterns=[%s]",
                             JoinSymbols(output_patterns, ",").c_str()));
  }
  if (is_double_colon)
    v.push_back("is_double_colon");
  if (is_suffix_rule)
    v.push_back("is_suffix_rule");
  if (!cmds.empty()) {
    v.push_back(StringPrintf("cmds=[%s]", JoinValues(cmds, ",").c_str()));
  }
  return JoinStrings(v, " ");
}
