#import "/tools/common.typ": *

= Collections <collections>

#opening[Chapter 4 (stems and blocks), chapter 5 (buffers).][
- What places hold several values, and how is their size decided?
- What happens when you add to a full list or take from an empty queue?
- How do I filter or transform an array into another place?
]

Every collection has its capacity fixed by its declaration, and its room is placed in the state's block ahead of time.
No storage grows while running. So "how much can it hold" is always written in the source.

== Arrays

`array NAME type capacity` is a row of cells that is always full. Every cell starts at zero, and `len` is always the
capacity. A cell is read with `i ARRAY idx` and stored with `value i set ARRAY` (the same `set` that stores into a global). `assign` fills it from the first cell,
outside definitions.

#ex("manual", "06-array.pnx")

An index out of range is a fault. Compare an index you cannot be sure of with `len` before using it.

#ex("manual", "06-index-range.pnx")

== Lists

A `list` is an array whose length grows from 0 up to its capacity. `push` adds at the end and `pop` takes from the
end. Only indexes below the length may be used.

#ex("manual", "06-list.pnx")

Here are the words for lists in one place, with remarks saying what each line does.

#ex("manual", "06-list-steps.pnx")

#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Written*], [*Stack*], [*What it does*]),
  [`LIST len`], [`— → int`], [the length now],
  [`LIST capacity`], [`— → int`], [the declared capacity],
  [`i LIST idx`], [`int → value`], [reads cell `i`; outside the length the fault `range`],
  [`v i set LIST`], [`value int → —`], [stores into cell `i`; outside the length the fault `range`],
  [`v LIST push`], [`value → —`], [adds at the end; fails with `full` when full],
  [`LIST pop`], [`— → value`], [takes from the end; fails with `range` when empty],
)

`push` and `pop` can fail: a `push` into a full list fails with `full`, a `pop` from an empty list with `range`. A
`push` that fails does not change the list.

#ex("manual", "06-list-full.pnx")

Because they can fail, `push` and `pop` are always followed by a guard word (chapter 8); leaving it out is refused at
load. To ignore the failure, write `or_else (e) end`, so that the source says it is ignored.

#ex("manual", "06-push-unguarded.pnx")

#qa[Isn't a generous capacity a waste of memory?][It is. But the amount is in the source and fixed at load. Instead of
a program stopping when memory runs out, it shows as a failing `push`, and that failure has to be handled before the
program is even loaded.]

== Queues

In a `queue`, what goes in first comes out first. `put` puts in, `take` takes out, and `peek` looks without taking. `wait_take` and `wait_put`, which wait between tasks for an empty or full queue, are in chapter 13.

#ex("manual", "06-queue.pnx")

#mistake[A queue can be walked with `each` too][Only arrays and lists are stems. Queues, pools, buffers and indexes are
not. To handle the elements of a queue in order, call `take` in a loop.
#ex("manual", "06-queue-stem.pnx")]

== Filtering and mapping into a place

`map_into` and `filter_into` put the results of transforming or filtering the elements of a stem with a block into a
#strong[place that already exists]; no new array is made. A list destination is added to at the end; an array is written
from cell 0, and the number of cells written is given. When the destination is full, they fail.

#ex("manual", "06-map-filter.pnx")

== Passing a collection to a word

A word can take a collection as a parameter. Write its kind and what its cells hold --- `list int`, `array city`,
`queue int`, `list str(8)` --- or `buffer`, and give the collection's name where you call the word. Inside, the parameter
works wherever a collection's name does (even as the destination of `map_into`). The same word now serves every list of that kind, whatever its capacity.

#ex("manual", "06-coll-param.pnx")

#mistake[A table's name as a parameter type takes the table][Written as a parameter type, the name of a table of records
means #emph[one row] of it (chapter 7). To take the whole table, write `list RECORD` or `array RECORD`.
#ex("manual", "06-coll-param-row.pnx")]

#summary[
- A word takes a collection as `list T`, `array T`, `queue T` or `buffer` (9.9).
- The capacity of a collection is fixed by its declaration; no storage grows while running (standard 8.1).
- An array is always full; a list's length grows; an index out of range is a fault (8.4, 8.5).
- `push` `pop` `put` `take` can fail: `full` when full, `range` when empty (8.5, 8.6).
- Only arrays and lists are stems (10.6).
- `map_into` and `filter_into` write into the destination they are given (10.6).
]
