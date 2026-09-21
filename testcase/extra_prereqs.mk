.EXTRA_PREREQS: extra-prerequisite

all: ordinary-prerequisite
	@test -f extra-prerequisite
	@test "$^" = ordinary-prerequisite
	@printf 'extra-prereqs=%s\n' "$^"

extra-prerequisite:
	@printf 'extra\n' > $@

ordinary-prerequisite:
	@printf 'ordinary\n' > $@
