// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <thread>

#include "eval.h"
#include "file_cache.h"
#include "fileutil.h"
#include "flags.h"
#include "func.h"
#include "graph_optimization.h"
#include "log.h"
#include "parser.h"
#include "session.h"
#include "stats.h"
#include "symtab.h"
#include "var.h"

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
  const std::string one = PublishGraphObject(base, "first");
  const std::string two = PublishGraphObject(base, "second");
  assert(one != two);
  assert(PublishGraphObject(base, "first") == one);
  std::string bytes;
  assert(ReadGraphCache(one, &bytes) && bytes == "first");
  assert(ReadGraphCache(two, &bytes) && bytes == "second");
  const std::string raw = base + "-raw";
  assert(mkfifo(raw.c_str(), 0600) == 0);
  assert(PublishGraphObject(raw, "shell script", true) != raw);
  std::filesystem::remove_all(directory);
}
