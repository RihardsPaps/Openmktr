# TODO: Fix this. Maybe $(wildcard) always runs at eval-phase.

# GNU make 4 agrees with ckati.
MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))
ifeq ($(MAKE)$(MAKEVER),make4)
$(error test skipped)
endif

files = $(wildcard *,*)

# if make starts without foo,bar, it will be empty, although expect foo,bar.
test: foo,bar
	echo $(files)
	echo $(wildcard foo*)

# first $(files) will be empty since no foo,bar exists.
# second $(files) expects foo, but empty.
foo,bar:
	echo $(files)
	touch foo,bar
	echo $(files)

$(shell mkdir dir)
$(info $(wildcard dir/not_exist))
$(shell touch dir/file)
# This should show nothing.
$(info $(wildcard dir/file))

# FORK MODIFICATION NOTICE (2026)
# Changed by the GNU-free Kati fork, maintained by Rihards Paps and
# Haralds Paps. Adapted regression fixtures for fork behavior and GNU-free testing.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
# at the repository root. Original upstream notices remain applicable.
