// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef NATIVE_OBSERVATION_H_
#define NATIVE_OBSERVATION_H_

#include <memory>
#include <string>

struct FindCommand;

// Evaluate only the native find interpreter in an isolated process.
// Diagnostics, unsupported syntax and incomplete results are misses, never
// shell execution.
bool QueryNativeFind(const std::string& command, std::string* packet);
bool DecodeNativeFind(const std::string& command,
                      const std::string& packet,
                      std::string* result,
                      std::unique_ptr<FindCommand>* descriptor);
int NativeFindWorker(const char* command, bool warnings_are_errors);

#endif
