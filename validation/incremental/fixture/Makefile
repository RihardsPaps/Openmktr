# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
ifeq ($(LAYOUT),recursive)
.PHONY: all
all:
	+$(MAKE) -f compile.mk LAYOUT=depfile all
else
include compile.mk
endif
