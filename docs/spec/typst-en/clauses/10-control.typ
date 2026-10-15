#import "/tools/common.typ": *

= Control <control>

#part("Syntax")
```ebnf
control = "if" { body } [ "else" { body } ] "end"
        | "loop" [ "(" NAME ")" ] { body } "end"
        | "case" { branch } [ "otherwise" { body } "end" ] "end" ;
branch  = values "of" { body } "end" ;
values  = case-value | "(" case-value { "," case-value } ")" ;
leave   = ( "while" | "until" | "break" ) [ "(" NAME ")" ] | "return" ;
block   = "do" [ "(" effect ")" ] { body } "end" ;
```

#para("1")[`end` closes the innermost open construct. The words that open one are `word` `test` `record` `do` `if`
`loop` `case` `of` `otherwise` `or_else`.]

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

#para("1")[`loop body end` repeats its body. Inside it, these words leave the loop:
- `condition while` --- leaves when the condition is false;
- `condition until` --- leaves when the condition is true;
- `break` --- leaves without a condition.
Each may be used any number of times, directly in the body of the loop or inside a branch in it (`if` `else` `case`
`or_else`). They leave the innermost loop.]

#para("2")[One turn shall leave the stack as it found it, and at every place where the loop is left the stack shall be
as it was when the loop was entered. A result of a loop is kept in a local.]

#ex("spec", "10-loop.pnx")

#para("3")[`loop (NAME)` names a loop. `while (NAME)`, `until (NAME)` and `break (NAME)` leave everything up to that
loop at once. The name is visible only inside the loop's body and is not a value. It follows the rule against hiding
(9.5).]

#ex("spec", "10-loop-named.pnx")

#para("4")[A loop with no `while`, `until` or `break` that leaves it never ends. A loop that never ends counts as a
leaving branch, so a word does not need results after it. An `until` of an inner loop does not count as leaving the
outer one, but an `until (outer name)` written in the inner loop does.]

#ex("spec", "10-forever.pnx")

#part("Constraints")

#para("5")[A turn that changes the stack is refused.]
#ex("spec", "10-loop-net.pnx")

#para("6")[`while`, `until` or `break` outside a loop is refused. They cannot leave an outer loop from inside a `do`
block either.]
#ex("spec", "10-while-outside.pnx")

#para("7")[Code right after a leaving word (`return` `fail` `break`) or a loop that never ends, in the same branch, can
never be reached and is refused.]
#ex("spec", "10-unreachable.pnx")

== Choosing <control-case>

#para("1")[`value case branches… end` takes the `int` or `str` on top, compares it with the values of the branches and
runs the body of the first equal one. The values of a branch are literals or `const`s of this source; several values
go in parentheses with commas. When no branch matches, the `otherwise` body runs. Every branch shall leave the same
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

#para("1")[`return` ends the word on the spot. The stack there shall match the results (9.2 (4)).]

#part("Constraints")

#para("2")[`return` is used only inside a definition (a word, a block value, a test). In code outside definitions it is
refused.]
#ex("spec", "10-return-outside.pnx")

== Blocks <control-blocks>

#para("1")[`do … end` is a #term("block"). A block is used in two ways; the form is the same, and what takes the block
decides.
- An #term("inline block") --- a block given directly to a combining word (10.6). It is expanded in place, so it can
  read and store the locals around it, and a `return` inside it leaves the word around it.
- A #term("block value") --- a block stored in a local, or given to a parameter of type `word (signature)`. It becomes a
  nameless word, called with `call`. It does not see the locals around it (it captures nothing); it sees globals.]

#para("2")[A signature may follow `do`; its names become locals of the block, as in a word definition. When what takes
the block fixes its effect, the signature may be left out.]

#ex("spec", "10-inline-locals.pnx")
#ex("spec", "10-value-block.pnx")

#part("Constraints")

#para("3")[A block value that uses a local from outside is refused (the name is not declared inside the block).]
#ex("spec", "10-value-block-outer.pnx")

#para("4")[A block expanded into a loop (`each`, `times` and so on) shall leave the stack as it found it.]
#ex("spec", "10-block-net.pnx")

== Stems and combining words <control-stems>

#para("1")[A #term("stem") yields values one after another. A stem is not a value: it cannot be stored, and the
combining word right after it shall take it. Together they make one loop. These make stems:
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Stem*], [*Elements*],
  [the name of an array or list], [the cells' values (rows, for records); a list up to its length. Queues, pools, buffers and indexes are not stems],
  [`lo hi range`], [the `int`s from `lo` up to but not including `hi`],
  [`s sep str.split`, `s str.lines`, `s str.words`, `s str.chars`], [`str` views (16.1); `chars` one UTF-8 character each],
  [`s str.bytes`], [`int`: the bytes (16.1)],
  [`s pat ptn.matches`, `s re rex.matches`, `s re rex.split`], [the parts matched (16.6, 16.7)],
  [`s csv.fields`], [the fields (16.9)],
)]

