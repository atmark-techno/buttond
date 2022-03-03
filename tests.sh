#!/bin/bash

TESTDIR=$(mktemp -d /tmp/buttond.XXXXXX)
trap "rm -rf '$TESTDIR'" EXIT

BUTTOND=$(realpath -e ./buttond)
GEN_EVENTS=$(realpath -e ./gen_events.py)
cd "$TESTDIR" || exit 1
declare -A PROCESSES=( )
declare -A CHECKS=( )
FAIL=0

error() {
	printf "%s\n" "$@" >&2
	exit 1
}

run_pattern() {
	local testname="$1"
	shift
	declare -a keys=( )
	while [[ $# -gt 0 ]]; do
		if [[ "$1" = "--" ]]; then
			shift
			break
		fi
		keys+=( "$1" )
		shift
	done

	"$GEN_EVENTS" "${keys[@]}" | "$BUTTOND" -i /dev/stdin "$@" 2>/dev/null &
	PROCESSES[$testname]=$!
}

add_check() {
	local testname="$1" file
	shift
	for file; do
		[[ -z "${CHECKS[$file]}" ]] && [[ -z "${CHECKS[-$file]}" ]] \
			|| error "file $file used in $testname was previously used in ${CHECKS[$file]}"
		CHECKS["$file"]="$testname"
	done
}

fail() {
	printf "%s\n" "$@" >&2
	FAIL=$((FAIL+1))
}

check_all() {
	local file testname

	for file in "${!CHECKS[@]}"; do
		testname="${CHECKS[$file]}"
		if [[ -n "${PROCESSES[$testname]}" ]]; then
			wait "${PROCESSES[$testname]}"
			unset "PROCESSES[$testname]"
		fi

		case "$file" in
		-*) file="${file#-}"; [[ -e "${file#-}" ]] && fail "check $testname failed: $file exists";;
		*) [[ -e "${file#-}" ]] || fail "check $testname failed: $file does not exist";;
		esac 
	done
}

# tests take time to run by definition: run all in background then check.
run_pattern shortkey 148,1,100 148,0,0 -- -s 148 -a "touch shortkey"
add_check shortkey shortkey

run_pattern longkey 148,1,2200 -- -l 148 -t 2000 -a "touch longkey"
add_check longkey longkey

run_pattern longkey_norun 148,1,100 148,0,2000 -- -l 148 -t 2000 -a "touch longkey_norun"
add_check longkey_norun -longkey_norun

run_pattern shortlong 148,1,100 148,0,100 148,1,2200 -- -s 148 -a "touch shortlong_short" -l 148 -t 2000 -a "touch shortlong_long"
add_check shortlong shortlong_short shortlong_long

check_all

if ((FAIL)); then
	echo "Some tests failed"
	exit 1
fi
echo "All ok"
