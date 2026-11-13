#import "/tools/common.typ": *

== `sheet` --- a book of tables of rows and columns <std-sheet>

#para("1")[The module `sheet` keeps tables of rows and columns, called #term("sheets"), in a buffer (8.7) the program declares:
the buffer holds a #term("book"). It is a library, not a type, and it works as an adapter does: the program opens the book,
makes sheets in it, chooses one by name or by number, and every other word then works on that #term("current sheet"). Every word of
`sheet` takes the buffer, written last, as a collection parameter (9.9) does. A cell holds nothing, an `int`, a `bool`, a
`dec(N)` or a `str`, whatever the program puts there. The kind of a cell is known only to the module: a value leaves a cell
through the word of its type (`int`, `str`, ...), so every word has a fixed signature and the stack never holds a value of no
type. How the cells, the pools and the indexes lie in the buffer is not part of the interface.]

#para("2")[Rows, columns and sheets are counted from 0. A row is added at the end by `add_row` and a sheet holds at most the
`rmax` rows `create` was given. The number of columns is fixed by `create`; `widen` changes it. A value put in a cell is
copied. Equal values are kept once, in a pool the whole book shares: a number that does not fit a cell, and every text, go
to the pool with a count of the cells that hold them (in any sheet), and a cell that is overwritten, cleared or cut off gives
its value back. A pool that has no room is `err.full`; free text space that is scattered is gathered when a new text needs it
(and by `compact`). `sheet` is for tables that are read and written by index and by what they hold. What is found by a name is
kept in an index (8.10).]

=== Books and sheets

#para("3")[A word other than the ones of this group needs a current sheet: without one it is the fault `state`. The first sheet
made in a book is the current one. A name is 1 to 255 bytes and is kept once in the book. A sheet number is the order in which
it was made; the number of a dropped sheet is given again to the next sheet made.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`open_book`], [`(nmax int, b buffer or fail)`], [formats the buffer as a book of at most `nmax` (1 to 256) sheets, with none made; `err.full` when the buffer is too small. The buffer then reads as full: nothing can be appended over a book],
  [`create` `create_cols`], [`(name str, rmax int, cols int, b buffer gives int or fail)`], [makes a sheet of at most `rmax` rows and exactly `cols` columns and gives its number. `create_cols` stores the sheet column by column: a scan of one column is several times faster, reading a whole row is slower. `err.format` when the name is taken; `err.full` when there is no room or number left],
  [`activate` `activate_n`], [`(name str, b buffer or fail)` `(n int, b buffer or fail)`], [makes the sheet of that name, or that number, the current one; `err.not_found` when there is none. A read-only book cannot change its current sheet],
  [`current` `sheets`], [`(b buffer gives int)`], [the number of the current sheet (-1 when there is none); one more than the highest number in use],
  [`sheet_name`], [`(n int, b buffer gives str or fail)`], [a view of the name of sheet `n`, good until the next write to the book; `err.not_found` for a number not in use],
  [`find_sheet`], [`(name str, b buffer gives int or fail)`], [the number of the sheet of that name; `err.not_found` otherwise],
  [`remove`], [`(name str, b buffer or fail)`], [removes a sheet with its indexes and gives back what its cells held. When it was the current sheet there is none afterwards],
  [`rename`], [`(n int, name str, b buffer or fail)`], [gives sheet `n` another name; `err.format` when the name is taken],
  [`copy_to`], [`(row1 int, col1 int, n int, row2 int, col2 int, b buffer or fail)`], [copies a cell of the current sheet into a cell of sheet `n`, sharing the value; `err.not_found` when there is no sheet `n`],
)]

=== Making and growing

#para("4")[The words of this group work on the current sheet.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`rows` `cols` `rows_max`], [`(b buffer gives int)`], [the rows now, the columns, the most rows],
  [`add_row`], [`(b buffer gives int or fail)`], [adds an empty row and gives its number; `err.full` at `rmax`],
  [`truncate_rows`], [`(n int, b buffer)`], [keeps the first `n` rows and gives back what the others held; more than there are is the fault `range`],
  [`widen`], [`(cols int, b buffer or fail)`], [makes the sheet again with `cols` columns, keeping the rows, the values and the indexes. More columns need room in the book (`err.full`); fewer columns only when the cut columns are all empty (`err.format`). The old room is left as a hole that `compact` closes],
  [`clear`], [`(b buffer)`], [removes every row],
  [`compact`], [`(b buffer)`], [gathers the free space of the texts and closes the holes between the sheets],
)]

