define emit_header
	echo \#include <generated>
endef

all: out

out:
	$(emit_header) > $@
