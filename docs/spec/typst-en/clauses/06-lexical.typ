#import "/tools/common.typ": *

= Lexical elements <lexical>

#para("1")[This clause defines how a source is split into tokens. The syntax rules (from clause 7 on) read the sequence
of tokens this clause produces.]

== Characters of a source <lex-chars>

#part("Syntax")
```ebnf
letter      = "A" | ... | "Z" | "a" | ... | "z" | "_" ;
digit       = "0" | ... | "9" ;
space       = ? SPACE, TAB, CR, LF ? ;
newline     = ? LF, or CR LF ? ;
shebang     = "#!" { ? any byte but CR and LF ? } newline ;   (* the first line only *)
source-text = [ shebang ] { space | remark | token } ;
```

#para("1")[A source is a sequence of bytes written in UTF-8. Characters outside ASCII appear only inside remarks (6.3)
and character, string and text literals (6.5).]

#para("2")[White space only separates tokens; it has no other meaning. A line break closes nothing. The exceptions are
line remarks, the line rules of `note` and `text`, and strings, which must close on their line.]

#para("3")[When the first line of a file begins with `#!`, the line is ignored, so that a script file can be run
directly on Unix-like systems.]

#part("Constraints")

#para("4")[Outside literals and remarks only the tokens of 6.6 and white space may appear. Any other character ---
`;` `{` `+` and the like, and any character outside ASCII --- is refused. Names cannot contain characters outside
ASCII either.]

#ex("spec", "06-semicolon.pnx")
#ex("spec", "06-hangul-name.pnx")

== Runs and names <lex-runs>

#part("Syntax")
```ebnf
run    = ( letter | digit ) { letter | digit } ;
NAME   = letter { letter | digit } ;             (* and not a reserved word *)
PATH   = NAME "." member { "." member } ;        (* no space around a dot *)
member = letter { letter | digit } ;              (* a reserved word too: json.array *)
```

#para("1")[Letters, digits and underscores that follow each other are read whole, as one #term("run"), and only then
classified. A run that is a reserved word (6.6) is a reserved word; one that begins with a digit is a number (6.4);
anything else is a name. So `remark` is a name, not a remark, and `12ab` is neither a name nor a number.]

#para("2")[A #term("path") is names joined by dots, with no space around a dot. A dot appears only in paths and in
float literals (not in this edition); it is never a token of its own. A path names a word of a module (`io.println`),
a field of a record (`p.x`) or a constant (`str.digit`).]

#para("3")[A run after a dot is part of the path even when it is a reserved word (`json.array`). The first run of a
path cannot be a reserved word.]

#part("Constraints")

#para("4")[A dot shall be followed by a name, with no space.]

#ex("spec", "06-dot-apart.pnx")

#para("5")[Names are case-sensitive: `Total` and `total` are different names. A name is at most 31 bytes long
(annex C).]

== Remarks <lex-remarks>

#part("Syntax")
```ebnf
remark       = line-remark | block-remark ;
line-remark  = "rem" { ? any byte but CR and LF ? } newline ;   (* "rem" as a whole run *)
block-remark = "note" TAG { ? any byte but CR and LF ? } newline
               { ? a line that does not start with TAG at the first column ? newline }
               TAG ;                                             (* at the first column *)
TAG          = NAME ;
```

#para("1")[The run `rem` makes a remark from there to the end of the line, but only when the whole run is `rem`:
`remark` is a name. The end of the file stands in for the line break.]

#para("2")[The run `note` and a tag open a #term("block remark"). The remark is closed by a line that starts, in the
first column, with that tag, not followed by a letter or digit. An indented tag, or a longer run that begins with the
tag (`DOCUMENT`), does not close it.]

#ex("spec", "06-remarks.pnx")

#para("3")[A tool may read the remark right before a declaration as its description. There is no separate mark for
documentation remarks.]

== Numbers <lex-numbers>

