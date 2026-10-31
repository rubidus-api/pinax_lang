#import "/tools/common.typ": *

= Locals and flow <control>

#opening[Chapter 3 (signatures, named inputs).][
- When is a local made, and how far is it visible?
- How are `if`, `loop` and `case` written, and what must they keep about the stack?
- What is the short way to write a loop over an array?
- How is a word given as a value?
]

== Locals

In a body, `to NAME` puts the top value into that name. The first time a name is used, a local of the value's type is
made; there is no separate line that declares it.

#ex("manual", "04-locals.pnx")

`to` #strong[takes] the top value off the stack and keeps it in the name. Using the name puts the value back on. Instead
of holding values on the stack for long, keep them in names and take them out when needed.

#trace(
  ("a b add", "12", [`a` is 3, `b` is 9.]),
  ("c add", "18", [`c` is 6.]),
  ("to total", "—", [Takes 18 off and keeps it in a new local `total`. The stack is empty.]),
  ("total 3 div", "6", [Using `total` puts 18 on; it is divided by 3.]),
  ("to mean", "—", [Keeps 6 in `mean`.]),
)

A local is visible from where it is first stored to the end of the branch, loop or block around it. A name first made
inside a branch cannot be read outside it: coming by the other branch, it would hold no value.

#ex("manual", "04-scope.pnx")

A global is stored with `set`, not `to`; `to` stores only into locals.

#ex("manual", "04-store-global.pnx")

#mistake[Storing into a global with `to total`][`to` with the name of a global is refused, and the message names `set`.
A global cannot be hidden, so no local of that name can be made; the rule also keeps a `to` meant to make a local from
changing a global silently.
#ex("manual", "04-store-global-bare.pnx")]

== Branches

`condition if … else … end` looks at the `bool` on top and runs one side. Both branches shall leave the same stack; a
branch that leaves with `return` does not need to match.

#ex("manual", "04-if.pnx")

`if` #strong[takes] the `bool` off the top and chooses the way. The two branches have to leave the same stack because the
code after `end` does not know which branch it came from. Whichever way it came, the stack must be the same for the
processor to check what follows.

#ex("manual", "04-sign.pnx")

#trace(
  ("n 0 lt", "true", [With `n` at -4 the comparison leaves `true`.]),
  ("if", "—", [Takes the `bool` off and goes to the first branch.]),
  ("\"negative\"", "\"negative\"", [This branch leaves one `str`.]),
  ("end", "\"negative\"", [The `else` branch also leaves one `str`, so the two ways agree.]),
)

An `if` without `else` shall leave the stack as it was. Leaving out the `else` of an `if` that leaves a value is
refused.

#ex("manual", "04-if-unequal.pnx")

== Loops

`loop … end` repeats. Inside it, `condition until` leaves when the condition is true, `condition while` when it is
false, and `break` without a condition. One turn shall leave the stack as it found it --- so the values a loop makes are
gathered in locals.

#ex("manual", "04-loop.pnx")

One turn leaves the stack as it found it for the same reason. The head of the loop sees the stack both when the loop
is first entered and when a turn comes back, and both must be the same. If each turn piled up one more value, the
processor, which cannot know how many turns there will be, could not fix the depth of the stack. So the values a loop
makes are gathered in locals.

#ex("manual", "04-loop-sum.pnx")
#ex("manual", "04-loop-leaves.pnx")

`count_up` makes a value with `i 1 add` every turn, and nothing takes it. With `to i` after it the value goes into `i`
and the stack is as it was at the start of the turn.

A named loop can be left all at once from an inner loop.

#ex("manual", "04-named-loop.pnx")

#qa[Can I give the number of loops to leave instead?][No. With a number, wrapping a loop in one more would quietly leave
another loop. A name cannot do that.]

== Choosing

`case` compares an `int` or `str` with the values of its branches. Several values for one branch go in parentheses.
What to do when nothing matches goes in `otherwise`; with no `otherwise` and no match, it is a fault.

#ex("manual", "04-case.pnx")

The values of the branches are literals or `const`s of this source. Constants of modules such as `err.full` or
`json.key` cannot be `case` values; compare them with `eq`.

== Stems and blocks

A loop over an array or a list is written with a #strong[stem] and a #strong[combining word] instead. The name of the
array is the stem, and a `do … end` block after it, with a word such as `each`, `count` or `fold`, takes it. The block
is expanded in place, so it can use the locals around it.

#ex("manual", "04-stems.pnx")

`lo hi range` is a stem too (`hi` is not included). `fold` takes a first value before the stem, and its block takes
(the value so far, an element) and gives the new value.

== Words as values

A block that is not given to a combining word is a #strong[value]: it can be kept in a local, returned, or given to a
parameter of type `word (signature)`, and `call` calls it. Such a block sees no locals around it. To give a word of
the program as a value, write `do NAME end` with no signature: the value is that word, with its signature.

#ex("manual", "04-word-values.pnx")

#qa[How is a word of the language, such as `str.starts_with`, given as a value?][With the block's signature written
as types only: `do (str, str gives bool) str.starts_with end`. Inputs written as types stay on the stack, so the word
takes them as it is. Without a signature the block takes nothing, and the message shows both forms.]

#mistake[A `case` branch can hold the word itself: `"prefix" of str.starts_with end`][A branch runs its words; it
does not make a value of them. There `str.starts_with` is called, and finds too few values on the stack. Make the
value with a block: `"prefix" of do (str, str gives bool) str.starts_with end end`, or `do NAME end` for a word of the
program.
#ex("manual", "04-word-values-branch.pnx")]

#qa[What if the two branches of `if` should leave values of different types?][They cannot. The code after `if` does not
know which branch ran, so both leave the same number of values of the same types. When one branch has a value and the
other has not, that is a failure (chapter 8) or a row asked with `is_none` (chapter 7).]

#summary[
- `to NAME` stores into a local, `set NAME` into a global (standard 9.4).
- A name made inside a branch is not visible outside it (9.4).
- `to` takes the top value off into a name; using the name puts it back on (9.4).
- The two branches of `if` and the branches of `case` leave the same stack; one turn of a loop leaves the stack it found
  (10).
- A named loop is left at once with `until (NAME)` (10.2).
- Arrays and lists are walked with stems: `each` `each_index` `count` `fold` `sum` … (10.6).
- `do NAME end` is the word `NAME` as a value; any other word is given as `do (signature) word end` (10.5).
]
