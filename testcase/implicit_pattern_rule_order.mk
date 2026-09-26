all: out/item.o

out/%.o: arch/%.S
	@echo selected architecture source: $<

out/%.o: generic/%.S
	@echo selected generic source: $<

arch/item.S:
	@echo architecture prerequisite

generic/item.S:
	@echo generic prerequisite