#para("2")[The combining words are below; the "block" column is what the block takes and gives.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Block*], [*Result*],
  [`each`], [`(T)`], [none],
  [`each_index`], [`(T, i int)`], [none],
  [`fold`], [`(acc U, T gives U)`], [`U`; the first value goes before the stem: `0 xs do … end fold`],
  [`count`], [`(T gives bool)`], [`int`],
  [`any` `all`], [`(T gives bool)`], [`bool`; stops early once the answer is known],
  [`first`], [`(T gives bool)`], [`T or fail`; `err.not_found` when none],
  [`sum`], [none], [`int` (of a stem of `dec(N)`, a `dec(N)`); an overflow is the fault `overflow`],
  [`minimum` `maximum`], [none], [`int or fail` (`dec(N)` likewise); `err.not_found` for an empty stem],
  [`map_into`], [`(T gives U)`], [a destination after it; `or fail`],
  [`filter_into`], [`(T gives bool)`], [a destination after it; `or fail`],
  [`n times`], [`()`], [none; no stem, `n` times],
)]

#ex("spec", "10-combinators.pnx")
#ex("spec", "10-first.pnx")

#para("3")[`map_into` and `filter_into` make no new storage; they write into the destination. A list destination is
pushed onto and nothing is given. An array destination is written from cell 0, and the number of cells written is
given (`gives n int or fail`). When the destination is full, they fail with `err.full`; what was written stays.]

#ex("spec", "10-map-into.pnx")

== Deferred code <control-defer>

#part("Syntax")
```ebnf
defer = "defer" { body } "end" ;
```

#para("1")[`defer CODE end` writes code that runs when the enclosing block is left. The block is a word's body, a
loop's body (at every turn) or the `do` block of a combining word (at every run). The code runs on every way out of that
block: the block's `end`, the end of a turn, `break`, a `while` or `until` that leaves the loop, `return`, `or_return`,
`or_fail` and `fail`. Several run in the reverse of the order they are written. They do not run when a fault ends the
run (the state closes the handles then, 19.6).]

#ex("spec", "10-defer.pnx")

#para("2")[When deferred code gives an owned value (`m release`, `f file.close`), the defer takes that value over. It
counts as given on every way out of the block, so no failure path needs code of its own to close it. Inside the block
the value can still be lent.]

#ex("spec", "10-defer-owned.pnx")

#part("Constraints")

#para("3")[`defer` is written right in a word's body, a loop's body or a `do` block. In a branch of `if`, `case` or
`or_else`, or outside a definition, it is refused.]
#ex("spec", "10-defer-branch.pnx")

#para("4")[Deferred code leaves the stack as it found it and leaves nothing on it. It cannot hold `return` `fail`
`break` `while` `until` `to` `set` `loop` `do` `case` `defer`, or `or_fail` `or_return`; a word that can fail is taken with
`or_else` inside it.]

#para("5")[Giving an owned value that a defer took over, inside the block, is refused.]
#ex("spec", "10-defer-taken.pnx")

#part("Diagnostics")
#diag(
  ([Two branches leave different stacks], "the two branches of this if leave different stacks"),
  ([A turn changes the stack], "one turn of a loop must leave the stack as it found it"),
  ([`while` `until` `break` outside a loop], "while, until and break belong inside a loop"),
  ([Unreachable code], "this code cannot be reached: the code before it always leaves"),
  ([The same value twice in a `case`], "this value is already in an earlier branch of the case"),
  ([An expanded block changes the stack], "this block must leave the stack as it found it"),
  ([`return` outside a definition], "return belongs inside a definition"),
  ([A branch value that is not a constant], "a case value is a literal or the name of a const"),
  ([A combining word on something that is not a stem], "only an array or a list is a stem (not a queue, a pool, a buffer or an index)"),
  ([`defer` in a branch], "defer is written right in a word's body, a loop's body or a do block, not in a branch"),
  ([`defer` outside a definition], "defer belongs inside a definition"),
  ([Deferred code changes the stack], "a defer leaves the stack as it found it"),
  ([A word that leaves, in deferred code], "a defer holds no return, fail, break, while, until, to, set, loop, do, case or defer"),
  ([`or_fail` `or_return` in deferred code], "a defer does not leave the word; a failure in it is taken with or_else"),
  ([Giving a value a defer took over], "a defer gives this owned value when the block is left; here it can only be lent"),
)
