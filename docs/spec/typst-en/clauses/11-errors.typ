#import "/tools/common.typ": *

= Failures and faults <errors>

#para("1")[Things go wrong in two ways. A *failure* is something expected to happen now and then: text that does not
read as a number, a full list, a missing key. The caller handles it on the spot. A *fault* means the program is wrong:
an overflow, a division by zero, an index out of range. It ends the run and is reported to the host. A script has no
way to catch a fault.]

== Error codes <errors-codes>

#para("1")[Failures and faults have an #term("error code"), of type `err`. Codes are named by constants written after
`err.` (no `use` needed). The word `err_text` gives the name of a code as a `str`.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Constant*], [*Name*], [*Meaning*],
  [`err.failed`], [`failed`], [a failure in general; a false `assert`],
  [`err.overflow`], [`overflow`], [an integer overflowed],
  [`err.div_zero`], [`div_zero`], [division by zero],
  [`err.range`], [`range`], [an index out of range; taking from an empty queue or list],
  [`err.full`], [`full`], [a capacity is used up],
  [`err.depth`], [`depth`], [the call depth limit],
  [`err.stale`], [`stale`], [a row or handle used after it was given back],
  [`err.cancelled`], [`cancelled`], [the task was cancelled (clause 14)],
  [`err.interrupted`], [`interrupted`], [the host cut off what was waited for (clause 19)],
  [`err.deadlock`], [`deadlock`], [every task left waits for a queue that nothing can change (14.3)],
  [`err.format`], [`format`], [text or bytes of the wrong form],
  [`err.not_found`], [`not_found`], [what was looked for is not there],
)
The values are those of `pinax_err` in the host interface (19.1). Host modules may add codes of their own.]

#ex("spec", "11-errors.pnx")

== Failures <errors-failure>

#part("Syntax")
```ebnf
guard = "or_fail" | "or_return" | "or_else" [ "(" NAME ")" ] { body } "end" ;
```

#para("1")[A word whose signature ends in `or fail` can fail. When it fails it has no results, only an error code.]

#para("2")[The token right after a call of a word that can fail shall be a #term("guard"):
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Guard*], [*When the call fails*],
  [`or_fail`], [this word fails too, with the same code; its signature shall have `or fail`],
  [`or_return`], [this word ends; the stack there shall match its results],
  [`or_else … end`], [the body makes the results in place of the call; `or_else (e) … end` takes the code into the local `e`; the body may also leave with `return`, `fail` or `break`],
)
When the call succeeds the guard does nothing and the results stay on the stack.]

#para("3")[`code fail` makes this word fail with that code. Its signature shall have `or fail`.]

#ex("spec", "11-failure.pnx")
#ex("spec", "11-guards.pnx")

#para("4")[To receive overflow and division by zero as failures instead of faults, use `chk_add` `chk_sub` `chk_mul`
`chk_div` (15.1).]

#ex("spec", "11-chk.pnx")

#part("Constraints")

#para("5")[A word that can fail with no guard after it is refused: a failure cannot be passed over unnoticed.]
#ex("spec", "11-guard-missing.pnx")

#para("6")[`or_fail` and `fail` are used only in words whose signature has `or fail`. Code outside definitions counts as
having no `or fail`.]
#ex("spec", "11-or-fail-needs.pnx")
#ex("spec", "11-fail-needs.pnx")

#para("7")[The body of `or_else` shall leave what the word would have given on success, or leave.]
#ex("spec", "11-or-else-stack.pnx")

#para("8")[On the paths of failures too, `owned` values shall be handed over (12.4). A processor checks this at every
`or_fail` and `or_return`.]

== Faults <errors-fault>

#para("1")[A fault ends the run on the spot. It is the return value of the host function that started the run
(`pinax_run`, `pinax_load` and so on). A fault inside a task also ends the whole run and every task (14.4). The state
remains and takes the next run.]

#para("2")[Faults come from:
- arithmetic overflow (`overflow`) and division by zero (`div_zero`) --- 7.1, 15.1;
- an index out of range of an array or list, a `case` with no matching branch, the none row, text longer than its
  `str(N)` (`range`) --- 8, 10.3;
- a value that does not fit a narrow field (`overflow`) --- 8.8;
- a region or task space too small (`full`) --- 12.2;
- the call depth (`depth`) --- 9.6;
- a row or handle given back (`stale`) --- 8.9, 19.6;
- a false `assert` (`failed`).]

#para("3")[What a run that ends with a fault made in the region is rewound. The handles that run opened are closed by
the processor, so on a path of faults `owned` values need no closing by hand. Values already stored in globals, and
what host words have already done, stay.]

#para("4")[A conforming processor reports the kind of a fault and its line. `pnx` also shows the locals of the word the
fault happened in.]

#ex("spec", "11-div-zero.pnx")
#ex("spec", "11-assert.pnx")

#part("Diagnostics")
#diag(
  ([No guard after a word that can fail], "a word that can fail is followed by or_fail, or_return or or_else"),
  ([`or_fail` in a word without `or fail`], "or_fail passes the failure on, so this word needs `or fail` in its signature"),
  ([`fail` in a word without `or fail`], "fail needs `or fail` in the signature of this word"),
  ([The body of `or_else` leaves other values], "the body of or_else must leave what the word would have given, or leave the word or the loop"),
)
