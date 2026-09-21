all:
	@test "$(shell false)$(.SHELLSTATUS)" = 1
	@printf 'shellstatus-direct=%s\n' "$(.SHELLSTATUS)"
