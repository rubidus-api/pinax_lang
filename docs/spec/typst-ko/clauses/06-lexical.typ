#import "/tools/common.typ": *

= 어휘 <lexical>

#para("1")[이 절은 소스를 토큰으로 나누는 규칙을 정한다. 구문 규칙(7절부터)은 이 절이 낸 토큰의 열을
읽는다.]

== 소스의 글자 <lex-chars>

#part("구문")
```ebnf
letter      = "A" | ... | "Z" | "a" | ... | "z" | "_" ;
digit       = "0" | ... | "9" ;
space       = ? SPACE, TAB, CR, LF ? ;
newline     = ? LF, or CR LF ? ;
shebang     = "#!" { ? any byte but CR and LF ? } newline ;   (* the first line only *)
source-text = [ shebang ] { space | remark | token } ;
```

#para("1")[소스는 UTF-8 로 적은 바이트의 열이다. ASCII 밖의 글자는 주석(6.3)과 문자·문자열·텍스트
리터럴(6.5) 안에만 쓸 수 있다.]

#para("2")[공백은 토큰을 가를 뿐 다른 뜻이 없다. 줄바꿈은 아무것도 닫지 않는다. 예외는 줄 주석과
`note`·`text` 의 줄 규칙, 그리고 한 줄 안에서 닫혀야 하는 문자열이다.]

#para("3")[파일의 첫 줄이 `#!` 로 시작하면 그 줄은 없는 것으로 친다. 유닉스 계열에서 스크립트 파일을 바로
실행할 수 있게 하려는 것이다.]

#part("제약")

#para("4")[리터럴과 주석 밖에는 6.6 이 정한 토큰과 공백만 올 수 있다. 다른 글자 --- `;` `{` `+` 같은 것,
그리고 ASCII 밖의 글자 --- 는 거절된다. 이름에도 ASCII 밖의 글자는 쓰지 못한다.]

#ex("spec", "06-semicolon.pnx")
#ex("spec", "06-hangul-name.pnx")

== 덩이와 이름 <lex-runs>

#part("구문")
```ebnf
run    = ( letter | digit ) { letter | digit } ;
NAME   = letter { letter | digit } ;             (* and not a reserved word *)
PATH   = NAME "." member { "." member } ;        (* no space around a dot *)
member = letter { letter | digit } ;              (* a reserved word too: json.array *)
```

#para("1")[영문자, 숫자, 밑줄이 이어진 것은 끊지 않고 #term("덩이", en: "run") 하나로 읽은 다음에 갈래를
정한다. 덩이가 예약 낱말(6.6)이면 예약 낱말이고, 숫자로 시작하면 수(6.4)이며, 그 밖에는 이름이다.
그래서 `remark` 는 주석이 아니라 이름이고, `12ab` 는 이름도 수도 아니다.]

#para("2")[#term("경로", en: "path")는 이름을 점으로 이은 것이다. 점의 앞뒤에 공백이 없어야 한다. 점은 경로와
(이 판에 없는) 실수 리터럴 안에서만 쓰이며 홀로 토큰이 되지 않는다. 경로는 모듈의 낱말(`io.println`),
레코드의 필드(`p.x`), 상수(`str.digit`)를 가리킨다.]

#para("3")[점 뒤의 덩이는 예약 낱말이어도 경로의 일부다(`json.array`). 경로의 첫 덩이는 예약 낱말일 수 없다.]

#part("제약")

#para("4")[점 뒤에는 공백 없이 이름이 와야 한다.]

#ex("spec", "06-dot-apart.pnx")

#para("5")[이름은 대소문자를 가른다. `Total` 과 `total` 은 다른 이름이다. 이름의 길이는 31 바이트까지다
(부속서 C).]

== 주석 <lex-remarks>

#part("구문")
```ebnf
remark       = line-remark | block-remark ;
line-remark  = "rem" { ? any byte but CR and LF ? } newline ;   (* "rem" as a whole run *)
block-remark = "note" TAG { ? any byte but CR and LF ? } newline
               { ? a line that does not start with TAG at the first column ? newline }
               TAG ;                                             (* at the first column *)
TAG          = NAME ;
```

#para("1")[덩이 `rem` 은 그 자리부터 줄 끝까지를 주석으로 만든다. 덩이 전체가 `rem` 일 때만 그렇다
--- `remark` 는 이름이다. 파일의 끝은 줄바꿈을 대신한다.]

