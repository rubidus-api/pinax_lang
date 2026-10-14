#import "/tools/common.typ": *

= Failures and faults <errors>

#opening[Chapter 3 (`or fail` in signatures), chapter 6 (a failing `push`).][
- What does Pinax do when something goes wrong? With no exceptions, how is it handled?
- Can a failure be passed over unhandled?
- What is a fault, and how does it differ from a failure?
]

== Two kinds of wrong

Pinax tells two kinds of wrong apart.
- A #strong[failure] is something expected now and then: text that is not a number, a full list, a missing key. A word
  that can fail has `or fail` in its signature, and the caller handles it #strong[right there].
- A #strong[fault] means the program is wrong: an overflow, a division by zero, an index out of range. The run stops on
  the spot, and the host receives it. A script has no way to catch a fault.

With no exceptions, an error never skips several words and gets caught somewhere unexpected.

== Three words that handle a failure

Right after a word that can fail comes one #strong[guard]:
- `or_fail` --- this word fails too, for the same reason; its signature needs `or fail` as well;
- `or_return` --- this word ends quietly;
- `or_else … end` --- makes a value in its place; `or_else (e) … end` takes the reason.

#ex("manual", "08-parse.pnx")

`parse` can fail in three places and passes each on with `or_fail`. Its own rule (0 to 100) fails with
`err.range fail`. `report` takes the failure with `or_else (e)`, reports it with its reason, and skips the line.

#ex("manual", "08-or-return.pnx")

#ex("manual", "08-guard-missing.pnx")

#mistake[Using `or_fail` at the outermost level of a file][Code outside definitions counts as having no `or fail`
signature, so neither `or_fail` nor `or_return` nor `return` can be used there. Put the work into a word, and take its
failure with `or_else` at the outer level.
#ex("manual", "08-load-code-or-fail.pnx")]

== Faults

#ex("manual", "08-fault.pnx")

`pnx` shows the kind of the fault, its line and the locals of the word. What a run that ends with a fault made in the
region is rewound, and handles such as files it opened are closed by the processor.

Where a fault could happen and you want a failure instead, use the `chk_` words.

#ex("manual", "08-chk.pnx")

`assert` makes a fault when it is false. It writes down a promise the caller must keep.

#ex("manual", "08-assert.pnx")

#qa[How do I decide what is a failure and what is a fault?][What happens because of data from outside (user input,
files, the network) is a failure. What goes away only by fixing the program (a miscomputed index, a broken promise) is
a fault.]

#summary[
- After a word that can fail comes `or_fail`, `or_return` or `or_else` (standard 11.2).
- `or_fail` and `fail` only inside an `or fail` signature; outside definitions only `or_else` (11.2).
- A fault stops the run, and the processor closes the handles the run opened (11.3).
- To receive overflow and division by zero as failures, use the `chk_` words (15.1).
]
