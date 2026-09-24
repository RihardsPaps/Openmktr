# TODO: Fix - "override export define A" is invalid "override" directive.

# GNU make 4 accepts this syntax. Note kati doesn't agree with make 4
# either.
MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))
ifeq ($(MAKE)$(MAKEVER),make4)
$(error test skipped)
endif

export override define A
PASS_A
endef
