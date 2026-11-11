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

`pnx repl` reads and runs one line at a time and shows the stack after each line, the bottom on the left. The stack
stays from line to line, as in Forth: later lines use the definitions of earlier ones and the values they left. A line
that is refused or ends with a fault leaves the stack as it was. A line that begins with `clear` starts from an empty
stack.

#ex("manual", "15-repl.pnx")

The line `1 0 div` ended with a fault and left the stack as it was. The last line, `cube`, was refused because the
top of the stack is a `str`. The message of a refusal goes to standard error, so it is not in the output above.

A line that declares a word or a global adds just what it declares to the program, so even in a long session a
line loads in the time its own length takes, and the memory block fills with what was declared. A line that only computes
values uses none of it. When three quarters of the block are used, `pnx` says so once on standard error. Start a
session that declares many large globals with a larger block through `PNX_BLOCK_KB` (the default is 16384, that is
16 MiB).

What the stack holds after each line, followed. The next line starts from it.

#trace(
  ("2 3", "2 3", [Two values stay on the stack.]),
  ("add", "5", [The next line takes them.]),
  ("word cube … end", "5", [A definition leaves the stack as it is.]),
  ("cube", "125", [Uses the definition and the value of the lines before.]),
  ("\"pinax\" str.upper", "125 \"PINAX\"", [A `str` stays too, up to 255 bytes.]),
  ("1 0 div", "125 \"PINAX\"", [A line that ends with a fault leaves the stack as it was.]),
  ("cube", "125 \"PINAX\"", [Refused: the top is a `str`, not an `int`.]),
)

A word can be defined again at any time: write `word` with its name once more. When the signature is the same, every
word that calls it calls the new one, so a fix reaches every caller. When the signature is another, the new word takes
the name, and the words compiled before keep calling the old body, as in Forth; `pnx repl` says which they are.

#ex("manual", "15-redefine.pnx")

After the first redefinition `quad` doubles twice (12). After the second, `sq` multiplies two values, and `quad` still
runs the old `sq`. A word of a file is changed in the file, then loaded again.

A statement may end with a comma or a dot. A comma waits for nothing. A dot is a barrier: what the statements before it
started is finished there, so output held in a buffer is written. Neither is required; the end of a line at the
prompt, and the end of a program's code, are a dot already.

#ex("manual", "15-dot.pnx")

In bash terms, a comma is like `&` and a dot like `wait`. Use a dot where what came before must be seen, or must have
happened, before going on: before a long computation, after a progress line.

`pnx see --effects` tells, without running anything, what each word of a file may do beyond its own stack and
locals: the globals it reads and writes, the host modules it reaches, whether it may yield or fail. A word marked
`none` gives the same results for the same inputs and touches nothing else.

#ex("manual", "15-effects.pnx")

== Shell scripts

The module `sh` runs commands through the system shell, and `set_order` lets its words and those of `io` be written
bare. A file that begins with `#!/usr/bin/env pnx` runs as a command.

#ex("manual", "15-shell.pnx")

A command's status is a value, not a failure: `"exit 2" run` gives 2. Only a shell that cannot start fails. To put data
into a command, quote it first: `cat ("ls ", name sh.quote) run`. `sh.open_read` and `sh.open_write` give a pipe as a
`file`, read and written with the words of `file`; closing it waits for the command, and `sh.status` then tells how it
ended. `sh.exit` ends the program with a status of its own. A host that embeds Pinax has `sh` only when it registers it
(standard 17.5).

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