#para("2")[덩이 `note` 와 꼬리표 하나는 #term("덩이 주석", en: "block remark")을 연다. 첫 칸에서 그 꼬리표로
시작하고, 꼬리표 바로 뒤가 이름의 글자가 아닌 줄이 주석을 닫는다. 들여 쓴 꼬리표나 꼬리표로 시작하는 더 긴
덩이(`DOCUMENT`)는 닫지 않는다.]

#ex("spec", "06-remarks.pnx")

#para("3")[선언 바로 앞의 주석을 도구가 그 선언의 설명으로 읽을 수 있다. 따로 문서 주석의 표시는 없다.]

== 수 <lex-numbers>

#part("구문")
```ebnf
sign        = "+" | "-" ;                    (* attached: no space before the digits *)
digits      = digit { [ "_" ] digit } ;
INT         = [ sign ] ( digits | "0x" hex-digits | "0b" bit-digits ) ;
hex-digits  = hex { [ "_" ] hex } ;          (* hex: digit, A-F, a-f *)
bit-digits  = bit { [ "_" ] bit } ;          (* bit: 0, 1 *)
```

#para("1")[정수 리터럴의 값은 64 비트 2 의 보수 정수다(7.1). 십진, 십육진(`0x`), 이진(`0b`)으로 적는다.
팔진 표기는 없다 --- `0755` 는 십진 755 다.]

#para("2")[부호는 숫자에 붙어 있을 때 수의 일부다. `-42` 는 수 하나다. 덧셈과 뺄셈은 낱말 `add` `sub` 이며
`+` `-` 는 홀로 쓰는 글자가 아니다.]

#para("3")[밑줄은 숫자와 숫자 사이에 하나씩 둘 수 있고 값에는 들지 않는다. `0x` `0b` 는 소문자만 받는다.
십육진 숫자 `A`--`F` 는 대소문자 모두 받는다.]

#ex("spec", "06-numbers.pnx")

#part("제약")

#para("4")[수 바로 뒤에 영문자, 밑줄, 점이 이어지면 거절된다.]
#ex("spec", "06-number-letters.pnx")

#para("5")[부호와 숫자 사이에 공백이 있으면 거절된다.]
#ex("spec", "06-sign-apart.pnx")

#para("6")[값이 64 비트 부호 있는 정수의 범위(−2#super[63] 부터 2#super[63]−1)를 벗어나면 거절된다.]
#ex("spec", "06-int-range.pnx")

#para("7")[밑줄은 숫자와 숫자 사이에만, 하나만 온다. `1__000` `_1` `1_` `0x_F` 는 거절된다.]
#ex("spec", "06-underscore.pnx")

#para("8")[실수 리터럴(`1.5`, `2e3`)의 모양은 문법(부속서 A)에 있으나 이 판에는 실수 타입이 없어 거절된다
(부속서 D).]

== 문자, 문자열, 텍스트 <lex-strings>

#part("구문")
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

#para("1")[#term("문자 리터럴", en: "character literal")의 값은 `int` 다. 접두사가 없으면 바이트 하나의 값,
`U` 가 붙으면 코드포인트 하나의 값이다(`'A'` 는 65, `U'가'` 는 44032).]

#para("2")[#term("문자열 리터럴", en: "string literal")의 값은 `str` 다. 이스케이프는 위의 열넷뿐이고 자리
수는 고정이다. `\u` 와 `\U` 는 유니코드 코드포인트(서로게이트가 아닌, U+10FFFF 이하의 값)를 UTF-8 로 넣는다.
`\x` 는 바이트 하나를 넣는다.]

#para("3")[#term("날 문자열", en: "raw string")은 덩이 `r` 바로 뒤에 `"` 가 붙은 것이다. 역슬래시는 보통
글자이며 안에 `"` 를 넣는 길은 없다. 경로나 정규 표현식을 적을 때 쓴다.]

#para("4")[#term("텍스트 리터럴", en: "text literal")은 여러 줄을 그대로 적는다. `text TAG` 줄에는 꼬리표
뒤에 공백만 올 수 있고, 값은 다음 줄부터 닫는 꼬리표 줄 앞까지다. 닫는 줄 바로 앞의 줄바꿈 하나는 값에
들지 않는다. 이스케이프를 풀지 않는다. 타입은 `str` 다.]

#ex("spec", "06-strings.pnx")

#para("5")[`true` 와 `false` 는 `bool` 의 두 값이다.]

#part("제약")

