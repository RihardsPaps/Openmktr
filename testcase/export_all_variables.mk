.EXPORT_ALL_VARIABLES:

FROM_FILE = exported-value

all:
	@test "$$FROM_FILE" = exported-value
	@printf 'export-all=%s\n' "$$FROM_FILE"
