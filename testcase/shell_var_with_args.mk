
MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))

ifeq ($(MAKEVER),4)

# GNU make 4 escapes $(SHELL).
test:
	echo test skipped

else

export FOO=-x

override SHELL := PS4="cmd: " /bin/sh $${FOO}
$(info $(shell echo foo))

test:
	@echo baz

endif

# FORK MODIFICATION NOTICE (2026)
# Changed by the GNU-free Kati fork, maintained by Rihards Paps and
# Haralds Paps. Adapted regression fixtures for fork behavior and GNU-free testing.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by PolyForm Perimeter 1.0.1; see docs/LICENSING.md and NOTICE
# at the repository root. Original upstream notices remain applicable.
