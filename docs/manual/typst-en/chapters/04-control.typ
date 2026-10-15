#import "/tools/common.typ": *

= Locals and flow <control>

#opening[Chapter 3 (signatures, named inputs).][
- When is a local made, and how far is it visible?
- How are `if`, `loop` and `case` written, and what must they keep about the stack?
- What is the short way to write a loop over an array?
]

== Locals

In a body, `to NAME` puts the top value into that name. The first time a name is used, a local of the value's type is
made; there is no separate line that declares it.

#ex("manual", "04-locals.pnx")

A local is visible from where it is first stored to the end of the branch, loop or block around it. A name first made
inside a branch cannot be read outside it: coming by the other branch, it would hold no value.

#ex("manual", "04-scope.pnx")

Inside a word, a global is stored with `global.` in front, as in `to global.total`; it is read by its name alone.

#ex("manual", "04-store-global.pnx")

#mistake[Storing into a global with `to total` inside a word][Inside a word, `to NAME` always stores into a local. When
the name is a global it is refused, and the message names `to global.total`. The rule keeps a store meant to make a
local from changing a global. Outside words (load code) no local is made, so `to total` stores into the global there.
#ex("manual", "04-store-global-bare.pnx")]

== Branches

`condition if … else … end` looks at the `bool` on top and runs one side. Both branches shall leave the same stack; a
branch that leaves with `return` does not need to match.

#ex("manual", "04-if.pnx")

An `if` without `else` shall leave the stack as it was. Leaving out the `else` of an `if` that leaves a value is
refused.

#ex("manual", "04-if-unequal.pnx")

== Loops

`loop … end` repeats. Inside it, `condition until` leaves when the condition is true, `condition while` when it is
false, and `break` without a condition. One turn shall leave the stack as it found it --- so the values a loop makes are
gathered in locals.

#ex("manual", "04-loop.pnx")

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

#summary[
- `to NAME` makes a local; with a global's name it stores into the global (standard 9.4).
- A name made inside a branch is not visible outside it (9.4).
- The two branches of `if` and the branches of `case` leave the same stack; one turn of a loop leaves the stack it found
  (10).
- A named loop is left at once with `until (NAME)` (10.2).
- Arrays and lists are walked with stems: `each` `each_index` `count` `fold` `sum` … (10.6).
]
