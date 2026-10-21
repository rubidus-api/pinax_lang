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

A line that declares a word or a global is loaded again with the whole program so far. So in a long session a later
line takes a little longer to load, and the memory block fills with what was declared. A line that only computes
values uses none of it. When three quarters of the block are used, `pnx` says so once on standard error. Start a
session that declares many large globals with a larger block through `PNX_BLOCK_KB` (the default is 16384, that is
16 MiB).

What each line leaves on the stack, followed. The values left at the end of a line are what is shown; the next line
starts from an empty stack again.

#trace(
  ("2 3 add", "5", [Shows the value left, `5`.]),
  ("word cube … end", "—", [A definition leaves no value.]),
  ("4 cube", "64", [Uses the definition of the line before.]),
  ("1 0 div", "—", [A line that ends with a fault leaves nothing.]),
  ("cube cube", "—", [Refused: no `int` to take from an empty stack.]),
)

== Seeing the stack, and formatting

`pnx see` shows, for each line of a definition, the stack where that line ends. Use it when the stack is not what you
think.

#ex("manual", "15-see.pnx")

Right of the line number is the code of that line; right of `|` is the stack where the line ends, the bottom on the left
and the top on the right --- as in the trace tables of this book. It is most useful when standard modules or `cat`
make the stack hard to follow.

`pnx see` also works on a source it refuses. It shows the stack after each line up to the place of the refusal, then
gives the message. So what is left on the stack can be seen before the message is read.

#ex("manual", "15-see-wrong.pnx")

The signature promised one `str`, but the stack after the last line is `str int str`. The `swap` of line 3 sent
`"won"` down, and the result of line 2 (an `int`) was used by nothing. Removing lines 2 and 3 leaves only the result of
`cat`.

`pnx fmt` changes only indentation and spaces. Tokens, the places of lines and remarks stay as they are.

#ex("manual", "15-fmt.pnx")

#qa[Do tests go in a file of their own, or beside the words?][Either. `pnx test FILE` loads the file and then runs only
the tests in it. Beside the words, a test is in sight when the word is changed. When there are many, a test file of its
own brings the module in with `use`.]

#qa[Why does the stack of `pnx see` show types only, and no values?][`see` loads and does not run. At load the processor
knows the depth and types of the stack, nothing more; values exist only when the program runs. To see values, run a line
at a time in `pnx repl`.]

#summary[
- `test … end` and `pnx test`; a failed test makes the exit status 1 (standard 20.2).
- `pnx repl` shows what each line leaves (20.3).
- `pnx see` shows the stack after each line; on a refused source, up to the place, then the message (20.4).
- `pnx fmt` changes only indentation and spaces (20.4).
]
