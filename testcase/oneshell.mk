.ONESHELL:

test:
	@value=one
	value=$$value-two
	printf '%s\n' "$$value"