=== Cells

#para("5")[Words that take a row and a column fault with `range` when the cell is outside the table. A buffer that is not a table
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

#para("6")[A search compares numbers as numbers and texts by what they say. It looks at one column going down, or at one
row going across. A text that is in no cell is found to be so at once.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`find_row_int` `find_row_str`], [`(key int, col int, b buffer gives int or fail)` `(key str, col int, ...)`], [the first row whose column `col` holds `key`; `err.not_found` otherwise. This is `MATCH` of a spreadsheet; the value is then read with `int`, `str`, ... (there is no word that gives a value of no type, as `VLOOKUP` does)],
  [`find_col_int` `find_col_str`], [`(key int, row int, b buffer gives int or fail)`], [the first column of row `row`, across],
  [`find_row_from_int` `find_row_from_str` `find_col_from_int` `find_col_from_str`], [`(key, line int, from int, b buffer gives int or fail)`], [the same, starting at row or column `from`],
)]

#para("7")[A sheet keeps a #term("selection"): a list of rows, at first all of them. `where_*` starts again from all the rows and
keeps those whose column holds a value in the relation `op` with `v` (`sheet.eq` `ne` `lt` `le` `gt` `ge`); `narrow_*` keeps,
of the rows now selected, those that are. Cells of another kind, and empty ones, are never in the relation. Because each
word works on the selection of the sheet, a line of them is a pipe. The selection is kept in the book, per sheet.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`where_int` `narrow_int`], [`(col int, op int, v int, b buffer)`], [selects, or narrows, by a number],
  [`where_str` `narrow_str`], [`(col int, op int, v str, b buffer)`], [selects, or narrows, by a text (byte order)],
  [`select_all`], [`(b buffer)`], [selects every row],
  [`sel_len`], [`(b buffer gives int)`], [how many rows are selected],
  [`sel_row`], [`(k int, b buffer gives int)`], [the row number of the `k`-th selected row; out of range is the fault `range`],
  [`order_by` `order_by_desc`], [`(col int, b buffer or fail)`], [puts the selected rows in order of column `col`, smallest (largest) first, stably; numbers, then texts, then the rest. Room for the numbers is taken from the free end of the buffer (`err.full`)],
)]

#para("8")[The totals look at the int cells only. `sum_*` is the fault `overflow` when the sum does not fit; `min_*` and `max_*`
fail with `err.not_found` when there is no number.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`sum_col` `min_col` `max_col` `count_col`], [`(col int, b buffer gives int)`], [over column `col`, all rows; `min` and `max` `or fail`],
  [`sum_row` `min_row` `max_row` `count_row`], [`(row int, b buffer gives int)`], [over row `row`, all columns],
  [`sum_sel` `min_sel` `max_sel` `count_sel`], [`(col int, b buffer gives int)`], [over column `col` of the selected rows],
)]

=== Ordered indexes

#para("9")[A column can have an #term("index"): an ordered structure over its numbers and texts (numbers first, then texts by
bytes, ties by row), a B+tree of pages taken from the free end of the book. `index` builds it, writes to the column keep it in
step, `unindex` frees its pages. With an index `find_row_*` (down the column), `where_*` with every relation but `ne`, and
`order_by` with every row selected answer without scanning, in time that grows with the logarithm of the rows and the number of rows
found; the answers are the same as without it, rows in row order. An index costs a write, and a book whose pages and sheets
fill the buffer refuses a new one with `err.full`; a write that would need a page that is not there is `err.full` too and
changes nothing. The index is a means, never a meaning: a program that does not use it works as before.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`index`], [`(col int, b buffer or fail)`], [builds the index of column `col` of the current sheet (nothing when there is one); `err.full` when the book has no pages left],
  [`unindex`], [`(col int, b buffer)`], [removes it],
  [`indexed`], [`(col int, b buffer gives bool)`], [whether the column has one],
)]

=== Files