#para("6")[문자열은 연 줄에서 닫혀야 한다.]
#ex("spec", "06-string-open.pnx")

#para("7")[위에 없는 이스케이프는 거절된다. 역슬래시를 그대로 두려면 날 문자열을 쓴다.]
#ex("spec", "06-escape-unknown.pnx")

#para("8")[`\u` 와 `\U` 가 코드포인트가 아닌 값(서로게이트 D800--DFFF, 10FFFF 보다 큰 값)을 적으면 거절된다.]
#ex("spec", "06-escape-surrogate.pnx")

#para("9")[문자열의 접두사는 `r` 하나뿐이다. `u"…"` `U"…"` `R"…"` 는 거절된다.]
#ex("spec", "06-string-prefix.pnx")

#para("10")[문자 리터럴은 문자 하나(또는 이스케이프 하나)를 담는다.]
#ex("spec", "06-char-two.pnx")

== 예약 낱말과 구두점 <lex-reserved>

#part("구문")
```ebnf
punct    = "(" | ")" | "," ;
token    = reserved | NAME | PATH | INT | CHAR | STRING | RAWSTRING | TEXT | BOOL | punct ;
reserved = "rem" | "note" | "text" | "true" | "false" | "pinax"
         | "use" | "as" | "export"
         | "word" | "global" | "const" | "array" | "list" | "pool" | "queue" | "buffer"
         | "tasks" | "record" | "type" | "extern" | "from" | "forward" | "index" | "bound"
         | "make" | "test" | "row" | "assign" | "big_endian" | "columns"
         | "gives" | "or" | "fail" | "owned"
         | "to" | "do" | "end" | "if" | "else" | "loop" | "while" | "until" | "break"
         | "case" | "of" | "otherwise" | "return" | "or_fail" | "or_return" | "or_else" | "cat"
         | "defer" ;
```

#para("1")[#term("예약 낱말", en: "reserved word")의 목록은 닫혀 있다. 예약 낱말은 이름으로 쓰지 못한다.]

#para("2")[`or` 는 서명 안에서만 예약 낱말(`or fail`)이다. 낱말의 몸에서 `or` 는 핵 낱말(논리합)이다.]

#para("3")[구두점은 `(` `)` `,` 셋뿐이다. 붙여 써도 따로 토큰이다 --- `str(32)` 는 토큰 넷이다.]

#para("4")[타입의 이름(`int` `str` `bool` …)과 핵 낱말의 이름(`add` `dup` …)은 예약 낱말이 아니라 미리
선언된 이름이다. 그러나 그 이름을 다시 짓는 것은 가림 금지 규칙(9.5)이 거절한다.]

#para("5")[`make` 는 앞으로의 기능을 위해 예약만 해 두었고 쓰면 거절된다(부속서 D).]

== 형식 판 <lex-version>

#part("구문")
```ebnf
source = [ "pinax" INT ] { item } ;
```

#para("1")[소스의 첫 토큰으로 `pinax` 와 정수를 적으면 그 소스가 따르는 #term("형식 판", en: "format
version")을 밝힌다. 이 문서는 판 2 를 정한다. 적지 않으면 판 2 로 읽는다.]

#ex("spec", "06-version.pnx")

#part("제약")

#para("2")[처리기가 읽지 않는 판을 적은 소스는 거절된다.]
#ex("spec", "06-version-other.pnx")

#part("진단")

#diag(
  ([리터럴과 주석 밖의 허용되지 않은 글자], "this character is not allowed outside literals and remarks"),
  ([점 뒤에 공백이나 이름이 아닌 것], "a dot is followed by a name, with no space"),
  ([수에 글자·밑줄·점이 붙음], "a number must not run into letters, an underscore or a dot"),
  ([부호와 숫자 사이의 공백], "a sign is attached to its number; arithmetic uses words (add, sub)"),
  ([64 비트를 넘는 정수], "the integer does not fit in 64 bits"),
  ([잘못 놓인 밑줄], "an underscore stands between two digits"),
  ([줄 안에서 닫히지 않은 문자열], "the string is not closed on its line"),
  ([모르는 이스케이프], "unknown escape; a raw string r\"...\" keeps backslashes as written"),
  ([코드포인트가 아닌 이스케이프], "the escape is not a code point"),
  ([`r` 이 아닌 접두사], "the only prefix of a string is r (raw string)"),
  ([둘 이상의 문자], "a character literal holds one character"),
  ([다른 형식 판], "this source is written for another format version; this build reads version 2"),
)
