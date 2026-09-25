test:
	@touch input
	@find . -name input -exec touch found \;
	@test -f found;