#para("9a")[CSV and xlsx files are read into the current sheet and written from it, without any power over files: the program reads the
file into a `buffer` (`file.read_all_into`) and writes the buffer out (`file.write` of `contents`). These words take #term("two buffers"),
the file first and the book last (`0 blob book sheet.load_csv`); the two are different buffers, and a name for the file may be a collection
parameter (9.9) while the book is a buffer of its own. A word that takes the file may change the bytes of its buffer: it is a work area
(a shared buffer is refused). A load adds rows after the last row of the current sheet and gives the number added; when it fails, the sheet is as it was.
Rows beyond `rows_max` or columns beyond the sheet are `err.full`, a text that is not a CSV or a file that is not an xlsx is `err.format`.
The `mode` is 0 or 1 (any other is a fault).]
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`load_csv`], [`(mode int, f buffer, b buffer gives n int or fail)`], [reads the text in `f`: lines end with LF or CRLF, fields with commas, a quoted field holds commas, line breaks and doubled quotes (it is unescaped in place), blank lines and a UTF-8 BOM are skipped. Mode 0: a field that is an integer in its canonical form (`-?(0|[1-9][0-9]*)`, fitting an int) becomes an `int`; a canonical integer with 1 to 18 digits after the point, fitting, becomes a `dec` of that many digits (`1.50` is `dec(2)`); an empty field is an empty cell; any other, `007` and `-0` too, is text. Mode 1: all text],
  [`load_xlsx`], [`(mode int, n int, f buffer, b buffer gives rows int or fail)`], [reads sheet `n` (0 first, in the order of the workbook; `err.not_found` when there is none) of the xlsx in `f`. Numbers follow the CSV rule (the digits after the point beyond 18 are rounded; an exponent is applied; a value that does not fit is text), shared, inline and formula-result strings are text (rich text is joined, phonetic runs dropped), `t="b"` is a `bool`, an error value (`#N/A`) is text, a formula gives its stored result. A cell whose number format is a date or time (built-in formats 14 to 22, 27 to 36, 45 to 47, 50 to 58, 71 to 81, or a custom format with `y`, `d`, `h`, `s` or `m` outside quotes and brackets, `[h]` and `[mm]` included) is, in mode 0, an `int` of Unix seconds (UTC, the 1900 or 1904 date system of the file, a time of day alone counted from midnight); in mode 1 it stays the serial number as a `dec`. Rows and columns the file leaves out are empty cells],
  [`save_csv`], [`(dst buffer, b buffer gives n int or fail)`], [replaces the contents of `dst` with the current sheet as CSV (up to its last used row and column; texts quoted when they hold a comma, a quote or a line break; `dec` with its digits; `bool` as `true` and `false`) and gives the bytes; `err.full` when `dst` is too small (it is then empty)],
  [`save_xlsx`], [`(dst buffer, b buffer gives n int or fail)`], [replaces the contents of `dst` with an xlsx of the current sheet (one sheet, named like it, at most 31 characters without `[]:*?/\`; texts as inline strings; numbers, `dec` and `bool` as such; no date type, so a date is written as the number it holds). The ZIP is stored, not compressed],
)
#para("9b")[#term("work area"): nothing is allocated (clause 12). `load_xlsx` uses the spare capacity of the buffer that holds the file after its last byte, about
130 KB (a window for the inflate, the scanner's buffers) plus the decoded shared strings (4 bytes for each and their text). The buffer is declared larger than the
file; when the room does not suffice the load is `err.full`. A part of the file is read through the central directory of the ZIP: ZIP64, encrypted parts and methods other than
stored and deflate are `err.format`, as is a part larger than 256 MiB when inflated. Every byte inflated is charged to the instruction budget.]
#ex("spec", "16-sheet-files.pnx")

#ex("spec", "16-sheet.pnx")
#ex("spec", "16-sheet-pipe.pnx")
#ex("spec", "16-sheet-book.pnx")
#ex("spec", "16-sheet-index.pnx")

#part("Constraints")

#para("10")[The last argument is a buffer; the others are of the types of the signatures above.]
#ex("spec", "16-sheet-args.pnx")

#part("Faults")

#para("11")[A row or a column outside the sheet, a word that needs a current sheet when there is none (`state`), a buffer that was not formatted by `open_book` (or was changed since), and a sum
that does not fit are faults.]
#ex("spec", "16-sheet-range.pnx")

#part("Diagnostics")
#diag(
  ([A value of another type for a word of `sheet`], "this word of sheet takes int for row, column and number, str for text, bool for put_bool, dec(N) for put_dec"),
  ([A word of `sheet` without a buffer last], "the words of sheet take a buffer last: v row column grid sheet.put_int"),
  ([A word of a file with a book that is a reference, or with a file that is not a buffer], "this word of sheet takes the buffer of the file, then the book: 0 file book sheet.load_csv"),
)
