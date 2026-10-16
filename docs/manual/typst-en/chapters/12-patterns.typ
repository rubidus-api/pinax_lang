#import "/tools/common.typ": *

= Patterns <patterns>

#opening[Chapter 5 (views), chapter 4 (stems).][
- What finds and replaces text by its shape?
- Why must patterns be constants?
- When is `ptn` the right choice, and when `rex`?
]

== Small patterns: `ptn`

The patterns of `ptn` are the size of Lua's: classes such as `%d` `%a` `%s`, `[…]` sets, `*` `+` `-` `?`, and captures
`(…)`. There is no alternation (`|`). After a match, `n ptn.capture` reads a capture.

#ex("manual", "12-ptn.pnx")

A pattern is a row of small pieces. The pieces used most are these (the full list is in standard 16.6).

#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*Piece*], [*Matches*]),
  [an ordinary character such as `a`], [that one byte],
  [`.`], [any one byte],
  [`%d` `%a` `%s` `%w`], [one digit, letter, space, letter or digit; in capitals (`%D` …), anything else],
  [`%.` `%%`], [a point, a `%` itself],
  [`[ae]` `[a-z]` `[^0-9]`], [one of those listed, one in the range, one not listed],
  [`+` `*` `?` after a piece], [once or more, zero times or more, zero times or once],
  [`^` and `$`], [at the start of the pattern: only at the start of the text; at its end: only at the end],
  [`( … )`], [captures what it matched; read it with `n ptn.capture`],
)

Trying patterns one at a time shows what each piece means.

#ex("manual", "12-ptn-steps.pnx")

`ptn.capture` reads the last match #strong[of this run]. Reading it before any match is the fault `range`. A match made
in another run is not seen --- its text may be gone by then.

#ex("manual", "12-capture-none.pnx")

== Regular expressions: `rex`

`rex` has regular expressions with alternation, counts (`{n,m}`), word boundaries (`\b`) and flags (`(?i)` and so on).
It matches without going back, so any pattern ends in time proportional to the length of the text. A pattern is made
into a program with `rex.rx`, and the block of `matches` takes the groups as parameters.

#ex("manual", "12-rex.pnx")

A block can also make the text that replaces each match.

#ex("manual", "12-replace-by.pnx")

#mistake[Writing a regular expression in an ordinary string][`"\d+"` is refused, because in an ordinary string `\d` is
an unknown escape. Worse is `"\b"`: it becomes a backspace character and quietly means something else. Write patterns
in raw strings `r"…"`.
#ex("manual", "12-escape.pnx")]

== Patterns are constants

The patterns of `ptn` and of `rex.rx` shall be literals or `const`s, so that the processor can check them at load and
make them into programs. A malformed pattern is refused before anything runs.

#ex("manual", "12-pattern-variable.pnx")

When a pattern made while running is needed, use `rex.compile`. A malformed pattern comes back as a failure (`format`).

#ex("manual", "12-compile.pnx")

#qa[Which do I use, `ptn` or `rex`?][For short patterns with a few captures `ptn` is lighter. When you need alternation
or counts, when the pattern comes from outside, or when the text is very long, use `rex`. `ptn` goes back and tries
again, so a bad pattern runs into its step limit and stops with a fault.]

#summary[
- `ptn` has Lua-sized patterns; captures are read with `ptn.capture`, and with no match in the run it is the fault
  `range` (standard 16.6).
- `rex` has regular expressions that never go back; groups become block parameters (16.7).
- Patterns are literals or `const`s; one made while running goes through `rex.compile` (16.6, 16.7).
- Regular expressions are written in raw strings (6.5).
]
