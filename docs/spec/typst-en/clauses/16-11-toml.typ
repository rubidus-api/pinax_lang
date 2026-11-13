#import "/tools/common.typ": *

== `toml` --- TOML <std-toml>

#para("1")[The module `toml` reads TOML text of version 1, a file of settings, without building a tree. A value is given as the bytes it has
in the text, and a string without its quotes; a string with escapes, or of several lines with a backslash at the end of a line, is
decoded into text written in the region (12.2). Nothing is converted: a number is given as written (`5_000`, `0xff`), to be read by
the program with the word of the type it wants. The module reads; it does not write TOML.]

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
given twice, a table defined twice and the range of a number or the existence of a date are not checked.]

#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`valid`], [`(s str gives bool)`], [whether every statement of `s` is well formed],
  [`at`], [`(path str, s str gives kind int, value str or fail)`], [the value `path` names in `s`; `err.not_found`, `err.format`],
  [`next`], [`(s str gives kind int, key str, value str, rest str or fail)`], [the first statement of `s` and what is left after it; `err.not_found` at the end, `err.format`],
)

#ex("spec", "16-toml.pnx")

#part("Constraints")

#para("6")[The arguments are `str`. The path may have at most 40 steps, a key at most 16 parts, and arrays and inline tables inside one another may go at most 32 deep; a text of more than 32 different arrays of tables is `err.format` for `toml.at`.]
#ex("spec", "16-toml-type.pnx")

#part("Diagnostics")
#diag(
  ([A value of another type for a word of `toml`], "this word takes a str (the text, and the path before it)"),
)
