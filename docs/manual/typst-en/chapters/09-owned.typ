#import "/tools/common.typ": *

= Owned values and the region <owned>

#opening[Chapter 5 (the region, `mark`/`release`), chapter 7 (rows of pools), chapter 8 (failures).][
- How does the processor make sure that something like an open file gets closed?
- How does it know a file is closed on the paths of failures too?
- How does a loop that makes much text spare the region?
- How can the closing code be written once instead of on every way out?
]

== Values that must be handed over

The signature of `file.open_read` is `(path str gives owned file or fail)`. `owned` means the value is an #strong[owned
value]. Whoever receives it shall #strong[hand it over] exactly once on every path: give it to a word that takes it
`owned` (`file.close`), store it in a global, or return it as an `owned` result. It can be #strong[lent] any number of
times to words that take it without `owned` (`file.write`, `file.read_line`).

Side by side, the two uses compare like this.

#table(columns: (auto, 1fr, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([], [*Lending*], [*Giving*]),
  [The receiver's signature], [`(f file …)`: no `owned`], [`(f owned file …)`, or an `owned` result],
  [How often], [any number of times], [exactly once on every path],
  [Afterwards], [still usable], [no longer usable],
  [Examples], [`f file.write`, `f file.read_line`], [`f file.close`, storing into a global, returning it],
)

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
it would be closed twice; dropped, never. Put it into a local and lend it by name.
#ex("manual", "09-drop-owned.pnx")]

== Sparing the region

`cat` writes new text into the region. The region has a fixed size, so a loop that keeps making text gives the region
back in every turn with `mark` and `release`. It gives it back on the way out through a failure too --- what `mark` gives
is an owned value as well.

#ex("manual", "09-region-loop.pnx")

One turn, followed. The mark that `mark` gives is an owned value too, so the way out on failure hands it to
`release` as well.

#trace(
  ("mark", "m", [Puts the region's current place on as a mark: an owned value.]),
  ("to m", "—", [Names it.]),
  ("cat (…)", "\"row 1: 1\\n\"", [New text goes into the region.]),
  ("report append", "—", [Copies it into the buffer. Fails when the buffer is full.]),
  ([`or_else (e) m release e fail end`], "—", [The way of failure: gives the region back and fails for the same reason. On success, skipped.]),
  ("m release", "—", [The region goes back to the mark; the next turn reuses the bytes `cat` wrote.]),
)

Bytes appended to a buffer are copied into it, so they survive giving the region back.

== Deferring: `defer`

`copy_lines` repeated, at each place that can fail, code that closes everything opened so far. `defer CODE end` lets
that code be written once. It runs on every way out of its block (a word's body, one turn of a loop, a `do` block):
at the end, through `or_fail`, through `return` or `break`. Several run in the reverse of the order they are written.

#ex("manual", "09-copy-defer.pnx")

After `defer src file.close end`, the defer takes `src` over. The body only lends it, as in `src file.read_line`, and
the defer closes it on the way out. The closing code in `or_else` goes away and `or_fail` can be used as it is. A mark of
the region works the same way.

#ex("manual", "09-region-defer.pnx")

#mistake[Closing at the end as well, after writing `defer`][Giving away a value that a defer took over is refused: it
would be closed twice on the way out. Leave the closing to the defer.
#ex("manual", "09-defer-close.pnx")]

`defer` is written right in a word's body, a loop's body or a `do` block; not inside a branch of `if`. Deferred code
leaves the stack as it found it, and a word that can fail inside it is taken with `or_else` (standard 10.7).

#qa[Who closes an `owned` value I put into a global?][Nobody. Storing into a global is a handing over too, so the check
ends there and the rest is the program's business. Write a word that takes it out of the global and closes it, and
call it before the run ends. Only the handles of a run that ended with a fault are closed by the processor.]

#summary[
- An `owned` value is handed over exactly once on every path; lending is allowed any number of times (standard 12.4).
- It is handed over on the paths of failures (`or_fail` `or_return`) and at `return` too (11.2).
- `defer CODE end` runs on every way out of a block; the value it takes over is only lent in the body (10.7).
- `owned` values cannot be `dup`ed or `drop`ped (12.4).
- On a fault the processor closes the handles (11.3).
- Loops give the region back with `mark`/`release` (12.2).
]
