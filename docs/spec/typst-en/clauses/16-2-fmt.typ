#import "/tools/common.typ": *

== `fmt` --- numbers as text <std-fmt>

#para("1")[`cat` puts an `int` in decimal (9.8). Other forms are made into a `str` by the words of the module `fmt`.
There is no small language of formats (such as `%08x`). Results are written in the region.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`hex` `bin`], [`(n gives str)`], [hexadecimal (lower case), binary; `n` an `int` or a narrow integer (7.6): a negative `int` as 64-bit two's complement, a narrow integer as the bits of its width],
  [`hex_pad`], [`(n, width int gives str)`], [hexadecimal filled with leading zeros to `width` digits; `n` as for `hex`],
  [`zero_pad`], [`(n, width int gives str)`], [decimal filled with leading zeros to `width`; the sign takes a place; `n` an `int` or a narrow integer (a `u64` unsigned)],
  [`append_hex`], [`(n, buf or fail)`], [appends the hexadecimal to a buffer; `n` as for `hex`],
)
Decimal is `str.to_str`; decimal appended to a buffer is `append_int` (15.3). A `width` of `hex_pad` or `zero_pad`
below 0 or from 2#super[32] up is the fault `range`; when the region is used up it is the fault `full`.]

#ex("spec", "16-fmt.pnx")

#part("Constraints")

#para("2")[The words of `fmt` take an `int`. A value of another type is refused. A `dec(N)` is written by `str.to_str`
and `cat` (7.5).]
#ex("spec", "16-fmt-type.pnx")

#part("Faults")

#para("3")[A `width` of `hex_pad` or `zero_pad` below 0 or from 2#super[32] up is the fault `range`.]
#ex("spec", "16-fmt-width.pnx")

#para("4")[A result that does not fit in the region is the fault `full`. The width below is allowed, but larger than
the region.]
#ex("spec", "16-fmt-full.pnx")

#part("Diagnostics")
#diag(
  ([a value that is not an `int`], "this word takes an int"),
)
