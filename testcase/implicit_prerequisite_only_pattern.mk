.DEFAULT_GOAL := test
.SUFFIXES:
.SUFFIXES: .S .oS

test: localedata/stamp.oS

# A narrower prerequisite-only implicit rule routes this target through the
# recursive directory target instead of deriving stamp.S via the suffix rule.
localedata/stamp%: localedata/subdir_lib

localedata/subdir_lib:
	@echo recurse into subdirectory

.S.oS:
	@echo suffix fallback: $<
