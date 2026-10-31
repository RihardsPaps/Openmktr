// FORK MODIFICATION NOTICE (2026)
// Echo ordinary Make recipes by default; retain opt-in compact progress output.
// Index lazy variable definitions over immutable buffers and reuse validated
// sealed contexts within one Ninja invocation, with checked standalone
// fallback. Decode runtime variable records without copying fields and restore
// ASTs lazily. Certify actual direct include-remake reads without rerunning
// bootstrap tools. Exclude unexported environment values unless a recipe reads
// them. Retain command locations in the versioned runtime recipe payload. Bind
// observed variable reads to successful runtime recipes and full-context
// fallback. Bound literal-reference runtime contexts and inspect all recipe
// branches. Serialize and restore recipes that consume execution-time generated
// contents. Defer wildcard observations until prerequisite producers have
// completed. Refresh recipe-time target assignments before expanding retained
// scopes. Preserve private graph controls across recursive recipe environments.
// Track and validate incremental evaluation, include proofs and imported Ninja
// graphs. Preserve live Ninja load limits across deferred recursive
// environments. Preserve literal saved compiler commands when resolving
// deferred assignments. Accept recursive assignment operators when expanding
// deferred values. Exclude recipe control prefixes when recognizing deferred
// assignments. Changed by the Openmktr fork, maintained by Rihards Paps and
// Haralds Paps.
// Changed build execution, evaluation, or portability for this fork.
// Keep jobserver reservations live instead of copying them as make exports.
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

#include "command.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "dep.h"
#include "eval.h"
#include "exec.h"
#include "file_cache.h"
#include "fileutil.h"
#include "flags.h"
#include "func.h"
#include "graph_optimization.h"
#include "log.h"
#include "strutil.h"
#include "var.h"

extern char** environ;

namespace {
// Structural inspection never evaluates a variable or a function. Literal
// names also cover call's macro operand. Computed references require a full
// environment snapshot because their eventual names depend on runtime input.
struct RecipeInspection {
  Evaluator* ev;
  std::unordered_set<Symbol> visited;
  bool io = false;
  bool dynamic = false;
  bool conservative = false;
  void Variable(const std::string& name) {
    if (name == ".VARIABLES" || name == ".KATI_SYMBOLS")
      conservative = true;
    Symbol symbol = Intern(name);
    if (!visited.insert(symbol).second)
      return;
    // Lookup records environment/undefined dependencies without expanding a
    // value or executing a function.
    Var* var = ev->LookupVar(symbol);
    if (auto* recursive = dynamic_cast<RecursiveVar*>(var))
      Expression(recursive->v_->IncrementalIdentity());
  }
  void Expression(std::string_view record) {
    // Once full-context runtime evaluation is required, further reference
    // collection cannot narrow the snapshot. Keep traversing bounded recipes.
    if (dynamic || (io && conservative))
      return;
    std::string kind, field;
    if (!ConsumeGraphString(&record, &kind)) {
      dynamic = true;
      return;
    }
    if (kind == "literal")
      return;
    if (kind == "reference") {
      if (ConsumeGraphString(&record, &field))
        Variable(field);
      return;
    }
    if (kind == "dynamic-reference" || kind == "substitution")
      dynamic = true;
    if (kind == "function") {
      if (!ConsumeGraphString(&record, &field))
        return;
      io |= field == "shell" || field == "file" || field == "wildcard" ||
            field == "realpath";
      if (field == "eval" || field == "foreach")
        conservative = true;
      if (field == "call" || field == "value" || field == "origin" ||
          field == "flavor") {
        std::string argument, name, tag;
        if (!ConsumeGraphString(&record, &argument)) {
          dynamic = true;
          return;
        }
        std::string_view operand(argument);
        if (ConsumeGraphString(&operand, &tag) && tag == "literal" &&
            ConsumeGraphString(&operand, &name) && operand.empty())
          Variable(name);
        else
          dynamic = true;
        Expression(argument);
      }
    }
    while (!dynamic && !(io && conservative) &&
           ConsumeGraphString(&record, &field))
      Expression(field);
  }
};

std::string RecipeVariable(Symbol symbol, Var* var) {
  std::string record;
  AppendGraphString(symbol.str(), &record);
  AppendGraphString(std::to_string(static_cast<int>(var->Origin())), &record);
  auto* recursive = dynamic_cast<RecursiveVar*>(var);
  AppendGraphString(recursive ? "recursive" : "simple", &record);
  AppendGraphString(
      recursive ? recursive->v_->IncrementalIdentity() : var->String(),
      &record);
  AppendGraphString(var->String(), &record);
  return record;
}

bool IsAutomaticRecipeVar(Var* var);

bool ReuseRuntimeReads(const std::string& path,
                       const std::string& structural,
                       Evaluator* ev,
                       std::string* identity,
                       std::string* generation) {
  std::string payload, owner, success, version, key, previous_identity,
      definitions;
  if (!ReadGraphCache(path, &payload))
    return false;
  std::string_view data(payload);
  for (std::string* field : {&owner, &success, &version, &key, generation,
                             &previous_identity, &definitions})
    if (!ConsumeGraphString(&data, field))
      return false;
  std::string recorded;
  if (!data.empty() || version != "runtime-reads-v1" || key != structural ||
      !ReadNinjaRecipeState(owner + ".success", &recorded) ||
      recorded != success)
    return false;
  uint64_t counter = 0;
  if (generation->empty() || generation->size() > 19)
    return false;
  for (char c : *generation) {
    if (c < '0' || c > '9')
      return false;
    counter = counter * 10 + (c - '0');
  }
  bool unchanged = true;
  std::string_view entries(definitions);
  while (!entries.empty()) {
    std::string name, expected;
    if (!ConsumeGraphString(&entries, &name) ||
        !ConsumeGraphString(&entries, &expected) || name == ".VARIABLES" ||
        name == ".KATI_SYMBOLS")
      return false;
    Symbol symbol = Intern(name);
    Var* var = ev->PeekVar(symbol);
    std::string current;
    if (var->IsDefined() && var->Origin() != VarOrigin::AUTOMATIC &&
        !IsAutomaticRecipeVar(var)) {
      if (!dynamic_cast<SimpleVar*>(var) && !dynamic_cast<RecursiveVar*>(var)) {
        return false;
      }
      current = RecipeVariable(symbol, var);
    }
    unchanged &= current == expected;
  }
  if (unchanged) {
    *identity = std::move(previous_identity);
  } else {
    *generation = std::to_string(counter + 1);
    identity->clear();
    AppendGraphString("observed-runtime-identity-v1", identity);
    AppendGraphString(structural, identity);
    AppendGraphString(*generation, identity);
  }
  return true;
}
}  // namespace

void CommitRuntimeRecipeReads(const std::string& state_path,
                              const std::string& state) {
  std::string pending;
  const std::string temporary = state_path + ".reads-pending";
  if (!ReadGraphCache(temporary, &pending))
    return;
  std::string_view record(pending);
  std::string path, verified;
  if (!ConsumeGraphString(&record, &path))
    ERROR("invalid pending runtime reads; regenerate the Ninja graph");
  AppendGraphString(state_path, &verified);
  AppendGraphString(state, &verified);
  verified.append(record);
  if (!WriteGraphCache(path, verified)) {
    unlink((state_path + ".success").c_str());
    ERROR("cannot record successful runtime reads; regenerate the Ninja graph");
  }
  unlink(temporary.c_str());
}

struct RuntimeRecipeReadCapture::Impl {
  struct Context {
    std::string bytes;
    std::unordered_map<Symbol, std::string_view> definitions;
    std::unordered_set<Symbol> environment;
  };
  Evaluator* evaluator;
  bool tracking = false;
  std::shared_ptr<const Context> globals;
  Context locals;
  std::unordered_set<Symbol> reads;
  std::string observation, structural, generation, identity;

  static void Decode(Context* context) {
    std::string_view data(context->bytes);
    while (!data.empty()) {
      const std::string_view original = data;
      std::string definition, name, origin;
      if (!ConsumeGraphString(&data, &definition))
        ERROR("invalid bootstrap variable context");
      std::string_view fields(definition);
      if (!ConsumeGraphString(&fields, &name) ||
          !ConsumeGraphString(&fields, &origin))
        ERROR("invalid bootstrap variable definition");
      Symbol symbol = Intern(name);
      context->definitions[symbol] = original.substr(8, definition.size());
      if ((origin == std::to_string(static_cast<int>(VarOrigin::ENVIRONMENT)) ||
           origin == std::to_string(
                         static_cast<int>(VarOrigin::ENVIRONMENT_OVERRIDE))) &&
          !HasPrefix(name, "KATI_") && !HasPrefix(name, "OMKTR_"))
        context->environment.insert(symbol);
    }
  }
};

