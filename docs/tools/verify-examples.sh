#!/bin/sh
# Runs every example of one document tree and captures what it prints. The documents print
# only what this script captured; nothing is copied by hand. MIT (see ../manual/LICENSE-CODE).
#
#   sh docs/tools/verify-examples.sh spec|manual      (from the repository root, after ./nob)
#
# An example is <tree>/examples/NAME.pnx or NAME.c. Its first lines say what must happen:
#   rem run                  runs; exit 0 and nothing on standard error
#   rem fault                the run ends with a fault (exit 1); standard error is captured
#   rem refused              pnx refuses it at load (exit 1); the message is captured
#   rem test                 pnx test; every test passes (exit 0)
#   rem test-fail            pnx test; a test fails (exit 1)
#   rem repl                 the rest of the file is typed into pnx repl
#   rem fmt | rem see        the output of pnx fmt or pnx see
#   rem see-refused          pnx see on a source it refuses: the listing up to the place, then the message
#   rem args: A B            arguments after the file name
#   rem stdin: NAME.in       standard input (a file beside the example)
#   rem with: OTHER.pnx      another example the program uses (copied beside it)
#   rem hide                 an example used only by others (not run on its own)
#   // embed                 (first line of a .c file) compiled with include/pinax.h and the
#                            library, then run; exit 0
#   // embed host            the same, with the host library as well (src/host: the standard
#                            modules, the loop and its channels) and the system's threads
# The contract lines are removed before the program runs, so the line numbers in messages
# are those of the program as printed. Output: <tree>/build/examples-out/NAME.{src,out,kind}.
set -u
tree=${1:?usage: verify-examples.sh spec|manual}
root=$(pwd)
pnx=${PNX:-$root/build/native/pnx}
lib=${PINAX_LIB:-$root/build/native/libpinax.a}
host_lib=${PINAX_HOST_LIB:-$root/build/native/libpinax_host.a}
cc=${CC:-cc}
[ -x "$pnx" ] || { echo "verify-examples: no $pnx (run ./nob first)" >&2; exit 2; }
src_dir=$root/docs/$tree/examples
out_dir=$root/docs/$tree/build/examples-out
run_dir=$root/docs/$tree/build/examples-run
rm -rf "$out_dir" "$run_dir"
mkdir -p "$out_dir" "$run_dir"

contract() { sed -n "s/^rem $2\$/yes/p; s/^rem $2: *//p" "$1" | head -n 1; }

