// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef GRAPH_IMPORT_H_
#define GRAPH_IMPORT_H_
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "symtab.h"

struct Command;
const std::vector<std::string>& ImportValidationPlans();
void AddImportValidationPlan(const std::string& path);
bool ValidateImportedGraph(const std::string& path);
// Immutable dependency frontier reconstructed from Kbuild's fixdep records.
// Unknown syntax invalidates coverage rather than silently omitting an owner.
// Shared path symbols belong to the current BuildSession; only canonical
// strings and dictionary offsets are written to persistent cache records.
struct KbuildDependencyIndex {
  bool complete = false;
  std::map<std::string, std::vector<Symbol>> objects;
  std::unordered_set<Symbol> covered_inputs;
  std::map<std::string, std::pair<std::string, std::string>> directories;
  void Read(const std::string& directory, const std::string& source_directory);
  bool Covers(const std::string& input) const;
  std::vector<std::string> Inputs(const std::string& directory) const;
  std::string Encode() const;
  std::string TreeProof() const;
  bool Decode(std::string_view bytes, const std::string& directory = "");
};
bool StaticImportSources(std::vector<std::string>* files);
bool ResolveImportedCommandEnvironment(Command* command);
bool ParseImportCommand(const Command& command, std::vector<std::string>* argv);
int GenerateImportedGraph(const Command& command,
                          const std::vector<std::string>& argv,
                          const std::string& directory,
                          const std::string& gate,
                          bool dynamic = false,
                          bool allow_dynamic = false);
#endif
