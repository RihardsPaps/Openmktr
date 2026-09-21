all: out

out:
	@{ echo \#include <generated>; } > $@