RuntimeRecipeReadCapture::RuntimeRecipeReadCapture(const Command& command,
                                                   Evaluator* evaluator)
    : impl_(new Impl) {
  impl_->evaluator = evaluator;
  std::string payload;
  if (!ReadGraphCache(command.runtime_sidecar, &payload))
    ERROR("cannot read bootstrap runtime metadata");
  std::string_view data(payload);
  std::vector<std::string> fields;
  while (!data.empty()) {
    fields.emplace_back();
    if (!ConsumeGraphString(&data, &fields.back()))
      ERROR("invalid bootstrap runtime metadata");
  }
  if (fields.size() != 14 || fields[0] != "runtime-recipe-v5")
    ERROR("incompatible bootstrap runtime metadata");
  impl_->observation = fields[10];
  if (impl_->observation.empty())
    return;
  impl_->structural = fields[11];
  impl_->generation = fields[12];
  impl_->identity = fields[13];
  // One immutable global table is shared across the serialized expansions;
  // target records remain separate and never duplicate the whole context.
  static std::string cached_path;
  static std::string cached_file_identity;
  static std::shared_ptr<const Impl::Context> cached_globals;
  const std::string file_identity = GetFileIdentity(fields[7]);
  if (!cached_globals || cached_path != fields[7] ||
      cached_file_identity != file_identity) {
    auto globals = std::make_shared<Impl::Context>();
    if (!ReadGraphCache(fields[7], &globals->bytes))
      ERROR("cannot read bootstrap global context");
    Impl::Decode(globals.get());
    cached_path = fields[7];
    cached_file_identity = file_identity;
    cached_globals = std::move(globals);
  }
  impl_->globals = cached_globals;
  impl_->locals.bytes = std::move(fields[8]);
  Impl::Decode(&impl_->locals);
  std::unordered_map<Symbol, bool> exports;
  data = fields[9];
  while (!data.empty()) {
    std::string name, exported;
    if (!ConsumeGraphString(&data, &name) ||
        !ConsumeGraphString(&data, &exported))
      ERROR("invalid bootstrap exports");
    exports[Intern(name)] = exported == "1";
    if (exported == "1")
      impl_->reads.insert(Intern(name));
  }
  for (const Impl::Context* context :
       {impl_->globals.get(),
        static_cast<const Impl::Context*>(&impl_->locals)})
    for (Symbol name : context->environment) {
      auto exported = exports.find(name);
      if (exported == exports.end() || exported->second)
        impl_->reads.insert(name);
    }
  if (unlink(impl_->observation.c_str()) != 0 && errno != ENOENT)
    ERROR("cannot retire bootstrap runtime observations");
  impl_->tracking = true;
  evaluator->set_recipe_reads(&impl_->reads);
}

RuntimeRecipeReadCapture::~RuntimeRecipeReadCapture() {
  if (impl_->tracking)
    impl_->evaluator->set_recipe_reads(nullptr);
}

void RuntimeRecipeReadCapture::Finish(const std::string& owner) {
  if (!impl_->tracking)
    return;
  impl_->evaluator->set_recipe_reads(nullptr);
  impl_->tracking = false;
  if (impl_->reads.count(Intern(".VARIABLES")) ||
      impl_->reads.count(Intern(".KATI_SYMBOLS")))
    return;
  std::vector<Symbol> names(impl_->reads.begin(), impl_->reads.end());
  std::sort(names.begin(), names.end(),
            [](Symbol a, Symbol b) { return a.str() < b.str(); });
  std::string definitions, pending;
  for (Symbol name : names) {
    AppendGraphString(name.str(), &definitions);
    auto local = impl_->locals.definitions.find(name);
    auto global = impl_->globals->definitions.find(name);
    const std::string_view definition =
        local != impl_->locals.definitions.end()      ? local->second
        : global != impl_->globals->definitions.end() ? global->second
                                                      : std::string_view();
    AppendGraphString(definition, &definitions);
  }
  for (const std::string& value :
       {impl_->observation, std::string("runtime-reads-v1"), impl_->structural,
        impl_->generation, impl_->identity, definitions})
    AppendGraphString(value, &pending);
  if (!WriteGraphCache(owner + ".reads-pending", pending))
    unlink((owner + ".reads-pending").c_str());
}

