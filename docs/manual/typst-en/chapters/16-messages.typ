#import "/tools/common.typ": *
// shape: a reference chapter: its examples are those of the chapters each row points to

= Messages you will meet <messages>

#opening[Chapters 1 to 15. A chapter to look things up in.][
- The processor refused my program. What does the message mean, and how is it fixed?
- The program stopped while running. What does the name of the fault mean?
]

A message has the form `file:line:column: reason`. Lines and columns count from 1, and the column is the byte position
in the line. The processor reports the first mistake it meets, so after a fix the next one may show. The messages below
are exactly what the processor prints; the chapters in the right column have examples that produce them.

== The stack and types

#table(columns: (1.3fr, 1fr, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Message*], [*Meaning and fix*], [*Chapter*]),
  [`an argument has another type than the word's signature asks for`],
  [A value given to a word has another type than its signature asks. Read the signature, and convert the value or fix
   the order.], [3],
  [`this word takes a str`, `this word takes an int`],
  [That word takes that one type. A number becomes text with `str.to_str`, text becomes a number with `str.to_int`.], [5],
  [`the two values compared must have the same type`],
  [A comparison such as `eq` or `lt` got two values of different types. Convert one; `int` and `dec` are different
   types too.], [2],
  [`two dec values of different scales: rescale one first (d 2 rescale)`],
  [Decimals with different numbers of digits after the point, like `4.20` and `0.5`. `rescale` one, or write both with
   the same digits.], [2],
  [`a declared type is added to and subtracted from its own type, and multiplied, divided or taken modulo by an int; for other arithmetic take raw`],
  [Arithmetic a declared type (such as `cents`) does not allow. Take the integer out with `raw` and compute with it.], [10],
  [`the two branches of this if leave different stacks`],
  [The two branches of an `if` leave different counts or types. Make both leave the same.], [4],
  [`this block must leave the stack as it found it`],
  [The block of a combining word such as `each` left a value. Use it inside the block, or `drop` it.], [4],
  [`stored text is str(N); a str view lives only in locals, arguments and results`],
  [A `str` was put where a global, a cell or a field lives. Places that live long take `str(N)`.], [5],
  [`the stack does not hold enough values for this word`],
  [The word takes more values than the stack holds. Check that the values were put on first, and in the right order.
   `pnx see` shows the stack after each line.], [2, 15],
  [`values are left on the stack at the end of the source`],
  [A value that nothing took is left when the program ends. Print it, `drop` it, or add the word that was forgotten.], [2],
  [`the stack does not hold exactly the results of the signature here`],
  [What is left when the body of a word ends differs from the results of its signature. Write the results into the
   signature, or clear what is left.], [3],
  [`this word takes two int values, or two of the same declared type`],
  [An arithmetic word got another type. Text becomes a number with `str.to_int`, a number becomes text with
   `str.to_str`.], [2],
  [`an if without else must leave the stack as it found it`],
  [An `if` without `else` leaves a value. Add an `else` branch that leaves a value of the same type.], [4],
  [`one turn of a loop must leave the stack as it found it`],
  [One turn of a loop leaves or takes a value. Keep what it makes in a local with `to NAME`.], [4],
  [`inputs are all named or all bare types, not a mix`],
  [A signature mixes named inputs and inputs given as types only. Name them all.], [3],
)

== Characters and literals

#table(columns: (1.3fr, 1fr, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Message*], [*Meaning and fix*], [*Chapter*]),
  [`this character is not allowed outside literals and remarks`],
  [A symbol such as `+`, `;` or `{` is in the code. Arithmetic and separation are all words: `add`, `mul`, a line
   break.], [1],
  [`a sign is attached to its number; arithmetic uses words (add, sub)`],
  [Written like `3 - 1`. A `-` is only the sign of a number; write `3 1 sub`.], [2],
  [`unknown escape; a raw string r"..." keeps backslashes as written`],
  [An ordinary string holds an escape such as `\d`. Patterns go into raw strings `r"…"`.], [12],
  [`rx takes a string literal or a const name; a pattern made while running is rex.compile`],
  [A variable was given to `rex.rx`. A pattern is a literal or a `const`; one made while running is `rex.compile`.], [12],
)

== Names

