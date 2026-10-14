#import "/tools/common.typ": *

= Tools <tools>

#opening[Chapter 1 (`pnx`), chapter 3 (words).][
- What do I use to test words?
- How do I try things out a line at a time?
- How do I see the stack at every line, or make indentation even?
]

== Tests

`test NAME … end` defines a test: a word that takes and gives nothing, and does not run at load. `pnx test FILE` runs
the tests in order. A false `assert` makes that test fail.

#ex("manual", "15-test.pnx")
#ex("manual", "15-test-fail.pnx")

`clamp` raises a negative to 0, so the test `wrong` writes down a wrong expectation. When a test fails, `pnx` exits
with 1.

#mistake[Writing just the comparison makes a test][`21 double 42 eq` only leaves a `bool`; it checks nothing. A test
takes and gives nothing, so a test that leaves a value is refused. End it with `assert`.
#ex("manual", "15-test-leaves.pnx")]

== Sessions

`pnx repl` reads and runs one line at a time and shows what each line leaves. Later lines use the definitions of earlier
ones. A line that is refused or ends with a fault leaves nothing behind.

#ex("manual", "15-repl.pnx")

The last line, `cube cube`, was refused because the stack was empty. The message of a refusal goes to standard error,
so it is not in the output above.

== Seeing the stack, and formatting

`pnx see` shows, for each line of a definition, the stack where that line ends. Use it when the stack is not what you
think.

#ex("manual", "15-see.pnx")

`pnx fmt` changes only indentation and spaces. Tokens, the places of lines and remarks stay as they are.

#ex("manual", "15-fmt.pnx")

#summary[
- `test … end` and `pnx test`; a failed test makes the exit status 1 (standard 20.2).
- `pnx repl` shows what each line leaves (20.3).
- `pnx see` shows the stack of each line, `pnx fmt` makes indentation even (20.4).
]
