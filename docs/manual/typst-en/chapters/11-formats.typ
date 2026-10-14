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

`unpack` writes into the row only when every field is right. So each line is tried on a spare row (`scratch`) first, and
a row is added to the list only when it fits.

#mistake[Taking the last row of a list of records off with `pop`][Lists of records have no `pop` (only lists of values
do). A row once added cannot be taken back, so check before adding.
#ex("manual", "11-pop-records.pnx")]

Reading JSON by tokens picks out just what is needed, at any depth. `json.next` gives the kind of the token, a view of
its value, and the rest.

#ex("manual", "11-json-read.pnx")

== Bytes

The module `bytes` writes and reads fixed widths in a given byte order. With `big_endian` in a record, `bytes.pack` writes
the most significant byte first. A `varint` field writes small numbers in few bytes.

#ex("manual", "11-binary.pnx")

#qa[Does the record lie in memory in big-endian order too?][No. `big_endian` sets only the order of the bytes that
`pack` and `unpack` use. The layout in memory is the processor's, and a script cannot see it.]

== Other conversions

`fmt` turns numbers into hexadecimal, binary and zero-padded text. `hash` gives CRC-32, FNV-1a and SipHash; `encode`
gives Base64, hexadecimal and URL percent-encoding. Words that write their results into a buffer fail when the buffer is
full. The full lists are in clause 16 of the standard.

#summary[
- `json.next` and `csv.fields` read one token, one field at a time; no tree is built (standard 16.8, 16.9).
- `pack` writes into a buffer; `unpack` writes into the row only when all is right (16.5, 16.8, 16.9).
- `big_endian` sets only the byte order (16.5).
- Lists of records have no `pop` (15.3).
]
