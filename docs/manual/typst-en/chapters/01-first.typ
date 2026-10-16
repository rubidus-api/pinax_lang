#import "/tools/common.typ": *

= The first program <first>

#opening[Nothing; this is the first chapter.][
- What does the smallest Pinax program look like?
- How is a program run, and how is a wrong program reported?
- What does it take to make a word and call it?
]

== A program that says hello

#ex("manual", "01-hello.pnx")

These two lines are the whole program. The first line, `use io`, opens the module `io` for input and output. The
second puts a string on the stack and calls the word `io.println`, which takes the string off the top of the stack,
writes it to standard output and ends the line.

Pinax code reads in this order: #strong[values, then the word]. Values pile up on the stack; a word takes off what it
needs, does its work and puts its results back. This is called #term("postfix") notation.

Follow the order in which the processor reads these two lines. It reads one token at a time from the left: a value
goes onto the stack, a word is set to work. The top of the stack is written on the right, and `—` is an empty stack.

#trace(
  ("use io", "—", [Opens the module `io`. The stack is not touched.]),
  ("\"Hello, Pinax!\"", "\"Hello, Pinax!\"", [One string value goes onto the stack.]),
  ("io.println", "—", [Takes the string off the top, prints it and ends the line. The stack is empty again.]),
)

Note that the stack is empty when the program ends. A program that leaves values on it is refused (chapter 2).

#qa[Is there no start function like `main`?][No. Code written outside word definitions runs once, from top to bottom,
when the file is loaded; that is where a program starts. A larger program gathers its work into words and calls one of
them at the very end.]

== Running with `pnx`

The command `pnx` reads a file and runs it. The first line of the output box above, `$ pnx 01-hello.pnx`, is the
command that was run. Before running a file, `pnx` #strong[loads] it: it splits the tokens and checks every name, type
and stack. If anything is wrong, it refuses without running a single line.

#ex("manual", "01-no-use.pnx")

This program uses `io.println` without opening `io`. A message has the form `file:line:column: reason`; the column is
where the offending token starts.

#mistake[Adding with `+`][Writing `1 + 2`, as in other languages, is refused. Outside literals and remarks Pinax allows
only the characters of names and `( ) , .`. Addition is the word `add`, written after the values.
#ex("manual", "01-plus.pnx")
A sign attached to a number, as in `-7`, is part of the number and fine. Only a `+` or `-` standing apart is not.]

== Remarks and line breaks

A line that starts with `rem` is a remark. `rem` after code makes the rest of the line a remark. The examples of this
book use remarks to say what each line does.

#ex("manual", "01-comments.pnx")

A line break is the same as a space. The code of one call may run over two lines, and one line may hold several pieces
of work. Indentation and line breaks are for the reader; `pnx fmt` evens them out (chapter 15).

#qa[Why `rem` and not `//` or `#`?][Outside literals and remarks, Pinax allows only the characters of names and
`( ) , .`. So a remark also opens with a word-like `rem`. `rem` is reserved and cannot be a name.]

== Making words

Here is a program that adds up 1 to 100, three ways.

#ex("manual", "01-sum.pnx")

`word NAME (signature) body end` defines a word. The signature `(n int gives int)` means "takes one `int`, named `n`,
and gives one `int`". When the body ends, the stack shall hold exactly the results the signature promises.

- `sum_loop` is a loop with the locals `total` and `i`. `to NAME` puts the top value into that name. `loop … end`
  repeats, and `i n gt until` means "leave when `i` is greater than `n`".
- `sum_range` makes a #strong[stem] of 1 to `n` with `1 n 1 add range` and gives it to `sum`.
- `sum_formula` is the formula $n(n+1)/2$ written in postfix.

#qa[Why the `str.to_str`?][`io.println` takes only strings. To print an integer it has to become text first, and the
word for that is `to_str` of the module `str` --- hence `use str`. There is no output word that takes an integer as it
is: each word has exactly one signature.]

== Taking arguments

The arguments of `pnx FILE ARGUMENTS…` are read with the module `env`.

#ex("manual", "01-greet.pnx")

`1 env.arg` gives the first argument. There may be none, so this word can #strong[fail]; that is why `or_else … end`
follows it right away, with the value to use instead. Calling a word that can fail and leaving the place after it empty
is refused. Chapter 8 covers failures.

#summary[
- Code is values, then words. Code outside definitions runs once at load (standard 5.2).
- Words of a module need `use MODULE` and are written `MODULE.WORD` (13.1).
- There are no characters like `+` or `;`; arithmetic is words (6.1, 6.4).
- `word NAME (signature) … end` makes a word; when the body ends the stack shall match the results (9.2).
- A wrong program is refused before it runs (4).
]
