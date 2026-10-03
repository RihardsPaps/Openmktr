MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))

# GNU make 4 doesn't sort glob results.
ifeq ($(MAKEVER,4))

$(info test skipped)

else

test1:
	echo '$$(info foo)' > foo.d
	echo '$$(info bar)' > bar.d

test2:
	echo $(wildcard *.d)

-include *.d

endif

# FORK MODIFICATION NOTICE (2026)
# Changed by the GNU-free Kati fork, maintained by Rihards Paps and
# Haralds Paps. Adapted regression fixtures for fork behavior and GNU-free testing.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
# at the repository root. Original upstream notices remain applicable.
