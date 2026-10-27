// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "incremental_eval.h"

#include <limits.h>
#include <unistd.h>
#include <algorithm>

#include "eval.h"
#include "expr.h"
#include "fileutil.h"
#include "find.h"
#include "flags.h"
#include "func.h"
#include "graph_optimization.h"
#include "log.h"
#include "native_observation.h"
#include "stmt.h"
#include "strutil.h"
#include "var.h"

namespace {
thread_local IncrementalEvaluator* active_evaluation = nullptr;
}

size_t IncrementalEvaluator::EntryBytes(const Entry& entry) const {
  size_t bytes = 256 + 2 * entry.key.size() + entry.result.size();
  for (const auto& read : entry.reads)
    bytes += 128 + 2 * read.first.size() + read.second.size();
  for (const auto& condition : entry.preconditions)
    bytes += 96 + condition.first.size() + condition.second.size();
  for (const auto& event : entry.uses)
    bytes += 32 + event.size();
  return bytes;
}

void IncrementalEvaluator::EraseEntry(const std::string& key) {
  auto found = entries_.find(key);
  if (found != entries_.end()) {
    entries_bytes_ -= EntryBytes(found->second);
    entries_.erase(found);
  }
}

void IncrementalEvaluator::StoreEntry(Entry entry) {
  const size_t bytes = EntryBytes(entry);
  EraseEntry(entry.key);
  if (bytes > 4 * 1024 * 1024)
    return;
  while (!entries_.empty() && (entries_bytes_ + bytes > 60 * 1024 * 1024 ||
                               entries_.size() >= 100000))
    EraseEntry(entries_.begin()->first);
  const std::string key = entry.key;
  entries_.emplace(key, std::move(entry));
  entries_bytes_ += bytes;
}

void RecordEvaluationIntern(std::string_view name) {
  if (active_evaluation)
    active_evaluation->Interned(name);
}

uint64_t IncrementalEvaluator::BeginLocal(std::string_view name,
                                          const Var* binding) {
  if (!enabled_)
    return 0;
  const uint64_t token = ++local_token_;
  locals_.push_back(
      {token, std::string(name), binding, regions_.size(), states_.size()});
  return token;
}

void IncrementalEvaluator::EndLocal(uint64_t token) {
  if (!token)
    return;
  locals_.erase(std::remove_if(locals_.begin(), locals_.end(),
                               [&](const LocalBinding& binding) {
                                 return binding.token == token;
                               }),
                locals_.end());
}

EvaluationLocalBinding::EvaluationLocalBinding(Evaluator* evaluator,
                                               std::string_view name,
                                               const Var* binding)
    : evaluator_(evaluator->incremental_evaluator()),
      token_(evaluator_->BeginLocal(name, binding)) {}

EvaluationLocalBinding::~EvaluationLocalBinding() {
  evaluator_->EndLocal(token_);
}

void IncrementalEvaluator::Interned(std::string_view name) {
  if (checking_state_ ||
      (regions_.empty() && (states_.empty() || !states_.back().reusable ||
                            states_.back().replay || !states_.back().depth)))
    return;
  const std::string event = "i" + std::string(name);
  for (auto& region : regions_) {
    if (!region.reusable)
      continue;
    if (region.entry.uses.size() >= 4096)
      region.reusable = false;
    else
      region.entry.uses.push_back(event);
  }
  if (!states_.empty()) {
    auto& state = states_.back();
    if (state.reusable && !state.replay && state.depth &&
        !state.operands.empty()) {
      auto& uses = state.operands.back().uses;
      if (++state.use_events > 4096)
        state.reusable = false;
      else
        uses.push_back(event);
    }
  }
}

void IncrementalEvaluator::Observe(std::string record) {
  if (!Observing() || checking_state_)
    return;
  if (record.size() > 1024 * 1024) {
    Effect();
    return;
  }
  const std::string event = "o" + record;
  for (auto& region : regions_) {
    if (!region.reusable)
      continue;
    region.observation_bytes += event.size();
    if (region.observation_bytes > 1024 * 1024 ||
        region.entry.uses.size() >= 4096)
      region.reusable = false;
    else
      region.entry.uses.push_back(event);
  }
  if (!states_.empty()) {
    auto& state = states_.back();
    if (state.reusable && !state.replay && state.depth) {
      state.observation_bytes += event.size();
      if (state.observation_bytes > 1024 * 1024 || ++state.use_events > 4096)
        state.reusable = false;
      else
        state.operands.back().uses.push_back(event);
    }
  }
}

