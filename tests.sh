#!/bin/bash

# shellcheck disable=SC2094 ## incorrectly thinks we read/write from same file

TESTDIR=$(mktemp -d /tmp/buttond.XXXXXX)
trap "rm -rf '$TESTDIR'" EXIT

for d in . ..; do
	[ -e "$BUTTOND" ] || BUTTOND=$(realpath "$d/buttond")
	[ -e "$GEN_EVENTS" ] || GEN_EVENTS=$(realpath "$d/gen_events.py")
done
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

	declare -a args=( )
	declare -a inputs=( )
	declare -a commands=( )
	local command
	while [[ $# -gt 0 ]]; do
		if [[ "$1" = "--" ]]; then
			shift
			if [[ -z "$DRYRUN" ]]; then
				exec {FD}< <("$GEN_EVENTS" "${args[@]}")
				inputs+=( "/proc/self/fd/$FD" )
			else
				printf -v command "\"%s\" " "$GEN_EVENTS" "${args[@]}"
				commands+=( "$command" )
			fi
			args=( )
		fi
		args+=( "$1" )
		shift
	done

	if [[ -n "$DRYRUN" ]]; then
		printf '"%s" ' "$BUTTOND" --test_mode
		printf -- "<(%s) " "${commands[@]}"
		printf '"%s" ' "${args[@]}"
		echo
		return
	fi >&2
	"$BUTTOND" --test_mode "${inputs[@]}" "${args[@]}" &
	PROCESSES[$testname]=$!
}

run_inotify() {
	local testname="$1"
	local pipe="$testname"
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
		printf '"%s" ' "$BUTTOND" --test_mode -i "$pipe" "$@"
		echo '&'
		echo "sleep 1"
		echo "mkfifo $pipe"
		printf '"%s" ' "$GEN_EVENTS" "${keys[@]}"
		echo "> $pipe"
		echo 'wait $!'
		return
	fi >&2
	(
		"$BUTTOND" --test_mode -i "$pipe" "$@" 2>/dev/null &
		BPID=$!
		sleep 1
		mkfifo "$pipe"
		"$GEN_EVENTS" "${keys[@]}" > "$pipe"
		wait $BPID
	) &
	PROCESSES[$testname]=$!
}

check_fail() {
	local testname="$1"
	shift

	# skip tests we didn't ask for
	case ",$ONLY," in
	",,"|*",$testname,"*) ;;
	*) return;;
	esac

	if [[ -n "$DRYRUN" ]]; then
		printf '"%s" ' "$BUTTOND" "$@"
		return
	fi

	if "$BUTTOND" "$@" 2>/dev/null; then
		fail
	fi
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
	-s PROG1 -a "touch shortkey"
add_check shortkey e-shortkey

run_pattern shortkey_norun 148,1,1100 148,0,0 -- \
	-s prog1 -a "touch shortkey_norun"
add_check shortkey_norun ne-shortkey_norun

run_pattern short_twohits 148,1,100 148,0,100 148,1,100 148,0,0 -- \
	-s 148 -a "echo short" \
	--debounce-time 0 > short_twohits
add_check short_twohits l2-short_twohits

run_pattern short_debounce 148,1,100 148,0,5 148,1,100 148,0,0 -- \
	-s 148 -a "echo short" \
	--debounce-time 50 > short_debounce
add_check short_debounce l1-short_debounce

run_pattern longkey 148,1,2200 -- \
	-l 148 -t 2000 -a "touch longkey"
add_check longkey e-longkey

run_pattern longkey_norun 148,1,100 148,0,2000 -- \
	-l 148 -t 2000 -a "touch longkey_norun"
add_check longkey_norun ne-longkey_norun

run_pattern long_twohits 148,1,1100 148,0,100 148,1,1100 148,0,0 -- \
	-l 148 -t 1000 -a "echo long" > long_twohits
add_check long_twohits l2-long_twohits

run_pattern long_debounce 148,1,600 148,0,5 148,1,600 148,0,0 -- \
	-l 148 -t 1000 -a "echo long" \
	--debounce-time 50 > long_debounce
add_check long_debounce l1-long_debounce

run_pattern shortlong 148,1,100 148,0,100 148,1,2200 -- \
	-s 148 -a "touch shortlong_short" \
	-l 148 -t 2000 -a "touch shortlong_long"
add_check shortlong e-shortlong_short e-shortlong_long

