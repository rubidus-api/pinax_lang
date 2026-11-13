#import "/tools/common.typ": *

== `json` --- JSON <std-json>

#para("1")[The module `json` reads and writes JSON (RFC 8259) text in one direction, without building a tree: it reads
one token at a time and writes straight into a buffer.]

=== Reading

#para("2")[`s json.next` reads the first token of `s` and gives `(kind int, value str, rest str)` (`or fail`). At the
end it fails with `err.not_found`; on something that is not a token, with `err.format`. White space, commas and colons
between tokens are passed over. The structure of the grammar --- matching brackets, where commas go --- is not checked.]

#para("3")[`kind` is one of the constants below. The `value` of a string is its text without the quotes; when it has
escapes, they are decoded into text written in the region. The `value` of a number is its text as written.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Constant*], [*Token*],
  [`json.object` `json.object_end`], [`{` `}`],
  [`json.array` `json.array_end`], [`[` `]`],
  [`json.key`], [a string followed by a colon],
  [`json.string`], [any other string],
  [`json.number`], [a number],
  [`json.boolean` `json.null`], [`true` `false`, `null`],
)]

#para("4")[`s json.skip` passes over one value (with everything inside, for an object or array) and gives the rest
(`gives str or fail`).]

#ex("spec", "16-json-read.pnx")

=== Writing

#para("5")[The writing words all append to a buffer and fail with `err.full` when there is not enough room. They put
the commas in themselves.
#table(columns: (auto, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*],
  [`begin_object` `end_object` `begin_array` `end_array` `put_null`], [`(buf or fail)`],
  [`put_key` `put_str`], [`(s str, buf or fail)`; the word does the escaping],
  [`put_int`], [`(n int, buf or fail)`],
  [`put_dec`], [`(d dec(N), buf or fail)`],
  [`put_bool`], [`(b bool, buf or fail)`],
)]

#ex("spec", "16-json-write.pnx")

=== Path, validity and layout

#para("5a")[Four more words read the text as a whole, with the strict grammar of RFC 8259 (the token reader above is
lenient on purpose: it reads a stream). A text is valid when it is one value and white space; a nesting deeper than 64 is
not valid here. A path names a value: names joined by dots, an index in brackets, and a name that holds a dot or a
bracket as `["a.b"]` (`user.langs[1]`, `[0].id`); the empty path names the whole text. A name is compared with the key as it
is after its escapes are decoded. The reading stops where it finds the value, so the words do not check the rest of the text.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`valid`], [`(s str gives bool)`], [whether `s` is one JSON value, with only white space around it],
  [`at`], [`(path str, s str gives kind int, value str or fail)`], [the value `path` names in `s`: its kind (the constants above, never `json.key`) and its text, as `json.next` gives it, except that an object or an array is given whole, as written, to be read again by `json.next` or `json.at`. `err.not_found` when there is no such value (a name missing, an index past the end, a name asked of an array or an index of an object); `err.format` when the path is malformed or the text is malformed where it was read],
  [`minify`], [`(s str, b buffer or fail)`], [appends `s` without the white space between its tokens; `err.format` when `s` is not valid, `err.full` when it does not fit (the buffer is then as it was)],
  [`pretty`], [`(s str, indent int, b buffer or fail)`], [appends `s` with each member and element on a line of its own, `indent` blanks (0 to 16; any other number is a fault) to a level, `": "` after a key; an empty object or array stays as it is. The failures are those of `minify`],
)]
#ex("spec", "16-json-path.pnx")

=== Records

#para("6")[`row buf json.pack` appends a row of a record as an object whose keys are the field names. `s row
json.unpack` reads an object into the fields of the same names and gives the rest after the object. Values of unknown
keys are passed over. Numbers shall be integers that fit the field's width; a `dec(N)` field takes a number with at most
`N` digits after the point and is written with all `N` (7.5); `null` is not accepted. The row is written
only when everything is right; when it fails (`err.format`) the row is unchanged.]

#ex("spec", "16-json-record.pnx")
#ex("spec", "16-dec-text.pnx")

#part("Constraints")

#para("7")[`json.next` and `json.skip` take only a `str`; `pack` and `unpack` only rows of records. The source text of `minify`, `pretty`, `valid` and
`at` is not the buffer they write into.]
#ex("spec", "16-json-type.pnx")

#part("Diagnostics")
#diag(
  ([JSON text that is not a `str`], "this word takes the JSON text as a str"),
)
