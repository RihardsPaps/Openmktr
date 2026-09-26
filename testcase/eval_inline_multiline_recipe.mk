define generated-recipe
echo generated > $@.tmp
mv $@.tmp $@
endef

define generated-rule
output: input; $$(generated-recipe)
endef

input:
	@touch $@

$(eval $(generated-rule))

all: output
