#import "/tools/common.typ": *

== `ptn` --- small patterns <std-ptn>

#para("1")[The module `ptn` finds and replaces text with #term("pattern")s of the size of Lua's. It has no alternation
(`|`), no repetition of groups and no back references; `rex` (16.7) takes the larger grammar.]

=== The text of a pattern

#para("2")[A pattern is a sequence of the items below, matched byte by byte.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Item*], [*Matches*],
  [an ordinary character], [that byte],
  [`.`], [any byte],
  [`%a` `%d` `%l` `%u` `%s` `%w` `%x` `%p` `%c`], [letters, digits, lower case, upper case, white space, letters or digits, hexadecimal digits, punctuation, control characters (ASCII); in upper case, everything else],
  [`%` and a character that is not a letter or digit], [that byte itself (`%.` `%%` `%(`)],
  [`[…]` `[^…]`], [a set and its complement, with `a-z` ranges and `%` classes inside; a `]` right after `[` or `[^` is a member],
  [`*` `+` `-` `?` after an item], [zero or more (longest), one or more (longest), zero or more (shortest), zero or one],
  [`( … )`], [a capture; up to nine],
  [`^` first, `$` last], [anchored at the start, at the end of the text],
)]

#ex("spec", "16-ptn-classes.pnx")

=== Words

#para("3")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`is_match`], [`(s str, pat gives bool)`], [is there a match anywhere],
  [`match`], [`(s str, pat gives str or fail)`], [a view of the first match; `err.not_found` when none],
  [`capture`], [`(n int gives str)`], [capture `n` (1--9) of the last `match`; 0 is the whole match],
  [`matches`], [stem], [the matches in order, each search starting where the last ended],
  [`replace`], [`(s str, pat, rep str gives str)`], [new text with every match replaced by `rep`; in `rep`, `%0`--`%9` are captures and `%%` is `%`],
)]

#para("4")[A pattern that begins with `^` is tried at the start only, so `matches` and `replace` match at most once.]

#para("5")[The last match `capture` reads belongs to each run (task) separately; another `match` changes it. A run the
host starts begins with no match: the match of an earlier run is not seen, since its captures are views of text that
run matched, which may be gone.]

#ex("spec", "16-ptn.pnx")

#part("Constraints")

#para("6")[A pattern shall be a string literal or a `const` of one, so that it is checked at load. To search with text
made while running, use `rex.compile` (16.7).]
#ex("spec", "16-ptn-variable.pnx")

#para("7")[A malformed pattern (an unclosed set or capture, a final `%`, a `)` that closes nothing, ten or more
captures) is refused; so are `%b`, `%f`, the back reference `%1` and the position capture `()`.]
#ex("spec", "16-ptn-bad.pnx")
#ex("spec", "16-ptn-balance.pnx")

#part("Faults")

#para("8")[A `capture` whose `n` the last match did not make, or with no match yet, is the fault `range`.]
#ex("spec", "16-ptn-capture-none.pnx")

#para("9")[Matching goes back and tries again. A word that takes more than 2#super[24] steps in one call stops with the
fault `depth`. Rewrite such a pattern with fewer shortest repetitions (`-`), or use `rex`, which does not go back.]
#ex("spec", "16-ptn-steps.pnx")

#part("Diagnostics")
#diag(
  ([A pattern that is not a constant], "a pattern is a string literal or a const name, so that it is checked at load"),
  ([An unclosed set], "a set is not closed by ]"),
  ([`%b`, `%f`, back references], "%b, %f and back references are not supported"),
)
