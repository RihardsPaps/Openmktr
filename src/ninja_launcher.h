// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#ifndef NINJA_LAUNCHER_H_
#define NINJA_LAUNCHER_H_
#include <string>
#include <utility>
#include <vector>

// A checked generation artifact carries literal exports and launch options.
// env.sh remains the shell interface for raw Ninja recipes and saved graphs.
struct NinjaLaunchPlan {
  std::string load, ninja, graph, public_graph, jobs, remote_jobs, goals;
  std::vector<std::pair<std::string, std::string>> exports;
  std::vector<std::string> unsets;
  std::string Encode() const;
  bool Decode(const std::string& bytes);
  void ApplyEnvironment(int argc, char** argv) const;
};
#endif