bool IncrementalEvaluator::CheckObservation(std::string_view record,
                                            std::string* accepted) {
  const std::string original(record);
  std::string kind, argument, expected;
  if (!ConsumeGraphString(&record, &kind))
    return false;
  bool valid = false;
  if (kind == "file") {
    valid = ConsumeGraphString(&record, &argument) &&
            ConsumeGraphString(&record, &expected) && record.empty() &&
            expected != "unreadable" && GetFileIdentity(argument) == expected;
  } else if (kind == "glob") {
    if (!ConsumeGraphString(&record, &argument))
      return false;
    std::string current;
    for (const auto& match : Glob(argument))
      AppendGraphString(match, &current);
    valid = record == current;
  } else if (kind == "find") {
    std::string shell, shellflag, packet, current;
    std::unique_ptr<FindCommand> descriptor;
    if (!g_flags.use_find_emulator || !ConsumeGraphString(&record, &shell) ||
        !ConsumeGraphString(&record, &shellflag) ||
        !ConsumeGraphString(&record, &argument) ||
        !ConsumeGraphString(&record, &packet) || !record.empty() ||
        !DecodeNativeFind(argument, packet, &expected, &descriptor) ||
        !QueryNativeFind(argument, &packet) ||
        !DecodeNativeFind(argument, packet, &current, &descriptor))
      return false;
    valid = expected == current;
    if (valid) {
      accepted->clear();
      for (std::string_view field :
           {std::string_view("find"), std::string_view(shell),
            std::string_view(shellflag), std::string_view(argument),
            std::string_view(packet)})
        AppendGraphString(field, accepted);
    }
  }
  if (valid) {
    if (kind != "find")
      *accepted = original;
    ++observation_hits_;
  } else {
    ++observation_misses_;
  }
  return valid;
}

void IncrementalEvaluator::ReplayObservation(std::string_view record) {
  const std::string original(record);
  std::string kind, argument, expected;
  if (!ConsumeGraphString(&record, &kind))
    return;
  if (kind == "file") {
    if (ConsumeGraphString(&record, &argument) &&
        ConsumeGraphString(&record, &expected) && record.empty())
      ReplayNativeFileRead(evaluator_, argument, expected == "missing");
  } else if (kind == "glob") {
    // Validation already registered the complete positive/negative query.
  } else if (kind == "find") {
    std::string shell, shellflag, packet;
    if (ConsumeGraphString(&record, &shell) &&
        ConsumeGraphString(&record, &shellflag) &&
        ConsumeGraphString(&record, &argument) &&
        ConsumeGraphString(&record, &packet) && record.empty())
      ReplayNativeFind(evaluator_, shell, shellflag, argument, packet);
  }
  Observe(original);
}

std::string IncrementalEvaluator::StatementIdentity(const Stmt* statement) {
  std::string key = "state-program-v1";
  auto text = [&](std::string_view value) { AppendGraphString(value, &key); };
  auto expression = [&](const Value* value) {
    text(value ? value->IncrementalIdentity() : std::string_view());
  };
  text(statement->loc().filename ? statement->loc().filename : "");
  text(std::to_string(statement->loc().lineno));
  if (const auto* s = dynamic_cast<const AssignStmt*>(statement)) {
    text("assign");
    expression(s->lhs);
    expression(s->rhs);
    text(s->orig_rhs);
    key += static_cast<char>(s->op);
    key += static_cast<char>(s->directive);
    key += s->is_final ? '1' : '0';
  } else if (const auto* s = dynamic_cast<const RuleStmt*>(statement)) {
    text("rule");
    expression(s->lhs);
    expression(s->rhs);
    key += static_cast<char>(s->sep);
    key += s->has_literal_colon ? '1' : '0';
  } else if (const auto* s = dynamic_cast<const IfStmt*>(statement)) {
    text("conditional");
    key += static_cast<char>(s->op);
    expression(s->lhs);
    expression(s->rhs);
  } else if (const auto* s = dynamic_cast<const IncludeStmt*>(statement)) {
    text("include");
    expression(s->expr);
    key += s->should_exist ? '1' : '0';
  } else if (const auto* s = dynamic_cast<const ExportStmt*>(statement)) {
    text("export");
    expression(s->expr);
    key += s->is_export ? '1' : '0';
  } else if (const auto* s = dynamic_cast<const VpathStmt*>(statement)) {
    text("vpath");
    expression(s->expr);
  } else if (const auto* s = dynamic_cast<const UndefineStmt*>(statement)) {
    text("undefine");
    expression(s->lhs);
  } else if (const auto* s = dynamic_cast<const CommandStmt*>(statement)) {
    text("command");
    expression(s->expr);
    text(s->orig);
  } else {
    return {};
  }
  key += evaluator_->is_bootstrap_ ? '1' : '0';
  key += evaluator_->is_commandline_ ? '1' : '0';
  key += evaluator_->avoid_io() ? '1' : '0';
  key += evaluator_->current_scope() ? '1' : '0';
  key += evaluator_->last_rule_ ? '1' : '0';
  key += evaluator_->secondary_expansion_ ? '1' : '0';
  key += evaluator_->is_posix_ ? '1' : '0';
  key += evaluator_->IsEvaluatingCommand() ? '1' : '0';
  text(std::to_string(evaluator_->eval_depth()));
  if (!AppendScopeIdentity(&key))
    return {};
  return key;
}