bool MakeRuntimeRecipe(const DepNode& node,
                       Evaluator* ev,
                       const std::string& directory,
                       std::vector<Command>* commands) {
  if (node.cmds.empty())
    return false;
  // With no prerequisites there is no producer whose completion Ninja must
  // await. Retain ordinary generation-time expansion for such recipes,
  // including existing diagnostics and source-tree find emulation.
  if (node.actual_inputs.empty() && node.actual_order_only_inputs.empty())
    return false;
  ev->set_current_scope(node.rule_vars);
  ev->ApplyRecipeTargetUpdates(node.lexical_output);
  RecipeInspection inspection{ev, {}};
  for (Value* value : node.cmds)
    inspection.Expression(value->IncrementalIdentity());
  inspection.Variable("SHELL");
  inspection.Variable(".SHELLFLAGS");
  // Recursive execution reads this transport variable implicitly after
  // expanding the recipe. Its include paths and overrides are dependencies
  // even when the recipe never spells $(MAKEFLAGS).
  inspection.Variable("MAKEFLAGS");
  for (const auto& entry : ev->exports())
    if (entry.second)
      inspection.Variable(entry.first.str());
  if (ev->current_scope())
    for (Symbol name : ev->current_scope()->exported())
      if (ev->current_scope()->IsExported(name))
        inspection.Variable(name.str());
  if (node.export_all_variables)
    inspection.dynamic = true;
  if (!inspection.io && !inspection.dynamic) {
    ev->set_current_scope(nullptr);
    return false;
  }
  inspection.dynamic |= inspection.conservative;
  // Computed references and every deferred shell can observe a newly added
  // inherited export. Reparse before reusing the bounded definitions/read set;
  // a watch list of previously existing environment names cannot detect it.
  MakefileCacheManager::Get().RequireEvaluation();
  if (node.double_colon_group_inputs.size() > 1)
    ERROR(
        "runtime recipe evaluation does not support multiple double-colon "
        "groups");
  std::string payload;
  AppendGraphString("runtime-recipe-v5", &payload);
  AppendGraphString(node.lexical_output.str(), &payload);
  AppendGraphString(
      node.output_pattern.IsValid() ? node.output_pattern.str() : "", &payload);
  std::string flags;
  for (bool flag : {node.oneshell, node.ignore_errors, node.silent,
                    node.export_all_variables})
    flags += flag ? '1' : '0';
  AppendGraphString(flags, &payload);
  std::string identity = payload;
  auto reads_auto = [&](const std::string& name) {
    return inspection.dynamic || inspection.visited.count(Intern(name)) ||
           inspection.visited.count(Intern(name + "D")) ||
           inspection.visited.count(Intern(name + "F"));
  };
  for (const auto* inputs :
       {&node.actual_inputs, &node.actual_order_only_inputs}) {
    std::string record;
    const bool normal = inputs == &node.actual_inputs;
    const bool all = normal
                         ? reads_auto("+") || reads_auto("^") || reads_auto("?")
                         : reads_auto("|");
    const size_t count = all ? inputs->size()
                         : normal && reads_auto("<") && !inputs->empty() ? 1
                                                                         : 0;
    for (size_t i = 0; i < count; ++i)
      AppendGraphString((*inputs)[i].str(), &record);
    AppendGraphString(record, &payload);
  }
  std::string recipes;
  const size_t identity_suffix = payload.size();
  for (Value* value : node.cmds) {
    if (value->IncrementalIdentity().empty())
      ERROR(
          "cannot serialize runtime recipe; regenerate with supported "
          "expressions");
    std::string recipe;
    AppendGraphString(
        value->Location().filename ? value->Location().filename : "", &recipe);
    AppendGraphString(std::to_string(value->Location().lineno), &recipe);
    AppendGraphString(value->IncrementalIdentity(), &recipe);
    AppendGraphString(recipe, &recipes);
  }
  AppendGraphString(recipes, &payload);
  std::string variables;
  static Evaluator* context_evaluator = nullptr;
  static uint64_t context_epoch = UINT64_MAX;
  static std::string full_context_path;
  std::string context_path;
  if (inspection.dynamic && context_evaluator == ev &&
      context_epoch == ev->recipe_snapshot_epoch() &&
      full_context_path.compare(0, directory.size() + 1, directory + "/") ==
          0) {
    context_path = full_context_path;
  } else {
    std::string globals;
    std::vector<std::string> selected;
    if (inspection.dynamic) {
      for (size_t i = 0; i < g_symbols->size(); ++i)
        selected.push_back(*(*g_symbols)[i]);
    } else {
      for (Symbol symbol : inspection.visited)
        selected.push_back(symbol.str());
    }
    std::sort(selected.begin(), selected.end());
    for (const std::string& name : selected) {
      Symbol symbol = Intern(name);
      Var* var = symbol.PeekGlobalVar();
      if (var->IsDefined() && var->Origin() != VarOrigin::AUTOMATIC &&
          (dynamic_cast<SimpleVar*>(var) || dynamic_cast<RecursiveVar*>(var)))
        AppendGraphString(RecipeVariable(symbol, var), &globals);
    }
    context_path = directory + "/.kati_runtime-context-" + GraphDigest(globals);
    std::string previous;
    if ((!ReadGraphCache(context_path, &previous) || previous != globals) &&
        !WriteGraphCache(context_path, globals))
      ERROR("cannot write runtime variable context");
    if (inspection.dynamic) {
      context_evaluator = ev;
      context_epoch = ev->recipe_snapshot_epoch();
      full_context_path = context_path;
    }
  }
  AppendGraphString(context_path, &payload);
  // Target-specific bindings override the shared global definitions.
  std::vector<std::string> names;
  if (ev->current_scope()) {
    for (const auto& entry : *ev->current_scope())
      names.push_back(entry.first.str());
  }
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    Symbol symbol = Intern(name);
    if (!inspection.dynamic && !inspection.visited.count(symbol))
      continue;
    Var* var = ev->PeekVar(symbol);
    if (!var->IsDefined() || var->Origin() == VarOrigin::AUTOMATIC)
      continue;
    AppendGraphString(RecipeVariable(symbol, var), &variables);
  }
  AppendGraphString(variables, &payload);
  std::string exports;
  std::map<std::string, bool> ordered_exports;
  for (const auto& entry : ev->exports()) {
    ordered_exports[entry.first.str()] = entry.second;
  }
  if (ev->current_scope()) {
    for (Symbol name : ev->current_scope()->exported()) {
      ordered_exports[name.str()] = ev->current_scope()->IsExported(name);
    }
  }
  for (const auto& entry : ordered_exports) {
    AppendGraphString(entry.first, &exports);
    AppendGraphString(entry.second ? "1" : "0", &exports);
  }
  AppendGraphString(exports, &payload);
  // Automatic prerequisite data changes when depfiles populate. Freshness
  // is checked against the real graph inputs; it is not a command change.
  identity.append(payload.data() + identity_suffix,
                  payload.size() - identity_suffix);
  std::string structural;
  AppendGraphString(node.lexical_output.str(), &structural);
  AppendGraphString(flags, &structural);
  AppendGraphString(
      node.output_pattern.IsValid() ? node.output_pattern.str() : "",
      &structural);
  AppendGraphString(recipes, &structural);
  AppendGraphString(exports, &structural);
  // Every shell can read its inherited exports, including a name that did
  // not exist when a successful read set was recorded. Track that name set
  // structurally; bounded contexts also retain the exported definitions.
  static Evaluator* environment_evaluator = nullptr;
  static uint64_t environment_epoch = UINT64_MAX;
  static std::vector<Symbol> environment_symbols;
  if (environment_evaluator != ev ||
      environment_epoch != ev->recipe_snapshot_epoch()) {
    environment_symbols.clear();
    for (size_t i = 0; i < g_symbols->size(); ++i) {
      Symbol name = Intern(*(*g_symbols)[i]);
      Var* var = name.PeekGlobalVar();
      if (var->IsDefined() &&
          (var->Origin() == VarOrigin::ENVIRONMENT ||
           var->Origin() == VarOrigin::ENVIRONMENT_OVERRIDE) &&
          !HasPrefix(name.str(), "KATI_") && !HasPrefix(name.str(), "OMKTR_"))
        environment_symbols.push_back(name);
    }
    std::sort(environment_symbols.begin(), environment_symbols.end(),
              [](Symbol a, Symbol b) { return a.str() < b.str(); });
    environment_evaluator = ev;
    environment_epoch = ev->recipe_snapshot_epoch();
  }
  std::string environment_names, environment_definitions;
  for (Symbol name : environment_symbols) {
    auto exported = ordered_exports.find(name.str());
    if (exported != ordered_exports.end() && !exported->second)
      continue;
    AppendGraphString(name.str(), &environment_names);
    if (!inspection.dynamic)
      AppendGraphString(RecipeVariable(name, ev->PeekVar(name)),
                        &environment_definitions);
  }
  AppendGraphString(environment_names, &structural);
  if (!inspection.dynamic)
    AppendGraphString(environment_definitions, &identity);
  const std::string observation =
      inspection.dynamic && !node.export_all_variables && !node.is_phony
          ? directory + "/.kati_runtime-reads-" + GraphDigest(structural)
          : "";
  std::string generation = "0";
  if (!observation.empty() &&
      !ReuseRuntimeReads(observation, structural, ev, &identity, &generation)) {
    // Missing or invalid observations must never reuse an identity retained
    // from an older, differently observed context. Fail conservatively.
    AppendGraphString(
        std::to_string(getpid()) + ":" +
            std::to_string(
                std::chrono::system_clock::now().time_since_epoch().count()) +
            ":" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()),
        &identity);
  }
  AppendGraphString(observation, &payload);
  AppendGraphString(structural, &payload);
  AppendGraphString(generation, &payload);
  AppendGraphString(identity, &payload);
  ev->set_current_scope(nullptr);
  const std::string path = directory + "/.kati_runtime-" + GraphDigest(payload);
  std::string previous;
  if ((!ReadGraphCache(path, &previous) || previous != payload) &&
      !WriteGraphCache(path, payload))
    ERROR("cannot write runtime recipe sidecar: %s", path.c_str());
  std::string executable = GetExecutablePath();
  std::string quoted_path = path;
  EscapeShell(&executable);
  EscapeShell(&quoted_path);
  Command command(node.output);
  command.runtime_sidecar = path;
  command.runtime_context = context_path;
  command.runtime_identity = std::move(identity);
  command.cmd = "\"" + executable + "\" --internal-evaluate-recipe \"" +
                quoted_path + "\"";
  command.shell = "/bin/sh";
  command.shellflag = "-c";
  command.echo = false;
  commands->push_back(std::move(command));
  return true;
}

namespace {
struct RuntimeContextBuffer {
  std::shared_ptr<const void> owner;
  std::string_view bytes;
  bool validated = false;
};

bool ValidateRuntimeVariables(std::string_view records) {
  while (!records.empty()) {
    std::string_view fields, name, origin, flavor, value, original;
    if (!ConsumeGraphStringView(&records, &fields) ||
        !ConsumeGraphStringView(&fields, &name) ||
        !ConsumeGraphStringView(&fields, &origin) || origin.size() != 1 ||
        origin[0] < '0' || origin[0] > '7' ||
        !ConsumeGraphStringView(&fields, &flavor) ||
        !ConsumeGraphStringView(&fields, &value) ||
        !ConsumeGraphStringView(&fields, &original) || !fields.empty() ||
        (flavor != "simple" && flavor != "recursive") ||
        (flavor == "recursive" && !Value::ValidateRecipeExpression(value)))
      return false;
  }
  return true;
}

std::string RuntimeContextIdentity(const std::string& path) {
  struct stat metadata;
  if (lstat(path.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode))
    return {};
  return GetFileIdentityFromStat(metadata);
}

bool ReadRuntimeContext(const std::string& path, RuntimeContextBuffer* result) {
#if defined(__linux__) && defined(F_GET_SEALS)
  // This descriptor is an invocation-local capability inherited from the
  // launcher. Require all seals before trusting its validated, immutable data.
  const char* inherited = getenv("KATI_RUNTIME_CONTEXT_FD");
  char* end = nullptr;
  errno = 0;
  const long descriptor = inherited ? strtol(inherited, &end, 10) : -1;
  const int seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
  int actual_seals = -1;
  struct stat metadata;
  if (inherited && end != inherited && !*end && !errno && descriptor >= 3 &&
      descriptor <= INT_MAX &&
      (actual_seals = fcntl(descriptor, F_GET_SEALS)) >= 0 &&
      (actual_seals & seals) == seals && fstat(descriptor, &metadata) == 0 &&
      S_ISREG(metadata.st_mode) && metadata.st_size > 0 &&
      metadata.st_size <= 256 * 1024 * 1024) {
    const size_t size = metadata.st_size;
    void* mapping = mmap(nullptr, size, PROT_READ, MAP_SHARED, descriptor, 0);
    if (mapping != MAP_FAILED) {
      auto owner = std::shared_ptr<const void>(mapping, [size](const void* p) {
        munmap(const_cast<void*>(p), size);
      });
      std::string_view input(static_cast<const char*>(mapping), size);
      std::string_view version, binary;
      std::string absolute;
      AbsPath(path, &absolute);
      const std::string identity = RuntimeContextIdentity(path);
      if (!identity.empty() && ConsumeGraphStringView(&input, &version) &&
          version == "runtime-context-snapshot-v1" &&
          ConsumeGraphStringView(&input, &binary) &&
          binary == GetFileIdentity(GetExecutablePath())) {
        while (!input.empty()) {
          std::string_view name, saved_identity, bytes;
          if (!ConsumeGraphStringView(&input, &name) ||
              !ConsumeGraphStringView(&input, &saved_identity) ||
              !ConsumeGraphStringView(&input, &bytes))
            break;
          if (name == absolute && saved_identity == identity &&
              RuntimeContextIdentity(path) == identity) {
            *result = {std::move(owner), bytes, true};
            return true;
          }
        }
      }
    }
  }
#endif
  auto bytes = std::make_shared<std::string>();
  if (!ReadGraphCache(path, bytes.get()))
    return false;
  *result = {bytes, *bytes, false};
  return true;
}
}  // namespace

