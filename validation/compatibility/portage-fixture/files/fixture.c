/* SPDX-License-Identifier: MPL-2.0 */
#include <stdio.h>

#ifndef PORTAGE_FIXTURE
#error Compiler flags were not forwarded
#endif

int main(void) {
  printf("marker=%d version=%d\n", PORTAGE_FIXTURE, FIXTURE_VERSION);
  return 0;
}
