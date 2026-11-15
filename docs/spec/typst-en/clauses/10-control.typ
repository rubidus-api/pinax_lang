#import "/tools/common.typ": *

= Control <control>

#part("Syntax")
```ebnf
control = "if" { body } [ "else" { body } ] "end"
        | "loop" { body } "end"
        | "match" { branch } [ "otherwise" { body } "end" ] "end" ;
branch  = "when" case-value { case-value } "do" { body } "end" ;
leave   = ( "break" | "continue" ) [ INT ] "." | "return" ;
block   = "do" { "to" NAME } { body } "end"
        | "def" effect "do" { body } "end"       (* a block value, 10.5 *)
        | "ref" NAME ;
```

#para("1")[`end` closes the innermost open construct. The words that open one are `def` `forward` `extern` `test` `record` `do` `if`
`loop` `match` `when` `otherwise` `or_else` `defer`. (`forward` and `extern` are closed by the `end` after their signature, and so is `type NAME proc … end`.) (A `text` literal and a `note` remark close on their own tag, 6.3,
6.5.)]

#para("2")[At load a processor compares the stacks of all paths. Where paths meet again, the stack shall be the same
whichever path was taken. A #term("leaving branch") --- one that ends with `return`, `fail`, `break` or a loop that
never ends --- is not compared.]

== Branches <control-if>

