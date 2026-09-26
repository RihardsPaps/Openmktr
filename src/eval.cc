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

#include "eval.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

#include "expr.h"
#include "file.h"
#include "file_cache.h"
#include "fileutil.h"
#include "parser.h"
#include "rule.h"
#include "stats.h"
#include "stmt.h"
#include "strutil.h"
#include "symtab.h"
#include "var.h"

namespace {

// Rule prerequisites are evaluated before they are tokenized.  Comments in
// the rule's original text must therefore remain comments after expansion;
// otherwise an '=' in an inline comment can be mistaken for a rule-specific
// variable assignment (for example: "target: ## TESTS=<name>").
std::string_view StripRuleComment(const std::string& text,
                                  std::string* without_comment) {
  bool escaped = false;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '#' && !escaped) {
      *without_comment = text.substr(0, i);
      return *without_comment;
    }
    if (c == '\\')
      escaped = !escaped;
    else
      escaped = false;
  }
  return text;
}

}  // namespace

Frame::Frame(FrameType type, Frame* parent, Loc loc, const std::string& name)
    : type_(type), parent_(parent), name_(name), location_(loc) {
  CHECK((parent == nullptr) == (type == FrameType::ROOT));
}

Frame::~Frame() {}

void Frame::Add(std::unique_ptr<Frame> child) {
  children_.push_back(std::move(child));
}

void Frame::PrintJSONTrace(FILE* f, int indent) const {
  if (type_ == FrameType::ROOT) {
    return;
  }

  std::string indent_string = std::string(indent, ' ');
  std::string desc = name_;
  if (location_.filename != nullptr) {
    desc += StringPrintf(" @ %s", location_.filename);
    if (location_.lineno > 0) {
      desc += StringPrintf(":%d", location_.lineno);
    }
  }

  const char* comma = parent_->type_ == FrameType::ROOT ? "" : ",";
  fprintf(f, "%s\"%s\"%s\n", indent_string.c_str(), desc.c_str(), comma);
  parent_->PrintJSONTrace(f, indent);
}

ScopedFrame::ScopedFrame(Evaluator* ev, Frame* frame) : ev_(ev), frame_(frame) {
  if (!ev->trace_) {
    return;
  }

  ev_->stack_.back()->Add(std::unique_ptr<Frame>(frame));
  ev_->stack_.push_back(frame);
}

ScopedFrame::ScopedFrame(ScopedFrame&& other) noexcept
    : ev_(other.ev_), frame_(other.frame_) {
  other.ev_ = nullptr;
  other.frame_ = nullptr;
}

ScopedFrame::~ScopedFrame() {
  if (ev_ == nullptr || !ev_->trace_) {
    return;
  }

  CHECK(frame_ == ev_->stack_.back());
  ev_->stack_.pop_back();
}

IncludeGraphNode::IncludeGraphNode(const Frame* frame)
    : filename_(frame->Name()) {}

IncludeGraphNode::~IncludeGraphNode() {}

IncludeGraph::IncludeGraph() {}

IncludeGraph::~IncludeGraph() {}

void IncludeGraph::DumpJSON(FILE* output) {
  fprintf(output, "{\n");
  fprintf(output, "  \"include_graph\": [");
  bool first_node = true;

  for (const auto& node : nodes_) {
    if (first_node) {
      first_node = false;
      fprintf(output, "\n");
    } else {
      fprintf(output, ",\n");
    }

    fprintf(output, "    {\n");
    // TODO(lberki): Quote all these strings properly
    fprintf(output, "      \"file\": \"%s\",\n", node.first.c_str());
    fprintf(output, "      \"includes\": [");
    bool first_include = true;
    for (const auto& include : node.second->includes_) {
      if (first_include) {
        first_include = false;
        fprintf(output, "\n");
      } else {
        fprintf(output, ",\n");
      }

      fprintf(output, "        \"%s\"", include.c_str());
    }
    fprintf(output, "\n      ]\n");
    fprintf(output, "    }");
  }

  fprintf(output, "\n");
  fprintf(output, "  ]\n");
  fprintf(output, "}\n");
}

void IncludeGraph::MergeTreeNode(const Frame* frame) {
  if (frame->Type() == FrameType::PARSE) {
    std::unique_ptr<IncludeGraphNode>& graph_node = nodes_[frame->Name()];
    if (graph_node.get() == nullptr) {
      graph_node.reset(new IncludeGraphNode(frame));
    }

    if (!include_stack_.empty()) {
      IncludeGraphNode* parent_node =
          nodes_[include_stack_.back()->Name()].get();
      parent_node->includes_.insert(frame->Name());
    }
    include_stack_.push_back(frame);
  }

  for (const auto& child : frame->Children()) {
    MergeTreeNode(child.get());
  }

  if (frame->Type() == FrameType::PARSE) {
    include_stack_.pop_back();
  }
}

