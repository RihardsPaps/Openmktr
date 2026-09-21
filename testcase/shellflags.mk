.SHELLFLAGS := -ec

test:
	@echo "flags=$(.SHELLFLAGS)"
	@false