bool IncrementalEvaluator::AppendScopeIdentity(std::string* key) {
  // Scope identities contain owned names, never process-local scope pointers.
  // Binding values are checked independently as ordered observations.
  AppendGraphString(evaluator_->current_scope() ? "target" : "global", key);
  if (evaluator_->current_scope()) {
    if (evaluator_->current_scope()->size() > 4096)
      return false;
    std::vector<std::string> names;
    for (const auto& binding : *evaluator_->current_scope())
      names.push_back(binding.first.str());
    std::sort(names.begin(), names.end());
    AppendGraphString(std::to_string(names.size()), key);
    for (const auto& name : names)
      AppendGraphString(name, key);
  }
  AppendGraphString(std::to_string(locals_.size()), key);
  for (const auto& binding : locals_) {
    const std::string identity = Fingerprint(binding.binding);
    if (identity.empty())
      return false;
    AppendGraphString(binding.name, key);
    AppendGraphString(identity, key);
    if (key->size() > 1024 * 1024)
      return false;
  }
  return true;
}

Var* IncrementalEvaluator::StateBinding(const std::string& observation) {
  Symbol name = FindSymbol(std::string_view(observation).substr(1));
  if (!name.IsValid())
    return Var::Undefined();
  if (observation.front() == 's')
    return evaluator_->PeekVarInCurrentScope(name);
  if (observation.front() == 'g')
    return name.PeekGlobalVar();
  return evaluator_->PeekVar(name);
}

bool IncrementalEvaluator::DecodeOperands(const std::string& bytes,
                                          std::vector<OperandRecord>* operands,
                                          std::vector<std::string>* writes) {
  std::string_view view(bytes);
  std::string count;
  auto number = [&](size_t* value) {
    if (!ConsumeGraphString(&view, &count) || count.empty() ||
        count.find_first_not_of("0123456789") != std::string::npos)
      return false;
    char* end;
    unsigned long n = strtoul(count.c_str(), &end, 10);
    if (*end || n > 4096)
      return false;
    *value = n;
    return true;
  };
  size_t n;
  if (!number(&n))
    return false;
  size_t total_uses = 0;
  for (size_t i = 0; i < n; ++i) {
    OperandRecord operand;
    size_t uses;
    if (!ConsumeGraphString(&view, &operand.identity) ||
        !ConsumeGraphString(&view, &operand.value) || !number(&uses) ||
        operand.identity.size() > 1024 * 1024 ||
        operand.value.size() > 1024 * 1024 || (total_uses += uses) > 4096)
      return false;
    for (size_t j = 0; j < uses; ++j) {
      std::string name;
      if (!ConsumeGraphString(&view, &name) || name.empty() ||
          name.find_first_of("iovsg") != 0)
        return false;
      operand.uses.push_back(std::move(name));
    }
    operands->push_back(std::move(operand));
  }
  if (!number(&n))
    return false;
  for (size_t i = 0; i < n; ++i) {
    std::string operation;
    if (!ConsumeGraphString(&view, &operation))
      return false;
    writes->push_back(std::move(operation));
  }
  return view.empty();
}

