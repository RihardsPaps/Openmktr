# TODO(ninja): We're exporting `(echo )` for the last line, while make a kati(w/o ninja) uses `echo \`

SHELL:=/bin/sh

define func
$(info INFO: $(1))
echo $(1)
endef

$(info INFO2: $(call func, \
	foo))

test:
	$(call func, \
	foo)
	$(call func, \)

# FORK MODIFICATION NOTICE (2026)
# Changed by the GNU-free Kati fork, maintained by Rihards Paps and
# Haralds Paps. Adapted regression fixtures for fork behavior and GNU-free testing.
# Upstream material retains its Apache-2.0 terms. Fork modifications are
# covered by PolyForm Perimeter 1.0.1; see docs/LICENSING.md and NOTICE
# at the repository root. Original upstream notices remain applicable.
