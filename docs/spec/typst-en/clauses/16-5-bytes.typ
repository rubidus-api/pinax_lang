#import "/tools/common.typ": *

== `bytes` --- binary data <std-bytes>

#para("1")[The module `bytes` writes numbers and text into a buffer as fixed byte forms, and reads them from bytes. The
writing words are `(value, buf or fail)` (`err.full` when there is not enough room; nothing is written). The reading
words are `(s str gives value, rest str or fail)` (`err.format` when the bytes run short or have the wrong form).]

#para("2")[
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Bytes*],
  [`put_u8` `put_u16` `put_u32` `put_u64` \ `put_i8` `put_i16` `put_i32` `put_i64`], [an integer of that width, least significant byte first],
  [the same names with `_be`], [most significant byte first],
  [`get_u8` … `get_i64`, `get_u16_be` …], [their pairs; `u8` `u16` `u32` read as non-negative; `u64` puts the 64 bits into the `int` as they are (negative when the top bit is set)],
  [`put_varint` `get_varint`], [LEB128: the value as an unsigned 64-bit number, 7 bits at a time (a negative takes 10 bytes)],
  [`put_str` `get_str`], [a length (varint), then the bytes],
)]

#ex("spec", "16-bytes.pnx")

=== Records <std-bytes-records>

#para("3")[`row buf bytes.pack` appends one row of a record (8.8) to a buffer, field by field in order (`or fail`).
`s row bytes.unpack` reads the fields from bytes into the row and gives the rest (`gives str or fail`).
`row bytes.packed_size` is the number of bytes `pack` would write.]

#para("4")[The bytes of a field: an integer width as that width (most significant byte first when the record has
`big_endian`, least significant first otherwise), `int` as 8 bytes, `bool` as 1 byte, `varint` as LEB128, `str(N)` as a
length (varint) and the bytes, a field holding a row as the number it holds (varint).]

#para("5")[When `pack` fails the buffer goes back to the length it had before the call. `unpack` writes into the row
only when every field can be read; when it fails the row is unchanged.]

#ex("spec", "16-bytes-pack.pnx")

#part("Constraints")

#para("6")[`pack`, `unpack` and `packed_size` take only rows of records.]
#ex("spec", "16-bytes-pack-int.pnx")

#part("Faults")

#para("7")[A value that does not fit the width written is the fault `overflow`; it is not cut.]
#ex("spec", "16-bytes-narrow.pnx")
