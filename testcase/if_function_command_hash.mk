CONFIG_NAME := sandbox

define header
	echo "/* header */"; echo \#define CFG; $(if $(CONFIG_NAME),echo \#include <configs/$(CONFIG_NAME).h> ;)
endef

all: out

out:
	$(header)