#table(columns: (1.3fr, 1fr, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Message*], [*Meaning and fix*], [*Chapter*]),
  [`no module of this name is registered or loaded`],
  [The module of `use` is not there. Check the names of the standard and host modules; for a file module, that
   `NAME.pnx` lies beside the program.], [10],
  [`the module exports nothing of this name`],
  [The module has no such name, or did not `export` it. Add `export` in the module file.], [10],
  [`this name is already defined`],
  [A word or a declaration of that name exists already. Use another name; in a session, `redefine word` replaces it.], [3, 15],
  [`this parameter takes one row of a table; a parameter that takes the whole table is written list RECORD or array RECORD`],
  [A table's name as a parameter type means one row. To take the whole table write `list RECORD` or `array RECORD`.], [6],
  [`the collection given here is of another kind, or holds another type, than the parameter says`],
  [The collection given differs from the parameter in kind or element type: an `array int` or a `list str(8)` cannot
   stand where `list int` is asked for.], [6],
  [`this name is not declared before this place`],
  [The name does not exist, or is used before it is declared. Check the spelling and where it is declared; words that
   call each other use `forward word`.], [1, 3],
  [`this path names neither a module in use nor a record`],
  [The module of `module.word` was not opened. Write `use module` at the top of the file.], [1, 10],
  [`this name is already visible here; a new name may not hide it`],
  [A visible name of the program (a word, a global, a constant, a local outside) was used for a new name. Choose
   another.], [3],
  [`NAME is a word of the language; choose another name`],
  [A word of the language --- a reserved word, a core word such as `sum`, `count` or `first`, a word such as `sort`, a
   type --- was used for a new name, wherever the name was made. Choose another.], [3],
  [`rows do not move: sort a list of their numbers by a field: order table sort_by record.field`],
  [`sort` was given a table of records. Fill a list with the row numbers and sort it with `sort_by`.], [7],
  [`outside a definition `+"`to`"+` makes no new local; a global is written with set`],
  [`to` was used outside a definition, where there are no locals. Put the work into a word, or store into a global with
   `set`.], [4],
  [`this name is a global; a global is written with set: value set name`],
  [A global, or a cell of a global collection, was stored into with `to`. Write `value set name`; a cell is
   `value i set name`.], [4, 6],
)

== Failures and owned values

#table(columns: (1.3fr, 1fr, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Message*], [*Meaning and fix*], [*Chapter*]),
  [`an owned value in a local is not given on every path (close, free or store it)`],
  [An owned value (an open file, a row of a pool, a mark) is not handed on along some path. Look at the ways out on
   failure and at `return` too; `defer` makes this easy.], [9],
  [`this local gave its owned value away; it holds nothing to read`],
  [The name was used again after its value was handed on. A closed file cannot be used.], [9],
  [`a defer gives this owned value when the block is left; here it can only be lent`],
  [A value left to `defer` was handed on (closed) in the body as well. Leave the closing to `defer` and only lend
   the value in the body.], [9],
  [`this str was made after a mark that was released since; the region may hold other bytes there now`],
  [Text made between a mark and its `release` was read after the release. Text to keep goes into `str(N)` or a buffer
   before the release.], [5, 9],
  [``or_fail passes the failure on, so this word needs `or fail` in its signature``],
  [The word using `or_fail` has no `or fail` in its signature. Add it, or handle the failure there with `or_else`.], [8],
  [`this list's rows are kept by a field, a global, a named row or an index; it cannot be truncated`],
  [A list whose rows are held by something cannot be truncated. Check before adding instead.], [11],
  [`a word that can fail is followed by or_fail, or_return or or_else`],
  [A word that can fail (`push`, `str.to_int`, a word with `or fail` in its signature) has no guard after it. Even
   to ignore the failure, write `or_else (e) end`.], [6, 8],
  [`the body of or_else must leave what the word would have given, or leave the word or the loop`],
  [The body of `or_else` leaves other values than success would. Leave a stand-in of the same type, or leave with
   `return` or `break`.], [8],
  [`an owned value is kept in a local or given on at once; this word would lose it`],
  [An `owned` value is about to be lost with `drop`, or left on the stack. Keep it in a local and close it at the end
   (`defer` makes it easy).], [9],
  [`a field holds str(N); a str view lives only in locals, arguments and results`],
  [A view `str` was used for a field or a global. Declare it `str(N)`.], [5],
)

== When a run stops: faults

A program that loaded can still stop with a #strong[fault] while it runs. `pnx` shows the reason, the line and the
locals of the word.

#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Fault*], [*What `pnx` says*], [*Meaning*]),
  [`overflow`], [`integer overflow`], [An integer went beyond 64 bits (chapter 2). The `chk_` words give it as a failure.],
  [`div_zero`], [`division by zero`], [Division by 0.],
  [`range`], [`index out of range`], [A cell number outside the length, or a field of the "none" row (chapters 6, 7).],
  [`full`], [`a declared capacity is used up`], [The region or the room of a stored text ran out (chapter 5).],
  [`depth`], [`call depth limit`], [Calls went too deep. Look for a recursion that does not end (chapter 3).],
  [`stale`], [`a row or handle was used after it was given back`], [A pool row given back, or a closed handle, was used
   (chapters 7, 9).],
  [`deadlock`], [`deadlock: waiting for a queue that nothing can change`], [Every task waits for a queue (chapter 13).],
)

#qa[Can a fault be caught inside the script?][No. A fault points at a place where the program has to be fixed. Where
something can go wrong because of data from outside, use a word that gives a failure instead (`chk_mul`, `str.to_int`
and so on) and handle it with `or_else` (chapter 8).]

#summary[
- A refusal reports the first mistake met, with `file:line:column` (standard 4).
- Mistakes with the stack, names, failures and `owned` values all show before the program runs (standard 4).
- A fault is a mistake that can only be known while running, and it stops the run (11.3).
]
