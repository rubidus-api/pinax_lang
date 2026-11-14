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

One `push` and one `pop`, followed on the stack. Both can fail, so a guard follows each.

#trace(
  ("70", "70", [The value to put in.]),
  ("scores push", "—", [Puts it at the end. Had the list been full, it would fail and leave nothing.]),
  ([`or_else (e) end`], "—", [Skipped, since it succeeded. On failure the body runs; it is empty, so nothing is left.]),
  ("scores pop", "90", [Takes the last value off.]),
  ([`or_else (e) -1 end`], "90", [Success: skipped. On failure `-1` would fill the place.]),
  ("str.to_str io.println", "—", [Prints it.]),
)

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

Because they can fail, `push` and `pop` are always followed by a guard (chapter 8); leaving it out is refused at
load. To ignore the failure, write `or_else (e) end`, so that the source says it is ignored.

#ex("manual", "06-push-unguarded.pnx")

#qa[Isn't a generous capacity a waste of memory?][It is. But the amount is in the source and fixed at load. Instead of
a program stopping when memory runs out, it shows as a failing `push`, and that failure has to be handled before the
program is even loaded.]

== Queues

In a `queue`, what goes in first comes out first. `put` puts in, `take` takes out, and `peek` looks without taking. `wait_take` and `wait_put`, which wait between tasks for an empty or full queue, are in chapter 13.

#ex("manual", "06-queue.pnx")

#mistake[A queue can be walked with `each` too][Only arrays and lists are stems, and a pool for `each` alone (chapter 7).
Queues, buffers and indexes are not. To handle the elements of a queue in order, call `take` in a loop.
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

#qa[How do I empty a list, or make it shorter?][`LIST clear` sets its length to 0 and `n LIST truncate` to `n`. The
cells keep their values, but past the length they are not seen. An array is always full, so it cannot be emptied;
its cells are written one by one with `set`.]

== A table of rows and columns

When a table of rows and columns is wanted that holds a number in one cell and a text in the next, as a spreadsheet
does, the module `sheet` keeps it in a buffer. It is a library; it is not a type. The buffer is made a #strong[book] by
`open_book`, and `create` makes named sheets in it (at most so many rows, exactly so many columns). A sheet is chosen by name
(`activate`) or by number (`activate_n`), and every other word then works on the chosen #strong[current sheet]: the shape of connecting
to a service, choosing one thing in it and asking. A row is added at a time, and every word of `sheet` takes the buffer last.
A cell comes out through the word of its kind: `int`, `str`, `bool`, `dec`. Equal texts and big numbers are stored once,
however many cells hold them and in however many sheets.

Searching and choosing work on a #strong[selection], the rows chosen so far. `where_str` and `where_int` start it from all
the rows; `narrow_int` and `narrow_str` keep, of the rows selected, those that are still right. A line of such words is
the pipe, and `order_by`, `sum_sel` and `sel_row` read what is left.

#ex("manual", "06-sheet.pnx")

With several sheets `find_sheet`, `sheet_name`, `copy_to`, `remove` and `rename` help, and a column that is searched often, or
chosen by range, can be given an ordered index (a B+tree) by `index`. With one, `find_row_*`, `where_*` and `order_by`
answer without scanning, and the answers are the same. A write costs a little more.

#ex("manual", "06-sheet-book.pnx")

Spreadsheets and CSV files come in and go out through two buffers: the file, then the book. `load_csv` and `load_xlsx` add the file's rows
to the current sheet (`mode` 0 turns numbers into numbers, and in an xlsx a date into Unix seconds; mode 1 keeps the text, or the serial number; mode 2 is mode 0 with a date kind, `put_date` and `date`, for the dates of an xlsx and for CSV fields such as `2024-02-29` or `2024-02-29T08:30:00`, and a date is written back as a date),
and `save_csv` and `save_xlsx` write the sheet into a buffer, which the program then writes to a file. The words give the number of rows
or bytes. They have no power over files, because the program moves the bytes itself (`file.read_all_into`, `file.write`), and nothing is
allocated: an xlsx is read in the spare room after the file's bytes in its buffer, so declare that buffer larger than the file.

#ex("manual", "06-sheet-files.pnx")

Columns do not grow: `widen` rebuilds the table, in the same buffer, with another number of columns. A cell of another
kind than the word asks for fails with `err.format`; ask `kind` first when it can be so.

#summary[
- A word takes a collection as `list T`, `array T`, `queue T` or `buffer` (9.9).
- The capacity of a collection is fixed by its declaration; no storage grows while running (standard 8.1).
- An array is always full; a list's length grows; an index out of range is a fault (8.4, 8.5).
- `push` `pop` `put` `take` can fail: `full` when full, `range` when empty (8.5, 8.6).
- Only arrays and lists are stems (10.6).
- `map_into` and `filter_into` write into the destination they are given (10.6).
- `sheet` keeps rows and columns of any kind of cell in a buffer; rows grow, columns are fixed by `create`; several sheets in one book, ordered indexes, and CSV and xlsx files in and out through two buffers (standard 16.10).
]
