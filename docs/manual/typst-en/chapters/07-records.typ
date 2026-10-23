#import "/tools/common.typ": *

= Records, pools, indexes <records>

#opening[Chapter 6 (collections), chapter 5 (stored text).][
- How are data with several fields held?
- How are linked data such as trees and lists built without pointers?
- How do I find a row by its name?
]

== Records and rows

`record … end` is a sequence of fields, each with a type (`u8` `u16` `i32` `int` `bool` `str(N)` …): a `u8` field reads
as a `u8` and takes a `u8` only (`2016 u16 b to book.year`). Records do not
travel as values. You declare a list (or array, or pool) of records and handle its #strong[rows]. A row's type is named
after its table.

#ex("manual", "07-records.pnx")

Fields are written `RECORD.FIELD`: `b book.year` reads, `2016 lua to book.year` stores. `row` adds a row to a list
outside definitions, and can name it. Adding rows while running is `add_row`.

#ex("manual", "07-add-row.pnx")

Here are the words for rows together, with remarks. The `r` that `add_row` gives is a number inside the table; a field
is stored with `value row to record.field`, and read with `row record.field`, which takes one row off and puts the
field's value on.

#ex("manual", "07-row-steps.pnx")

#trace(
  ("cart add_row", "1", [Adds a row at the end and puts its number on (it can fail: `or_fail` follows).]),
  ("to r", "—", [Keeps the number in `r`.]),
  ("n r", "\"eraser\" 1", [The value to store, and the row.]),
  ("to item.name", "—", [Stores into the `name` field of row 1; both come off.]),
  ("r item.price", "80", [Takes one row off and puts its `price` on.]),
)

#qa[Is a row the address of a record?][No. A row is a #strong[number] within its table, and its range is checked on
every use. So holding the wrong row cannot damage memory; at worst it stops with a fault.]

== Linking by rows

A field whose type is the name of a table holds a row of that table. It starts as "none", asked with `is_none`. That is
how a tree is built without pointers. Since the record refers to the table that holds it, the table is announced first
with `forward`.

#ex("manual", "07-tree.pnx")

Reading or storing a field of the "none" row is a fault. When following rows, ask `is_none` first (as the first line
of `show` above does).

#ex("manual", "07-none-field.pnx")

== Pools: taking and giving back

A `pool` is a table whose rows are taken (`alloc`) and given back (`free`). The row `alloc` gives is an #strong[owned
value]: on every path it shall be stored into a field or global, or given to `free`, exactly once. The processor checks
this at load.

#ex("manual", "07-pool-leak.pnx")

A row of a pool carries a generation besides its number. Using a row that was given back --- even when someone has
taken that slot again since --- stops with a fault.

#ex("manual", "07-stale.pnx")

== Indexes

`index NAME TABLE RECORD.FIELD` is a hash table that finds rows by one field. `lookup` finds the row of a key (failing
with `not_found` when there is none). Rows written with `row` are in it from the start.

#ex("manual", "07-index.pnx")

#mistake[A row added with `add_row` is in the index too][It is not. A row added while running is found by `lookup` only
after `insert`, as `register` above does. When the key is there already, `insert` fails and the row added with `add_row`
stays in the list --- so it is cleaner to `lookup` the key first and add after.]

#qa[Can a row of one table go into another table?][No. The type of a row is the name of its table. A row of `cart`
is used with `cart` only; given to a field or a list of another table it is refused as the wrong type. To move a row
between two tables of the same record, read the fields one by one into a new row, or copy with `filter_into`
(chapter 6).]

#summary[
- Records are handled as rows of tables; fields are read and stored as `RECORD.FIELD` (standard 8.8).
- A row is a number checked on every use; a field of the none row is a fault (8.8).
- A row of a pool is handed over once; a row given back is a fault (8.9, 12.4).
- An index holds the rows of `row`; rows added while running are `insert`ed (8.10).
]
