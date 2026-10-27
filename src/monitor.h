// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef MONITOR_H_
#define MONITOR_H_
#include <string>
#include <vector>

int MonitorCommand(const std::string& action);
bool MonitorSnapshot(const std::vector<std::string>& paths,
                     std::vector<std::string>* identities,
                     std::vector<double>* timestamps,
                     std::string* checkpoint);
bool MonitorCheckpointValid(const std::string& checkpoint);
#endif