#para("1")[`condition if body end` takes the `bool` on top and runs the body when it is true. `condition if body else
body end` runs the `else` body when it is false.]

#para("2")[The body of an `if` without `else` shall leave the stack as it found it. With `else`, the two bodies shall
leave the same stack.]

#ex("spec", "10-if.pnx")

#part("Constraints")

#para("3")[Two branches that leave different stacks are refused.]
#ex("spec", "10-if-unequal.pnx")

== Loops <control-loop>

#para("1")[`loop body end` repeats its body. Inside it, these statements leave the loop:
- `break .` --- leaves the loop and goes on after its `end`;
- `continue .` --- skips the rest of the turn and starts the next one;
- `break N .` and `continue N .` --- do it to the `N`th loop counted from the innermost (`N` is an integer literal of 1 or more, 1 when left out); the loops in between are left at once.
A condition is written with `if`: `x 0 eq if break . end`. A statement ends with a period. Each may be used any number of times, directly in the body of the loop or inside a branch in it (`if` `else` `match` `or_else`).]

#para("2")[One turn shall leave the stack as it found it, and at every place where the loop is left the stack shall be
as it was when the loop was entered. A result of a loop is kept in a local.]

#ex("spec", "10-loop.pnx")

#para("3")[A loop has no name. To leave an outer loop, count: `break 2 .` leaves two loops at once.]

#ex("spec", "10-loop-named.pnx")

#para("4")[A loop with no `break` that leaves it never ends. A loop that never ends counts as a
leaving branch, so a procedure does not need results after it. A `break .` of an inner loop does not count as leaving the
outer one, but a `break 2 .` written in the inner loop does.]

#ex("spec", "10-forever.pnx")

#part("Constraints")

#para("5")[A turn that changes the stack is refused.]
#ex("spec", "10-loop-net.pnx")

#para("6")[`break` or `continue` outside a loop is refused. They cannot leave an outer loop from inside a `do`
block either; in the block of `each`, `each_index` or `times` they leave that block (10.6 (4)).]
#ex("spec", "10-while-outside.pnx")

#para("7")[Code right after a leaving word (`return` `fail` `break` `continue`) or a loop that never ends, in the same branch, can
never be reached and is refused.]
#ex("spec", "10-unreachable.pnx")

== Choosing <control-match>

#para("1")[`value match when values… do body end … otherwise body end end` takes the `int`, narrow integer (7.6), `err` or `str` on top,
compares it with the values of the branches and runs the body of the first equal one. A branch starts with `when`. The values of a branch are
literals, `const`s of this source, error codes (`err.range`), or a constant or failure code another module exports
(`module.NAME`, 8.14); several values stand after `when` one after another (no commas, no parentheses), and the `do` after them opens the body. When no branch matches, the `otherwise` body runs. Every branch shall leave the same
stack.]

#ex("spec", "10-case.pnx")

#part("Constraints")

#para("2")[The same value in two branches is refused.]
#ex("spec", "10-case-twice.pnx")

#para("3")[A constant of a module (a path such as `json.key` or `err.full`) cannot be a branch value; compare such values
with `eq`.]
#ex("spec", "10-case-path.pnx")

#part("Faults")

#para("4")[With no `otherwise` and no matching branch, the fault is `range`.]
#ex("spec", "10-case-nomatch.pnx")

== Returning <control-return>

#para("1")[`return` ends the procedure on the spot. The stack there shall match the results (9.2 (4)).]

#part("Constraints")

#para("2")[`return` is used only inside a definition (a procedure, a block value, a test). In code outside definitions it is
refused.]
#ex("spec", "10-return-outside.pnx")

== Blocks <control-blocks>

#para("1")[`do … end` is a #term("block"). A block is given to a combining word only (there is no `do … end` standing by
itself); a block value is written with `def` (`def signature do … end`, (2a)). A block is used in two ways.
- An #term("inline block") --- a block given directly to a combining word (10.6). It is expanded in place, so it can
  read and store the locals around it, and a `return` inside it leaves the word around it.
- A #term("block value") --- `def signature do … end` stored in a local, or given to a parameter of a `proc … end` type (7.4). It becomes a
  nameless procedure, called with `call`. It does not see the locals around it (it captures nothing); it sees globals.]

#para("2")[An inline block takes the values the rail yields (and the number of `each_index`) into locals with `to NAME`, written one after another
right after `do`. The names are taken from the top down: in `do to i to s … end each_index` `s` is the element and `i` the number (the element is
put first, so `i` is on top). The signature of a block value is that of a procedure definition, and its names become locals.]

#ex("spec", "10-inline-locals.pnx")
#ex("spec", "10-value-block.pnx")

#para("2a")[`ref NAME` gives a procedure defined with `def`, of this source or exported by a module in use (`ref by_len`, `ref geo.area`),
as a value: its type is `proc` with the procedure's signature, and `call` calls it. Any other word is given in a block with the block's
signature; inputs given as types only stay on the stack (9.2 (2)), so the block's body is the word alone:
`def str, str gives bool do str.starts_with end`.]

#ex("spec", "10-word-value.pnx")

#part("Constraints")

#para("3")[A block value that uses a local from outside is refused (the name is not declared inside the block).]
#ex("spec", "10-value-block-outer.pnx")

#para("4")[A block expanded into a loop (`each`, `times` and so on) shall leave the stack as it found it.]
#ex("spec", "10-block-net.pnx")

#para("4a")[A block value with no signature takes nothing, so a word in it that needs values is refused, and the message
shows both forms of (2a).]
#ex("spec", "10-word-value-bare.pnx")

#para("4b")[A procedure declared with `forward` is given as a value only after its definition.]
#ex("spec", "10-word-value-forward.pnx")

== Rails and combining words <control-rails>

#para("1")[A #term("rail") yields values one after another. A rail is not a value: it cannot be stored, and the
combining word right after it shall take it. Together they make one loop. These make rails:
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Rail*], [*Elements*],
  [the name of an array or list], [the cells' values (rows, for records); a list up to its length. Queues, buffers and indexes are not rails],
  [the name of a pool], [its live rows, in the order of their slots; for `each` only (8.9 (3a))],
  [`lo hi range`], [the `int`s from `lo` up to but not including `hi`],
  [`s sep str.split`, `s str.lines`, `s str.words`, `s str.chars`], [`str` views (16.1); `chars` one UTF-8 character each],
  [`s str.bytes`], [`u8`: the bytes (16.1)],
  [`s pat ptn.matches`, `s re rex.matches`, `s re rex.split`], [the parts matched (16.6, 16.7)],
  [`s csv.fields`], [the fields (16.9)],
)]

#para("2")[The combining words are below; the "block" column is what the block takes and gives.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Block*], [*Result*],
  [`each`], [`(T)`], [none],
  [`each_index`], [`(T, i int)`], [none],
  [`fold`], [`(acc U, T gives U)`], [`U`; the first value goes before the rail: `0 xs do … end fold`],
  [`count`], [`(T gives bool)`], [`int`],
  [`any` `all`], [`(T gives bool)`], [`bool`; stops early once the answer is known],
  [`first`], [`(T gives bool)`], [`T or fail`; `err.not_found` when none],
  [`sum`], [none], [`int` (of a rail of `dec N`, a `dec N`); an overflow is the fault `overflow`],
  [`minimum` `maximum`], [none], [`int or fail` (`dec N` likewise); `err.not_found` for an empty rail],
  [`map_into`], [`(T gives U)`], [a destination after it; `or fail`],
  [`filter_into`], [`(T gives bool)`], [a destination after it; `or fail`],
  [`n times`], [`()`], [none; no rail, `n` times],
)]

#para("1a")[An #term("adapter") takes a rail and gives a rail. `skip N` drops the first N elements, `head N` keeps the first N, `step N`
takes every N-th element (the first one first), and `rev` walks from the last to the first. They apply in the order written and
however many there are, the loop is one: `scores skip 2 head 3` is the third to the fifth, `scores head 3 skip 1` the second and the third.
`N` is code that leaves one `int`; it ends at a comma, at `do`, at the next adapter and before `sum`, `minimum` or `maximum`. It is worked out once, before the loop.
A negative `N` (a `step` of less than 1) is the fault `range`. Adapters are for rails of known length (`range`, an array, a list, `str.bytes`);
`step` comes last; a rail takes four of them. `each_index` does not count along an adapted rail yet. `each`, `fold`, `count`, `any`, `all`,
`first`, `map_into`, `filter_into`, `sum`, `minimum` and `maximum` take them.]
#ex("spec", "10-rail-adapters.pnx")
#ex("spec", "10-rail-adapter-step.pnx")

#para("1b")[`filter`, `map`, `skip_while` and `head_while` take the block written right before them (`do to s s 4 gt end filter`).
The block takes the element and gives a `bool` (`filter` keeps the elements for which it is true, `skip_while` drops them while it is
true, `head_while` ends the walk at the first for which it is false), or, for `map`, a value of the same type that takes the element's place. They run
inside the loop, once for each element and in the order written, and never for an element an earlier one dropped; so there is one loop, no storage and no
call. A block adapter works on any rail, of known length or not (`str.words`, `str.lines`, ...). `skip`, `head`, `step` and `rev` come before them in a
chain. The name the block gives the element may be the one the final block uses.]
#ex("spec", "10-rail-block-adapters.pnx")
#ex("spec", "10-rail-block-late.pnx")

#para("1c")[`xs ys zip` walks two arrays or lists side by side, up to the shorter. A pass gives the pair, and the block of `each` binds
two names, the last pushed first: `do to y to x … end each`, with `x` from `xs` and `y` from `ys`. Their cells are `int`, `bool`, `err` or `dec N`. Only `each` takes a
zipped pair, and neither adapters nor ranges are zipped yet.]
#ex("spec", "10-rail-zip.pnx")
#ex("spec", "10-rail-zip-range.pnx")

#ex("spec", "10-combinators.pnx")
#ex("spec", "10-first.pnx")

#para("3")[`map_into` and `filter_into` make no new storage; they write into the destination. A list destination is
pushed onto and nothing is given. An array destination is written from cell 0, and the number of cells written is
given (`gives n int or fail`). When the destination is full, they fail with `err.full`; what was written stays. The
destination may be a collection parameter (9.9).]

#ex("spec", "10-map-into.pnx")

#para("4")[A rail and its combining word make one loop, and in the block of `each`, `each_index` or `times`, `break .`
leaves it and `continue .` goes to the next element: the rail stops and the code after the combining word runs. The rules of leaving a loop hold: the
stack is as it was when the block began (10.2 (2)), and the block's deferred code runs (10.7). A `break .` in a `loop` inside the
block leaves that loop. `break N .` and `continue N .` count such blocks and `loop`s alike as levels (10.2 (1)); the operands of the inner
blocks leave with them. The blocks of the other combining words, which give a result made from every element, are not left early,
and are not crossed.]
#ex("spec", "10-leave-each.pnx")
#ex("spec", "10-leave-two.pnx")

== Deferred code <control-defer>

#part("Syntax")
```ebnf
defer = "defer" { body } "end" ;
```

#para("1")[`defer CODE end` writes code that runs when the enclosing block is left. The block is a word's body, a
loop's body (at every turn) or the `do` block of a combining word (at every run). The code runs on every way out of that
block: the block's `end`, the end of a turn, `break`, `continue`, `return`, `or_return`,
`or_fail` and `fail`. Several run in the reverse of the order they are written. They do not run when a fault ends the
run (the state closes the handles then, 19.6).]

#ex("spec", "10-defer.pnx")

#para("2")[When deferred code gives an owned value (`m release`, `f file.close`), the defer takes that value over. It
counts as given on every way out of the block, so no failure path needs code of its own to close it. Inside the block
the value can still be lent.]

#ex("spec", "10-defer-owned.pnx")

#part("Constraints")

#para("3")[`defer` is written right in a word's body, a loop's body or a `do` block. In a branch of `if`, `match` or
`or_else`, or outside a definition, it is refused.]
#ex("spec", "10-defer-branch.pnx")

#para("4")[Deferred code leaves the stack as it found it and leaves nothing on it. It cannot hold `return` `fail`
`break` `continue` `to` `set` `loop` `do` `match` `defer`, or `or_fail` `or_return`; a word that can fail is taken with
`or_else` inside it.]

#para("5")[Giving an owned value that a defer took over, inside the block, is refused.]
#ex("spec", "10-defer-taken.pnx")

#part("Diagnostics")
#diag(
  ([Two branches leave different stacks], "the two branches of this if leave different stacks"),
  ([A turn changes the stack], "one turn of a loop must leave the stack as it found it"),
  ([`break` `continue` outside a loop], "break and continue belong inside a loop"),
  ([`break` in the block of a word that gives a result], "only the block of each, each_index or times is left early; this word gives a result made from every element"),
  ([`break N .` to a loop outside a `do` block], "a named loop outside this block is not left from inside it; break leaves the block"),
  ([Unreachable code], "this code cannot be reached: the code before it always leaves"),
  ([The same value twice in a `match`], "this value is already in an earlier branch of the case"),
  ([An expanded block changes the stack], "this block must leave the stack as it found it"),
  ([`return` outside a definition], "return belongs inside a definition"),
  ([A branch value that is not a constant], "a case value is a literal or the name of a const"),
  ([A combining word on something that is not a rail], "only an array, a list or a pool is a rail (not a queue, a buffer or an index)"),
  ([`defer` in a branch], "defer is written right in a word's body, a loop's body or a do block, not in a branch"),
  ([`defer` outside a definition], "defer belongs inside a definition"),
  ([Deferred code changes the stack], "a defer leaves the stack as it found it"),
  ([A word that leaves, in deferred code], "a defer holds no return, fail, break, continue, to, set, loop, do, match or defer"),
  ([`or_fail` `or_return` in deferred code], "a defer does not leave the word; a failure in it is taken with or_else"),
  ([Giving a value a defer took over], "a defer gives this owned value when the block is left; here it can only be lent"),
  ([Too few values in a block value with no signature], "a block without a signature takes nothing; a procedure of the program is given as ref NAME, any other word with the block's signature: def str, str gives bool do str.starts_with end"),
  ([A forward procedure as a value before its definition], "a word declared with forward is given as a value only after its definition"),
)

== Ends of statements <control-ends>

#para("1")[A comma or a dot written in code (not inside a signature or an `args … .`)
ends a #term("statement"): the code since the last comma or dot. Neither changes the stack, and neither is required:
code without them means what it meant before.]

#para("2")[A comma waits for nothing: the effects of the statement may still be under way when the next one starts.]

#para("3")[A dot is a #term("barrier"): every effect begun before it is finished before anything after it. The
processor's part is to call the function the host gave with `pinax_on_barrier` (19.4 (3)); the host's part is to
write what its host words hold back (standard output, a file's writes) and to wait for what they started in the
background. The end of the code of a load and the end of a session line are a dot, also when they end with a fault.
In a source loaded as data the two mark statements and call nothing.]

#para("4")[An effect whose result the statement needs (a read, a command's output) is done when it is reached. Between
two dots, effects on the same resource keep the order of the program; effects on different resources may overlap.
This edition runs every effect in the order of the program; the overlap is the freedom a later edition may use.]

#ex("spec", "10-statements.pnx")
