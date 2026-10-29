#!/bin/sh
# SPDX-License-Identifier: MIT
# Golden tests: each line of cases is "NAME ARGS".  The output of pinctrl
# ARGS, with stderr and the exit code, must equal expected/NAME.  With
# UPDATE=1, write the expected files instead.
set -uf

bin=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=$(cd "$(dirname "$0")" && pwd)
fail=0
count=0

mkdir -p "$dir/expected"
while read -r name args; do
	case $name in '' | '#'*) continue ;; esac
	count=$((count + 1))
	out=$(cd "$dir" && $bin $args 2>&1; echo "rc=$?")
	if [ "${UPDATE:-0}" = 1 ]; then
		printf '%s\n' "$out" > "$dir/expected/$name"
	elif ! printf '%s\n' "$out" | diff -u "$dir/expected/$name" - ; then
		echo "FAIL $name"
		fail=$((fail + 1))
	fi
done < "$dir/cases"

echo "$count tests, $fail failed"
[ "$fail" = 0 ]
