define filechk
	{ $(filechk_$(1)); } > $@
endef

define filechk_header
	echo "/* header */"; echo \#include <generated>
endef

all: out

out:
	$(call filechk,header)
