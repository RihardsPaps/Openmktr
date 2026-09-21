# GNU make's let is lexically scoped and unpacks the final variable.
outer := keep

test:
	@echo "$(let first rest,one two three,$(first)|$(rest)|$(outer))"
	@echo "$(outer)|$(let outer,shadow,$(outer))|$(outer)"
	@echo "$(let one two three,a b,[$(one)][$(two)][$(three)])"
