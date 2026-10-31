// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#include "ninja_launcher.h"
#include <cstdlib>
#include "graph_optimization.h"
#include "log.h"
#include "strutil.h"

namespace {
bool Identifier(const std::string& name) {
  return !name.empty() &&
         name.find_first_not_of(
             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
             "_") == std::string::npos &&
         (name.front() < '0' || name.front() > '9');
}
}  // namespace

std::string NinjaLaunchPlan::Encode() const {
  std::string bytes;
  AppendGraphString("ninja-launch-v1", &bytes);
  for (const auto* field :
       {&load, &ninja, &graph, &public_graph, &jobs, &remote_jobs, &goals})
    AppendGraphString(*field, &bytes);
  for (const auto& [name, value] : exports)
    for (const auto& field : {std::string("set"), name, value})
      AppendGraphString(field, &bytes);
  for (const auto& name : unsets)
    for (const auto& field : {std::string("unset"), name, std::string()})
      AppendGraphString(field, &bytes);
  return bytes;
}

bool NinjaLaunchPlan::Decode(const std::string& bytes) {
  *this = {};
  std::string_view data(bytes);
  auto take = [&](std::string* value) {
    return ConsumeGraphString(&data, value) &&
           value->find('\0') == std::string::npos;
  };
  std::string version;
  if (!take(&version) || version != "ninja-launch-v1")
    return false;
  for (auto* field :
       {&load, &ninja, &graph, &public_graph, &jobs, &remote_jobs, &goals})
    if (!take(field))
      return false;
  if (ninja.empty() || graph.empty() || graph.find('/') != std::string::npos ||
      graph == "." || graph == ".." || public_graph.empty())
    return false;
  while (!data.empty()) {
    std::string kind, name, value;
    if (exports.size() + unsets.size() >= 65536 || !take(&kind) ||
        !take(&name) || !take(&value) || !Identifier(name))
      return false;
    if (kind == "set")
      exports.emplace_back(std::move(name), std::move(value));
    else if (kind == "unset" && value.empty())
      unsets.push_back(std::move(name));
    else
      return false;
  }
  return true;
}

void NinjaLaunchPlan::ApplyEnvironment(int argc, char** argv) const {
  for (const auto& [name, value] : exports)
    if (setenv(name.c_str(), value.c_str(), 1) != 0)
      PERROR("set generated export");
  for (const auto& name : unsets)
    if (unsetenv(name.c_str()) != 0)
      PERROR("unset generated export");
  // Preserve the shell launcher's precedence: an inherited/exported KATI_JOBS
  // wins; otherwise the last runtime -j before -- wins over the CPU default.
  const char* inherited = getenv("KATI_JOBS");
  if (!inherited || !*inherited) {
    std::string selected = jobs;
    bool next = false;
    for (int i = 0; i < argc; ++i) {
      const std::string_view arg(argv[i]);
      if (arg == "--")
        break;
      if (arg == "-j") {
        next = true;
        continue;
      }
      if (HasPrefix(arg, "-j=") || (arg.size() > 2 && HasPrefix(arg, "-j") &&
                                    arg[2] >= '0' && arg[2] <= '9'))
        selected = std::string(arg.substr(arg[2] == '=' ? 3 : 2));
      else if (next && !arg.empty() && arg.front() >= '0' && arg.front() <= '9')
        selected = std::string(arg);
      next = false;
    }
    if (selected.empty())
      selected = jobs;
    if (setenv("KATI_JOBS", selected.c_str(), 1) != 0)
      PERROR("set Ninja job budget");
  }
  if (goals.empty())
    unsetenv("KATI_NINJA_DEFAULT_GOALS");
  else
    setenv("KATI_NINJA_DEFAULT_GOALS", goals.c_str(), 1);
}