# the programs without their contract lines
for f in "$src_dir"/*.pnx; do
    [ -e "$f" ] || continue
    n=$(basename "$f")
    awk 'BEGIN { head = 1 }
         head && /^rem (run|fault|refused|test|test-fail|repl|fmt|see|see-refused|hide)$/ { next }
         head && /^rem (args|stdin|with):/ { next }
         { head = 0; print }' "$f" >"$run_dir/$n"
    cp "$run_dir/$n" "$out_dir/$n.src"
done
for f in "$src_dir"/*.in; do [ -e "$f" ] && cp "$f" "$run_dir/"; done

total=0 failed=0
fail() { echo "  FAIL $1: $2"; [ -s "$3" ] && head -5 "$3" | sed 's/^/    /'; failed=$((failed + 1)); }

for f in "$src_dir"/*.pnx; do
    [ -e "$f" ] || continue
    n=$(basename "$f")
    if [ "$(contract "$f" hide)" = yes ]; then # shown as source only (a module the others use)
        : >"$out_dir/$n.out"
        echo module >"$out_dir/$n.kind"
        continue
    fi
    total=$((total + 1))
    kind=run
    for k in fault refused test test-fail repl fmt see see-refused; do
        [ "$(contract "$f" "$k")" = yes ] && kind=$k
    done
    args=$(contract "$f" args)
    stdin=$(contract "$f" stdin)
    input=/dev/null; [ -n "$stdin" ] && input=$run_dir/$stdin
    out=$out_dir/$n.out
    o=$run_dir/.stdout e=$run_dir/.stderr
    case $kind in
        run|fault|refused) cmd="pnx $n${args:+ $args}"
            # shellcheck disable=SC2086
            (cd "$run_dir" && "$pnx" "$n" $args <"$input" >"$o" 2>"$e") ;;
        test|test-fail) cmd="pnx test $n"
            (cd "$run_dir" && "$pnx" test "$n" <"$input" >"$o" 2>"$e") ;;
        repl) cmd="pnx repl"
            (cd "$run_dir" && "$pnx" repl <"$n" >"$o" 2>"$e") ;;
        fmt|see) cmd="pnx $kind $n"
            (cd "$run_dir" && "$pnx" "$kind" "$n" <"$input" >"$o" 2>"$e") ;;
        see-refused) cmd="pnx see $n"
            (cd "$run_dir" && "$pnx" see "$n" <"$input" >"$o" 2>"$e") ;;
    esac
    status=$?
    case $kind in
        run|test|fmt|see)
            [ $status = 0 ] || { fail "$n" "exit $status, wanted 0" "$e"; continue; }
            [ -s "$e" ] && { fail "$n" "wrote to standard error" "$e"; continue; }
            { echo "\$ $cmd"; cat "$o"; } >"$out" ;;
        repl)
            [ $status = 0 ] || { fail "$n" "exit $status, wanted 0" "$e"; continue; }
            { echo "\$ $cmd"; cat "$o"; } >"$out" ;;
        test-fail)
            [ $status = 1 ] || { fail "$n" "exit $status, wanted 1 (a failing test)" "$o"; continue; }
            { echo "\$ $cmd"; cat "$o" "$e"; } >"$out" ;;
        fault)
            [ $status = 1 ] || { fail "$n" "exit $status, wanted 1 (a fault)" "$o"; continue; }
            grep -q ':[0-9]*: ' "$e" || { fail "$n" "no fault message" "$e"; continue; }
            { echo "\$ $cmd"; cat "$o" "$e"; } >"$out" ;;
        see-refused)
            [ $status = 1 ] || { fail "$n" "exit $status, wanted 1 (refused)" "$o"; continue; }
            grep -q "^$n:[0-9]*:[0-9]*: " "$e" || { fail "$n" "no load diagnostic" "$e"; continue; }
            { echo "\$ $cmd"; cat "$o"; grep -v "^$n: [a-z][a-z ]*\$" "$e"; } >"$out" ;;
        refused)
            [ $status = 1 ] || { fail "$n" "exit $status, wanted 1 (refused)" "$o"; continue; }
            [ -s "$o" ] && { fail "$n" "printed output, so it was not refused at load" "$o"; continue; }
            grep -q "^$n:[0-9]*:[0-9]*: " "$e" || { fail "$n" "no load diagnostic" "$e"; continue; }
            { echo "\$ $cmd"; grep -v "^$n: [a-z][a-z ]*\$" "$e"; } >"$out" ;;
    esac
    echo "$kind" >"$out_dir/$n.kind"
done

for f in "$src_dir"/*.c; do
    [ -e "$f" ] || continue
    n=$(basename "$f")
    total=$((total + 1))
    head -n 1 "$f" | grep -q '^// embed' || { fail "$n" "a C example starts with // embed" /dev/null; continue; }
    sed '1d' "$f" >"$out_dir/$n.src"
    exe=$run_dir/${n%.c}
    if head -n 1 "$f" | grep -q '^// embed host$'; then
        set -- -I"$root/src/host" "$f" "$host_lib" "$lib" -lm -pthread
    else
        set -- "$f" "$lib" -lm
    fi
    if ! "$cc" -std=c23 -Wall -Wextra -Werror -I"$root/include" "$@" -o "$exe" 2>"$run_dir/.cc"; then
        fail "$n" "does not compile" "$run_dir/.cc"; continue
    fi
    (cd "$run_dir" && "$exe" >"$run_dir/.stdout" 2>"$run_dir/.stderr")
    status=$?
    [ $status = 0 ] || { fail "$n" "exit $status" "$run_dir/.stderr"; continue; }
    { echo "\$ ./${n%.c}"; cat "$run_dir/.stdout"; } >"$out_dir/$n.out"
    echo embed >"$out_dir/$n.kind"
done

echo "verify-examples $tree: $total examples, $failed failed"
[ $failed = 0 ]