Evaluator::Evaluator()
    : last_rule_(NULL),
      current_scope_(NULL),
      avoid_io_(false),
      eval_depth_(0),
      secondary_expansion_(false),
      posix_sym_(Intern(".POSIX")),
      is_posix_(false),
      export_error_(false) {
#if defined(__APPLE__)
  stack_size_ = pthread_get_stacksize_np(pthread_self());
  stack_addr_ = (char*)pthread_get_stackaddr_np(pthread_self()) - stack_size_;
#else
  pthread_attr_t attr;
  CHECK(pthread_getattr_np(pthread_self(), &attr) == 0);
  CHECK(pthread_attr_getstack(&attr, &stack_addr_, &stack_size_) == 0);
  CHECK(pthread_attr_destroy(&attr) == 0);
#endif

  lowest_stack_ = (char*)stack_addr_ + stack_size_;
  LOG_STAT("Stack size: %zd bytes", stack_size_);

  stack_.push_back(new Frame(FrameType::ROOT, nullptr, Loc(), "*root*"));

  trace_ = g_flags.dump_variable_assignment_trace || g_flags.dump_include_graph;
  assignment_tracefile_ = nullptr;
  assignment_sep_ = "\n";
}

Evaluator::~Evaluator() {
  if (assignment_tracefile_ != nullptr && assignment_tracefile_ != stderr) {
    fclose(assignment_tracefile_);
  }

  // delete vars_;
  // for (auto p : rule_vars) {
  //   delete p.second;
  // }
}

bool Evaluator::Start() {
  const char* fn = g_flags.dump_variable_assignment_trace;
  if (!fn) {
    return true;
  }

  if (!strcmp(fn, "-")) {
    assignment_tracefile_ = stderr;
  } else {
    assignment_tracefile_ = fopen(fn, "w");
    if (assignment_tracefile_ == nullptr) {
      // TODO(lberki): What about error checking for fwrite()?
      fprintf(stderr, "fopen(%s): %s", fn, strerror(errno));
      return false;
    }
  }

  fprintf(assignment_tracefile_, "{\n");
  fprintf(assignment_tracefile_, "  \"assignments\": [");
  return true;
}

void Evaluator::Finish() {
  if (assignment_tracefile_ == nullptr) {
    return;
  }

  fprintf(assignment_tracefile_, " \n ]\n");
  fprintf(assignment_tracefile_, "}\n");
}
void Evaluator::in_bootstrap() {
  is_bootstrap_ = true;
  is_commandline_ = false;
}
void Evaluator::in_command_line() {
  is_bootstrap_ = false;
  is_commandline_ = true;
}

void Evaluator::in_toplevel_makefile() {
  is_commandline_ = false;
  is_commandline_ = false;
}

Var* Evaluator::EvalRHS(Symbol lhs,
                        Value* rhs_v,
                        std::string_view orig_rhs,
                        AssignOp op,
                        bool is_override,
                        bool* needs_assign) {
  VarOrigin origin;
  Frame* current_frame = nullptr;

  if (is_bootstrap_) {
    origin = VarOrigin::DEFAULT;
  } else if (is_commandline_) {
    origin = VarOrigin::COMMAND_LINE;
  } else if (is_override) {
    origin = VarOrigin::OVERRIDE;
    current_frame = stack_.back();
  } else {
    origin = VarOrigin::FILE;
    current_frame = stack_.back();
  }

  Var* result = NULL;
  Var* prev = NULL;
  *needs_assign = true;

  // A command-line variable has higher precedence than ordinary makefile
  // assignments.  In particular, a later "VAR += value" must not mutate the
  // command-line Var in place before Vars::Assign() gets a chance to reject
  // the replacement.  The makefile can explicitly opt in with "override".
  // MAKEFLAGS is maintained by make itself and may be extended by a
  // makefile (Kbuild adds its source include directory this way).  The
  // normalized transport value installed before parsing must not turn it
  // into an immutable command-line assignment.
  if (!is_bootstrap_ && !is_commandline_ && !is_override &&
      !(lhs == Intern("MAKEFLAGS"))) {
    Var* command_line_var = PeekVarInCurrentScope(lhs);
    if (command_line_var->IsDefined() &&
        command_line_var->Origin() == VarOrigin::COMMAND_LINE) {
      command_line_var->Used(this, lhs);
      *needs_assign = false;
      return command_line_var;
    }
  }

  switch (op) {
    case AssignOp::COLON_EQ: {
      prev = PeekVarInCurrentScope(lhs);
      result = new SimpleVar(origin, current_frame, loc_, this, rhs_v);
      break;
    }
    case AssignOp::EQ:
      prev = PeekVarInCurrentScope(lhs);
      result = new RecursiveVar(rhs_v, origin, current_frame, loc_, orig_rhs);
      break;
    case AssignOp::PLUS_EQ: {
      prev = LookupVarInCurrentScope(lhs);
      if (!prev->IsDefined()) {
        result = new RecursiveVar(rhs_v, origin, current_frame, loc_, orig_rhs);
      } else if (prev->ReadOnly()) {
        Error(StringPrintf("*** cannot assign to readonly variable: %s",
                           lhs.c_str()));
      } else {
        result = prev;
        result->AppendVar(this, rhs_v);
        *needs_assign = false;
      }
      break;
    }
    case AssignOp::QUESTION_EQ: {
      prev = LookupVarInCurrentScope(lhs);
      if (!prev->IsDefined()) {
        result = new RecursiveVar(rhs_v, origin, current_frame, loc_, orig_rhs);
      } else {
        result = prev;
        *needs_assign = false;
      }
      break;
    }
  }

  if (prev != NULL) {
    prev->Used(this, lhs);
    if (prev->Deprecated() && *needs_assign) {
      result->SetDeprecated(prev->DeprecatedMessage());
    }
  }

  LOG("Assign: %s=%s", lhs.c_str(), result->DebugString().c_str());
  return result;
}