bool IncrementalEvaluator::BeginStatement(const Stmt* statement) {
  if (!enabled_ ||
      g_flags.graph_optimizations != Flags::GraphOptimizations::kAggressive)
    return false;
  // Nested eval creates mutations inside an expansion. That expansion must
  // execute normally; its child statements can still replay independently.
  if (!states_.empty() && states_.back().depth)
    states_.back().reusable = false;
  for (auto& expression : regions_)
    expression.reusable = false;
  StateRegion state;
  state.entry.key = StatementIdentity(statement);
  state.assignment = dynamic_cast<const AssignStmt*>(statement) != nullptr;
  state.source =
      std::string(statement->loc().filename ? statement->loc().filename : "") +
      ":" + std::to_string(statement->loc().lineno);
  state.kind = state.assignment                               ? "assign"
               : dynamic_cast<const RuleStmt*>(statement)     ? "rule"
               : dynamic_cast<const IncludeStmt*>(statement)  ? "include"
               : dynamic_cast<const IfStmt*>(statement)       ? "conditional"
               : dynamic_cast<const ExportStmt*>(statement)   ? "export"
               : dynamic_cast<const UndefineStmt*>(statement) ? "undefine"
               : dynamic_cast<const VpathStmt*>(statement)    ? "vpath"
               : dynamic_cast<const CommandStmt*>(statement)  ? "command"
                                                              : "unsupported";
  state.reusable = !state.entry.key.empty() && states_.size() < 128 &&
                   state.entry.key.size() <= 1024 * 1024;
  auto found = entries_.find(state.entry.key);
  if (state.reusable && found != entries_.end()) {
    checking_state_ = true;
    bool valid = true;
    for (const auto& name : found->second.read_order) {
      if (name.size() < 2 || name.find_first_of("vsg") != 0 ||
          Fingerprint(StateBinding(name)) != found->second.reads.at(name)) {
        valid = false;
        break;
      }
    }
    checking_state_ = false;
    valid = valid && DecodeOperands(found->second.result, &state.operands,
                                    &state.expected_writes);
    for (const auto& operand : state.operands)
      for (const auto& name : operand.uses)
        if (name.front() != 'i' && name.front() != 'o' &&
            !found->second.reads.count(name))
          valid = false;
    if (valid) {
      state.entry = found->second;
      state.replay = true;
    } else {
      state.operands.clear();
      state.expected_writes.clear();
    }
  }
  states_.push_back(std::move(state));
  return true;
}

void IncrementalEvaluator::StateWrite(std::string_view operation,
                                      std::string_view scope,
                                      std::string_view name,
                                      std::string_view value) {
  if (states_.empty() || !states_.back().reusable)
    return;
  auto& state = states_.back();
  if (state.writes.size() >= 4096 || value.size() > 1024 * 1024) {
    state.reusable = false;
    return;
  }
  std::string record;
  for (std::string_view field : {operation, scope, name, value})
    AppendGraphString(field, &record);
  state.write_bytes += record.size();
  if (state.write_bytes > 1024 * 1024) {
    state.reusable = false;
    return;
  }
  state.writes.push_back(std::move(record));
}

void IncrementalEvaluator::StateWriteVar(std::string_view scope,
                                         std::string_view name,
                                         Var* value) {
  if (states_.empty() || !states_.back().reusable)
    return;
  const std::string fingerprint = Fingerprint(value);
  if (fingerprint.empty()) {
    states_.back().reusable = false;
    return;
  }
  StateWrite("binding", scope, name, fingerprint);
}

void IncrementalEvaluator::StateRead(const std::string& name,
                                     Var* value,
                                     char kind,
                                     bool used) {
  if (states_.empty() || checking_state_)
    return;
  StateRegion& state = states_.back();
  if (!state.reusable || state.replay)
    return;
  for (auto binding = locals_.rbegin(); binding != locals_.rend(); ++binding)
    if (binding->name == name && binding->binding == value) {
      if (states_.size() <= binding->statements)
        return;
      break;
    }
  std::string fingerprint = Fingerprint(value);
  if (fingerprint.empty() || name == ".VARIABLES" || name == ".KATI_SYMBOLS") {
    state.reusable = false;
    return;
  }
  const std::string observation = kind + name;
  if (state.entry.reads.size() >= 4096 &&
      !state.entry.reads.count(observation)) {
    state.reusable = false;
    return;
  }
  auto inserted = state.entry.reads.emplace(observation, fingerprint);
  if (inserted.second) {
    state.entry.read_order.push_back(observation);
    state.observation_bytes += observation.size() + fingerprint.size();
    if (state.observation_bytes > 1024 * 1024)
      state.reusable = false;
  } else if (inserted.first->second != fingerprint)
    state.reusable = false;
  if (used && state.depth && !state.operands.empty()) {
    auto& uses = state.operands.back().uses;
    if (++state.use_events > 4096)
      state.reusable = false;
    else
      uses.push_back(observation);
  }
}