#part("Syntax")
```ebnf
sign        = "+" | "-" ;                    (* attached: no space before the digits *)
digits      = digit { [ "_" ] digit } ;
INT         = [ sign ] ( digits | "0x" hex-digits | "0b" bit-digits ) ;
DEC         = [ sign ] digits "." digits ;   (* a decimal, 7.5; no exponent *)
hex-digits  = hex { [ "_" ] hex } ;          (* hex: digit, A-F, a-f *)
bit-digits  = bit { [ "_" ] bit } ;          (* bit: 0, 1 *)
```

#para("1")[The value of an integer literal is a 64-bit two's complement integer (7.1). It is written in decimal,
hexadecimal (`0x`) or binary (`0b`). There is no octal form: `0755` is decimal 755.]

#para("2")[A sign is part of a number when it is attached to the digits: `-42` is one number. Addition and subtraction
are the words `add` and `sub`; `+` and `-` are not characters of their own.]

#para("3")[One underscore may stand between two digits; it is not part of the value. `0x` and `0b` are lower case only;
hexadecimal digits `A`--`F` may be either case.]

#ex("spec", "06-numbers.pnx")

#part("Constraints")

#para("4")[A number directly followed by a letter, an underscore or a dot is refused.]
#ex("spec", "06-number-letters.pnx")

#para("5")[A space between a sign and its digits is refused.]
#ex("spec", "06-sign-apart.pnx")

#para("6")[A value outside the range of a 64-bit signed integer (−2#super[63] to 2#super[63]−1) is refused.]
#ex("spec", "06-int-range.pnx")

#para("7")[An underscore stands only between two digits, one at a time: `1__000` `_1` `1_` `0x_F` are refused.]
#ex("spec", "06-underscore.pnx")

#para("8")[A number with a point and digits on both sides and no exponent is a decimal literal (`12.50`, 7.5). At most 18
digits follow the point, and the digits together, as an integer, shall fit in 64 bits. A number with an exponent (`2e3`,
`2.5e0`) or a hexadecimal one with `p` is a float literal; its form is in the grammar (annex A), but this edition has no
float type, so it is refused (annex D).]

== Characters, strings, text <lex-strings>

#part("Syntax")
```ebnf
CHAR      = [ "U" ] "'" ( char-byte | escape ) "'" ;
STRING    = '"' { string-byte | escape } '"' ;          (* closes on the same line *)
RAWSTRING = "r" '"' { raw-byte } '"' ;                  (* no escapes; same line *)
TEXT      = "text" TAG { blank } newline
            { ? a line that does not start with TAG at the first column ? newline }
            TAG ;                                       (* at the first column *)
escape    = "\\" | '\"' | "\'" | "\a" | "\b" | "\f" | "\n" | "\r" | "\t" | "\v" | "\0"
          | "\x" hex hex | "\u" hex hex hex hex | "\U" hex hex hex hex hex hex hex hex ;
BOOL      = "true" | "false" ;
```

#para("1")[The value of a #term("character literal") is, without a prefix, the `u8` of one byte, and with `U` the `u32` of
one code point (7.6; `'A'` is the `u8` 65, `U'가'` the `u32` 44032).]

#para("2")[The value of a #term("string literal") is a `str`. The fourteen escapes above are the only ones, each of a
fixed length. `\u` and `\U` put a Unicode code point (not a surrogate, at most U+10FFFF) as UTF-8; `\x` puts one byte.]

#para("3")[A #term("raw string") is the run `r` with `"` directly after it. Backslash is an ordinary character, and
there is no way to put a `"` inside. It is for paths and regular expressions.]

#para("4")[A #term("text literal") writes several lines as they are. The `text TAG` line may hold only spaces after the
tag; the value is the lines after it, up to the line of the closing tag. The line break right before the closing line is
not part of the value. Escapes are not decoded. The type is `str`.]

#ex("spec", "06-strings.pnx")

#para("5")[`true` and `false` are the two values of `bool`.]