void Evaluator::EvalUndefine(const UndefineStmt* stmt) {
  loc_ = stmt->loc();
  last_rule_ = NULL;

  std::string name;
  stmt->lhs->Eval(this, &name);

  Symbol sym = Intern(TrimSpace(name));
  if (sym.empty()) {
    Error("*** empty variable name.");
    return;
  }

  bool readonly = false;
  sym.SetGlobalVar(Var::Undefined(), false, &readonly);

  if (readonly) {
    Error(
        StringPrintf("*** cannot undefine readonly variable: %s", sym.c_str()));
  }
}

void Evaluator::EvalAssign(const AssignStmt* stmt) {
  loc_ = stmt->loc();
  last_rule_ = NULL;
  Symbol lhs = stmt->GetLhsSymbol(this);
  if (lhs.empty())
    Error("*** empty variable name.");

  if (lhs == kKatiReadonlySym) {
    std::string rhs;
    stmt->rhs->Eval(this, &rhs);
    for (auto const& name : WordScanner(rhs)) {
      Var* var = Intern(name).GetGlobalVar();
      if (!var->IsDefined()) {
        Error(StringPrintf("*** unknown variable: %s",
                           std::string(name).c_str()));
      }
      var->SetReadOnly();
    }
    return;
  }

  bool needs_assign;
  Var* var =
      EvalRHS(lhs, stmt->rhs, stmt->orig_rhs, stmt->op,
              stmt->directive == AssignDirective::OVERRIDE, &needs_assign);

  if (needs_assign) {
    bool readonly;
    lhs.SetGlobalVar(var, stmt->directive == AssignDirective::OVERRIDE,
                     &readonly);
    if (lhs == Intern("VPATH"))
      SetVpath("%", var->Eval(this));
    if (readonly) {
      Error(StringPrintf("*** cannot assign to readonly variable: %s",
                         lhs.c_str()));
    }
  }

  if (stmt->is_final) {
    var->SetReadOnly();
  }
  TraceVariableAssign(lhs, var);
}

// With rule broken into
//   <before_term> <term> <after_term>
// parses <before_term> into Symbol instances until encountering ':'
// Returns the remainder of <before_term>.
static std::string_view ParseRuleTargets(const Loc& loc,
                                         const std::string_view& before_term,
                                         std::vector<Symbol>* targets,
                                         bool* is_pattern_rule) {
  size_t pos = before_term.find(':');
  if (pos == std::string::npos) {
    ERROR_LOC(loc, "*** missing separator.");
  }
  std::string_view targets_string = before_term.substr(0, pos);
  size_t pattern_rule_count = 0;
  for (auto const& word : WordScanner(targets_string)) {
    const std::string_view trimmed_word = TrimLeadingCurdir(word);
    const bool current_directory = IsCurrentDirectoryPath(word);
    std::string target(trimmed_word);
    // Preserve parent-relative target spellings for the same reason that
    // Rule::ParseInputs preserves them: GNU make matches the lexical target
    // name used by an included makefile. Collapsing ../ here can select a
    // different implicit rule (or the built-in .c.o fallback).
    if (target.find("../") == std::string::npos && target != "..")
      NormalizeMakePath(&target);
    // NormalizePath uses an empty string for the current directory. In a
    // rule target, however, a standalone '.' is meaningful: recursive build
    // systems commonly attach work to that directory target.
    if (target.empty() && current_directory)
      target = ".";
    targets->push_back(Intern(target));
    if (Rule::IsPatternRule(target)) {
      ++pattern_rule_count;
    }
  }
  // A target-specific variable assignment may legally mix ordinary targets
  // and pattern targets.  Only classify the rule as a pattern rule when all
  // of its targets are patterns; mixed target lists remain explicit targets.
  *is_pattern_rule =
      pattern_rule_count != 0 && pattern_rule_count == targets->size();
  return before_term.substr(pos + 1);
}

// Strip leading spaces and trailing spaces and colons.
static std::string FormatRuleError(const std::string& before_term) {
  if (before_term.size() == 0) {
    return before_term;
  }
  size_t size = before_term.size();
  size_t start = 0;
  while (start < size && isspace(before_term[start])) {
    start++;
  }
  size_t end = size;  // we already handled length == 0
  while (end > start &&
         (isspace(before_term[end - 1]) || before_term[end - 1] == ':')) {
    end--;
  }
  return before_term.substr(start, end - start);
}

