
MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))

# GNU make 3.82 has this feature though.
ifeq ($(MAKEVER),3)

test:
	echo test skipped

else

$(info $(shell echo foo))
override SHELL := echo
$(info $(shell echo bar))
.POSIX:
$(info $(shell echo baz))
test:
	foobar

endif

# FORK MODIFICATION NOTICE (2026)
# Changed by the Openmktr fork, maintained by Rihards Paps and
# Haralds Paps. Adapted regression fixtures for fork behavior and GNU-free testing.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
# at the repository root. Original upstream notices remain applicable.
