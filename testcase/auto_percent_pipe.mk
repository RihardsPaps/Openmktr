.PHONY: force

archive(member.o): force | order-only
	@echo "percent=[$%] pipe=[$|]"

order-only:
	@:
