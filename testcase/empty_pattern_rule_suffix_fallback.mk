.SUFFIXES:
.SUFFIXES: .c .m .o

test: sample.c sample.m sample.o

sample.c sample.m:
	touch $@

# Cancellation rule used by binutils/gprof's Automake-generated Makefile.
%.o: %.m

.c.o:
	echo COMPILE $@ from $<
