
MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))

ifeq ($(MAKEVER),4)

# GNU make 4 escapes $(SHELL).
test:
	echo test skipped

else

export FOO=-x

override SHELL := PS4="cmd: " /bin/sh $${FOO}
$(info $(shell echo foo))

test:
	@echo baz

endif
