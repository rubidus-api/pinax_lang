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

== Paths, settings files and Markdown

Reading by tokens suits a stream. When the text is a document and one value is wanted, #word("json.at") takes a path: names joined by dots
and indexes in brackets, `user.langs[1]`. It gives the kind and the text of the value; an object or an array comes whole, as written, so it can be
asked again. A missing value is `err.not_found`, so `or_else` is the place for a default. #word("json.valid") says whether a text is JSON at
all, and `json.minify` and `json.pretty` write it into a buffer without its blanks, or one value to a line.

#ex("manual", "11-json-path.pnx")

TOML, the format of settings files, is read the same way by the module `toml`: `toml.at` with a path (`server.ports[1]`, `backend[1].url`
for the second `[[backend]]`), or `toml.next` for every statement in order, headers included. A value is given as written: a string without its
quotes (and with its escapes decoded), a number as its text. `toml.check` judges a whole file before the program trusts it: a key given twice, a table opened twice, an integer that does not fit 64 bits or a date that does not exist is `err.format`. `toml.set` changes one value by its path (or adds the key at the end of its table) and `toml.remove` drops a key, each into a buffer, and everything else in the file, comments included, stays as it was.

#ex("manual", "11-toml-config.pnx")

Markdown becomes HTML with #word("md.html"): headings, paragraphs, emphasis, code, links, lists, quotes and tables. Raw HTML in the text is not
passed on and a link to `javascript:` is written as plain text, so the result is safe to put in a page. `md.next` reads a document a block at a time,
and `md.inline` writes one line without a paragraph around it. Both are a subset of CommonMark (standard 16.12 says which).

#ex("manual", "11-md-html.pnx")

#qa[Is there a tree after `json.at`?][No. Each call reads the text again from the front up to the value it wants, so for many values from one large
document the token words, which read it once, are cheaper.]

== Bytes

The module `bytes` writes and reads fixed widths in a given byte order. With `big_endian` in a record, `bytes.pack` writes
the most significant byte first. A `varint` field writes small numbers in few bytes.

#ex("manual", "11-binary.pnx")

The writing words (`pack`, `put_…`, `encode.…_encode`) fail when the buffer is full, so a guard always follows
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

#qa[Can the JSON writer indent, or add line breaks?][The writing words of `json` append one line. For a layout for people, give the text to
`json.pretty`, which writes it with a member to a line.]

#summary[
- `json.next` and `csv.fields` read one token, one field at a time; no tree is built (standard 16.8, 16.9).
- `pack` writes into a buffer; `unpack` writes into the row only when all is right (16.5, 16.8, 16.9).
- `json.at` and `toml.at` find a value by a path; `md.html` makes safe HTML from Markdown (16.8, 16.11, 16.12).
- `big_endian` sets only the byte order (16.5).
- Lists of records have no `pop` (15.3).
]