std::string RuntimeContextManifestPath(const std::string& graph) {
  return ConcatDir(Dirname(graph),
                   ".kati_contexts-" + std::string(Basename(graph)));
}

void PrepareRuntimeContexts(const std::string& manifest) {
  // Recursive launchers own their own snapshot. Do not trust a saved export or
  // accidentally retain a parent's context when the graph directory changes.
  unsetenv("KATI_RUNTIME_CONTEXT_FD");
#if defined(__linux__) && defined(SYS_memfd_create) && defined(F_ADD_SEALS)
  std::string payload;
  if (!ReadGraphCache(manifest, &payload))
    return;
  std::string_view input(payload), version;
  if (!ConsumeGraphStringView(&input, &version) ||
      version != "runtime-context-manifest-v1")
    return;
  std::string snapshot;
  AppendGraphString("runtime-context-snapshot-v1", &snapshot);
  AppendGraphString(GetFileIdentity(GetExecutablePath()), &snapshot);
  size_t count = 0;
  while (!input.empty()) {
    std::string name;
    if (!ConsumeGraphString(&input, &name) || ++count > 1024)
      return;
    struct stat metadata;
    // Small contexts cost less than building a shared snapshot. Never follow
    // links or block on special files while preparing an optional optimization.
    if (lstat(name.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size < 512 * 1024 || metadata.st_size > 64 * 1024 * 1024 ||
        snapshot.size() + metadata.st_size > 256 * 1024 * 1024 - 4096)
      continue;
    const std::string identity = GetFileIdentityFromStat(metadata);
    std::string bytes;
    if (!ReadGraphCache(name, &bytes) || !ValidateRuntimeVariables(bytes) ||
        RuntimeContextIdentity(name) != identity)
      continue;
    AppendGraphString(name, &snapshot);
    AppendGraphString(identity, &snapshot);
    AppendGraphString(bytes, &snapshot);
  }
  if (snapshot.size() < 512 * 1024)
    return;
  // MFD_ALLOW_SEALING, without CLOEXEC: descendants deliberately inherit it.
  int fd =
      static_cast<int>(syscall(SYS_memfd_create, "omktr-runtime-context", 2U));
  if (fd < 0)
    return;
  std::string_view remaining(snapshot);
  while (!remaining.empty()) {
    const ssize_t size =
        HANDLE_EINTR(write(fd, remaining.data(), remaining.size()));
    if (size <= 0) {
      close(fd);
      return;
    }
    remaining.remove_prefix(size);
  }
  const int seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
  if (fcntl(fd, F_ADD_SEALS, seals) != 0) {
    close(fd);
    return;
  }
  // Keep the capability away from stdio and conventional shell redirections.
  const int inherited = fcntl(fd, F_DUPFD, 64);
  close(fd);
  if (inherited >= 0)
    setenv("KATI_RUNTIME_CONTEXT_FD", std::to_string(inherited).c_str(), 1);
#else
  (void)manifest;
#endif
}

int RunRuntimeRecipe(const char* path) {
  g_flags.executable_path = GetExecutablePath();
  std::string payload;
  if (!ReadGraphCache(path, &payload))
    ERROR(
        "missing or corrupt runtime recipe sidecar; regenerate the Ninja "
        "graph");
  std::string_view data(payload);
  auto take = [](std::string_view* input) {
    std::string value;
    if (!ConsumeGraphString(input, &value))
      ERROR("invalid runtime recipe sidecar; regenerate the Ninja graph");
    return value;
  };
  if (take(&data) != "runtime-recipe-v5")
    ERROR("incompatible runtime recipe sidecar; regenerate the Ninja graph");
  Evaluator evaluator;
  evaluator.Start();
  DepNode node(Intern(take(&data)), false, false);
  node.output_pattern = Intern(take(&data));
  const std::string flags = take(&data);
  if (flags.size() != 4 || flags.find_first_not_of("01") != std::string::npos)
    ERROR("invalid runtime recipe flags; regenerate the Ninja graph");
  node.oneshell = flags[0] == '1';
  node.ignore_errors = flags[1] == '1';
  node.silent = flags[2] == '1';
  node.export_all_variables = flags[3] == '1';
  for (auto* inputs : {&node.actual_inputs, &node.actual_order_only_inputs}) {
    std::string storage = take(&data);
    std::string_view record(storage);
    while (!record.empty())
      inputs->push_back(Intern(take(&record)));
  }
  std::vector<std::unique_ptr<Value>> expressions;
  std::string storage = take(&data);
  std::string_view record(storage);
  while (!record.empty()) {
    const std::string recipe = take(&record);
    std::string_view fields(recipe);
    const std::string filename = take(&fields);
    const std::string line = take(&fields);
    char* end = nullptr;
    errno = 0;
    const long number = strtol(line.c_str(), &end, 10);
    if (errno || end == line.c_str() || *end || number < -1 || number > INT_MAX)
      ERROR("invalid runtime recipe location; regenerate the Ninja graph");
    const Loc loc(filename.empty() ? nullptr : Intern(filename).c_str(),
                  static_cast<int>(number));
    expressions.emplace_back(
        Value::RestoreRecipeExpression(take(&fields), 0, loc));
    if (!fields.empty())
      ERROR(
          "invalid runtime recipe expression data; regenerate the Ninja graph");
    if (!expressions.back())
      ERROR("invalid runtime recipe expression; regenerate the Ninja graph");
    node.cmds.push_back(expressions.back().get());
  }
  RuntimeContextBuffer globals;
  if (!ReadRuntimeContext(take(&data), &globals))
    ERROR(
        "missing or corrupt runtime variable context; regenerate the Ninja "
        "graph");
  std::unordered_map<Symbol, std::string_view> initial_definitions;
  std::unordered_set<Symbol> reads;
  std::unordered_set<Symbol> environment_reads;
  auto restore_variables = [&](std::string_view record,
                               std::shared_ptr<const void> owner,
                               bool validated) {
    auto take_view = [](std::string_view* input) {
      std::string_view value;
      if (!ConsumeGraphStringView(input, &value))
        ERROR("invalid runtime variable record; regenerate the Ninja graph");
      return value;
    };
    while (!record.empty()) {
      const std::string_view original_record = record;
      const std::string_view definition = take_view(&record);
      std::string_view fields(definition);
      Symbol symbol = Intern(take_view(&fields));
      const std::string_view origin_text = take_view(&fields);
      if (origin_text.size() != 1 || origin_text[0] < '0' ||
          origin_text[0] > '7')
        ERROR("invalid runtime variable origin; regenerate the Ninja graph");
      VarOrigin origin = static_cast<VarOrigin>(origin_text[0] - '0');
      initial_definitions[symbol] =
          original_record.substr(8, definition.size());
      if ((origin == VarOrigin::ENVIRONMENT ||
           origin == VarOrigin::ENVIRONMENT_OVERRIDE) &&
          !HasPrefix(symbol.str(), "KATI_") &&
          !HasPrefix(symbol.str(), "OMKTR_"))
        environment_reads.insert(symbol);
      const std::string_view flavor = take_view(&fields);
      const std::string_view value = take_view(&fields);
      const std::string_view original = take_view(&fields);
      if (flavor == "recursive") {
        if (!validated && !Value::ValidateRecipeExpression(value))
          ERROR(
              "invalid runtime variable expression; regenerate the Ninja "
              "graph");
      } else if (flavor != "simple") {
        ERROR("invalid runtime variable flavor; regenerate the Ninja graph");
      }
      if (!fields.empty())
        ERROR("invalid runtime variable data; regenerate the Ninja graph");
      symbol.SetLazyGlobalVar(
          origin, [owner, flavor, value, original, origin]() -> Var* {
            if (flavor == "recursive") {
              Value* expression =
                  Value::RestoreValidatedLazyRecipeExpression(value, owner);
              return new RecursiveVar(expression, origin, nullptr, Loc(),
                                      original);
            }
            return new SimpleVar(std::string(value), origin, nullptr, Loc());
          });
    }
  };
  restore_variables(globals.bytes, globals.owner, globals.validated);
  const auto local_variables = std::make_shared<const std::string>(take(&data));
  restore_variables(*local_variables, local_variables, false);
  storage = take(&data);
  record = storage;
  while (!record.empty()) {
    std::string name = take(&record);
    const bool exported = take(&record) == "1";
    evaluator.RestoreRecipeExport(Intern(name), exported);
    if (exported)
      reads.insert(Intern(name));
  }
  for (Symbol name : environment_reads) {
    auto exported = evaluator.exports().find(name);
    if (exported == evaluator.exports().end() || exported->second)
      reads.insert(name);
  }
  const std::string observation = take(&data);
  const std::string structural = take(&data);
  const std::string generation = take(&data);
  const std::string identity = take(&data);
  const char* owner_env = getenv("KATI_RECIPE_STATE_PATH");
  const std::string owner = owner_env ? owner_env : "";
  const bool observe = !observation.empty() && !owner.empty();
  if (!data.empty())
    ERROR("trailing runtime recipe data; regenerate the Ninja graph");
  CommandEvaluator commands(&evaluator);
  if (observe) {
    // A real execution can take a different branch after prerequisite updates.
    // Retire the old read set before effects run; only its successful parent
    // supervisor may publish the replacement.
    if (unlink(observation.c_str()) != 0 && errno != ENOENT)
      ERROR("cannot retire runtime variable observations");
    evaluator.set_recipe_reads(&reads);
  }
  const auto expanded = commands.Eval(node);
  evaluator.set_recipe_reads(nullptr);
  if (!evaluator.rules().empty())
    ERROR(
        "runtime recipe cannot change dependency rules; regenerate the Ninja "
        "graph");
  for (const auto& entry : evaluator.rule_vars()) {
    if (!(entry.first == node.lexical_output))
      ERROR("runtime recipe cannot assign another target's variables");
  }
  if (observe && (reads.count(Intern(".VARIABLES")) ||
                  reads.count(Intern(".KATI_SYMBOLS")))) {
    if (unlink(observation.c_str()) != 0 && errno != ENOENT)
      ERROR("cannot invalidate runtime variable observations");
  } else if (observe) {
    std::vector<std::string> names;
    for (Symbol name : reads)
      names.push_back(name.str());
    std::sort(names.begin(), names.end());
    std::string definitions, pending;
    for (const std::string& name : names) {
      AppendGraphString(name, &definitions);
      auto found = initial_definitions.find(Intern(name));
      AppendGraphString(found == initial_definitions.end() ? std::string_view()
                                                           : found->second,
                        &definitions);
    }
    for (const std::string& part :
         {observation, std::string("runtime-reads-v1"), structural, generation,
          identity, definitions})
      AppendGraphString(part, &pending);
    if (!WriteGraphCache(owner + ".reads-pending", pending)) {
      // Observation reuse is optional. A large read set or an unavailable
      // cache must fall back to the complete context, never stale evidence.
      if (unlink(observation.c_str()) != 0 && errno != ENOENT)
        ERROR("cannot invalidate runtime variable observations");
      unlink((owner + ".reads-pending").c_str());
    }
  }
  // The existing recipe supervisor owns cleanup, cancellation and the live
  // reservation. Replace this evaluator with a single ordinary shell rather
  // than keeping another process and output pipe alive for the whole recipe.
  const char* reservation = getenv("KATI_JOBSERVER_RESERVED");
  if (expanded.size() == 1 && !expanded.front().ignore_error &&
      (!getenv("KATI_JOBSERVER_FIFO") ||
       (reservation && std::string_view(reservation) == "1"))) {
    const Command& command = expanded.front();
    if (!command.shell.empty() && command.shell.front() == '/' &&
        command.shell.find_first_of(" $") == std::string::npos &&
        command.shellflag.find_first_of(" \t\r\n'\"\\$`") ==
            std::string::npos &&
        command.cmd.size() <= 120000) {
      for (const std::string& name : command.unset_environment)
        if (name != "KATI_JOBSERVER_RESERVED" &&
            name != "KATI_RUNTIME_CONTEXT_FD")
          unsetenv(name.c_str());
      for (const std::string& entry : command.environment) {
        const size_t separator = entry.find('=');
        const std::string name = entry.substr(0, separator);
        if (separator != std::string::npos &&
            name != "KATI_JOBSERVER_RESERVED" &&
            name != "KATI_RUNTIME_CONTEXT_FD")
          setenv(name.c_str(), entry.c_str() + separator + 1, 1);
      }
      if (command.verbose)
        printf("%s\n", command.cmd.c_str());
      fflush(nullptr);
      const char* args[] = {command.shell.c_str(), command.shellflag.c_str(),
                            command.cmd.c_str(), nullptr};
      execv(args[0], const_cast<char* const*>(args));
      PERROR("executing runtime recipe shell failed");
    }
  }
  for (const Command& command : expanded) {
    if (command.verbose)
      printf("%s\n", command.cmd.c_str());
    const int status = RunCommand(
        command.shell, command.shellflag, command.cmd, RedirectStderr::NONE,
        nullptr, !IsRecursiveKatiCommand(command.cmd),
        [](std::string_view output) {
          fwrite(output.data(), 1, output.size(), stdout);
          fflush(stdout);
        },
        -1, &command.environment, &command.unset_environment);
    if (status && !command.ignore_error)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  }
  return 0;
}

namespace {

static void ParseCommandPrefixes(std::string_view* s,
                                 bool* echo,
                                 bool* ignore_error,
                                 bool* force_run);

// Recursive make recipes sometimes pass a deferred make variable as a
// command-line assignment, for example 'CXX=$$(RAW_CXX_FOR_TARGET)'.  The
// recipe evaluator turns '$$' into a literal '$'; when the command is later
// run by Ninja or by Kati's executor, the child would otherwise receive an
// unresolved reference and install an empty command-line override.  Resolve
// deferred references occurring in quoted assignment values while the
// parent evaluator is still in scope.
static std::string ExpandDeferredAssignments(std::string_view input,
                                             Evaluator* ev) {
  std::string output;
  output.reserve(input.size());
  char quote = 0;
  size_t word_start = 0;
  // Prefix flags are still present until the caller builds its Command.
  // Reuse the same parser to identify the first actual shell word without
  // removing flags or changing their echo/error/force behavior here.
  std::string_view command = input;
  bool ignored_echo = true;
  bool ignored_error = false;
  bool ignored_force_run = false;
  ParseCommandPrefixes(&command, &ignored_echo, &ignored_error,
                       &ignored_force_run);
  const size_t command_start = input.size() - command.size();

  auto assignment_prefix = [&](size_t end) {
    const size_t start = std::max(word_start, command_start);
    std::string_view prefix = input.substr(start, end - start);
    // Accept 'CC=$(VALUE)' and CC='$(VALUE)', but not an equals sign
    // embedded in arbitrary quoted data such as a saved compiler command.
    if (!prefix.empty() && prefix.front() == '\'')
      prefix.remove_prefix(1);
    if (!prefix.empty() && prefix.back() == '\'')
      prefix.remove_suffix(1);
    if (prefix.empty() || prefix.back() != '=')
      return false;
    prefix.remove_suffix(1);
    if (!prefix.empty() &&
        (prefix.back() == '+' || prefix.back() == ':' || prefix.back() == '?'))
      prefix.remove_suffix(1);
    if (prefix.empty() ||
        !(isalpha(static_cast<unsigned char>(prefix.front())) ||
          prefix.front() == '_'))
      return false;
    return std::all_of(prefix.begin(), prefix.end(),
                       [](unsigned char c) { return isalnum(c) || c == '_'; });
  };

  for (size_t i = 0; i < input.size(); ++i) {
    char c = input[i];
    if (c == '\\' && quote != '\'') {
      output += c;
      if (i + 1 < input.size())
        output += input[++i];
      continue;
    }
    if (quote == 0 && (isspace(static_cast<unsigned char>(c)) || c == ';' ||
                       c == '|' || c == '&' || c == '(' || c == ')'))
      word_start = i + 1;
    if (c == '\'' || c == '"' || c == '`') {
      if (quote == 0)
        quote = c;
      else if (quote == c)
        quote = 0;
      output += c;
      continue;
    }

    if (c == '$' && i + 1 < input.size() && input[i + 1] == '(' &&
        quote == '\'' && assignment_prefix(i)) {
      size_t end = i + 2;
      int depth = 1;
      while (end < input.size() && depth != 0) {
        if (input[end] == '(')
          ++depth;
        else if (input[end] == ')')
          --depth;
        ++end;
      }

      if (depth == 0) {
        const std::string name(input.substr(i + 2, end - i - 3));
        Var* var = ev->LookupVar(Intern(name));
        if (var != nullptr && var->IsDefined()) {
          output += var->Eval(ev);
          i = end - 1;
          continue;
        }
      }
    }

    output += c;
  }

  return output;
}

static std::string ShellQuoteCommand(std::string_view value) {
  std::string quoted = "'";
  for (char c : value) {
    if (c == '\'')
      quoted += "'\\''";
    else
      quoted += c;
  }
  quoted += "'";
  return quoted;
}

static bool IsShellIdentifierCommand(std::string_view name) {
  if (name.empty())
    return false;
  if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
    return false;
  for (size_t i = 1; i < name.size(); ++i) {
    if (!std::isalnum(static_cast<unsigned char>(name[i])) && name[i] != '_')
      return false;
  }
  return true;
}

// MAKEFLAGS and MAKEOVERRIDES are recursive-make transport channels.  They
// describe the invocation being constructed, not ordinary environment
// variables.  Copying their current process-environment values into a
// deferred recursive command freezes state from an earlier recursion level
// and can override assignments made by the recipe immediately before the
// child is started.
// Jobserver reservations are also live transport state: RunCommand assigns
// ownership to one child, so a saved make export must not duplicate it.
static bool IsRecursiveTransportVariable(std::string_view name) {
  return name == "MAKEFLAGS" || name == "MAKEOVERRIDES" ||
         name == "MAKELEVEL" || name == "KATI_JOBSERVER_RESERVED" ||
         name == "KATI_NINJA_LOAD_LIMIT" || name == "KATI_NINJA_LOAD_STATE" ||
         name == "KATI_RUNTIME_CONTEXT_FD" || name == "KATI_GRAPH_SETTINGS";
}

// Recursive commands executed directly by Kati do not pass through the
// Ninja emitter.  Export the effective values at this command boundary so a
// child sees directory-local modifications just as it would under make.
static void ExportRecursiveEnvironment(Command* command, Evaluator* ev) {
  const bool recursive = IsRecursiveKatiCommand(command->cmd);

  std::string exports;
  std::unordered_set<std::string> emitted;
  auto emit = [&](Symbol name) {
    const std::string name_string(name.str());
    if (IsRecursiveTransportVariable(name_string))
      return;
    auto explicit_export = ev->exports().find(name);
    const Vars* scope = ev->current_scope();
    const bool scoped_export = scope != nullptr &&
                               scope->exported().exists(name) &&
                               scope->IsExported(name);
    if (explicit_export != ev->exports().end() && !explicit_export->second &&
        !scoped_export)
      return;
    Var* variable = ev->LookupVar(name);
    if (variable == nullptr || !variable->IsDefined() ||
        !IsShellIdentifierCommand(name_string) ||
        !emitted.insert(name_string).second)
      return;
    const std::string value = ev->EvalVar(name);
    command->environment.push_back(name_string + "=" + value);
    if (g_flags.generate_ninja)
      exports +=
          "export " + name_string + "=" + ShellQuoteCommand(value) + "; ";
  };

  // GNU make exports command-line assignments to every directly executed
  // recipe. Ninja's deferred environment is materialized separately; adding
  // these prefixes to generated commands changes their shell semantics.
  if (!g_flags.generate_ninja) {
    for (std::string_view assignment : g_flags.cl_vars) {
      size_t equal = assignment.find('=');
      if (equal == std::string_view::npos || equal == 0)
        continue;
      std::string_view name = TrimSpace(assignment.substr(0, equal));
      if (!name.empty() &&
          (name.back() == ':' || name.back() == '+' || name.back() == '?'))
        name.remove_suffix(1);
      // GNU make treats SHELL specially on Unix: a command-line SHELL
      // selects the recipe interpreter but is not exported automatically.
      if (!name.empty() && name != "SHELL")
        emit(Intern(name));
    }
  }

  if (!recursive) {
    if (ev->current_scope() != nullptr) {
      for (Symbol name : ev->current_scope()->exported())
        if (ev->current_scope()->IsExported(name))
          emit(name);
    }
    if (g_flags.generate_ninja)
      command->cmd.insert(0, exports);
    return;
  }

  for (const auto& [name, is_exported] : ev->exports()) {
    if (is_exported)
      emit(name);
  }
  if (ev->current_scope() != nullptr) {
    for (Symbol name : ev->current_scope()->exported())
      if (ev->current_scope()->IsExported(name))
        emit(name);
  }

  for (char** p = environ; p != nullptr && *p != nullptr; ++p) {
    const char* equal = strchr(*p, '=');
    if (equal == nullptr || equal == *p)
      continue;
    const std::string_view name_view(*p, equal - *p);
    if (IsRecursiveTransportVariable(name_view))
      continue;
    const Symbol name = Intern(name_view);
    auto explicit_export = ev->exports().find(name);
    if (explicit_export != ev->exports().end() && !explicit_export->second)
      continue;
    // Environment-origin variables are exported to recursive makes even
    // when their value is unchanged.  Serialize them explicitly because a
    // deferred child may run behind a generated environment snapshot or
    // after another recursive command altered its shell environment.
    emit(name);
  }

  // Preserve the complete MAKEFLAGS value, including its command-line
  // assignments. Environment exports alone are insufficient: a recursive
  // makefile may assign CC=... itself, so command-line values such as
  // CC=clang must remain command-line overrides in the child.
  for (const auto& item : ev->exports())
    if (!item.second)
      command->unset_environment.push_back(item.first.str());
  command->unset_environment.push_back("MAKEOVERRIDES");
  Var* makeflags = ev->LookupVar(Intern("MAKEFLAGS"));
  if (g_flags.generate_ninja && makeflags->IsDefined())
    command->environment.push_back("MAKEFLAGS=" + makeflags->Eval(ev));
  if (!g_flags.generate_ninja) {
    if (makeflags != nullptr && makeflags->IsDefined())
      command->environment.push_back("MAKEFLAGS=" + makeflags->Eval(ev));
    else
      command->unset_environment.push_back("MAKEFLAGS");
    command->unset_environment.push_back("MAKEOVERRIDES");
    return;
  }
  if (makeflags != nullptr && makeflags->IsDefined()) {
    exports = "export MAKEFLAGS=" + ShellQuoteCommand(makeflags->Eval(ev)) +
              "; unset MAKEOVERRIDES; " + exports;
  } else {
    exports = "unset MAKEFLAGS MAKEOVERRIDES; " + exports;
  }
  command->cmd.insert(0, exports);
}

// .EXPORT_ALL_VARIABLES makes every make variable whose name can be passed
// through a POSIX environment visible to every recipe.  Keep this at the
// recipe boundary rather than changing Kati's own process environment: the
// latter would leak target-local values between parallel recipes.
static void ExportAllVariables(Command* command, Evaluator* ev) {
  std::string exports;
  std::unordered_set<std::string> emitted;
  for (std::string_view name_view : GetSymbolNames(
           [](Var* var) { return var->IsDefined() && !var->Obsolete(); })) {
    if (name_view == "MAKELEVEL" || name_view == "KATI_JOBSERVER_RESERVED" ||
        name_view == "KATI_RUNTIME_CONTEXT_FD" ||
        name_view == "KATI_NINJA_LOAD_LIMIT" ||
        name_view == "KATI_NINJA_LOAD_STATE" ||
        name_view == "KATI_GRAPH_SETTINGS" ||
        !IsShellIdentifierCommand(name_view) ||
        !emitted.insert(std::string(name_view)).second)
      continue;
    Symbol name = Intern(name_view);
    Var* variable = ev->LookupVar(name);
    if (variable == nullptr || !variable->IsDefined())
      continue;
    const std::string value = ev->EvalVar(name);
    if (g_flags.generate_ninja)
      exports += "export " + std::string(name_view) + "=" +
                 ShellQuoteCommand(value) + "; ";
    else
      command->environment.push_back(std::string(name_view) + "=" + value);
  }
  if (g_flags.generate_ninja)
    command->cmd.insert(0, exports);
}

class AutoVar : public Var {
 public:
  AutoVar() : Var(VarOrigin::AUTOMATIC, nullptr, Loc()) {}
  virtual const char* Flavor() const override { return "undefined"; }

  virtual void AppendVar(Evaluator*, Value*) override { CHECK(false); }

  virtual std::string_view String() const override {
    // Automatic variables are computed values rather than makefile text, so
    // GNU make's raw-value query expands to the empty string.
    return "";
  }

  virtual std::string DebugString() const override {
    return std::string("AutoVar(") + sym_ + ")";
  }

  virtual bool IsFunc(Evaluator*) const override { return true; }

 protected:
  AutoVar(CommandEvaluator* ce, const char* sym) : ce_(ce), sym_(sym) {}
  virtual ~AutoVar() = default;

  CommandEvaluator* ce_;
  const char* sym_;
};

bool IsAutomaticRecipeVar(Var* var) {
  return dynamic_cast<AutoVar*>(var) != nullptr;
}

#define DECLARE_AUTO_VAR_CLASS(name)                                  \
  class name : public AutoVar {                                       \
   public:                                                            \
    name(CommandEvaluator* ce, const char* sym) : AutoVar(ce, sym) {} \
    virtual ~name() = default;                                        \
    virtual void Eval(Evaluator* ev, std::string* s) const override;  \
  }

DECLARE_AUTO_VAR_CLASS(AutoAtVar);
DECLARE_AUTO_VAR_CLASS(AutoLessVar);
DECLARE_AUTO_VAR_CLASS(AutoHatVar);
DECLARE_AUTO_VAR_CLASS(AutoPlusVar);
DECLARE_AUTO_VAR_CLASS(AutoStarVar);
DECLARE_AUTO_VAR_CLASS(AutoQuestionVar);
DECLARE_AUTO_VAR_CLASS(AutoPercentVar);
DECLARE_AUTO_VAR_CLASS(AutoPipeVar);

class AutoSuffixDVar : public AutoVar {
 public:
  AutoSuffixDVar(CommandEvaluator* ce, const char* sym, Var* wrapped)
      : AutoVar(ce, sym), wrapped_(wrapped) {}
  virtual ~AutoSuffixDVar() = default;
  virtual void Eval(Evaluator* ev, std::string* s) const override;

 private:
  Var* wrapped_;
};

class AutoSuffixFVar : public AutoVar {
 public:
  AutoSuffixFVar(CommandEvaluator* ce, const char* sym, Var* wrapped)
      : AutoVar(ce, sym), wrapped_(wrapped) {}
  virtual ~AutoSuffixFVar() = default;
  virtual void Eval(Evaluator* ev, std::string* s) const override;

 private:
  Var* wrapped_;
};

void AutoAtVar::Eval(Evaluator*, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (n)
    *s += n->lexical_output.str();
}

static bool IsVpathSourceAlias(const DepNode* node) {
  if (node->cmds.empty())
    return true;
  if (node->cmds.size() != 1 || !node->cmds.front()->IsLiteral())
    return false;

  std::string_view recipe = node->cmds.front()->GetLiteralValueUnsafe();
  bool echo = true;
  bool ignore_error = false;
  bool force_run = false;
  ParseCommandPrefixes(&recipe, &echo, &ignore_error, &force_run);
  return !echo && TrimLeftSpace(recipe) == ":";
}

static Symbol EffectivePrerequisite(Evaluator* ev,
                                    const DepNode* node,
                                    Symbol input) {
  if (g_flags.generate_ninja) {
    // Recipe-less VPATH rules are emitted as phony aliases to their source
    // provider, without creating a local file. Automatic variables must name
    // that provider so the consuming recipe can open it. Rule-bearing VPATH
    // targets, on the other hand, have a local Ninja output and must retain
    // their logical spelling so consumers follow that output after rebuilds.
    auto find_source_alias =
        [input](const std::vector<NamedDepNode>& dependencies) {
          for (const NamedDepNode& dependency : dependencies) {
            const DepNode* child = dependency.second;
            if (!(dependency.first == input) &&
                !(child->lexical_output == input))
              continue;
            if (!child->is_phony && IsVpathSourceAlias(child) &&
                child->vpath_provider.IsValid())
              return child->vpath_provider;
          }
          return Symbol();
        };
    Symbol provider = find_source_alias(node->deps);
    if (!provider.IsValid())
      provider = find_source_alias(node->order_onlys);
    return provider.IsValid() ? provider : input;
  }
  if (Exists(input.str()))
    return input;
  auto find_provider = [ev,
                        input](const std::vector<NamedDepNode>& dependencies) {
    for (const NamedDepNode& dependency : dependencies) {
      const DepNode* child = dependency.second;
      if (!(dependency.first == input) && !(child->lexical_output == input))
        continue;
      if (child->is_phony ||
          child->vpath_discarded.load(std::memory_order_acquire))
        return input;
      if (child->vpath_provider.IsValid())
        return child->vpath_provider;
      if (!(child->output == input) && !child->has_rule)
        return child->output;
      const std::string resolved = ev->ResolveVpath(input);
      if (!resolved.empty())
        return Intern(resolved);
      return input;
    }
    return Symbol();
  };
  Symbol provider = find_provider(node->deps);
  if (!provider.IsValid())
    provider = find_provider(node->order_onlys);
  if (provider.IsValid())
    return provider;
  // Implicit suffix prerequisites can be materialized without a named
  // dependency edge. They still use their VPATH source for automatic vars.
  const std::string resolved = ev->ResolveVpath(input);
  if (!resolved.empty())
    return Intern(resolved);
  return input;
}

void AutoLessVar::Eval(Evaluator* ev, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (!n)
    return;
  auto& ai = n->actual_inputs;
  if (!ai.empty())
    *s += EffectivePrerequisite(ev, n, ai[0]).str();
}

void AutoHatVar::Eval(Evaluator* ev, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (!n)
    return;
  std::unordered_set<std::string_view> seen;
  WordWriter ww(s);
  for (Symbol ai : n->actual_inputs) {
    ai = EffectivePrerequisite(ev, n, ai);
    if (seen.insert(ai.str()).second)
      ww.Write(ai.str());
  }
}

void AutoPlusVar::Eval(Evaluator* ev, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (!n)
    return;
  WordWriter ww(s);
  for (Symbol ai : n->actual_inputs) {
    ai = EffectivePrerequisite(ev, n, ai);
    ww.Write(ai.str());
  }
}

void AutoStarVar::Eval(Evaluator*, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();

  if (!n || !n->output_pattern.IsValid())
    return;

  Pattern pat(n->output_pattern.str());
  s->append(pat.Stem(n->lexical_output.str()));
}

void AutoQuestionVar::Eval(Evaluator* ev, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (!n)
    return;
  std::unordered_set<std::string_view> seen;

  if (ev->avoid_io()) {
    // Check timestamps using the shell at the start of rule execution
    // instead.
    *s += "${KATI_NEW_INPUTS}";
    // Command-state files can outlive their output. GNU make does not
    // consider a saved command sufficient when the target itself is absent;
    // FORCE-only rules must still execute. Preserve that rule generically
    // instead of relying on a stale savedcmd value.
    if (!Exists(n->output.str()))
      *s += " kati_missing_output";
    if (!ce_->found_new_inputs()) {
      std::string def;

      bool have_probe_inputs = false;
      def = "KATI_NEW_INPUTS=$( ";
      def += ShellQuoteCommand(GetExecutablePath());
      def += " --internal-newer-inputs ";
      def += ShellQuoteCommand(n->lexical_output.str());
      // Command expansion happens before BuildPlan has populated n->deps.
      // Use the already materialized prerequisite list instead of the later
      // dependency-node list; otherwise secondary-expanded rules can lose
      // prerequisites when the deferred timestamp probe runs.
      for (Symbol ai : n->actual_inputs) {
        if (seen.insert(ai.str()).second) {
          def += " ";
          def += ShellQuoteCommand(ai.str());
          have_probe_inputs = true;
        }
      }
      if (have_probe_inputs) {
        def += " --";
        for (Symbol ai : n->low_resolution_inputs) {
          def += " ";
          def += ShellQuoteCommand(ai.str());
        }
        def += " ) && export KATI_NEW_INPUTS";
      } else {
        def = "KATI_NEW_INPUTS=; export KATI_NEW_INPUTS";
      }
      ev->add_delayed_output_command(def);
      ce_->set_found_new_inputs(true);
    }
  } else {
    WordWriter ww(s);
    // GNU make computes $? against the target timestamp observed before
    // updating prerequisites. A prerequisite recipe may also rewrite the
    // target as a side effect; restating it here would erase the original
    // newer-prerequisite list used by generated makefile refresh recipes.
    double target_age = ce_->target_age();
    for (Symbol ai : n->actual_inputs) {
      ai = EffectivePrerequisite(ev, n, ai);
      double input_age = GetTimestamp(ai.str());
      if (std::find(n->low_resolution_inputs.begin(),
                    n->low_resolution_inputs.end(),
                    ai) != n->low_resolution_inputs.end())
        input_age = std::floor(input_age);
      if (seen.insert(ai.str()).second && input_age > target_age) {
        ww.Write(ai.str());
      }
    }
  }
}

void AutoPercentVar::Eval(Evaluator*, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (!n)
    return;
  const std::string output = n->lexical_output.str();
  const size_t open = output.find('(');
  if (open != std::string::npos && output.back() == ')' &&
      open + 1 < output.size()) {
    s->append(output, open + 1, output.size() - open - 2);
  }
}

void AutoPipeVar::Eval(Evaluator* ev, std::string* s) const {
  const DepNode* n = ce_->current_dep_node();
  if (!n)
    return;
  WordWriter ww(s);
  for (Symbol input : n->actual_order_only_inputs)
    ww.Write(EffectivePrerequisite(ev, n, input).str());
}

void AutoSuffixDVar::Eval(Evaluator* ev, std::string* s) const {
  std::string buf;
  wrapped_->Eval(ev, &buf);
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(buf)) {
    ww.Write(Dirname(tok));
  }
}

void AutoSuffixFVar::Eval(Evaluator* ev, std::string* s) const {
  std::string buf;
  wrapped_->Eval(ev, &buf);
  WordWriter ww(s);
  for (std::string_view tok : WordScanner(buf)) {
    ww.Write(Basename(tok));
  }
}

void ParseCommandPrefixes(std::string_view* s,
                          bool* echo,
                          bool* ignore_error,
                          bool* force_run) {
  *s = TrimLeftSpace(*s);
  while (true) {
    char c = s->empty() ? 0 : s->front();
    if (c == '@') {
      *echo = false;
    } else if (c == '-') {
      *ignore_error = true;
    } else if (c == '+') {
      *force_run = true;
    } else {
      break;
    }
    *s = TrimLeftSpace(s->substr(1));
  }
}

}  // namespace