run_pattern shortshortlong_1 148,1,100 148,0,100 -- \
	-s 148 -a "touch shortshortlong_1_short" \
	-s 148 -t 2000 -a "touch shortshortlong_1_short2" \
	-l 148 -t 2000 -a "touch shortshortlong_1_long"
add_check shortshortlong_1 e-shortshortlong_1_short ne-shortshortlong_1_short2 ne-shortshortlong_1_long

run_pattern shortshortlong_2 148,1,1200 148,0,100 -- \
	-s 148 -a "touch shortshortlong_2_short" \
	-s 148 -t 2000 -a "touch shortshortlong_2_short2" \
	-l 148 -t 2000 -a "touch shortshortlong_2_long"
add_check shortshortlong_2 ne-shortshortlong_2_short e-shortshortlong_2_short2 ne-shortshortlong_2_long

run_pattern shortshortlong_3 148,1,2200 148,0,100 -- \
	-s 148 -a "touch shortshortlong_3_short" \
	-l 148 -t 1000 -a "touch shortshortlong_3_long" \
	-l 148 -t 2000 -a "touch shortshortlong_3_long2"
add_check shortshortlong_3 ne-shortshortlong_3_short ne-shortshortlong_3_long e-shortshortlong_3_long2

run_pattern shortlonglong_1 148,1,100 148,0,100 -- \
	-s 148 -a "touch shortlonglong_1_short" \
	-l 148 -t 2000 -a "touch shortlonglong_1_long2" \
	-l 148 -t 1000 -a "touch shortlonglong_1_long"
add_check shortlonglong_1 e-shortlonglong_1_short ne-shortlonglong_1_long ne-shortlonglong_1_long2

run_pattern shortlonglong_2 148,1,1200 148,0,100 -- \
	-s 148 -a "touch shortlonglong_2_short" \
	-l 148 -t 2000 -a "touch shortlonglong_2_long2" \
	-l 148 -t 1000 -a "touch shortlonglong_2_long"
add_check shortlonglong_2 ne-shortlonglong_2_short e-shortlonglong_2_long ne-shortlonglong_2_long2

run_pattern shortlonglong_3 148,1,2200 -- \
	-l 148 -t 2000 -a "touch shortlonglong_3_long2" \
	-s 148 -a "touch shortlonglong_3_short" \
	-l 148 -t 1000 -a "touch shortlonglong_3_long"
add_check shortlonglong_3 ne-shortlonglong_3_short ne-shortlonglong_3_long e-shortlonglong_3_long2

run_pattern multikey_short 148,1,10 149,1,100 148,0,10 149,0,0 -- \
	-s 148 -a 'touch multikey_short_1' \
	-s 149 -a 'touch multikey_short_2'
add_check multikey_short e-multikey_short_1 e-multikey_short_2

run_pattern multikey_shortlong 148,1,10 149,1,100 149,0,2500 -- \
	-s 148 -a 'touch multikey_shortlong_1' \
	-l 148 -t 2000 -a 'touch multikey_shortlong_2' \
	-s 149 -a 'touch multikey_shortlong_3' \
	-l 149 -t 2000 -a 'touch multikey_shortlong_4'
add_check multikey_shortlong ne-multikey_shortlong_1 e-multikey_shortlong_2 \
	e-multikey_shortlong_3 ne-multikey_shortlong_4

run_pattern multiinput 148,1,100 148,0,0 -- \
	149,1,100 149,0,0 -- \
	-s 148 -a "touch multiinput_1" \
	-s 149 -a "touch multiinput_2"
add_check multiinput e-multiinput_1 e-multiinput_2

run_inotify inotify 148,1,100 148,0,0 -- \
	-s 148 -a "touch inotify_ok"
add_check inotify e-inotify_ok

run_inotify reopen 148,1,100 fdsf 148,0,0  -- \
	-s 148 -a "touch reopen"
add_check reopen e-reopen

check_fail sametime_short /dev/null \
	-s 148 -t 1000 -a "echo 1" \
	-s 148 -t 1000 -a "echo 1"

check_fail sametime_long /dev/null \
	-s 148 -t 1000 -a "echo 1" \
	-s 148 -t 1000 -a "echo 1"

check_fail short_longer_long /dev/null \
	-s 148 -t 2000 -a "echo 1" \
	-l 148 -t 1000 -a "echo 1"

check_all

if ((FAIL)); then
	echo "Some tests failed"
	exit 1
fi
echo "All ok"