void Evaluator::MarkVarsReadonly(Value* vars_list) {
  std::string vars_list_string;
  vars_list->Eval(this, &vars_list_string);
  for (auto const& name : WordScanner(vars_list_string)) {
    Var* var = current_scope_->Lookup(Intern(name));
    if (!var->IsDefined()) {
      Error(
          StringPrintf("*** unknown variable: %s", std::string(name).c_str()));
    }
    var->SetReadOnly();
  }
}

void Evaluator::EvalRuleSpecificAssign(const std::vector<Symbol>& targets,
                                       const RuleStmt* stmt,
                                       const std::string_view& after_targets,
                                       size_t separator_pos) {
  std::string_view var_name;
  std::string_view rhs_string;
  AssignOp assign_op;
  std::string_view assign_text = after_targets;
  bool is_override = HasPrefix(TrimLeftSpace(assign_text), "override ");

  ParseAssignStatement(assign_text, separator_pos, &var_name, &rhs_string,
                       &assign_op);

  // GNU make treats export/unexport (like private/override) as modifiers on
  // target-specific assignments.  Keep the export state with the target
  // scope instead of making it a global export, since the value is often
  // intentionally visible only to a recursive command for this target.
  bool is_export = false;
  bool is_unexport = false;
  for (;;) {
    if (HasPrefix(var_name, "export ")) {
      is_export = true;
      var_name = TrimSpace(var_name.substr(7));
    } else if (HasPrefix(var_name, "unexport ")) {
      is_unexport = true;
      var_name = TrimSpace(var_name.substr(9));
    } else {
      break;
    }
  }

  Symbol var_sym = Intern(var_name);
  bool is_final = (stmt->sep == RuleStmt::SEP_FINALEQ);
  for (Symbol target : targets) {
    auto p = rule_vars_.emplace(target, nullptr);
    if (p.second) {
      p.first->second = new Vars;
    }

    // ParseRule already stores the complete right-hand side in stmt->rhs.
    // rhs_string is the same text as seen after the target-specific
    // assignment operator; combining both duplicates the RHS and changes
    // GNU make's target-specific assignment semantics.
    Value* rhs = stmt->rhs;
    if (!rhs)
      rhs = Value::NewLiteral(rhs_string);

    Vars* target_scope = p.first->second;
    Vars* enclosing_scope = current_scope_;
    // Target-specific assignments are evaluated with the target scope
    // active.  Vars lookup falls back to the global scope, while the target
    // scope also retains preceding target-specific assignments.  This is
    // required for immediate assignments such as:
    //   target: CPPFLAGS += -DLOCAL
    //   target: CPPFLAGS := -I$(dir) $(CPPFLAGS)
    current_scope_ = target_scope;
    // A target-specific "+=" must not append directly to an inherited
    // global variable.  LookupVarInCurrentScope() deliberately falls back
    // to the global scope, but EvalRHS() mutates the returned Var for +=.
    // Materialize that inherited value in the target scope first, preserving
    // its flavor so later target-specific assignments see the same value
    // GNU make would expose through the target-specific scope.
    if (assign_op == AssignOp::PLUS_EQ &&
        target_scope->find(var_sym) == target_scope->end()) {
      Var* inherited = LookupVarGlobal(var_sym);
      if (inherited->IsDefined()) {
        Var* local;
        if (inherited->Flavor() == std::string("simple")) {
          local = new SimpleVar(
              std::string(inherited->String()),
              is_override ? VarOrigin::OVERRIDE : inherited->Origin(),
              stack_.back(), loc_);
        } else {
          // Recursive variables must retain their complete expression tree
          // when an inherited value is materialized in a target-specific
          // scope.  String() is only the original source spelling for a
          // recursive variable; after one or more += assignments it omits
          // the appended expression.  Copy the expression tree instead so
          // both deferred references and appended values survive.
          const RecursiveVar* recursive =
              dynamic_cast<const RecursiveVar*>(inherited);
          CHECK(recursive != nullptr);
          local = new RecursiveVar(
              recursive->v_,
              is_override ? VarOrigin::OVERRIDE : inherited->Origin(),
              stack_.back(), loc_, inherited->String());
        }
        bool readonly;
        target_scope->Assign(var_sym, local, &readonly);
        if (readonly)
          Error(StringPrintf("*** cannot assign to readonly variable: %s",
                             var_name));
      }
    }
    if (assign_op == AssignOp::COLON_EQ) {
      std::string expanded_rhs;
      rhs->Eval(this, &expanded_rhs);
      // Literal values retain a string_view, so intern the expanded text
      // before storing it in the target-specific variable.
      rhs = Value::NewLiteral(Intern(expanded_rhs).str());
    }
    if (var_sym == kKatiReadonlySym) {
      MarkVarsReadonly(rhs);
    } else {
      bool needs_assign;
      Var* rhs_var = EvalRHS(var_sym, rhs, std::string_view("*TODO*"),
                             assign_op, is_override, &needs_assign);
      if (needs_assign) {
        bool readonly;
        rhs_var->SetAssignOp(assign_op);
        target_scope->Assign(var_sym, rhs_var, &readonly);
        if (readonly) {
          Error(StringPrintf("*** cannot assign to readonly variable: %s",
                             var_name));
        }
      }
      if (is_final) {
        rhs_var->SetReadOnly();
      }
    }
    if (is_export || is_unexport)
      target_scope->SetExported(var_sym, is_export && !is_unexport);
    current_scope_ = enclosing_scope;
  }
}

