MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))

test: abcd

abcd:

# GNU make 3 does not prioritize the rule with a shortest stem.
ifeq ($(MAKEVER),4)
a%:
	echo FAIL
endif
abc%:
	echo PASS
ab%:
	echo FAIL
