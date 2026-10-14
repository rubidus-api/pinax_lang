#import "/tools/common.typ": *

= Owned values and the region <owned>

#opening[Chapter 5 (the region, `mark`/`release`), chapter 7 (rows of pools), chapter 8 (failures).][
- How does the processor make sure that something like an open file gets closed?
- How does it know a file is closed on the paths of failures too?
- How does a loop that makes much text spare the region?
]

== Values that must be handed over

The signature of `file.open_read` is `(path str gives owned file or fail)`. `owned` means the value is an #strong[owned
value]. Whoever receives it shall #strong[hand it over] exactly once on every path: give it to a word that takes it
`owned` (`file.close`), store it in a global, or return it as an `owned` result. It can be #strong[lent] any number of
times to words that take it without `owned` (`file.write`, `file.read_line`).

#ex("manual", "09-copy-file.pnx")

`copy_lines` has two files and four ways out. When the second file does not open, it closes the first and fails. When
writing fails, it closes both and fails. The processor follows all these paths at load, and refuses if any one of them
does not close.

#ex("manual", "09-forgot-close.pnx")

This word leaves through `or_fail` when `read_line` fails, and on that path it does not close `f`; it does not close it
on the path of success either. The processor points at where `f` was made.

#ex("manual", "09-close-twice.pnx")

#qa[What happens to open files on a fault?][The handles a run that ends with a fault opened are closed by the processor.
So the `owned` check looks only at paths that are not faults.]

#mistake[Keeping a `dup` of it is handy][An `owned` value cannot be doubled with `dup` or dropped with `drop`. Doubled,
it would be closed twice; dropped, never. Put it into a local and lend it by name.]

== Sparing the region

`cat` writes new text into the region. The region has a fixed size, so a loop that keeps making text gives the region
back in every turn with `mark` and `release`. It gives it back on the way out through a failure too --- what `mark` gives
is an owned value as well.

#ex("manual", "09-region-loop.pnx")

Bytes appended to a buffer are copied into it, so they survive giving the region back.

#summary[
- An `owned` value is handed over exactly once on every path; lending is allowed any number of times (standard 12.4).
- It is handed over on the paths of failures (`or_fail` `or_return`) and at `return` too (11.2).
- `owned` values cannot be `dup`ed or `drop`ped (12.4).
- On a fault the processor closes the handles (11.3).
- Loops give the region back with `mark`/`release` (12.2).
]
