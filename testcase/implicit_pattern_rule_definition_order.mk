# When equally specific implicit rules can build a target, use the first
# applicable rule in makefile definition order.
all: out/item.o

out/%.o: arch/%.S
	@echo architecture: $<

out/%.o: generic/%.S
	@echo generic: $<

arch/item.S:
	@echo architecture prerequisite

generic/item.S:
	@echo generic prerequisite
