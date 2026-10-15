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

=== Records

#para("6")[`row buf json.pack` appends a row of a record as an object whose keys are the field names. `s row
json.unpack` reads an object into the fields of the same names and gives the rest after the object. Values of unknown
keys are passed over. Numbers shall be integers that fit the field's width; a `dec(N)` field takes a number with at most
`N` digits after the point and is written with all `N` (7.5); `null` is not accepted. The row is written
only when everything is right; when it fails (`err.format`) the row is unchanged.]

#ex("spec", "16-json-record.pnx")
#ex("spec", "16-dec-text.pnx")

#part("Constraints")

#para("7")[`json.next` and `json.skip` take only a `str`; `pack` and `unpack` only rows of records.]
#ex("spec", "16-json-type.pnx")

#part("Diagnostics")
#diag(
  ([JSON text that is not a `str`], "this word takes the JSON text as a str"),
)
