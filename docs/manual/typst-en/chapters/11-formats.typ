#import "/tools/common.typ": *

= Text and binary formats <formats>

#opening[Chapter 5 (views, buffers), chapter 7 (records), chapter 8 (failures).][
- How are CSV and JSON read and written without building a tree?
- How is a record sent as bytes and read back?
- Where are hashes and conversions like Base64?
]

Pinax has no heap, so it does not read JSON into a tree of objects. Instead it reads #strong[in one direction]: one
token, one field at a time. It writes straight into a buffer. Fitting records is done by code made from the record's
declaration.

== CSV and JSON

`csv.unpack` reads one line into the fields of a record in order, and `json.pack` writes a record as an object keyed by
field names.

#ex("manual", "11-csv-to-json.pnx")

The row is added first; when `unpack` fails, `before cities truncate` takes it back by cutting the list to the length it
had before.

#mistake[Taking the last row of a list of records off with `pop`][Lists of records have no `pop` (only lists of values
do). The last row is taken back with `truncate`. When an index or a field holds rows of that list, `truncate` is refused
too; then check before adding.
#ex("manual", "11-pop-records.pnx")]

Reading JSON by tokens picks out just what is needed, at any depth. `json.next` gives the kind of the token, a view of
its value, and the rest.

#ex("manual", "11-json-read.pnx")

Look at the three things `json.next` gives, one token at a time. `s json.next` gives `(kind int, value str, rest str)`:
the kind of the token, its text (a view), and the text left after it. Giving what is left to `json.next` again, over and
over, walks the whole text once from the front. At the end of the text `json.next` fails, and that failure ends the
loop.

#ex("manual", "11-json-tokens.pnx")

#trace(
  ("rest json.next", "json.object \"{\" …", [Reads `{`: kind `json.object`; what is left is `"name": …`.]),
  ([`or_else (e) break end`], "json.object \"{\" …", [It did not fail, so the body is skipped.]),
  ("to (kind, value, rest)", "—", [The three go into names; the rightmost, `rest`, takes the top value.]),
)

A kind is a module constant like `json.object`, so it is compared with `eq`, not `case` (chapter 4). Only the work for
each kind has to be chosen, so the values needed can be picked out at any depth; `11-json-read.pnx` added up the
`json.number` tokens only.

A value of no interest is skipped in one go with #word("json.skip"); an object or an array is skipped with everything
inside it, so there is no depth to count. The kinds are the constants `json.object` `json.object_end` `json.array`
`json.array_end` `json.key` `json.string` `json.number` `json.boolean` `json.null`. The other way round, one object
fills a row of a record with #word("json.unpack"): the row is written only when every field fits, else it fails.

#ex("manual", "11-json-skip.pnx")

CSV is written one field at a time. #word("csv.put") appends text (quoted when it holds a comma, a quote or a line
break), `csv.put_int` `csv.put_dec` `csv.put_bool` numbers and booleans, and #word("csv.end_line") the end of the line;
the commas between fields are the words' business. One row of a record as one line is `csv.pack`.

#ex("manual", "11-csv-write.pnx")

== Bytes

The module `bytes` writes and reads fixed widths in a given byte order. With `big_endian` in a record, `bytes.pack` writes
the most significant byte first. A `varint` field writes small numbers in few bytes.

#ex("manual", "11-binary.pnx")

The writing words (`pack`, `put_…`, `encode.…_encode`) fail when the buffer is full, so a guard word always follows
them.

#ex("manual", "11-pack-unguarded.pnx")

#qa[Does the record lie in memory in big-endian order too?][No. `big_endian` sets only the order of the bytes that
`pack` and `unpack` use. The layout in memory is the processor's, and a script cannot see it.]

== Other conversions

`fmt` turns numbers into hexadecimal, binary and zero-padded text. `hash` gives CRC-32, FNV-1a and SipHash; `encode`
gives Base64, hexadecimal and URL percent-encoding. Words that write their results into a buffer fail when the buffer is
full. The full lists are in clause 16 of the standard.

A hash takes text and gives an `int`. #word("hash.crc32") is a checksum; `hash.fnv1a` and `hash.hash64` mix keys. How
many bytes one row of a record becomes under `bytes.pack` is told in advance by #word("bytes.packed_size"), which
helps in sizing a buffer.

#ex("manual", "11-hash.pnx")

#qa[Can the JSON writer indent, or add line breaks?][No. The writing words of `json` append one line. When a layout for
people is needed, the receiving side makes it. The reading side accepts spaces and line breaks as they come.]

#summary[
- `json.next` and `csv.fields` read one token, one field at a time; no tree is built (standard 16.8, 16.9).
- `pack` writes into a buffer; `unpack` writes into the row only when all is right (16.5, 16.8, 16.9).
- `big_endian` sets only the byte order (16.5).
- Lists of records have no `pop` (15.3).
]
