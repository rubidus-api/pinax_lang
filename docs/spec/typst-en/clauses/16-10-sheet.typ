#import "/tools/common.typ": *

== `sheet` --- a table of rows and columns <std-sheet>

#para("1")[The module `sheet` keeps a table of rows and columns in a buffer (8.7) the program declares. It is a library,
not a type: the buffer is formatted by `init`, and every word of `sheet` takes the buffer, written last, as a collection
parameter (9.9) does. A cell holds nothing, an `int`, a `bool`, a `dec(N)` or a `str`, whatever the program puts there.
The kind of a cell is known only to the module: a value leaves a cell through the word of its type (`int`, `str`, ...),
so every word has a fixed signature and the stack never holds a value of no type.]

#para("2")[Rows and columns are counted from 0. A row is added at the end by `add_row` and the table holds at most the
`Rmax` rows `init` was given. The number of columns is fixed by `init`; `widen` changes it by rebuilding the table in the
same buffer. A value put in a cell is copied. Equal values are kept once: a number that does not fit a cell, and every
text, go to a pool with a count of the cells that hold them, and a cell that is overwritten, cleared or cut off gives its
value back. A pool that has no room is `err.full`; free text space that is scattered is gathered when a new text needs it
(and by `compact`). `sheet` is for a table that is read and written by index and by what it holds. What is found by a
name is kept in an index (8.10).]

=== Making and growing

#para("3")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`init`], [`(rmax int, cols int, b buffer or fail)`], [formats the buffer as a table of at most `rmax` rows and exactly `cols` columns, with no rows; `err.full` when the buffer is too small. The buffer then reads as full: nothing can be appended over a table],
  [`rows` `cols` `rows_max`], [`(b buffer gives int)`], [the rows now, the columns, the most rows],
  [`add_row`], [`(b buffer gives int or fail)`], [adds an empty row and gives its number; `err.full` at `rmax`],
  [`truncate_rows`], [`(n int, b buffer)`], [keeps the first `n` rows and gives back what the others held; more than there are is the fault `range`],
  [`widen`], [`(cols int, b buffer or fail)`], [rebuilds the table with `cols` columns, keeping the rows and the values. More columns need room in the buffer (`err.full`); fewer columns only when the cut columns are all empty (`err.format`)],
  [`clear` `compact`], [`(b buffer)`], [removes every row; gathers the free space of the texts],
)]

=== Cells

#para("4")[Words that take a row and a column fault with `range` when the cell is outside the table. A buffer that is not a table
is the same fault.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`put_int` `put_bool`], [`(v int, row int, col int, b buffer or fail)` `(v bool, row int, col int, b buffer or fail)`], [puts a copy in the cell; `err.full` when a pool is full],
  [`put_dec`], [`(v dec(N), row int, col int, b buffer or fail)`], [puts a decimal with its scale],
  [`put_str`], [`(s str, row int, col int, b buffer or fail)`], [puts a copy of the text; a view of this table may be given],
  [`clear_cell`], [`(row int, col int, b buffer)`], [makes the cell empty],
  [`copy_cell`], [`(row1 int, col1 int, row2 int, col2 int, b buffer)`], [copies a cell to another, sharing the value],
  [`int` `bool`], [`(row int, col int, b buffer gives int or fail)` `(... gives bool or fail)`], [the value; `err.format` when the cell holds another kind],
  [`dec`], [`(row int, col int, N int, b buffer gives dec(N) or fail)`], [a decimal with the scale `N` (a constant 0 to 18); `err.format` when the cell holds another kind or scale],
  [`str`], [`(row int, col int, b buffer gives str or fail)`], [a view of the text in the table, good until the next write to it; `err.format` for another kind],
  [`kind` `is_empty`], [`(row int, col int, b buffer gives int)` `(... gives bool)`], [the kind (`sheet.k_empty` 0, `k_int` 1, `k_bool` 2, `k_dec` 3, `k_str` 4); is it empty],
)]

=== Finding and choosing

#para("5")[A search compares numbers as numbers and texts by what they say. It looks at one column going down, or at one
row going across. A text that is in no cell is found to be so at once.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`find_row_int` `find_row_str`], [`(key int, col int, b buffer gives int or fail)` `(key str, col int, ...)`], [the first row whose column `col` holds `key`; `err.not_found` otherwise. This is `MATCH` of a spreadsheet; the value is then read with `int`, `str`, ... (there is no word that gives a value of no type, as `VLOOKUP` does)],
  [`find_col_int` `find_col_str`], [`(key int, row int, b buffer gives int or fail)`], [the first column of row `row`, across],
  [`find_row_from_int` `find_row_from_str` `find_col_from_int` `find_col_from_str`], [`(key, line int, from int, b buffer gives int or fail)`], [the same, starting at row or column `from`],
)]

#para("6")[A table keeps a #term("selection"): a list of rows, at first all of them. `where_*` starts again from all the rows and
keeps those whose column holds a value in the relation `op` with `v` (`sheet.eq` `ne` `lt` `le` `gt` `ge`); `narrow_*` keeps,
of the rows now selected, those that are. Cells of another kind, and empty ones, are never in the relation. Because each
word works on the selection of the table, a line of them is a pipe. The selection is one place of the buffer and is
lost with it.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`where_int` `narrow_int`], [`(col int, op int, v int, b buffer)`], [selects, or narrows, by a number],
  [`where_str` `narrow_str`], [`(col int, op int, v str, b buffer)`], [selects, or narrows, by a text (byte order)],
  [`select_all`], [`(b buffer)`], [selects every row],
  [`sel_len`], [`(b buffer gives int)`], [how many rows are selected],
  [`sel_row`], [`(k int, b buffer gives int)`], [the row number of the `k`-th selected row; out of range is the fault `range`],
  [`order_by` `order_by_desc`], [`(col int, b buffer or fail)`], [puts the selected rows in order of column `col`, smallest (largest) first, stably; numbers, then texts, then the rest. Room for the numbers is taken from the free end of the buffer (`err.full`)],
)]

#para("7")[The totals look at the int cells only. `sum_*` is the fault `overflow` when the sum does not fit; `min_*` and `max_*`
fail with `err.not_found` when there is no number.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`sum_col` `min_col` `max_col` `count_col`], [`(col int, b buffer gives int)`], [over column `col`, all rows; `min` and `max` `or fail`],
  [`sum_row` `min_row` `max_row` `count_row`], [`(row int, b buffer gives int)`], [over row `row`, all columns],
  [`sum_sel` `min_sel` `max_sel` `count_sel`], [`(col int, b buffer gives int)`], [over column `col` of the selected rows],
)]

#ex("spec", "16-sheet.pnx")
#ex("spec", "16-sheet-pipe.pnx")

#part("Constraints")

#para("8")[The last argument is a buffer; the others are of the types of the signatures above.]
#ex("spec", "16-sheet-args.pnx")

#part("Faults")

#para("9")[A row or a column outside the table, a buffer that was not formatted by `init` (or was changed since), and a sum
that does not fit are faults.]
#ex("spec", "16-sheet-range.pnx")

#part("Diagnostics")
#diag(
  ([A value of another type for a word of `sheet`], "this word of sheet takes int for row, column and number, str for text, bool for put_bool, dec(N) for put_dec"),
  ([A word of `sheet` without a buffer last], "the words of sheet take a buffer last: v row column grid sheet.put_int"),
)