CommandEvaluator::CommandEvaluator(Evaluator* ev)
    : ev_(ev), current_dep_node_(nullptr), found_new_inputs_(false) {
#define INSERT_AUTO_VAR(name, sym)                                      \
  do {                                                                  \
    Var* v = new name(this, sym);                                       \
    Intern(sym).SetGlobalVar(v);                                        \
    Intern(sym "D").SetGlobalVar(new AutoSuffixDVar(this, sym "D", v)); \
    Intern(sym "F").SetGlobalVar(new AutoSuffixFVar(this, sym "F", v)); \
  } while (0)
  INSERT_AUTO_VAR(AutoAtVar, "@");
  INSERT_AUTO_VAR(AutoLessVar, "<");
  INSERT_AUTO_VAR(AutoHatVar, "^");
  INSERT_AUTO_VAR(AutoPlusVar, "+");
  INSERT_AUTO_VAR(AutoStarVar, "*");
  INSERT_AUTO_VAR(AutoQuestionVar, "?");
  INSERT_AUTO_VAR(AutoPercentVar, "%");
  INSERT_AUTO_VAR(AutoPipeVar, "|");
}

CommandEvaluator::~CommandEvaluator() {
  // Automatic variables are installed in the process-global symbol table,
  // but their implementations point back to this evaluator.  Clear those
  // bindings before a restarted makefile evaluation can observe a stale
  // command evaluator through $@, $^, $<, and friends.
  static const char* kAutomaticSymbols[] = {"@", "<", "^", "+",
                                            "*", "?", "%", "|"};
  for (const char* sym : kAutomaticSymbols) {
    Intern(sym).SetGlobalVar(Var::Undefined());
    Intern(StringPrintf("%sD", sym)).SetGlobalVar(Var::Undefined());
    Intern(StringPrintf("%sF", sym)).SetGlobalVar(Var::Undefined());
  }
}