void Evaluator::EvalRule(const RuleStmt* stmt) {
  loc_ = stmt->loc();
  last_rule_ = NULL;

  const std::string&& before_term = stmt->lhs->Eval(this);

  // See semicolon.mk.
  if (before_term.find_first_not_of(" \t\n;") == std::string::npos) {
    if (stmt->sep == RuleStmt::SEP_SEMICOLON)
      Error("*** missing rule before commands.");
    return;
  }

  std::vector<Symbol> targets;
  bool is_pattern_rule;
  std::string_view after_targets =
      ParseRuleTargets(loc_, before_term, &targets, &is_pattern_rule);

  std::string expanded_after_targets(after_targets);
  std::string rule_without_comment;
  after_targets =
      StripRuleComment(expanded_after_targets, &rule_without_comment);

  // An empty prerequisite list is valid (for example, "target:"). Reading
  // element zero before checking the view's length is undefined behavior and
  // can crash while parsing a late recursive makefile.
  bool is_double_colon = !after_targets.empty() && after_targets[0] == ':';
  if (is_double_colon) {
    after_targets = after_targets.substr(1);
  }

  // Figure out if this is a rule-specific variable assignment.
  // It is an assignment when either after_targets contains an assignment token
  // or separator is an assignment token, but only if there is no ';' before the
  // first assignment token.
  size_t separator_pos = after_targets.find_first_of("=;");
  char separator = '\0';
  if (separator_pos != std::string::npos) {
    separator = after_targets[separator_pos];
  } else if (separator_pos == std::string::npos &&
             (stmt->sep == RuleStmt::SEP_EQ ||
              stmt->sep == RuleStmt::SEP_FINALEQ)) {
    separator_pos = after_targets.size();
    separator = '=';
  }

  // If variable name is not empty, we have rule- or target-specific
  // variable assignment.
  if (separator == '=' && separator_pos) {
    EvalRuleSpecificAssign(targets, stmt, after_targets, separator_pos);
    return;
  }

  if (!separator_pos) {
    // We used to make this a warning and otherwise accept it, but Make 4.1
    // calls this out as an error, so let's follow.
    Error("*** empty variable name.");
  }

  Rule* rule = new Rule();
  rule->loc = loc_;
  rule->is_double_colon = is_double_colon;
  if (is_pattern_rule) {
    rule->output_patterns.swap(targets);
  } else {
    rule->outputs.swap(targets);
  }
  // .SECONDEXPANSION is a declaration, not a build target.  Its effect is
  // lexical and applies to rules parsed after it, just like GNU make's
  // declaration.  Do not leave a synthetic special target in the graph.
  if (rule->outputs.size() == 1 &&
      rule->outputs[0] == Intern(".SECONDEXPANSION") && after_targets.empty() &&
      rule->cmds.empty()) {
    secondary_expansion_ = true;
    delete rule;
    return;
  }
  if (secondary_expansion_ && !after_targets.empty()) {
    rule->secondary_expansion = true;
    const size_t prereq_end = separator == ';' ? separator_pos
                                               : after_targets.size();
    rule->secondary_prerequisites =
        std::string(after_targets.substr(0, prereq_end));
  }
  rule->ParsePrerequisites(after_targets, separator_pos, stmt);

  if (stmt->sep == RuleStmt::SEP_SEMICOLON) {
    rule->cmds.push_back(stmt->rhs);
  }

  for (Symbol o : rule->outputs) {
    if (o == posix_sym_)
      is_posix_ = true;
  }

  LOG("Rule: %s", rule->DebugString().c_str());
  switch (GetAllowRules()) {
    case RULES_WARNING:
      WARN_LOC(loc_, "warning: Rule not allowed here for target: %s",
               FormatRuleError(before_term).c_str());
      break;
    case RULES_ERROR:
      PrintIncludeStack();
      ERROR_LOC(loc_, "*** Rule not allowed here for target: %s",
                FormatRuleError(before_term).c_str());
      break;
    default:  // RULES_ALLOWED
      break;
  }

  rules_.push_back(rule);
  last_rule_ = rule;
}

void Evaluator::EvalCommand(const CommandStmt* stmt) {
  loc_ = stmt->loc();

  if (!last_rule_) {
    std::vector<Stmt*> stmts;
    ParseNoStats(stmt->orig, stmt->loc(), &stmts);
    for (Stmt* a : stmts)
      a->Eval(this);
    return;
  }

  last_rule_->cmds.push_back(stmt->expr);
  if (last_rule_->cmd_lineno == 0)
    last_rule_->cmd_lineno = stmt->loc().lineno;
  LOG("Command: %s", Value::DebugString(stmt->expr).c_str());
}

