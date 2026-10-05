
MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))

files = $(wildcard M*)

$(shell mkdir -p tmp)
files += $(wildcard tmp/../M*)
files += $(wildcard not_exist/../M*)
files += $(wildcard tmp/../M* not_exist/../M* tmp/../M*)
# GNU make 4 does not sort the result of $(wildcard)
ifeq ($(MAKEVER),3)
files += $(wildcard [ABC] C B A)
endif

test1:
	touch A C B

test2:
	echo $(files)

# FORK MODIFICATION NOTICE (2026)
# Changed by the Openmktr fork, maintained by Rihards Paps and
# Haralds Paps. Adapted regression fixtures for fork behavior and GNU-free testing.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
# at the repository root. Original upstream notices remain applicable.