#part("Constraints")

#para("6")[A string shall close on the line where it opens.]
#ex("spec", "06-string-open.pnx")

#para("7")[An escape not listed above is refused. To keep backslashes as written, use a raw string.]
#ex("spec", "06-escape-unknown.pnx")

#para("8")[A `\u` or `\U` that writes a value that is not a code point (surrogates D800--DFFF, values above 10FFFF) is
refused.]
#ex("spec", "06-escape-surrogate.pnx")

#para("9")[The only prefix of a string is `r`: `u"…"` `U"…"` `R"…"` are refused.]
#ex("spec", "06-string-prefix.pnx")

#para("10")[A character literal holds one character (or one escape).]
#ex("spec", "06-char-two.pnx")

== Reserved words and punctuation <lex-reserved>

#part("Syntax")
```ebnf
punct    = "(" | ")" | "," ;
token    = reserved | NAME | PATH | INT | CHAR | STRING | RAWSTRING | TEXT | BOOL | punct ;
reserved = "rem" | "note" | "text" | "true" | "false" | "pinax"
         | "use" | "as" | "export"
         | "word" | "global" | "const" | "array" | "list" | "pool" | "queue" | "buffer"
         | "tasks" | "record" | "type" | "extern" | "from" | "forward" | "index" | "bound" | "shared"
         | "make" | "test" | "row" | "assign" | "big_endian" | "columns"
         | "gives" | "or" | "fail" | "owned"
         | "to" | "do" | "end" | "if" | "else" | "loop" | "while" | "until" | "break"
         | "case" | "of" | "otherwise" | "return" | "or_fail" | "or_return" | "or_else" | "cat"
         | "defer" | "set" ;
```

#para("1")[The list of #term("reserved word")s is closed. A reserved word cannot be a name.]

#para("2")[`or` is a reserved word only inside a signature (`or fail`). In the body of a word `or` is a core word
(logical or).]

#para("3")[The only punctuation is `(` `)` `,`. Each is a token of its own even when written against other text:
`str(32)` is four tokens.]

#para("4")[The names of types (`int` `str` `bool` …) and of core words (`add` `dup` …) are not reserved words but names
declared in advance. Declaring them again is refused by the rule against hiding names (9.5).]

#para("5")[`make` is reserved for a future feature and refused when used (annex D).]

== Format version <lex-version>

#part("Syntax")
```ebnf
source = [ "pinax" INT ] { item } ;
```

#para("1")[Writing `pinax` and an integer as the first tokens of a source states the #term("format version") the
source follows. This document defines version 2. Without it, a source is read as version 2.]

#ex("spec", "06-version.pnx")

#part("Constraints")

#para("2")[A source that states a version the processor does not read is refused.]
#ex("spec", "06-version-other.pnx")

#part("Diagnostics")

#diag(
  ([A character not allowed outside literals and remarks], "this character is not allowed outside literals and remarks"),
  ([A space or no name after a dot], "a dot is followed by a name, with no space"),
  ([A number runs into a letter, underscore or dot], "a number must not run into letters, an underscore or a dot"),
  ([A space between a sign and its digits], "a sign is attached to its number; arithmetic uses words (add, sub)"),
  ([An integer beyond 64 bits], "the integer does not fit in 64 bits"),
  ([More than 18 digits after the point], "a decimal has at most 18 digits after the point"),
  ([A decimal beyond 64 bits], "the decimal does not fit in 64 bits at its scale"),
  ([A misplaced underscore], "an underscore stands between two digits"),
  ([A string not closed on its line], "the string is not closed on its line"),
  ([An unknown escape], "unknown escape; a raw string r\"...\" keeps backslashes as written"),
  ([An escape that is not a code point], "the escape is not a code point"),
  ([A prefix other than `r`], "the only prefix of a string is r (raw string)"),
  ([More than one character], "a character literal holds one character"),
  ([Another format version], "this source is written for another format version; this build reads version 2"),
)
