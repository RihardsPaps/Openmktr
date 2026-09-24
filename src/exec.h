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

#ifndef EXEC_H_
#define EXEC_H_

#include <string>
#include <vector>

#include "dep.h"
class Evaluator;

// True when a shell recipe invokes this Kati executable recursively.  Both
// the direct executor and the Ninja emitter use the same boundary test so
// recursive scheduling and recursive environment handling cannot drift apart.
bool IsRecursiveKatiCommand(const std::string& command);

struct ExecResult {
  bool needs_build;
  bool failed;
  // Whether a requested root itself ran a recipe. Dependency recipes alone
  // must not trigger an included-makefile restart.
  bool roots_built;
};

ExecResult Exec(const std::vector<NamedDepNode>& roots,
                Evaluator* ev,
                bool parallel = true);

#endif  // EXEC_H_