void Evaluator::EvalIf(const IfStmt* stmt) {
  loc_ = stmt->loc();
  Rule* enclosing_rule = last_rule_;

  bool is_true;
  switch (stmt->op) {
    case CondOp::IFDEF:
    case CondOp::IFNDEF: {
      std::string var_name;
      stmt->lhs->Eval(this, &var_name);
      Symbol lhs = Intern(TrimRightSpace(var_name));
      if (const auto& s = lhs.str();
          std::find_if(s.begin(), s.end(), ::isspace) != s.end()) {
        Error("*** invalid syntax in conditional.");
      }
      Var* v = LookupVarInCurrentScope(lhs);
      v->Used(this, lhs);
      is_true = (v->String().empty() == (stmt->op == CondOp::IFNDEF));
      break;
    }
    case CondOp::IFEQ:
    case CondOp::IFNEQ: {
      const std::string&& lhs = stmt->lhs->Eval(this);
      const std::string&& rhs = stmt->rhs->Eval(this);

      is_true = ((lhs == rhs) == (stmt->op == CondOp::IFEQ));
      break;
    }
    default:
      CHECK(false);
      abort();
  }

  // A conditional does not end the preceding rule. Its condition can expand
  // functions such as $(eval), which evaluate other statements and change
  // last_rule_. Restore the rule before evaluating the selected branch so
  // recipes after the conditional still attach to it.
  last_rule_ = enclosing_rule;

  const std::vector<Stmt*>* stmts;
  if (is_true) {
    stmts = &stmt->true_stmts;
  } else {
    stmts = &stmt->false_stmts;
  }
  for (Stmt* a : *stmts) {
    LOG("%s", a->DebugString().c_str());
    a->Eval(this);
  }
}

void Evaluator::DoInclude(const std::string& fname) {
  CheckStack();
  COLLECT_STATS_WITH_SLOW_REPORT("included makefiles", fname.c_str());

  const Makefile& mk = MakefileCacheManager::Get().ReadMakefile(fname);
  if (!mk.Exists()) {
    Error(StringPrintf("%s does not exist", fname.c_str()));
  }

  Var* var_list = LookupVar(Intern("MAKEFILE_LIST"));
  var_list->AppendVar(
      this, Value::NewLiteral(Intern(TrimLeadingCurdir(fname)).str()));
  for (Stmt* stmt : mk.stmts()) {
    LOG("%s", stmt->DebugString().c_str());
    stmt->Eval(this);
  }

  for (auto& mk : profiled_files_) {
    stats.MarkInteresting(mk);
  }
  profiled_files_.clear();
}

void Evaluator::EvalInclude(const IncludeStmt* stmt) {
  loc_ = stmt->loc();
  last_rule_ = NULL;

  const std::string&& pats = stmt->expr->Eval(this);
  for (std::string_view pat : WordScanner(pats)) {
    std::vector<std::string> files = Glob(pat);
    if (files.empty() && !pat.empty() && pat.front() != '/' &&
        pat.find_first_of("?*[\\") == std::string_view::npos) {
      for (const std::string& dir : g_flags.include_dirs) {
        const std::string candidate = ConcatDir(dir, pat);
        const auto& matches = Glob(candidate);
        if (!matches.empty()) {
          files.assign(matches.begin(), matches.end());
          break;
        }
      }
    }

    if (files.empty()) {
      // GNU make attempts to remake missing makefiles for both include and
      // -include.  The latter suppresses an error only when no rule can
      // produce the file; it must not suppress the remake itself.  This is
      // required for generated metadata such as FFmpeg's *.version files.
      missing_includes_.push_back({std::string(pat), stmt->loc()});
      continue;
    }

    for (const std::string& fname : files) {
      if (!stmt->should_exist && g_flags.ignore_optional_include_pattern &&
          Pattern(g_flags.ignore_optional_include_pattern).Match(fname)) {
        continue;
      }

      if (std::find(included_makefiles_.begin(), included_makefiles_.end(),
                    fname) == included_makefiles_.end()) {
        included_makefiles_.push_back(fname);
      }

      // GNU make permits a makefile to include itself recursively.  This is
      // useful when each inclusion advances a variable-driven iterator (for
      // example, generating one rule per module).  Bound the nesting depth
      // to diagnose unbounded include cycles without rejecting valid
      // recursive includes.
      if (active_include_files_.size() >= 1024) {
        ERROR_LOC(loc_, "*** makefile include nesting exceeds 1024 levels");
      }

      include_stack_.push_back(stmt->loc());
      active_include_files_.push_back(fname);
      {
        ScopedFrame frame(Enter(FrameType::PARSE, fname, stmt->loc()));
        DoInclude(fname);
      }
      active_include_files_.pop_back();
      include_stack_.pop_back();
    }
  }
}

void Evaluator::EvalVpath(const Value* expr) {
  std::string value = expr->Eval(this);
  std::vector<std::string_view> words;
  for (std::string_view word : WordScanner(value))
    words.push_back(word);

  if (words.empty()) {
    vpaths_.clear();
    return;
  }

  std::string directories;
  for (size_t i = 1; i < words.size(); i++) {
    if (!directories.empty())
      directories += ' ';
    directories.append(words[i]);
  }
  SetVpath(words[0], directories, true, true);
}

