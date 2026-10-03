
MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))

all: a.h.x a.c.x a.h.z a.c.z b.h.x b.c.x b.h.z b.c.z

a.h.%:
	echo twice $@
a.c.%:
	echo twice $@

b.h.% b.c.%:
	echo once $@

b.h.z: pass

# GNU make 4 invokes this rule.
ifeq ($(MAKEVER,3))
b.c.z: fail
endif

pass:
	echo PASS

fail:
	echo FAIL

# FORK MODIFICATION NOTICE (2026)
# Changed by the GNU-free Kati fork, maintained by Rihards Paps and
# Haralds Paps. Adapted regression fixtures for fork behavior and GNU-free testing.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
# at the repository root. Original upstream notices remain applicable.
