.WAIT:

all: first second .WAIT third fourth
	@test -f first
	@test -f second
	@test -f third
	@test -f fourth
	@printf 'wait=ok\n'

first:
	@sleep 1
	@printf first > $@

second:
	@sleep 1
	@printf second > $@

third: first second
	@test -f first -a -f second
	@printf third > $@

fourth: first second
	@test -f first -a -f second
	@printf fourth > $@
