.PHONY: all

all: output/foo.one output/foo.two

output/%.one output/%.two:
	@echo run >> calls
	@mkdir -p output
	@touch output/$*.one output/$*.two
