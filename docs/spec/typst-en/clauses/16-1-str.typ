#import "/tools/common.typ": *

== `str` --- strings <std-str>

#para("1")[The module `str` works on strings as bytes. Byte positions count from 0. Only the words that work on
characters (`chars` `char_count` `is_utf8`) read UTF-8. Changing case (`upper` `lower`) changes ASCII only.]

#para("2")[Where a separator or a part to look for is taken (`sep`, `part`), a `str` or a single character (`int`,
6.5) may be given.]

=== Looking and cutting

#para("3")[All results are views of the string given.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`len` `is_empty`], [`(str gives int)` `(str gives bool)`], [the number of bytes; is it empty],
  [`take` `skip`], [`(s str, n int gives str)`], [the first `n` bytes; what follows them; when short, what there is],
  [`take_last` `skip_last`], [`(s str, n int gives str)`], [the last `n` bytes; what comes before them],
  [`slice`], [`(s str, start int, n int gives str)`], [`n` bytes from `start`; out of range is the fault `range`],
  [`trim` `trim_left` `trim_right`], [`(str gives str)`], [the view without white space (space, tab, CR, LF) at the ends],
  [`cut`], [`(s str, sep gives str, str)`], [before and after the first `sep`; without `sep`, `s` and the empty text],
  [`try_cut`], [`(s str, sep gives str, str or fail)`], [like `cut`, but `err.not_found` without `sep`],
  [`span`], [`(s str, class int gives str, str)`], [the leading bytes of that class, and the rest],
)]

#para("4")[The classes of `span` are the constants `str.digit` `str.alpha` `str.alnum` `str.space` `str.upper_case`
`str.lower_case` `str.hex_digit` `str.punct` (ASCII).]

#ex("spec", "16-str-views.pnx")

=== Searching and comparing

#para("5")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`starts_with` `ends_with` `contains`], [`(s str, part gives bool)`], [],
  [`find` `find_last`], [`(s str, part gives int or fail)`], [the first, last position; `err.not_found` when absent],
  [`compare`], [`(str, str gives int)`], [negative, zero or positive, comparing in byte order],
  [`byte_at`], [`(s str, i int gives int)`], [byte `i`; out of range is the fault `range`],
  [`char_count`], [`(str gives int)`], [the number of UTF-8 characters],
  [`is_utf8`], [`(str gives bool)`], [is it valid UTF-8],
)
The comparison words `eq` `ne` `lt` and the rest also compare strings in byte order (15.2).]

#ex("spec", "16-str-search.pnx")

=== Making new text

#para("6")[The results are new bytes written in the region. The words that read numbers can fail.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`upper` `lower`], [`(str gives str)`], [ASCII case changed],
  [`repeat`], [`(s str, n int gives str)`], [`n` copies joined],
  [`replace`], [`(s str, old str, new str gives str)`], [every `old` replaced by `new`],
  [`pad_left` `pad_right`], [`(s str, width int gives str)`], [filled with spaces to `width` bytes; as it is when longer],
  [`to_str`], [`(int gives str)`], [decimal text; a `dec(N)` with its `N` digits after the point (7.5)],
  [`to_dec`], [`(s str, N gives dec(N) or fail)`], [read a decimal: a sign, digits, and a point with at most `N` digits after it (`N` a constant); more digits, or anything else, `err.format`],
  [`to_int`], [`(str gives int or fail)`], [read as decimal: one sign and digits only (no spaces); otherwise, or beyond 64 bits, `err.format`],
  [`to_int_base`], [`(s str, base int gives int or fail)`], [read in base 2--36; another `base` is the fault `range`],
)]

#ex("spec", "16-str-make.pnx")

=== Stems and writing into buffers

#para("7")[Words that make stems (10.6):
- `s sep str.split` --- the pieces between `sep`s, empty ones included;
- `s str.words` --- the pieces between white space, no empty ones;
- `s str.lines` --- the lines, without their LF or CR LF;
- `s str.bytes` --- the bytes (`int`);
- `s str.chars` --- one view (`str`) for each UTF-8 character.]

#para("8")[Words that write into a buffer:
- `sep buf STEM str.join_into` --- appends the pieces of the stem joined by `sep` (`or fail`). The stem goes last,
  since the word right after a stem shall take it;
- `s old new buf str.replace_into` --- appends the text with replacements and gives their number
  (`gives n int or fail`).]

#ex("spec", "16-str-stems.pnx")

#part("Constraints")

#para("9")[A value that is not a `str` given to a word of `str` is refused.]
#ex("spec", "16-str-type.pnx")

#part("Faults")

#para("10")[An out-of-range position for `slice` or `byte_at` is a fault. For cuts that may run short, use `take` and
`skip`.]
#ex("spec", "16-str-slice-range.pnx")
#ex("spec", "16-str-byte-range.pnx")
