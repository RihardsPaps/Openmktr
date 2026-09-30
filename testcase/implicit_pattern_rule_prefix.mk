MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))

test: abcd

abcd:

# GNU make 3 does not prioritize the rule with a shortest stem.
ifeq ($(MAKEVER),4)
a%:
	echo FAIL
endif
abc%:
	echo PASS
ab%:
	echo FAIL

# FORK MODIFICATION NOTICE (2026)
# Changed by the GNU-free Kati fork, maintained by Rihards Paps and
# Haralds Paps. Adapted regression fixtures for fork behavior and GNU-free testing.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by PolyForm Perimeter 1.0.1; see docs/LICENSING.md and NOTICE
# at the repository root. Original upstream notices remain applicable.