void Evaluator::SetVpath(std::string_view pattern,
                         std::string_view directories,
                         bool append,
                         bool from_vpath) {
  if (!append) {
    vpaths_.erase(
        std::remove_if(vpaths_.begin(), vpaths_.end(),
                       [&](const Vpath& v) { return v.pattern == pattern; }),
        vpaths_.end());
  }
  if (directories.empty())
    return;

  Vpath vpath;
  vpath.pattern = pattern;
  vpath.from_vpath = from_vpath;
  for (std::string_view word : WordScanner(directories)) {
    std::string_view dirs = word;
    while (!dirs.empty()) {
      size_t colon = dirs.find(':');
      std::string dir(dirs.substr(0, colon));
      if (!dir.empty()) {
        NormalizePath(&dir);
        vpath.directories.push_back(dir);
      }
      if (colon == std::string_view::npos)
        break;
      dirs = dirs.substr(colon + 1);
    }
  }
  if (!vpath.directories.empty()) {
    if (append) {
      for (Vpath& existing : vpaths_) {
        if (existing.pattern == pattern && existing.from_vpath == from_vpath) {
          existing.directories.insert(existing.directories.end(),
                                      vpath.directories.begin(),
                                      vpath.directories.end());
          return;
        }
      }
    }
    vpaths_.push_back(std::move(vpath));
  }
}

std::string Evaluator::ResolveVpath(Symbol target) const {
  if (Exists(target.str())) {
    return target.str();
  }

  // GNU make searches matching vpath directives before the global VPATH,
  // regardless of where the VPATH assignment appeared in the makefile.
  for (int pass = 0; pass < 2; ++pass) {
    for (const Vpath& vpath : vpaths_) {
      if (vpath.from_vpath != (pass == 0) ||
          !Pattern(vpath.pattern).Match(target.str()))
        continue;
      for (const std::string& directory : vpath.directories) {
        std::string candidate = ConcatDir(directory, target.str());
        if (Exists(candidate))
          return candidate;
      }
    }
  }
  return std::string();
}

void Evaluator::EvalExport(const ExportStmt* stmt) {
  loc_ = stmt->loc();
  last_rule_ = NULL;

  const std::string&& exports = stmt->expr->Eval(this);
  for (std::string_view tok : WordScanner(exports)) {
    size_t equal_index = tok.find('=');
    std::string_view lhs;
    if (equal_index == std::string::npos) {
      lhs = tok;
    } else if (equal_index == 0 ||
               (equal_index == 1 &&
                (tok[0] == ':' || tok[0] == '?' || tok[0] == '+'))) {
      // Do not export tokens after an assignment.
      break;
    } else {
      std::string_view rhs;
      AssignOp op;
      ParseAssignStatement(tok, equal_index, &lhs, &rhs, &op);
    }
    Symbol sym = Intern(lhs);
    exports_[sym] = stmt->is_export;
    if (export_message_) {
      const char* prefix = "";
      if (!stmt->is_export) {
        prefix = "un";
      }

      if (export_error_) {
        Error(StringPrintf("*** %s: %sexport is obsolete%s.", sym.c_str(),
                           prefix, export_message_->c_str()));
      } else {
        WARN_LOC(loc(), "%s: %sexport has been deprecated%s.", sym.c_str(),
                 prefix, export_message_->c_str());
      }
    }
  }
}

Var* Evaluator::LookupVarGlobal(Symbol name) {
  Var* v = name.GetGlobalVar();
  if (v->IsDefined())
    return v;
  const bool first_use = !used_undefined_vars_.exists(name);
  used_undefined_vars_.insert(name);
  if (first_use && g_flags.warn_undefined_variables) {
    WARN_LOC(loc(), "warning: undefined variable '%s'", name.c_str());
  }
  return v;
}

bool Evaluator::IsTraced(Symbol& name) const {
  if (assignment_tracefile_ == nullptr) {
    return false;
  }

  bool trace_var = g_flags.traced_variables_pattern.size() == 0;
  // trace every variable unless filtered
  if (trace_var) {
    return true;
  }

  for (const auto& pat : g_flags.traced_variables_pattern) {
    if (pat.Match(name.c_str())) {
      return true;
    }
  }
  return false;
}

void Evaluator::TraceVariableLookup(const char* operation,
                                    Symbol& name,
                                    Var* var) {
  if (!IsTraced(name)) {
    return;
  }

  fputs(assignment_sep_, assignment_tracefile_);
  assignment_sep_ = ",\n";
  fprintf(assignment_tracefile_,
          "    {\n"
          "      \"name\": \"%s\",\n"
          "      \"operation\": \"%s\",\n"
          "      \"defined\": %s,\n",
          name.c_str(), operation, var->IsDefined() ? "true" : "false");
  fprintf(assignment_tracefile_, "      \"reference_stack\": [\n");
  CurrentFrame()->PrintJSONTrace(assignment_tracefile_, 8);
  fprintf(assignment_tracefile_,
          "      ]\n"
          "    }");
}

