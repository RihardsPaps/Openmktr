// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <thread>
#include <vector>

#include "eval.h"
#include "file_cache.h"
#include "fileutil.h"
#include "flags.h"
#include "func.h"
#include "graph_optimization.h"
#include "log.h"
#include "ninja_launcher.h"
#include "parser.h"
#include "recipe_state.h"
#include "session.h"
#include "stats.h"
#include "symtab.h"
#include "var.h"

namespace {
struct Counted : SessionAllocated<Counted> {
  explicit Counted(std::atomic<int>* count) : count(count) { ++*count; }
  ~Counted() { --*count; }
  std::atomic<int>* count;
};
struct Large : SessionAllocated<Large> {
  explicit Large(std::atomic<int>* count) : count(count) { ++*count; }
  ~Large() { --*count; }
  std::atomic<int>* count;
  char padding[100000] = {};
};
struct Throwing : SessionAllocated<Throwing> {
  Throwing() { throw std::runtime_error("constructor failed"); }
};
struct SharedCounter {
  std::atomic<int> value{0};
};

void ExerciseArena() {
  std::atomic<int> live{0};
  {
    BuildSession session;
    std::vector<Counted*> values;
    // Cross chunk boundaries, recycle early deletions, and leave live objects
    // for teardown. An inner session must not receive the outer object's free.
    for (int i = 0; i < 10000; ++i)
      values.push_back(new Counted(&live));
    {
      BuildSession inner;
      for (size_t i = 0; i < values.size(); i += 2)
        delete values[i];
    }
    for (int i = 0; i < 5000; ++i)
      new Counted(&live);
    auto* large = new Large(&live);
    delete large;
    new Large(&live);
    try {
      new Throwing;
      assert(false);
    } catch (const std::runtime_error&) {
    }
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
      workers.emplace_back([&] {
        SessionBinding binding(&session);
        for (int j = 0; j < 10000; ++j) {
          auto* value = new Counted(&live);
          ++SessionService<SharedCounter>().value;
          if (j % 10)
            delete value;
        }
      });
    }
    for (auto& worker : workers)
      worker.join();
    assert(live == 14001);
    assert(session.allocations() == 14001);
    assert(SessionService<SharedCounter>().value == 40000);
  }
  assert(live == 0);
}
}  // namespace

