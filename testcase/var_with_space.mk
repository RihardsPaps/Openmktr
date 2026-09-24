MAKEVER:=$(firstword $(subst ., ,$(MAKE_VERSION)))

ifeq ($(MAKEVER),4)
# A variable name with space is invalid on GNU make 4.
all:
	echo PASS
else
varname_with_ws:=hello, world!
$(varname_with_ws):=PASS
foo bar = PASS2
all:
	echo $(hello, world!)
	echo $(foo bar)
endif
