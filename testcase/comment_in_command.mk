MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))

test1:
	# foo
	echo PASS

test2: make$(MAKEVER)

make4:
	# foo  \
echo PASS

make3:
	# foo  \
	echo PASS

test3: $(shell echo foo #)

test4:
	echo $(shell echo OK # FAIL \
	FAIL2)

test5:
	echo $(shell echo $$(echo PASS))

foo:
	echo OK

# FORK MODIFICATION NOTICE (2026)
# Changed by the GNU-free Kati fork, maintained by Rihards Paps and
# Haralds Paps. Adapted regression fixtures for fork behavior and GNU-free testing.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
# at the repository root. Original upstream notices remain applicable.
