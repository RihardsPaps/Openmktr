.PHONY: all

all: output/foo.one output/foo.two

%.one %.two: input
	@echo run >> calls
	@mkdir -p output
	@touch $*.one $*.two
