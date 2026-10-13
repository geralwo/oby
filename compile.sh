#!/bin/bash

set -euo pipefail

PRELUDE_DIR="src/prelude"

CFLAGS="-std=c11 -Wall -Wextra -Werror -pedantic -Iinclude -O2 -DOBY_PRELUDE_DIR=\"$PRELUDE_DIR\""

BINARY_NAME="oby"
BUILD_DIR="build"
SRC_FILE=".sources"
BINARY_ARGS=""
EMBED_GEN="tools/embed_gen.c"
EMBED_SRC="$BUILD_DIR/prelude_embed.c"

# Bake the prelude into a C table so the compiler carries it inside the binary
# and finds it from any working directory (see include/core/embed.h).
function _build_embed()
{
	gcc $CFLAGS "$EMBED_GEN" -o "$BUILD_DIR/embed_gen"
	"$BUILD_DIR/embed_gen" "$PRELUDE_DIR" "$EMBED_SRC" "$PRELUDE_DIR"/*.oby
}

function _build_exe()
{
	echo ":: Building '$BINARY_NAME' ..."
	if [ ! -f "$SRC_FILE" ]; then echo -e "error no source file. add a file with $0 add src/file.c" && exit; fi
	if [ ! -d "$BUILD_DIR" ]; then mkdir "$BUILD_DIR" && echo ":: created build dir"; fi
	_build_embed
	gcc $CFLAGS $(cat "$SRC_FILE") "$EMBED_SRC" -o "$BUILD_DIR/$BINARY_NAME"
}

# Run the .oby and C tests. A passing .oby test must exit 0 and its stdout must
# match its `tests/<name>.out` golden file; CRLF and LF compare equal, because
# the repository lives on Windows and either side can be rewritten by an
# editor. Behaviour that depends on carriage returns is checked inside the
# test, not by the golden. `*_error.oby` must fail with diagnostics,
# `*_runtime_error.oby` must fail at runtime, and `*_warning.oby` must compile
# cleanly while reporting a warning. With `update` = 1 a passing test rewrites
# its golden instead of being compared to it.
function _run_tests()
{
	local update="$1"
	local pass=0
	local fail=0

	_build_exe
	set +e
	for t in tests/*.oby; do
		[ -f "$t" ] || continue
		case "$t" in
		*_runtime_error.oby)
			# Runtime error test: must fail when run (a `vm error`, not a crash).
			out=$(timeout 5 "$BUILD_DIR/$BINARY_NAME" "$t" 2>&1)
			ec=$?
			if [ "$ec" -ne 0 ] && printf '%s' "$out" | grep -q "vm error"; then
				echo "  ok  $t (runtime error)"
				pass=$((pass + 1))
			else
				echo "  FAIL $t (expected a runtime error)"
				printf '%s\n' "$out" | sed 's/^/        /'
				fail=$((fail + 1))
			fi
			;;
		*_error.oby)
			# Diagnostics test: must report errors.
			out=$(timeout 5 "$BUILD_DIR/$BINARY_NAME" -check "$t" 2>&1)
			ec=$?
			if [ "$ec" -ne 0 ]; then
				echo "  ok  $t (reports errors)"
				pass=$((pass + 1))
			else
				echo "  FAIL $t (expected errors, none reported)"
				fail=$((fail + 1))
			fi
			;;
		*_warning.oby)
			# Warning test: must compile cleanly, and report a warning.
			out=$(timeout 5 "$BUILD_DIR/$BINARY_NAME" -check "$t" 2>&1)
			ec=$?
			if [ "$ec" -eq 0 ] && printf '%s' "$out" | grep -q "warning:"; then
				echo "  ok  $t (reports a warning)"
				pass=$((pass + 1))
			else
				echo "  FAIL $t (expected a warning)"
				printf '%s\n' "$out" | sed 's/^/        /'
				fail=$((fail + 1))
			fi
			;;
		*)
			# A passing test: exit 0, and stdout must match `tests/<name>.out`.
			actual="$(mktemp)"
			timeout 5 "$BUILD_DIR/$BINARY_NAME" "$t" >"$actual"
			ec=$?
			golden="${t%.oby}.out"
			if [ "$update" = "1" ] && [ "$ec" -eq 0 ]; then
				cp "$actual" "$golden"
			fi
			if [ "$ec" -ne 0 ]; then
				echo "  FAIL $t"
				sed 's/^/        /' "$actual"
				fail=$((fail + 1))
			elif [ ! -f "$golden" ]; then
				echo "  FAIL $t (no golden file; run '$0 test-update' to create it)"
				fail=$((fail + 1))
			else
				# Fold CRLF into LF on both sides before comparing.
				golden_lf="$(mktemp)"
				actual_lf="$(mktemp)"
				sed $'s/\r$//' "$golden" >"$golden_lf"
				sed $'s/\r$//' "$actual" >"$actual_lf"
				if cmp -s "$golden_lf" "$actual_lf"; then
					echo "  ok  $t"
					pass=$((pass + 1))
				else
					echo "  FAIL $t (output differs from $golden)"
					diff -u "$golden_lf" "$actual_lf" | sed 's/^/        /'
					fail=$((fail + 1))
				fi
				rm -f "$golden_lf" "$actual_lf"
			fi
			rm -f "$actual"
			;;
		esac
	done
	# C tests: each tests/*.c builds its own binary and must exit 0.
	for c in tests/*.c; do
		[ -f "$c" ] || continue
		local name
		name="$(basename "$c" .c)"
		if ! build_out=$(gcc $CFLAGS "$c" src/core/*.c src/native/*.c src/vm/*.c src/compiler/*.c src/lsp/*.c src/util.c "$EMBED_SRC" -o "$BUILD_DIR/$name" 2>&1); then
			echo "  FAIL $c (build)"
			printf '%s\n' "$build_out" | sed 's/^/        /'
			fail=$((fail + 1))
			continue
		fi
		out=$(timeout 5 "$BUILD_DIR/$name" 2>&1)
		ec=$?
		if [ "$ec" -eq 0 ]; then
			echo "  ok  $c"
			pass=$((pass + 1))
		else
			echo "  FAIL $c"
			printf '%s\n' "$out" | sed 's/^/        /'
			fail=$((fail + 1))
		fi
	done
	set -e
	echo ":: $pass passed, $fail failed"
	if [ "$fail" -ne 0 ]; then return 1; fi
	return 0
}


if [ $# -eq 0 ]; then
	_build_exe
else
	case "$1" in
    "scaf"|"scaffold")
            if [ -e "$SRC_FILE" ] || [ -e "src" ] || [ -e "main.c" ]; then
                    echo "error: project already appears to be initialized"
                    exit 1
            fi
            mkdir -p src
            mkdir -p include

            cat > src/main.c <<EOF
#include <stdio.h>

int main(void)
{
printf("Hello, $BINARY_NAME!\\n");
return 0;
}

EOF
            printf '%s' "src/main.c\n" > "$SRC_FILE"
            echo ":: project scaffold build"
            echo ":: Run '$0 run' to build and execute"
            ;;
	"run")
		_build_exe
		echo ":: running $BINARY_NAME"
		shift
		BINARY_ARGS=$@
		set +e
		./$BUILD_DIR/$BINARY_NAME $@
		__exit_code=$?
		set -e
		echo ":: exit($__exit_code): $BINARY_NAME $BINARY_ARGS"
		exit $__exit_code
		;;
	"clean")
		if [ -d "build" ]; then
			rm -rf "./build"
			echo ":: Cleaned build directory ..."
		fi
		;;
	"add")
		touch "$SRC_FILE"

		shift

		if [ $# -eq 0 ]; then
			echo "usage: $0 add <files/globs>"
			exit 1
		fi
		for file in "$@"; do
			if [ -d "$file" ]; then
				echo "ignoring $file/"
				continue
			fi

			if [ ! -f "$file" ]; then
				echo "$file doesnt exist"
				continue
			fi

			if grep -Fxq "$file" "$SRC_FILE"; then
				echo "$file already present"
				continue
			fi

			printf '%s\n' "$file" >> "$SRC_FILE"
			echo "added $file"
		done

		;;
	"remove")
		if [ ! -f "$SRC_FILE" ]; then
			echo "no source file list"
			exit 1
		fi

		shift

		if [ $# -eq 0 ]; then
			echo "usage: $0 remove <files/globs>"
			exit 1
		fi

		for file in "$@"; do
			if [ -d "$file" ]; then
				echo "ignoring $file/"
				continue
			fi

			if ! grep -Fxq "$file" "$SRC_FILE"; then
				echo "$file not present"
				continue
			fi

			echo "removing $file"

			tmp_file="$(mktemp)"

			grep -Fxv "$file" "$SRC_FILE" > "$tmp_file"

			mv "$tmp_file" "$SRC_FILE"
		done
		;;
	"sources")
		if [ -f "$SRC_FILE" ]; then
			cat "$SRC_FILE"
		fi
		;;
	"test")
		_run_tests 0
		;;
	"test-update")
		_run_tests 1
		;;
	"config")
		if [ -f "$SRC_FILE" ]; then
			echo ":: source files"
			cat "$SRC_FILE"
			echo ":: prelude"
			echo "$PRELUDE_DIR -> $EMBED_SRC"
			echo ":: gcc flags"
			echo "$CFLAGS"
		fi
		;;
	"help"|"-help"|"h"|"-h")
		echo ":: available commands"
		echo " scaf	- creates basic project scaffold"
		echo " run	- builds and runs the project and passes args"
		echo " test	- runs tests/*.oby (*_error.oby must report errors, *_runtime_error.oby must fail at runtime, *_warning.oby must report a warning, other .oby must match tests/*.out) and tests/*.c"
		echo " test-update	- rewrite tests/*.out golden files from current output"
		echo " add	- adds a source file to the build config"
		echo " remove	- removes a source file from the build config"
		echo " sources  - displays source file inputs"
		echo " config   - displays cflags and source file inputs"
		;;
	*)
		echo ":: Unknown option '$1'"
		exit 1
		;;
	esac
fi