void Evaluator::TraceVariableAssign(Symbol& name, Var* var) {
  if (!IsTraced(name)) {
    return;
  }
  fputs(assignment_sep_, assignment_tracefile_);
  assignment_sep_ = ",\n";
  fprintf(assignment_tracefile_,
          "    {\n"
          "      \"name\": \"%s\",\n"
          "      \"operation\": \"assign\",\n"
          "      \"value\": \"%s\"",
          name.c_str(), var->DebugString().c_str());
  Frame* definition = var->Definition();
  if (definition != nullptr) {
    fprintf(assignment_tracefile_,
            ",\n"
            "      \"value_stack\": [\n");
    definition->PrintJSONTrace(assignment_tracefile_, 8);
    fprintf(assignment_tracefile_, "      ]");
  }
  fprintf(assignment_tracefile_,
          "\n"
          "    }");
}

Var* Evaluator::LookupVarForEval(Symbol name) {
  Var* var = LookupVar(name);
  if (var != nullptr) {
    if (symbols_for_eval_.find(name) != symbols_for_eval_.end()) {
      var->SetSelfReferential();
    }
    symbols_for_eval_.insert(name);
  }
  return var;
}

void Evaluator::VarEvalComplete(Symbol name) {
  symbols_for_eval_.erase(name);
}

Var* Evaluator::LookupVar(Symbol name) {
  Var* result = nullptr;
  if (current_scope_) {
    result = current_scope_->Lookup(name);
  }

  if (result == nullptr || !result->IsDefined()) {
    result = LookupVarGlobal(name);
  }

  TraceVariableLookup("lookup", name, result);
  return result;
}

Var* Evaluator::PeekVar(Symbol name) {
  Var* result = nullptr;

  if (current_scope_) {
    result = current_scope_->Peek(name);
  }

  if (result == nullptr || !result->IsDefined()) {
    result = name.PeekGlobalVar();
  }

  return result;
}

Var* Evaluator::LookupVarInCurrentScope(Symbol name) {
  Var* result;
  if (current_scope_) {
    result = current_scope_->Lookup(name);
  } else {
    result = LookupVarGlobal(name);
  }

  TraceVariableLookup("scope lookup", name, result);
  return result;
}

Var* Evaluator::PeekVarInCurrentScope(Symbol name) {
  Var* result;
  if (current_scope_) {
    result = current_scope_->Peek(name);
  } else {
    result = name.PeekGlobalVar();
  }

  return result;
}

std::string Evaluator::EvalVar(Symbol name) {
  return LookupVar(name)->Eval(this);
}

ScopedFrame Evaluator::Enter(FrameType frame_type,
                             const std::string& name,
                             Loc loc) {
  if (!trace_) {
    return ScopedFrame(this, nullptr);
  }

  Frame* frame = new Frame(frame_type, stack_.back(), loc, name);
  return ScopedFrame(this, frame);
}

std::string Evaluator::GetShell() {
  return EvalVar(kShellSym);
}

std::string Evaluator::GetShellFlag() {
  Var* shell_flags = PeekVar(kShellFlagsSym);
  if (shell_flags->IsDefined())
    return shell_flags->Eval(this);
  return is_posix_ ? "-ec" : "-c";
}

std::string Evaluator::GetShellAndFlag() {
  std::string shell = GetShell();
  shell += ' ';
  shell += GetShellFlag();
  return shell;
}

RulesAllowed Evaluator::GetAllowRules() {
  std::string val = EvalVar(kAllowRulesSym);
  if (val == "warning") {
    return RULES_WARNING;
  } else if (val == "error") {
    return RULES_ERROR;
  } else {
    return RULES_ALLOWED;
  }
}

void Evaluator::PrintIncludeStack() {
  for (auto& inc : include_stack_) {
    fprintf(stderr, "In file included from %s:%d:\n", LOCF(inc));
  }
}

void Evaluator::Error(const std::string& msg) {
  PrintIncludeStack();
  ERROR_LOC(loc_, "%s", msg.c_str());
}

void Evaluator::DumpStackStats() const {
  LOG_STAT("Max stack use: %zd bytes at %s:%d",
           ((char*)stack_addr_ - (char*)lowest_stack_) + stack_size_,
           LOCF(lowest_loc_));
}

void Evaluator::DumpIncludeJSON(const std::string& filename) const {
  IncludeGraph graph;
  graph.MergeTreeNode(stack_.front());
  FILE* jsonfile;
  if (filename == "-") {
    jsonfile = stdout;
  } else {
    jsonfile = fopen(filename.c_str(), "w");
    if (jsonfile == NULL) {
      fprintf(stderr, "cannot open JSON dump file: %s\n", strerror(errno));
      return;
    }
  }

  graph.DumpJSON(jsonfile);
  fclose(jsonfile);
}

bool Evaluator::IsEvaluatingCommand() const {
  return is_evaluating_command_;
}

void Evaluator::SetEvaluatingCommand(bool evaluating_command) {
  is_evaluating_command_ = evaluating_command;
}

SymbolSet Evaluator::used_undefined_vars_;
