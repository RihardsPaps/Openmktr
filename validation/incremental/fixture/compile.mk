# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
include flags.mk
include deps.mk
.PHONY: all
all: app
app: main.o worker.o spare.o
	$(CC) main.o worker.o spare.o -o $@
generated.h: config.txt
	$(PYTHON) helper.py generate $< $@
main.o: main.c generated.h flags.mk
worker.o: worker.c flags.mk $(EXTRA_DEPS)
spare.o: spare.c flags.mk
ifeq ($(LAYOUT),explicit)
main.o worker.o: shared.h
endif
%.o: %.c
	$(PYTHON) helper.py compile $@ $(CC) $(CFLAGS) -MMD -MF $@.d -c $< -o $@
-include main.o.d worker.o.d spare.o.d