std::string IncrementalEvaluator::Operand(const Value* expression) {
  if (states_.empty() || !states_.back().reusable)
    return expression->Eval(evaluator_);
  const size_t index = states_.size() - 1;
  StateRegion& state = states_[index];
  const std::string identity(expression->IncrementalIdentity());
  if (state.replay) {
    std::vector<std::string> observations;
    bool valid = state.cursor < state.operands.size() &&
                 state.operands[state.cursor].identity == identity;
    if (valid) {
      checking_state_ = true;
      for (const auto& name : state.operands[state.cursor].uses) {
        if (name.front() == 'i')
          continue;
        if (name.front() == 'o') {
          std::string current;
          if (!CheckObservation(std::string_view(name).substr(1), &current)) {
            valid = false;
            break;
          }
          observations.push_back(std::move(current));
          continue;
        }
        if (Fingerprint(StateBinding(name)) != state.entry.reads.at(name)) {
          valid = false;
          break;
        }
      }
      checking_state_ = false;
    }
    if (valid) {
      // Restore observation order at the operand's original execution point,
      // after any preceding mutations. Never intern names during validation.
      OperandRecord operand = state.operands[state.cursor++];
      size_t observation_index = 0;
      for (const auto& observation : operand.uses) {
        if (observation.front() == 'o') {
          ReplayObservation(observations[observation_index++]);
          continue;
        }
        Symbol name = Intern(std::string_view(observation).substr(1));
        if (observation.front() == 'i')
          continue;
        Var* value = observation.front() == 's'
                         ? evaluator_->LookupVarInCurrentScope(name)
                     : observation.front() == 'g'
                         ? evaluator_->LookupVarGlobal(name)
                         : evaluator_->LookupVar(name);
        value->Used(evaluator_, name);
      }
      return operand.value;
    }
    // An uncertain operation boundary falls through in the current state.
    // Earlier replayed operands were pure. No effects or invocation restart
    // are needed to recover, even after preceding statements executed effects.
    state.replay = false;
    state.operands.resize(state.cursor);
    state.expected_writes.clear();
    ++state_fallbacks_;
    return Operand(expression);
  }
  if (identity.empty() || state.operands.size() >= 4096) {
    state.reusable = false;
    return expression->Eval(evaluator_);
  }
  const size_t operand_index = state.operands.size();
  state.operands.push_back({identity, {}, {}});
  ++state.depth;
  std::string result = expression->Eval(evaluator_);
  // Nested eval may have reallocated states_; retain indices, not references.
  --states_[index].depth;
  states_[index].operands[operand_index].value = result;
  if (result.size() > 1024 * 1024)
    states_[index].reusable = false;
  return result;
}

void IncrementalEvaluator::EndStatement() {
  StateRegion state = std::move(states_.back());
  states_.pop_back();
  if (!state.reusable) {
    EraseEntry(state.entry.key);
    return;
  }
  if (state.replay) {
    if (state.cursor != state.operands.size()) {
      ++state_fallbacks_;
      EraseEntry(state.entry.key);
      return;
    }
    if (state.writes == state.expected_writes) {
      ++state_replays_;
      ++state_replay_kinds_[state.kind];
      if (g_flags.regen_debug)
        LOG_STAT("state replay: %s %s", state.source.c_str(),
                 state.kind.c_str());
      if (state.assignment)
        ++assignment_replays_;
      return;
    }
    // Includes revalidate source identities while walking their current child
    // regions. Replace the certificate with the accepted current mutations.
    ++state_fallbacks_;
  }
  std::string bytes;
  AppendGraphString(std::to_string(state.operands.size()), &bytes);
  for (const auto& operand : state.operands) {
    AppendGraphString(operand.identity, &bytes);
    AppendGraphString(operand.value, &bytes);
    AppendGraphString(std::to_string(operand.uses.size()), &bytes);
    for (const auto& name : operand.uses)
      AppendGraphString(name, &bytes);
    if (bytes.size() > 1024 * 1024)
      return;
  }
  AppendGraphString(std::to_string(state.writes.size()), &bytes);
  for (const auto& write : state.writes) {
    AppendGraphString(write, &bytes);
    if (bytes.size() > 1024 * 1024)
      return;
  }
  state.entry.result = std::move(bytes);
  StoreEntry(std::move(state.entry));
}

