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
capacity. A cell is read with `i ARRAY idx` and stored with `value i to ARRAY`. `assign` fills it from the first cell,
outside definitions.

#ex("manual", "06-array.pnx")

An index out of range is a fault. Compare an index you cannot be sure of with `len` before using it.

#ex("manual", "06-index-range.pnx")

== Lists

A `list` is an array whose length grows from 0 up to its capacity. `push` adds at the end and `pop` takes from the
end. Only indexes below the length may be used.

#ex("manual", "06-list.pnx")

`push` and `pop` can fail: a `push` into a full list fails with `full`, a `pop` from an empty list with `range`. A
`push` that fails does not change the list.

#ex("manual", "06-list-full.pnx")

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

#summary[
- The capacity of a collection is fixed by its declaration; no storage grows while running (standard 8.1).
- An array is always full; a list's length grows; an index out of range is a fault (8.4, 8.5).
- `push` `pop` `put` `take` can fail: `full` when full, `range` when empty (8.5, 8.6).
- Only arrays and lists are stems (10.6).
- `map_into` and `filter_into` write into the destination they are given (10.6).
]