std::vector<Command> CommandEvaluator::Eval(const DepNode& n) {
  return Eval(n, GetTimestamp(n.output.str()));
}

std::vector<Command> CommandEvaluator::Eval(const DepNode& n,
                                            double target_age) {
  return Eval(n, target_age, static_cast<size_t>(-1));
}

std::vector<Command> CommandEvaluator::Eval(const DepNode& n,
                                            double target_age,
                                            size_t double_colon_group) {
  std::vector<Command> result;
  const bool observe =
      !g_flags.generate_ninja && g_flags.GraphOptimizationEnabled("evaluation");
  if (!g_flags.generate_ninja &&
      (!observe || filesystem_epoch_ != FilesystemObservationEpoch())) {
    InvalidateEvaluationFilesystemQueries();
    filesystem_epoch_ = FilesystemObservationEpoch();
  }
  ScopedFileObservations observations(observe, &observations_);
  target_age_ = target_age;
  ev_->set_loc(n.loc);
  ev_->set_current_scope(n.rule_vars);
  ev_->ApplyRecipeTargetUpdates(n.lexical_output);
  ev_->SetEvaluatingCommand(true);
  current_dep_node_ = &n;
  if (ev_->runtime_recipe_exports()) {
    for (const auto& entry : ev_->exports()) {
      if (entry.first.str() == "KATI_RUNTIME_CONTEXT_FD")
        continue;
      if (entry.second)
        setenv(entry.first.c_str(), ev_->EvalVar(entry.first).c_str(), 1);
      else
        unsetenv(entry.first.c_str());
    }
  }
  found_new_inputs_ = false;
  // Makefiles control silence with their command prefixes, including
  // Automake's V-dependent prefixes. Compact progress is an explicit display
  // option and must not hide commands from output-inspecting build scripts.
  const char* kati_verbose = std::getenv("KATI_VERBOSE");
  // Silent recipes do not need a display mode. Avoid expanding an otherwise
  // unused V definition for them (it may be recursive or have side effects).
  auto is_verbose = [&](bool echo) {
    if (!echo)
      return false;
    Var* verbosity = ev_->LookupVar(Intern("V"));
    // V is a display extension, not a make variable used by this recipe.
    // Expanding a deferred definition here can run shell functions or fail.
    return kati_verbose == nullptr || std::string_view(kati_verbose) != "0" ||
           (verbosity->IsDefined() && verbosity->String() == "1");
  };
  if (n.oneshell) {
    std::string script;
    for (size_t i = 0; i < n.cmds.size(); ++i) {
      if (double_colon_group != static_cast<size_t>(-1) &&
          (i >= n.double_colon_group_for_cmd.size() ||
           n.double_colon_group_for_cmd[i] != double_colon_group))
        continue;
      Value* v = n.cmds[i];
      ev_->set_loc(v->Location());
      if (!script.empty())
        script += '\n';
      std::string line = ExpandDeferredAssignments(v->Eval(ev_), ev_);
      if (!script.empty()) {
        std::string_view remaining = line;
        bool ignored_echo = true;
        bool ignored_error = false;
        bool ignored_force_run = false;
        ParseCommandPrefixes(&remaining, &ignored_echo, &ignored_error,
                             &ignored_force_run);
        line = std::string(remaining);
      }
      script += line;
    }
    std::string_view cmds = script;
    bool echo = !g_flags.is_silent_mode;
    bool ignore_error = false;
    bool force_run = false;
    ParseCommandPrefixes(&cmds, &echo, &ignore_error, &force_run);
    cmds = TrimLeftSpace(cmds);
    if (!cmds.empty()) {
      Command& command = result.emplace_back(n.output);
      if (double_colon_group != static_cast<size_t>(-1))
        command.double_colon_group = double_colon_group;
      command.cmd = std::string(cmds);
      command.display_cmd = command.cmd;
      command.verbose = is_verbose(echo && !n.silent);
      command.force_run = force_run;
      // Generated Ninja recipes source env.sh, which is only a snapshot of
      // the process environment taken during graph generation.  Recursive
      // make commands must receive the effective exported make variables at
      // this recipe boundary as well; otherwise makefile assignments made
      // after the snapshot are lost in the child.
      ExportRecursiveEnvironment(&command, ev_);
      if (n.export_all_variables)
        ExportAllVariables(&command, ev_);
      command.echo = echo && !n.silent;
      command.ignore_error = ignore_error || n.ignore_errors;
    }
  } else {
    for (size_t command_index = 0; command_index < n.cmds.size();
         ++command_index) {
      if (double_colon_group != static_cast<size_t>(-1) &&
          (command_index >= n.double_colon_group_for_cmd.size() ||
           n.double_colon_group_for_cmd[command_index] != double_colon_group))
        continue;
      Value* v = n.cmds[command_index];
      ev_->set_loc(v->Location());
      const std::string cmds_buf = ExpandDeferredAssignments(v->Eval(ev_), ev_);
      std::string_view cmds = cmds_buf;
      bool global_echo = !g_flags.is_silent_mode;
      bool global_ignore_error = false;
      bool global_force_run = false;
      ParseCommandPrefixes(&cmds, &global_echo, &global_ignore_error,
                           &global_force_run);
      if (cmds == "")
        continue;
      while (true) {
        size_t lf_cnt;
        size_t index = FindEndOfLine(cmds, 0, &lf_cnt);
        if (index == cmds.size())
          index = std::string::npos;
        std::string_view cmd = TrimLeftSpace(cmds.substr(0, index));
        cmds = cmds.substr(index + 1);

        bool echo = global_echo;
        bool ignore_error = global_ignore_error;
        bool force_run = global_force_run;
        ParseCommandPrefixes(&cmd, &echo, &ignore_error, &force_run);

        if (!cmd.empty()) {
          Command& command = result.emplace_back(n.output);
          if (command_index < n.double_colon_group_for_cmd.size())
            command.double_colon_group =
                n.double_colon_group_for_cmd[command_index];
          command.cmd = std::string(cmd);
          command.display_cmd = command.cmd;
          command.verbose = is_verbose(echo && !n.silent);
          command.force_run = force_run;
          // See the oneshell case above: recursive commands need the current
          // makefile export state even when Ninja defers their execution.
          ExportRecursiveEnvironment(&command, ev_);
          if (n.export_all_variables)
            ExportAllVariables(&command, ev_);
          command.echo = echo && !n.silent;
          command.ignore_error = ignore_error || n.ignore_errors;
        }
        if (index == std::string::npos)
          break;
      }
      continue;
    }
  }

  // SHELL and .SHELLFLAGS may be target-specific. Capture their effective
  // values while the target-specific scope is active so direct execution and
  // generated Ninja rules cannot accidentally use the top-level shell.
  const std::string command_shell = ev_->GetShell();
  const std::string command_shellflag = ev_->GetShellFlag();
  for (Command& command : result) {
    command.shell = command_shell;
    command.shellflag = command_shellflag;
  }

  if (!ev_->delayed_output_commands().empty()) {
    std::vector<Command> output_commands;
    for (const std::string& cmd : ev_->delayed_output_commands()) {
      Command& c = output_commands.emplace_back(n.output);
      c.cmd = cmd;
      c.echo = false;
      c.ignore_error = false;
      c.force_no_subshell = true;
    }
    // Prepend |output_commands|.
    result.swap(output_commands);
    copy(output_commands.begin(), output_commands.end(), back_inserter(result));
    ev_->clear_delayed_output_commands();
  }

  ev_->set_current_scope(NULL);
  ev_->SetEvaluatingCommand(false);

  return result;
}
