#import "/tools/common.typ": *

== `md` --- Markdown <std-md>

#para("1")[The module `md` reads Markdown, a text for people, and writes it as HTML. It is a subset of CommonMark with the tables and
the strikethrough of GFM, made for the notes, the READMEs and the pages a script meets; it is not every corner of the specification.
Raw HTML in the text is never passed on: `<` is written as `&lt;`. A link or an image whose address has a scheme other than `http`,
`https`, `mailto`, `tel` and `ftp` is written as its text only. So the HTML is safe to put in a page whatever the Markdown was.]

#para("2")[What is read. Blocks: ATX headings (`#` to `######`) and setext headings, paragraphs, thematic breaks, block quotes, lists
(bullets `-` `+` `*`, numbers `1.` `1)`, nested, tight or loose), fenced code (three or more backticks or tildes, the first word of the
info string becoming `class="language-..."` when it is letters, digits and `- _ + # .`), indented code, and tables (a row, a row of
dashes with colons for the alignment, rows). Inline: emphasis `*a*` `_a_`, strong `**a**` `__a__`, strikethrough `~~a~~`, code spans, links
`[text](address "title")`, images `![alt](address)`, autolinks `<https://...>` and `<a@b.c>`, backslash escapes, character references
(`&amp;` `&#65;`), a hard break (two blanks or a backslash at the end of a line). Not read: link reference definitions, raw HTML, a
setext heading after a list, and the finer rules of CommonMark for emphasis next to punctuation; these give text or a plain
paragraph, never a failure. A nesting of containers or of emphasis deeper than 12 is written as text. The search for the closing mark of a link, an
emphasis or a code span is paid from a budget of sixteen bytes for each byte of the text; when it is spent, the marks left are text, so a text that is
all unclosed marks is read in a time that grows with its size.]

=== Writing HTML

#para("3")[`s b md.html` appends the HTML of the Markdown text `s` to the buffer `b`, `s b md.inline` that of one line or paragraph, with
no block around it. They fail with `err.full` when there is not enough room (the buffer is then as it was). The room is needed for the
HTML itself and, while a block quote or a list item is written, for a copy of it at the end of the buffer, one for each level the block lies in, none longer than the text. `s` is not the buffer.]

=== Reading by blocks

#para("4")[`s md.next` reads the first block of `s` and gives `(kind int, level int, text str, rest str)` (`or fail`); at the end it fails
with `err.not_found`. Nothing is copied: `text` is a view of `s`. `kind` is one of the constants below, `text` what the table says, and
`level` the level of a heading (1 to 6), 1 for an ordered list and 0 for the others.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Constant*], [*text*],
  [`md.heading`], [the text of the heading, without its `#` and its underline, to be read as inline Markdown],
  [`md.paragraph`], [the lines of the paragraph, as written],
  [`md.code`], [fenced: the lines between the fences; indented: the lines as written, with their indentation],
  [`md.quote` `md.list` `md.table`], [the block as written, with its markers, to be given to `md.html`],
  [`md.rule`], [empty],
)]

#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`html`], [`(s str, b buffer or fail)`], [appends the HTML of the Markdown text `s`; `err.full`],
  [`inline`], [`(s str, b buffer or fail)`], [appends the HTML of the text of one line or paragraph; `err.full`],
  [`next`], [`(s str gives kind int, level int, text str, rest str or fail)`], [the first block of `s` and what is left after it; `err.not_found` at the end],
)

#ex("spec", "16-md.pnx")

#part("Constraints")

#para("5")[`s` is a `str`, and the buffer of `html` and `inline` comes last.]
#ex("spec", "16-md-type.pnx")

#part("Diagnostics")
#diag(
  ([A value of another type for a word of `md`], "this word takes a str (the text, and the path before it)"),
)
