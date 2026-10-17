#import "/tools/common.typ": *

== `encode` --- bytes as text <std-encode>

#para("1")[The words of the module `encode` are all `(s str, buf or fail)`. They append the result to the buffer and
fail with `err.full` when there is not enough room, or `err.format` when the input has the wrong form.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Meaning*],
  [`base64_encode` `base64_decode`], [Base64 of RFC 4648; when decoding, the closing `=` may be left out],
  [`hex_encode` `hex_decode`], [two hexadecimal digits per byte; lower case when writing, either case when reading],
  [`url_encode` `url_decode`], [percent-encoding of RFC 3986: bytes other than letters, digits and `-` `.` `_` `~` as `%XX`],
)]

#ex("spec", "16-encode.pnx")
#ex("spec", "16-encode-bad.pnx")

#part("Constraints")

#para("2")[A value that is not a `str` and a buffer is refused. To encode a number, make text or bytes of it first
(16.1, 16.5).]
#ex("spec", "16-encode-type.pnx")

#part("Diagnostics")
#diag(
  ([a value that is not a `str` and a buffer], "this word takes a str and a buffer, as: s buf encode.base64_encode"),
)
