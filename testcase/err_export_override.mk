# TODO: Fix - "override export define A" is invalid "override" directive.

# GNU make 4 accepts this syntax. Note kati doesn't agree with make 4
# either.
MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))
ifeq ($(MAKE)$(MAKEVER),make4)
$(error test skipped)
endif

export override define A
PASS_A
endef

# FORK MODIFICATION NOTICE (2026)
# Changed by the Openmktr fork, maintained by Rihards Paps and
# Haralds Paps. Adapted regression fixtures for fork behavior and GNU-free testing.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
# at the repository root. Original upstream notices remain applicable.
