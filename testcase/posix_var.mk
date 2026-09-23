
MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))

# GNU make 3.82 has this feature though.
ifeq ($(MAKEVER),3)

test:
	echo test skipped

else

$(info $(shell echo foo))
override SHELL := echo
$(info $(shell echo bar))
.POSIX:
$(info $(shell echo baz))
test:
	foobar

endif