IncrementalEvaluator::IncrementalEvaluator(Evaluator* evaluator)
    : evaluator_(evaluator),
      enabled_(g_flags.generate_ninja && !g_flags.explain &&
               g_flags.GraphOptimizationEnabled("evaluation") &&
               !g_flags.dump_variable_assignment_trace &&
               !g_flags.dump_include_graph &&
               !g_flags.warn_undefined_variables) {
  if (!enabled_)
    return;
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) {
    enabled_ = false;
    return;
  }
  previous_active_ = active_evaluation;
  active_evaluation = this;
  identity_ = "evaluation-v7\n" + std::string(cwd) + "\n" +
              GetFileIdentity(GetExecutablePath()) + "\n" +
              g_flags.GraphOptimizationIdentity();
  AppendGraphString(GetExecutablePath(), &identity_);
  AppendGraphString(g_flags.evaluation_context, &identity_);
  identity_ += g_flags.use_find_emulator ? '1' : '0';
  identity_ += g_flags.werror_find_emulator ? '1' : '0';
  const std::string directory = g_flags.ninja_dir ? g_flags.ninja_dir : ".";
  path_ = directory + "/.kati_stamp.evaluation" +
          (g_flags.ninja_suffix ? g_flags.ninja_suffix : "");
  std::string bytes;
  if (!ReadGraphCache(path_, &bytes))
    return;
  std::string_view view(bytes);
  std::string identity, key, result, count, name, value;
  if (!ConsumeGraphString(&view, &identity) || identity != identity_)
    return;
  size_t loaded_bytes = 0;
  while (!view.empty() && entries_.size() < 100000) {
    Entry entry;
    if (!ConsumeGraphString(&view, &entry.key) ||
        !ConsumeGraphString(&view, &entry.result) ||
        !ConsumeGraphString(&view, &count)) {
      entries_.clear();
      return;
    }
    char* end;
    unsigned long n = strtoul(count.c_str(), &end, 10);
    if (end == count.c_str() || *end || n > 4096) {
      entries_.clear();
      return;
    }
    for (unsigned long i = 0; i < n; ++i) {
      if (!ConsumeGraphString(&view, &name) ||
          !ConsumeGraphString(&view, &value)) {
        entries_.clear();
        return;
      }
      if (!entry.reads.emplace(name, value).second) {
        entries_.clear();
        return;
      }
      entry.read_order.push_back(name);
    }
    if (!ConsumeGraphString(&view, &count)) {
      entries_.clear();
      return;
    }
    n = strtoul(count.c_str(), &end, 10);
    if (end == count.c_str() || *end || n > 4096) {
      entries_.clear();
      return;
    }
    for (unsigned long i = 0; i < n; ++i) {
      if (!ConsumeGraphString(&view, &name) ||
          !ConsumeGraphString(&view, &value) ||
          !entry.preconditions.emplace(name, value).second) {
        entries_.clear();
        return;
      }
    }
    if (!ConsumeGraphString(&view, &count) || count.empty() ||
        count.find_first_not_of("0123456789") != std::string::npos) {
      entries_.clear();
      return;
    }
    n = strtoul(count.c_str(), &end, 10);
    if (*end || n > 4096) {
      entries_.clear();
      return;
    }
    for (unsigned long i = 0; i < n; ++i) {
      if (!ConsumeGraphString(&view, &name) || name.empty() ||
          (name.front() != 'i' && name.front() != 'v' && name.front() != 'o') ||
          (name.front() == 'v' && !entry.reads.count(name.substr(1)))) {
        entries_.clear();
        return;
      }
      entry.uses.push_back(name);
    }
    const std::string key = entry.key;
    const size_t bytes = EntryBytes(entry);
    loaded_bytes += bytes;
    if (bytes > 4 * 1024 * 1024 || loaded_bytes > 60 * 1024 * 1024) {
      entries_.clear();
      return;
    }
    if (!entries_.emplace(key, std::move(entry)).second) {
      entries_.clear();
      return;
    }
  }
  if (!view.empty())
    entries_.clear();
  entries_bytes_ = entries_.empty() ? 0 : loaded_bytes;
}

IncrementalEvaluator::~IncrementalEvaluator() {
  if (active_evaluation == this)
    active_evaluation = previous_active_;
}

Var* IncrementalEvaluator::PeekBinding(const std::string& name) {
  const Symbol symbol = FindSymbol(name);
  return symbol.IsValid() ? evaluator_->PeekVar(symbol) : Var::Undefined();
}

