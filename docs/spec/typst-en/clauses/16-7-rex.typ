#import "/tools/common.typ": *

== `rex` --- regular expressions <std-rex>

#para("1")[The module `rex` finds and replaces with #term("regular expression")s. Its matching never goes back, so the
time it takes is bounded by the length of the text times the size of the pattern. Write patterns as raw strings (6.5):
in an ordinary string `"\d"` is refused as an unknown escape, and `"\b"` becomes a backspace.]

#para("2")[In a source that has `use rex` the type name `regex` is visible. A `regex` value is a pattern program that
cannot be changed once made. It is used only in locals, arguments and results.]

=== The text of a pattern

#para("3")[
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Form*], [*Meaning*],
  [a character], [its bytes; a multi-byte character, its byte sequence],
  [`.`], [one UTF-8 character; without `(?s)` not a line break],
  [`[abc]` `[a-z]` `[^…]`], [a set; ranges are ASCII; a complemented set takes one UTF-8 character],
  [`\d` `\w` `\s` `\D` `\W` `\S`], [digits, word characters, white space and their complements (ASCII)],
  [`*` `+` `?` `{n}` `{n,}` `{n,m}`], [repetition (longest); with `?` after it, shortest],
  [`a|b`], [alternation; the left first],
  [`( … )` `(?: … )`], [a group that captures, one that does not],
  [`^` `$`], [the start and end of the text; with `(?m)`, of each line],
  [`\b` `\B`], [a word boundary and its complement (ASCII)],
  [`\n` `\t` `\r` `\f` `\v` `\0` `\xNN`], [that byte],
  [`\` and any other character], [that character (`\.` `\*` `\\`)],
  [`\zs` `\ze`], [the match is narrowed to start, end here; the whole pattern must still match],
  [`(?i)` `(?m)` `(?s)`], [at the very start of a pattern only: ignore case (ASCII), anchors per line, `.` also matches a line break],
)
There are no back references, lookaround, Unicode properties or named groups.]

#para("4")[A pattern that repeats a body able to match the empty text (such as `(a*)*`) may give other captures than
Perl or Python.]

=== Words

#para("5")[`re` is a `regex` value. "The match" is the range narrowed by `\zs` and `\ze`.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`rx`], [`(pat gives regex)`], [makes the program at load; `pat` is a literal or a `const`],
  [`compile`], [`(pat str gives regex or fail)`], [makes it while running; a malformed pattern is `err.format`; the program lies in the region],
  [`is_match`], [`(s str, re gives bool)`], [is there a match],
  [`match`], [`(s str, re gives str or fail)`], [the first match; `err.not_found` when none],
  [`cut_match`], [`(s str, re gives str, str, str or fail)`], [what is before the first match, the match, and what is after],
  [`matches`], [stem], [the matches in order; the block takes the whole match and one `str` parameter per group],
  [`split`], [stem], [the pieces between the matches],
  [`with_match`], [`(s str, re, block or fail)`], [runs the block once with the first match; `err.not_found` when none],
  [`replace`], [`(s str, re, rep str gives str)`], [new text with every match replaced; in `rep`, `\0`--`\9` are groups],
  [`replace_into`], [`(s str, re, rep str, buf gives int or fail)`], [appends the replaced text to a buffer and gives the number of replacements],
  [`replace_by`], [`(s str, re, buf, block gives int or fail)`], [appends to a buffer, each match replaced by the text the block gives],
)]

#para("6")[When `matches`, `split` and `replace` search again from the next place, they still see the whole text, so
`^` and `\b` know the bytes before that place.]

#ex("spec", "16-rex.pnx")
#ex("spec", "16-rex-blocks.pnx")
#ex("spec", "16-rex-compile.pnx")

#part("Constraints")

#para("7")[The pattern of `rx` shall be a string literal or a `const`. A pattern made while running goes through
`compile`.]
#ex("spec", "16-rex-variable.pnx")

#para("8")[Malformed patterns and missing features (back references and the like) are refused.]
#ex("spec", "16-rex-bad.pnx")
#ex("spec", "16-rex-backref.pnx")

#para("9")[A `str` where a `regex` goes is refused. The group parameters of a block shall be `str`.]
#ex("spec", "16-rex-str.pnx")
#ex("spec", "16-rex-group-type.pnx")

#part("Faults")

#para("10")[The work space of a search is borrowed from the end of the region on every call. A region too small is the
fault `full`. The search below comes after the region was nearly filled.]
#ex("spec", "16-rex-full.pnx")

#part("Diagnostics")
#diag(
  ([A pattern for `rx` that is not a constant], "rx takes a string literal or a const name; a pattern made while running is rex.compile"),
  ([A count that goes downwards], "a counted repetition goes downwards"),
  ([A back reference], "back references are not supported"),
  ([A `str` where a `regex` goes], "this word takes a str and a regex: s re rex.match"),
  ([A group parameter that is not a `str`], "the groups are named str parameters"),
)
