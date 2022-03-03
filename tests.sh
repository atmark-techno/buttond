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

	# skip tests we didn't ask for
	case ",$ONLY," in
	",,"|*",$testname,"*) ;;
	*) return;;
	esac

	declare -a keys=( )
	while [[ $# -gt 0 ]]; do
		if [[ "$1" = "--" ]]; then
			shift
			break
		fi
		keys+=( "$1" )
		shift
	done

	if [[ -n "$DRYRUN" ]]; then
		printf '"%s" ' "$GEN_EVENTS" "${keys[@]}"
		printf "| "
		printf '"%s" ' "$BUTTOND" --test_mode -i /dev/stdin "$@"
		echo
		return
	fi
	"$GEN_EVENTS" "${keys[@]}" | "$BUTTOND" --test_mode -i /dev/stdin "$@" &
	PROCESSES[$testname]=$!
}

add_check() {
	local testname="$1" file
	shift

	# skip tests we didn't ask for
	case ",$ONLY," in
	",,"|*",$testname,"*) ;;
	*) return;;
	esac
	[[ -n "$DRYRUN" ]] && return

	for file; do
		[[ -z "${CHECKS[$file]}" ]] && [[ -z "${CHECKS[-$file]}" ]] \
			|| error "file $file used in $testname was previously used in ${CHECKS[$file]}"
		CHECKS["$file"]="$testname"
	done
}

fail() {
	printf "check $testname failed: %s\n" "$@" >&2
	FAIL=$((FAIL+1))
}

check_all() {
	local file check testname tmp

	for file in "${!CHECKS[@]}"; do
		testname="${CHECKS[$file]}"
		if [[ -n "${PROCESSES[$testname]}" ]]; then
			wait "${PROCESSES[$testname]}" || fail "returned non-zero"
			unset "PROCESSES[$testname]"
		fi
		check=${file%%-*}
		file=${file#*-}

		case "$check" in
		ne)
			[[ -e "$file" ]] && fail "$file exists"
			;;
		e)
			[[ -e "$file" ]] || fail "$file does not exist"
			;;
		l*)
			check="${check#l}"
			tmp="$(wc -l "$file")"
			tmp="${tmp%% *}"
			[[ "$tmp" = "$check" ]] || fail "Expected $check lines, got $tmp"
			;;
		esac 
	done
}

# tests take time to run by definition: run all in background then check.
run_pattern shortkey 148,1,100 148,0,0 -- \
	-s 148 -a "touch shortkey"
add_check shortkey e-shortkey

run_pattern shortkey_norun 148,1,1100 148,0,0 -- \
	-s 148 -a "touch shortkey_norun"
add_check shortkey_norun ne-shortkey_norun

run_pattern short_twohits 148,1,100 148,0,100 148,1,100 148,0,0 -- \
	-s 148 -a "echo short" > short_twohits
add_check short_twohits l2-short_twohits


run_pattern longkey 148,1,2200 -- \
	-l 148 -t 2000 -a "touch longkey"
add_check longkey e-longkey

run_pattern longkey_norun 148,1,100 148,0,2000 -- \
	-l 148 -t 2000 -a "touch longkey_norun"
add_check longkey_norun ne-longkey_norun

run_pattern shortlong 148,1,100 148,0,100 148,1,2200 -- \
	-s 148 -a "touch shortlong_short" \
	-l 148 -t 2000 -a "touch shortlong_long"
add_check shortlong e-shortlong_short e-shortlong_long

run_pattern shortlonglong_1 148,1,100 148,0,100 -- \
	-s 148 -a "touch shortlonglong_1_short" \
	-l 148 -t 1000 -a "touch shortlonglong_1_long" \
	-l 148 -t 2000 -a "touch shortlonglong_1_long2"
add_check shortlonglong_1 e-shortlonglong_1_short ne-shortlonglong_1_long ne-shortlonglong_1_long2

run_pattern shortlonglong_2 148,1,1100 148,0,100 -- \
	-s 148 -a "touch shortlonglong_2_short" \
	-l 148 -t 2000 -a "touch shortlonglong_2_long2" \
	-l 148 -t 1000 -a "touch shortlonglong_2_long"
add_check shortlonglong_2 ne-shortlonglong_2_short e-shortlonglong_2_long ne-shortlonglong_2_long2

run_pattern shortlonglong_3 148,1,2200 -- \
	-l 148 -t 2000 -a "touch shortlonglong_3_long2" \
	-s 148 -a "touch shortlonglong_3_short" \
	-l 148 -t 1000 -a "touch shortlonglong_3_long"
add_check shortlonglong_3 ne-shortlonglong_3_short ne-shortlonglong_3_long e-shortlonglong_3_long2

check_all

if ((FAIL)); then
	echo "Some tests failed"
	exit 1
fi
echo "All ok"
