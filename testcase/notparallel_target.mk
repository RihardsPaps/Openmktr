.NOTPARALLEL: all

all: first second
	@test -f first
	@test -f second
	@printf 'notparallel-target=ok\n'

first:
	@sleep 1
	@printf first > $@

second:
	@test -f first
	@printf second > $@
