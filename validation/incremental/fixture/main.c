/* SPDX-License-Identifier: MPL-2.0 */
#include "shared.h"
#include "generated.h"
int worker(void);
int spare(void);
int main(void) { return worker() + spare() + SHARED + GENERATED + FACTOR; }
