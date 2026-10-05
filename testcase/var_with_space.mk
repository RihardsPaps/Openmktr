MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))

ifeq ($(MAKEVER),4)
# A variable name with space is invalid on GNU make 4.
all:
	echo PASS
else
varname_with_ws:=hello, world!
$(varname_with_ws):=PASS
foo bar = PASS2
all:
	echo $(hello, world!)
	echo $(foo bar)
endif

# FORK MODIFICATION NOTICE (2026)
# Changed by the Openmktr fork, maintained by Rihards Paps and
# Haralds Paps. Adapted regression fixtures for fork behavior and GNU-free testing.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
# at the repository root. Original upstream notices remain applicable.
