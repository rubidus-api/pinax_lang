#import "/tools/common.typ": *
// shape: the command; its refusals are those of the language

= The command `pnx` <pnx>

#para("1")[`pnx` is a host that uses this library, and a development tool. It registers all the standard host modules
(clause 17).
```text
pnx [--lib NAME=PATH]... [--capacity NAME=N]... file.pnx [arguments]
pnx test file.pnx
pnx repl
pnx see file.pnx
pnx fmt file.pnx
pnx export-c file.pnx [NAME]
```]

== Running <pnx-run>

#para("1")[`pnx file.pnx arguments…` loads the file and runs its load code. For each module the file uses that is not
registered, it first loads `MODULE.pnx` from beside the file (13.1). The file is loaded as the module named after it
without `.pnx`; when that name is not written like a word (it begins with a digit or contains `-`), the module has no
name.]

#para("2")[There are three exit statuses.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Status*], [*When*],
  [0], [the load code ran to its end; for `pnx test`, every test passed],
  [1], [a refusal: `file:line:column: reason` on standard error; a fault: `file:line: kind` and the locals of the word; for `pnx test`, a test failed],
  [2], [the command line is wrong (the usage is shown), the file cannot be read, a library of `--lib` cannot be opened, a `--capacity` is not `NAME=N`],
)]

#para("3")[`--lib NAME=PATH` opens the shared library at `PATH` so that the resolver finds its functions for
`extern … from "NAME"` (clause 18). It finds only the functions that library defines (not those it imports). Without
`--lib`, every `extern` is refused.]

#para("3a")[`--capacity NAME=N` gives the capacity a source writes `host` (8 (4a), 19.4 (1d)), for a run, `pnx test`,
`pnx see`, `pnx export-c` and `pnx repl`. A plain `NAME` is a declaration of the file, `MODULE.NAME` one of a module
it uses. `N` is 1 to 2#super[31] − 1, and at most 16 are given. Without it, a source that writes `host` is refused at
load, with the name of the capacity.]

#para("3b")[What the script writes with `io.print` and `io.println` is kept in a buffer and written when the
buffer is full, before `io.eprint`, `io.read_line` and `clock.sleep`, and when the run ends or faults. So standard
output and standard error come out in the order the script wrote them. Except in `pnx repl`, `io.read_line` reads
standard input ahead. A host that embeds the library writes and reads both without a buffer.]

== Tests <pnx-test>

#part("Syntax")
```ebnf
test = "test" NAME { body } "end" ;
```

#para("1")[`test NAME … end` defines a #term("test"): a word that takes and gives nothing. Tests do not run at load.
`pnx test file.pnx` runs the tests of the file in order, writes `ok` or `FAIL` for each, and exits with 0 when all pass,
1 otherwise. A fault inside a test (a false `assert` included) is that test's failure.]

#ex("spec", "20-test.pnx")
#ex("spec", "20-test-fail.pnx")

== Sessions <pnx-repl>

#para("1")[`pnx repl` reads lines from standard input and runs them one at a time (`pinax_eval` of 19.4). It writes what
each line leaves on standard output. A line can use what earlier lines defined. A line that ends inside a definition or
a block is read together with the next. A line that is refused or ends with a fault leaves nothing behind.]

#para("2")[There are tool words only for sessions: `list_words` (the words defined), `stack` (the types on the stack
there), `see` (the stack after every line of the definitions that follow), `redefine word …` (replaces a word with the
same signature; code that calls it calls the new one).]

#ex("spec", "20-repl.pnx")

== Showing the stack, and formatting <pnx-tools>

#para("1")[`pnx see file.pnx` shows, for every line of a definition, the stack (as types) where that line ends.]

#ex("spec", "20-see.pnx")

#para("1a")[When the source is refused, `pnx see` shows the lines before the place of the refusal, gives the
diagnostic, and exits with 1.]

#ex("spec", "20-see-refused.pnx")

#para("1b")[`pnx see --checks file.pnx` loads the file as `pnx file.pnx` does (the code outside definitions runs),
then shows, per word, the run-time checks the loader proved cannot fire: the word's number of checks and of proven
ones, and for each proven instruction its line, the instruction and the reason. A proof changes nothing a program
does; the checks still run.]

#ex("spec", "20-see-checks.pnx")

#para("2")[`pnx fmt file.pnx` writes the text with only indentation and spaces changed to standard output. Tokens, line
breaks and remarks stay where they are. Formatting the result again changes nothing.]

#ex("spec", "20-fmt.pnx")

#para("3")[`pnx export-c file.pnx [NAME]` loads the file as `pnx file.pnx` does but runs none of its code, and
writes the program's code as C to standard output: a file that defines the table `NAME` (`pnx_native` when left out)
for `pinax_native_attach` (19.11).]
