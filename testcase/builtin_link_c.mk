CC = test-cc
CFLAGS = test-cflags
CPPFLAGS = test-cppflags
LDFLAGS = test-ldflags

.PHONY: test
test:
	@printf '%s\n' '$(LINK.c)'
