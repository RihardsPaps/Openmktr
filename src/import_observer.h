// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef IMPORT_OBSERVER_H_
#define IMPORT_OBSERVER_H_

#include <sys/types.h>
#include <string>

bool BeginImportObservation();
bool ImportObservationActive();
int ObserveImport(pid_t child, const std::string& cache_directory);

#endif