std::string IncrementalEvaluator::Fingerprint(const Var* value) {
  if (value->Deprecated() || value->Obsolete() ||
      !value->VisibilityPrefix().empty() ||
      (strcmp(value->Flavor(), "simple") != 0 &&
       strcmp(value->Flavor(), "recursive") != 0 && value->IsDefined()))
    return {};
  if (value->String().size() > 1024 * 1024)
    return {};
  std::string result = std::to_string(static_cast<int>(value->Origin()));
  AppendGraphString(value->Flavor(), &result);
  AppendGraphString(value->String(), &result);
  if (const auto* recursive = dynamic_cast<const RecursiveVar*>(value)) {
    if (recursive->v_->IncrementalIdentity().empty() ||
        value->SelfReferential())
      return {};
    AppendGraphString(recursive->v_->IncrementalIdentity(), &result);
  }
  result += value->IsDefined() ? '1' : '0';
  result += value->ReadOnly() ? '1' : '0';
  result += static_cast<char>(value->op());
  if (value->TargetAppend())
    AppendGraphString(value->TargetAppend()->IncrementalIdentity(), &result);
  return result;
}

bool IncrementalEvaluator::ShouldTrace(const Value* expression) const {
  return enabled_ && regions_.size() < 128 &&
         expression->IsIncrementalCandidate(
             g_flags.graph_optimizations ==
             Flags::GraphOptimizations::kAggressive);
}

bool IncrementalEvaluator::Begin(const Value* expression, std::string* output) {
  if (!enabled_)
    return false;
  const Loc& loc = expression->Location();
  std::string key = loc.filename ? loc.filename : "";
  key += ':' + std::to_string(loc.lineno);
  AppendGraphString(expression->IncrementalIdentity(), &key);
  key += evaluator_->avoid_io() ? '1' : '0';
  key += ':' + std::to_string(evaluator_->eval_depth());
  // Diagnostics and target-specific expansion use the caller's location.
  AppendGraphString(
      evaluator_->loc().filename ? evaluator_->loc().filename : "", &key);
  key += ':' + std::to_string(evaluator_->loc().lineno);
  if (!AppendScopeIdentity(&key))
    return BeginRegion({}, output, false);
  return BeginRegion(std::move(key), output,
                     !expression->IncrementalIdentity().empty());
}

bool IncrementalEvaluator::BeginRegion(std::string key,
                                       std::string* output,
                                       bool reusable) {
  reusable = reusable && !key.empty() && key.size() <= 1024 * 1024;
  auto found = reusable ? entries_.find(key) : entries_.end();
  if (found != entries_.end()) {
    bool valid = true;
    std::vector<std::string> observations;
    // Check assignment precedence before validating an RHS that ordinary
    // evaluation might skip. Validation must not intern obsolete names.
    for (const auto& condition : found->second.preconditions) {
      if (Fingerprint(PeekBinding(condition.first)) != condition.second) {
        valid = false;
        break;
      }
    }
    if (valid)
      for (const auto& name : found->second.read_order) {
        if (Fingerprint(PeekBinding(name)) != found->second.reads.at(name)) {
          valid = false;
          break;
        }
      }
    if (valid)
      for (const auto& event : found->second.uses)
        if (event.front() == 'o') {
          std::string current;
          if (!CheckObservation(std::string_view(event).substr(1), &current)) {
            valid = false;
            break;
          }
          observations.push_back(std::move(current));
        }
    if (valid) {
      // Repeat use tracking, not evaluation. This retains environment and
      // undefined-variable dependencies in the generated regeneration stamp.
      size_t observation_index = 0;
      for (const auto& event : found->second.uses) {
        if (event.front() == 'o') {
          ReplayObservation(observations[observation_index++]);
          continue;
        }
        Symbol name = Intern(std::string_view(event).substr(1));
        if (event.front() == 'v') {
          Var* value = evaluator_->LookupVar(name);
          value->Used(evaluator_, name);
        }
      }
      *output += found->second.result;
      ++hits_;
      return true;
    }
    ++changed_bindings_;
  } else {
    ++cold_regions_;
  }
  ++misses_;
  Region region;
  region.entry.key = std::move(key);
  region.reusable = reusable;
  region.start = output->size();
  regions_.push_back(std::move(region));
  return false;
}

