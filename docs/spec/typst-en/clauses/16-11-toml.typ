#import "/tools/common.typ": *

== `toml` --- TOML <std-toml>

#para("1")[The module `toml` reads TOML text of version 1, a file of settings, without building a tree. A value is given as the bytes it has
in the text, and a string without its quotes; a string with escapes, or of several lines with a backslash at the end of a line, is
decoded into text written in the region (12.2). Nothing is converted: a number is given as written (`5_000`, `0xff`), to be read by
the program with the word of the type it wants. The module can also change a file (below), keeping every byte it does not change.]

#para("2")[`kind` is one of the constants below.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Constant*], [*Value*],
  [`toml.table`], [a header `[a.b]`, or a table a path names: no value],
  [`toml.tables`], [a header `[[a.b]]` of an array of tables: no value],
  [`toml.string`], [a string of any of the four kinds],
  [`toml.int` `toml.float`], [a number as written; `inf` and `nan` are floats],
  [`toml.bool`], [`true` or `false`],
  [`toml.datetime`], [a date, a time or both, as written (`1979-05-27T07:32:00Z`)],
  [`toml.array` `toml.inline`], [an array or an inline table whole, as written, to be read again by `toml.at`],
)]

=== Reading

#para("3")[`s toml.next` reads the first statement of `s` and gives `(kind int, key str, value str, rest str)` (`or fail`): a header
(`kind` `toml.table` or `toml.tables`, `key` the name between the brackets as written, `value` empty) or a pair (`key` as written, dots
and quotes included, `value` as above). Blank lines and comments are passed over. At the end it fails with `err.not_found`; a
statement that is not TOML is `err.format`. The program keeps the header it is under; every `key` is relative to it.]

#para("4")[`path s toml.at` gives `(kind int, value str)` (`or fail`): the value `path` names, in the way a reader of the file would
see it. A path is names joined by dots (a quoted name, `"a.b"` or `'a.b'`, may hold dots) and indexes in brackets: `server.ports[1]`,
`user[0].name`, `a."b c".d`. Keys given with dots (`a.b.c = 1`), tables made by a header, inline tables and arrays are all followed, and
an index chooses an element of an array of values or of tables (the order of the `[[...]]` headers; those below an element belong to
it). A path that names a table gives `toml.table` and no value. `err.not_found` when there is none; `err.format` when the path is
malformed or the text is malformed where it was read.]

#para("5")[`s toml.valid` gives whether every statement of `s` is well formed. The check is of the syntax of each statement: a key
given twice, a table defined twice and the range of a number or the existence of a date are not checked by it.]

#para("6")[`s toml.check` (`or fail`) checks the rules of the whole document as well: a key is defined once, a table once (a table made
by a header may be named again only as the parent of one already made), a table made by dotted keys is not opened again by a header, an
inline table and an array are closed (nothing is added to them later), and an array of tables is not also a table. Every integer must fit 64 bits
signed, every float must be finite, and a date or time must exist (the month and the day of that year, hours to 23, minutes to 59, seconds to 60;
seconds are required, as in TOML 1). `err.format` when a rule is broken or a statement is malformed; `err.full` when the region has no room for the
table the check keeps (at most one node for each byte of `s`; it is given back when the word ends), never "valid". A name written with escapes is
the same key as the same name written bare, and a part with escapes longer than 256 bytes is `err.full`.]

#para("7")[`path s value kind b toml.set` (`or fail`) appends to `b` the text `s` with the value of `path` set: the value of a key that is there is replaced
(an element of an array too), the comments and the layout stay. A key that is not there is added at the end of the statements of its table (the root table:
before the first header), dotted from there when the path goes through tables that are not yet there (`server.tls.on = 1`); a table that exists only
through the headers of tables below it gets a header of its own at the end of the text. `kind` is `toml.string` (`value` is the text, written quoted and
escaped; it must be UTF-8), `toml.int`, `toml.float`, `toml.bool` or `toml.datetime` (`value` is the text of one, as the language writes it, and a number out of
range or a date that does not exist is refused), or `toml.array` (`value` is an array on one line). Another kind, a table as a path, a path under a value, and
an index beyond an array of tables are not written: `err.format`, except an index of an array of tables that is not there, which is `err.not_found`. The text `s`
and the result must keep the rules of `toml.check`, else `err.format`; `err.full` when the buffer has no room, and the buffer is then as it was.]

#para("8")[`path s b toml.remove` (`or fail`) appends to `b` the text `s` without the statement of the key `path` names, from the start of its line to the end of it
(a comment on that line goes with it). `err.not_found` when there is none; `err.format` for a table, for an element of an array or of an inline table (they are
not statements of their own), or when the text is malformed; `err.full` as for `toml.set`.]

#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`valid`], [`(s str gives bool)`], [whether every statement of `s` is well formed],
  [`check`], [`(s str gives or fail)`], [the rules of the whole document; `err.format`, `err.full`],
  [`at`], [`(path str, s str gives kind int, value str or fail)`], [the value `path` names in `s`; `err.not_found`, `err.format`],
  [`next`], [`(s str gives kind int, key str, value str, rest str or fail)`], [the first statement of `s` and what is left after it; `err.not_found` at the end, `err.format`],
  [`set`], [`(path str, s str, value str, kind int, b buffer or fail)`], [appends `s` with the value of `path` set; the rules above],
  [`remove`], [`(path str, s str, b buffer or fail)`], [appends `s` without the statement of the key `path`],
)

#ex("spec", "16-toml.pnx")

#ex("spec", "16-toml-check.pnx")

#ex("spec", "16-toml-edit.pnx")

#part("Constraints")

#para("9")[The arguments are `str` (the `kind` of `toml.set` is an `int`). `toml.check`, `toml.set` and `toml.remove` borrow memory from the region while they work, about 40 bytes for each byte of the text, and give it back; with less room they answer `err.full`. The path may have at most 40 steps, a key at most 16 parts, and arrays and inline tables inside one another may go at most 32 deep; a text of more than 32 different arrays of tables is `err.format` for `toml.at`.]
#ex("spec", "16-toml-type.pnx")

#part("Diagnostics")
#diag(
  ([A value of another type for a word of `toml`], "this word takes a str (the text, and the path before it)"),
)
