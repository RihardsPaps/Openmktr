// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef GRAPH_IMPORT_H_
#define GRAPH_IMPORT_H_
#include <string>
#include <vector>

struct Command;
bool StaticImportSources(std::vector<std::string>* files);
bool ParseImportCommand(const Command& command, std::vector<std::string>* argv);
int GenerateImportedGraph(const Command& command,
                          const std::vector<std::string>& argv,
                          const std::string& directory,
                          const std::string& gate);
#endif