static void Exercise(int iteration) {
  BuildSession session;
  assert(!FindSymbol("SESSION_VALUE").IsValid());
  assert(!Intern(".SHELLSTATUS").PeekGlobalVar()->IsDefined());
  assert(GetParseErrors().empty());
  assert(GetShellCommandResults().empty());
  assert(Vars::used_env_vars().size() == 0);
  assert(Evaluator::used_undefined_vars().size() == 0);
  assert(!MakefileCacheManager::Get().requires_evaluation());
  assert(FilesystemObservationEpoch() == 0);
  assert(!g_log_no_exit && !g_last_error);
  Evaluator evaluator;
  std::vector<Stmt*> statements;
  Parse(
      "SESSION_VALUE := first\nSESSION_VALUE += second\n"
      "DEFERRED = $(SESSION_VALUE)\n"
      "$(eval NESTED = $(DEFERRED))\n"
      "FUNCTION = $(1)-$(2)\n"
      "CALLED := $(call FUNCTION,one,two)\n"
      "all: private LOCAL = value\n"
      "all:; @echo $(NESTED)\n",
      Loc("session.mk", 1), &statements);
  for (auto* statement : statements)
    statement->Eval(&evaluator);
  assert(evaluator.EvalVar(Intern("NESTED")) == "first second");
  assert(evaluator.EvalVar(Intern("CALLED")) == "one-two");
  Intern("SESSION_VALUE").GetGlobalVar()->SetDeprecated(" isolated");
  ShellStatusVar::SetValue(iteration);
  Vars::add_used_env_vars(Intern("SESSION_VALUE"));
  MakefileCacheManager::Get().RequireEvaluation();
  InvalidateFileObservations();
  auto* symbol_storage = g_symbols;
  {
    BuildSession inner;
    assert(!FindSymbol("SESSION_VALUE").IsValid());
    Intern("SESSION_VALUE")
        .SetGlobalVar(new SimpleVar("inner", VarOrigin::FILE, nullptr, Loc()));
    assert(Intern("SESSION_VALUE").PeekGlobalVar()->String() == "inner");
    g_log_no_exit = true;
    ERROR("captured inner error");
    assert(*g_last_error == "captured inner error");
  }
  assert(g_symbols == symbol_storage);
  assert(Intern("SESSION_VALUE").PeekGlobalVar()->String() == "first second");
  assert(!g_log_no_exit && !g_last_error);
  std::thread worker([&] {
    SessionBinding binding(&session);
    assert(Intern("SESSION_VALUE").PeekGlobalVar()->String() == "first second");
  });
  worker.join();
  try {
    ERROR("unwind this invocation");
  } catch (const BuildError& e) {
    assert(std::string(e.what()) == "unwind this invocation");
  }
  assert(session.allocations() > 0);
}
int main() {
  {
    NinjaLaunchPlan plan;
    plan.ninja = "/path with spaces/ninja";
    plan.graph = "build.ninja";
    plan.public_graph = "./build.ninja";
    plan.jobs = "4";
    plan.exports.emplace_back("VALUE", "literal '$text'\nsecond line");
    plan.unsets.push_back("REMOVED");
    NinjaLaunchPlan decoded;
    const std::string encoded = plan.Encode();
    assert(decoded.Decode(encoded) && decoded.Encode() == encoded);
    assert(!decoded.Decode(encoded.substr(0, encoded.size() - 1)));
    plan.graph = "../build.ninja";
    assert(!decoded.Decode(plan.Encode()));
    plan.graph = "build.ninja";
    plan.unsets = {"BAD=NAME"};
    assert(!decoded.Decode(plan.Encode()));
    plan.unsets.clear();
    plan.exports.front().second = std::string("nul\0value", 9);
    assert(!decoded.Decode(plan.Encode()));
  }
  ExerciseArena();
  for (int i = 0; i < 100; ++i)
    Exercise(i);
  assert(BuildSession::Active() == nullptr);
  // Independent threads own disjoint sessions. Evaluation within each session
  // remains serialized; only immutable graph execution is shared with workers.
  std::thread first([] { Exercise(100); });
  std::thread second([] { Exercise(101); });
  first.join();
  second.join();
  BuildSession session;
  char directory[] = "/tmp/omktr-objects-XXXXXX";
  assert(mkdtemp(directory));
  const std::string base = std::string(directory) + "/same-hash";
  {
    GraphObjectBatch objects;
    RecipeStateBatch records;
    std::vector<std::string> paths;
    for (int i = 0; i < 600; ++i)
      paths.push_back(PublishNinjaRecipeState(
          base + std::to_string(i), "OMKTR_RECIPE1\n" + std::to_string(i)));
    records.Flush();
    objects.Flush();
    for (int i = 0; i < 600; ++i) {
      std::string state;
      assert(ReadNinjaRecipeState(paths[i], &state));
      assert(state == "OMKTR_RECIPE1\n" + std::to_string(i));
      assert(PublishNinjaRecipeState(base + std::to_string(i), state) ==
             paths[i]);
      assert(NinjaRecipeStateOwner(paths[i]) == base + std::to_string(i));
    }
    const std::string changed =
        PublishNinjaRecipeState(base + "0", "OMKTR_RECIPE1\nchanged");
    assert(changed != paths[0]);
    records.Flush();
    objects.Flush();
    std::string state;
    assert(ReadNinjaRecipeState(paths[0], &state) &&
           state == "OMKTR_RECIPE1\n0");
    // Malformed, out-of-bounds and nonregular pack descriptors are misses.
    const std::string malformed = base + ".packed-1";
    for (const char* descriptor :
         {"/etc/passwd", "omktr-p1:../outside:0:1",
          "omktr-p1:.pack-1:99999999999999999999:1", "omktr-p1:.pack-1:0:1"}) {
      unlink(malformed.c_str());
      assert(symlink(descriptor, malformed.c_str()) == 0);
      assert(!ReadNinjaRecipeState(malformed, &state));
    }
    const auto fifo_pack = std::string(directory) + "/.pack-1";
    assert(mkfifo(fifo_pack.c_str(), 0600) == 0);
    assert(!ReadNinjaRecipeState(malformed, &state));
  }
  const std::string one = PublishGraphObject(base, "first");
  const std::string two = PublishGraphObject(base, "second");
  assert(one != two);
  assert(PublishGraphObject(base, "first") == one);
  std::string bytes;
  assert(ReadGraphCache(one, &bytes) && bytes == "first");
  assert(ReadGraphCache(two, &bytes) && bytes == "second");
  struct stat sealed;
  assert(stat(one.c_str(), &sealed) == 0);
  assert((sealed.st_mode & 07777) == 0400);
  const std::string raw = base + "-raw";
  assert(mkfifo(raw.c_str(), 0600) == 0);
  assert(PublishGraphObject(raw, "shell script", true) != raw);
  // A durability failure must prevent the caller from committing its graph.
  {
    GraphObjectBatch batch;
    const auto pending = PublishGraphObject(base + "-pending", "pending");
    assert(unlink(pending.c_str()) == 0);
    bool failed = false;
    try {
      batch.Flush();
    } catch (const BuildError&) {
      failed = true;
    }
    assert(failed);
  }
  assert(ReadGraphCache(PublishGraphObject(base + "-pending", "recovered"),
                        &bytes) &&
         bytes == "recovered");
  std::filesystem::remove_all(directory);
}
