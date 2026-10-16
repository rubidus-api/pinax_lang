#import "/tools/common.typ": *
// shape: no constraint of its own; the words take int as clause 15 says

== `fmt` --- numbers as text <std-fmt>

#para("1")[`cat` puts an `int` in decimal (9.8). Other forms are made into a `str` by the words of the module `fmt`.
There is no small language of formats (such as `%08x`). Results are written in the region.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`hex` `bin`], [`(int gives str)`], [hexadecimal (lower case), binary; a negative number as 64-bit two's complement],
  [`hex_pad`], [`(n int, width int gives str)`], [hexadecimal filled with leading zeros to `width` digits],
  [`zero_pad`], [`(n int, width int gives str)`], [decimal filled with leading zeros to `width`; the sign takes a place],
  [`append_hex`], [`(n int, buf or fail)`], [appends the hexadecimal to a buffer],
)
Decimal is `str.to_str`; decimal appended to a buffer is `append_int` (15.3). A `width` of `hex_pad` or `zero_pad`
below 0 or from 2#super[32] up is the fault `range`; when the region is used up it is the fault `full`.]

#ex("spec", "16-fmt.pnx")
