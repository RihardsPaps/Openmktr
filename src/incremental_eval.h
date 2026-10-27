// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef INCREMENTAL_EVAL_H_
#define INCREMENTAL_EVAL_H_

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class Evaluator;
class Value;
class Var;
struct Stmt;
struct AssignStmt;

// Persist normalized statement programs and nested expansion observations.
// Replay always applies mutations to the current invocation's fresh state.
class IncrementalEvaluator {
 public:
  explicit IncrementalEvaluator(Evaluator* evaluator);
  ~IncrementalEvaluator();
  bool enabled() const { return enabled_; }
  bool ShouldTrace(const Value* expression) const;
  bool Begin(const Value* expression, std::string* output);
  void End(std::string* output);
  void Read(const std::string& name, Var* value);
  bool BeginStatement(const Stmt* statement);
  void EndStatement();
  std::string Operand(const Value* expression);
  // Read kind: visible binding, current scope only, or global binding.
  void StateRead(const std::string& name, Var* value, char kind, bool used);
  void StateWrite(std::string_view operation,
                  std::string_view scope,
                  std::string_view name,
                  std::string_view value);
  void StateWriteVar(std::string_view scope, std::string_view name, Var* value);
  bool RecordingStatement() const {
    return !states_.empty() && states_.back().reusable;
  }
  void Interned(std::string_view name);
  bool Observing() const {
    return enabled_ &&
           (!regions_.empty() || (!states_.empty() && states_.back().reusable &&
                                  states_.back().depth));
  }
  void Observe(std::string record);
  uint64_t BeginLocal(std::string_view name, const Var* binding);
  void EndLocal(uint64_t token);
  void Effect();
  void Save();

 private:
  struct Entry {
    std::string key;
    std::string result;
    std::map<std::string, std::string> reads;
    std::vector<std::string> read_order;
    std::map<std::string, std::string> preconditions;
    std::vector<std::string> uses;
  };
  struct Region {
    Entry entry;
    size_t start;
    bool reusable = true;
    size_t observation_bytes = 0;
  };
  struct OperandRecord {
    std::string identity;
    std::string value;
    std::vector<std::string> uses;
  };
  struct StateRegion {
    Entry entry;
    std::vector<OperandRecord> operands;
    size_t cursor = 0;
    size_t depth = 0;
    bool reusable = true;
    bool replay = false;
    bool assignment = false;
    std::string kind;
    std::string source;
    std::vector<std::string> writes;
    std::vector<std::string> expected_writes;
    size_t observation_bytes = 0;
    size_t use_events = 0;
    size_t write_bytes = 0;
  };
  struct LocalBinding {
    uint64_t token;
    std::string name;
    const Var* binding;
    size_t expressions;
    size_t statements;
  };
  std::vector<LocalBinding> locals_;
  uint64_t local_token_ = 0;
  IncrementalEvaluator* previous_active_ = nullptr;
  std::string StatementIdentity(const Stmt* statement);
  bool AppendScopeIdentity(std::string* key);
  Var* StateBinding(const std::string& observation);
  bool DecodeOperands(const std::string& bytes,
                      std::vector<OperandRecord>* operands,
                      std::vector<std::string>* writes);
  std::vector<StateRegion> states_;
  bool checking_state_ = false;
  size_t state_replays_ = 0;
  size_t state_fallbacks_ = 0;
  std::map<std::string, size_t> state_replay_kinds_;
  std::string Fingerprint(const Var* value);
  Var* PeekBinding(const std::string& name);
  bool BeginRegion(std::string key, std::string* output, bool reusable);
  Evaluator* evaluator_;
  bool enabled_;
  std::string path_;
  std::string identity_;
  std::vector<Region> regions_;
  std::unordered_map<std::string, Entry> entries_;
  size_t entries_bytes_ = 0;
  size_t EntryBytes(const Entry& entry) const;
  void StoreEntry(Entry entry);
  void EraseEntry(const std::string& key);
  bool CheckObservation(std::string_view record, std::string* accepted);
  void ReplayObservation(std::string_view record);
  size_t observation_hits_ = 0;
  size_t observation_misses_ = 0;
  size_t hits_ = 0;
  size_t misses_ = 0;
  size_t effects_ = 0;
  size_t assignment_replays_ = 0;
  size_t changed_bindings_ = 0;
  size_t cold_regions_ = 0;
};

void RecordEvaluationIntern(std::string_view name);

class EvaluationLocalBinding {
 public:
  EvaluationLocalBinding(Evaluator* evaluator,
                         std::string_view name,
                         const Var* binding);
  ~EvaluationLocalBinding();

 private:
  IncrementalEvaluator* evaluator_;
  uint64_t token_;
};

class EvaluationRegion {
 public:
  EvaluationRegion(Evaluator* evaluator,
                   const Value* expression,
                   std::string* output);
  ~EvaluationRegion();
  bool reused() const { return reused_; }

 private:
  IncrementalEvaluator* evaluator_;
  std::string* output_;
  bool reused_;
  bool active_;
};

#endif