void IncrementalEvaluator::Read(const std::string& name, Var* value) {
  StateRead(name, value, 'v', true);
  if (!enabled_ || regions_.empty())
    return;
  // Special built-ins observe more state than a variable's stored text.
  if (name == ".VARIABLES" || name == ".KATI_SYMBOLS") {
    Effect();
    return;
  }
  std::string fingerprint = Fingerprint(value);
  if (fingerprint.empty()) {
    Effect();
    return;
  }
  for (size_t index = 0; index < regions_.size(); ++index) {
    Region& region = regions_[index];
    if (!region.reusable)
      continue;
    bool local = false;
    for (auto binding = locals_.rbegin(); binding != locals_.rend(); ++binding)
      if (binding->name == name && binding->binding == value) {
        local = index < binding->expressions;
        break;
      }
    if (local)
      continue;
    if (region.entry.uses.size() >= 4096) {
      region.reusable = false;
      continue;
    }
    region.entry.uses.push_back("v" + name);
    if (region.entry.reads.size() >= 4096 && !region.entry.reads.count(name)) {
      region.reusable = false;
      region.entry.reads.clear();
      region.entry.read_order.clear();
      continue;
    }
    auto inserted = region.entry.reads.emplace(name, fingerprint);
    if (inserted.second)
      region.entry.read_order.push_back(name);
    if (inserted.second) {
      region.observation_bytes += name.size() + fingerprint.size();
      if (region.observation_bytes > 1024 * 1024)
        region.reusable = false;
    }
    if (!inserted.second && inserted.first->second != fingerprint)
      region.reusable = false;
  }
}

void IncrementalEvaluator::Effect() {
  InvalidateFileObservations();
  if (!enabled_)
    return;
  ++effects_;
  if (!states_.empty())
    states_.back().reusable = false;
  for (Region& region : regions_)
    region.reusable = false;
}

void IncrementalEvaluator::End(std::string* output) {
  if (!enabled_)
    return;
  Region region = std::move(regions_.back());
  regions_.pop_back();
  if (region.reusable) {
    if (output->size() < region.start ||
        output->size() - region.start > 1024 * 1024 ||
        region.entry.reads.size() > 4096 ||
        region.entry.key.size() > 1024 * 1024)
      return;
    region.entry.result = output->substr(region.start);
    StoreEntry(std::move(region.entry));
  }
}

void IncrementalEvaluator::Save() {
  if (!enabled_)
    return;
  LOG_STAT("state program replays: %zu, %zu boundary fallbacks", state_replays_,
           state_fallbacks_);
  LOG_STAT("native observations: %zu validated, %zu invalidated",
           observation_hits_, observation_misses_);
  for (const auto& kind : state_replay_kinds_)
    LOG_STAT("state operation replays (%s): %zu", kind.first.c_str(),
             kind.second);
  if (entries_.empty()) {
    LOG_STAT("evaluation regions: %zu hits, %zu misses, %zu effect barriers",
             hits_, misses_, effects_);
    return;
  }
  std::string bytes;
  AppendGraphString(identity_, &bytes);
  for (const auto& item : entries_) {
    const Entry& entry = item.second;
    AppendGraphString(entry.key, &bytes);
    AppendGraphString(entry.result, &bytes);
    AppendGraphString(std::to_string(entry.reads.size()), &bytes);
    for (const auto& name : entry.read_order) {
      AppendGraphString(name, &bytes);
      AppendGraphString(entry.reads.at(name), &bytes);
    }
    AppendGraphString(std::to_string(entry.preconditions.size()), &bytes);
    for (const auto& condition : entry.preconditions) {
      AppendGraphString(condition.first, &bytes);
      AppendGraphString(condition.second, &bytes);
    }
    AppendGraphString(std::to_string(entry.uses.size()), &bytes);
    for (const auto& event : entry.uses)
      AppendGraphString(event, &bytes);
    if (bytes.size() > 60 * 1024 * 1024)
      break;
  }
  WriteGraphCache(path_, bytes);
  LOG_STAT("evaluation regions: %zu hits, %zu misses, %zu effect barriers",
           hits_, misses_, effects_);
  LOG_STAT("assignment region replays: %zu", assignment_replays_);
  LOG_STAT("evaluation misses: %zu changed bindings, %zu cold regions",
           changed_bindings_, cold_regions_);
}

EvaluationRegion::EvaluationRegion(Evaluator* evaluator,
                                   const Value* expression,
                                   std::string* output)
    : evaluator_(evaluator->incremental_evaluator()),
      output_(output),
      reused_(false),
      active_(evaluator_->ShouldTrace(expression)) {
  if (active_)
    reused_ = evaluator_->Begin(expression, output);
}

EvaluationRegion::~EvaluationRegion() {
  if (active_ && !reused_)
    evaluator_->End(output_);
}
