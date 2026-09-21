test:
	@echo "$(intcmp 2,10,less,equal,greater)"
	@echo "$(intcmp 10,10,less,equal,greater)"
	@echo "$(intcmp 12,10,less,equal,greater)"
	@echo "$(intcmp 1,2,less)$(intcmp 2,1,,equal,greater)"
