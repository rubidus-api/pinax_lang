#import "/tools/common.typ": *

== `rex` --- 정규 표현식 <std-rex>

#para("1")[모듈 `rex` 는 #term("정규 표현식", en: "regular expression")으로 찾고 바꾼다. 맞추는 방법은 되돌아가지
않는 방식이라, 걸리는 시간이 글의 길이와 패턴의 크기의 곱을 넘지 않는다. 패턴을 적을 때는 날 문자열(6.5)을
쓴다 --- 보통 문자열에서 `"\d"` 는 모르는 이스케이프로 거절되고, `"\b"` 는 백스페이스가 된다.]

#para("2")[`use rex` 가 있는 소스에서는 타입 이름 `regex` 가 보인다. `regex` 값은 만든 뒤에 고치지 못하는 패턴
프로그램이다. 지역 변수, 인자, 결과에만 쓴다.]

=== 패턴의 글

#para("3")[
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*모양*], [*뜻*],
  [글자], [그 바이트들. 여러 바이트 글자는 그 바이트열],
  [`.`], [UTF-8 글자 하나. `(?s)` 가 없으면 줄바꿈은 뺀다],
  [`[abc]` `[a-z]` `[^…]`], [글자 집합. 범위는 ASCII. 부정 집합은 UTF-8 글자 하나를 먹는다],
  [`\d` `\w` `\s` `\D` `\W` `\S`], [숫자, 낱말 글자, 공백과 그 부정(ASCII)],
  [`*` `+` `?` `{n}` `{n,}` `{n,m}`], [되풀이(길게). 뒤에 `?` 를 붙이면 짧게],
  [`a|b`], [고르기. 왼쪽이 먼저],
  [`( … )` `(?: … )`], [묶음(갈무리함), 묶음(갈무리 안 함)],
  [`^` `$`], [글의 처음과 끝. `(?m)` 이면 줄마다],
  [`\b` `\B`], [낱말 경계와 그 부정(ASCII)],
  [`\n` `\t` `\r` `\f` `\v` `\0` `\xNN`], [그 바이트],
  [`\` 와 그 밖의 글자], [그 글자(`\.` `\*` `\\`)],
  [`\zs` `\ze`], [맞은 부분의 시작과 끝을 여기로 좁힌다. 패턴 전체는 그대로 맞아야 한다],
  [`(?i)` `(?m)` `(?s)`], [패턴의 맨 앞에만. 대소문자 무시(ASCII), 줄마다의 닻, `.` 이 줄바꿈도],
)
역참조, 둘러보기, 유니코드 속성, 이름 붙은 묶음은 없다.]

#para("4")[빈 글에 맞을 수 있는 몸을 되풀이하는 패턴(`(a*)*` 같은 것)은 Perl 이나 Python 과 갈무리의 결과가 다를
수 있다.]

=== 낱말

#para("5")[`re` 는 `regex` 값이다. "맞은 부분" 은 `\zs` `\ze` 로 좁힌 범위다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`rx`], [`(pat gives regex)`], [적재 때 패턴을 프로그램으로 만든다. `pat` 은 리터럴이나 `const`],
  [`compile`], [`(pat str gives regex or fail)`], [돌 때 만든다. 틀린 패턴은 `err.format`. 프로그램은 구역에 놓인다],
  [`is_match`], [`(s str, re gives bool)`], [맞는 곳이 있는가],
  [`match`], [`(s str, re gives str or fail)`], [처음 맞은 부분. 없으면 `err.not_found`],
  [`cut_match`], [`(s str, re gives str, str, str or fail)`], [처음 맞은 부분의 앞, 그 부분, 뒤],
  [`matches`], [줄기], [맞은 부분을 차례로. 블록은 맞은 전체와 묶음마다 `str` 매개변수를 받는다],
  [`split`], [줄기], [맞은 부분으로 가른 조각],
  [`with_match`], [`(s str, re, 블록 or fail)`], [처음 맞은 것으로 블록을 한 번 돌린다. 없으면 `err.not_found`],
  [`replace`], [`(s str, re, rep str gives str)`], [모두 바꾼 새 글. `rep` 의 `\0`--`\9` 는 묶음],
  [`replace_into`], [`(s str, re, rep str, buf gives int or fail)`], [바꾼 글을 버퍼에 붙이고 바꾼 횟수를 준다],
  [`replace_by`], [`(s str, re, buf, 블록 gives int or fail)`], [맞은 곳마다 블록이 준 글로 바꿔 버퍼에 붙인다],
)]

#para("6")[`matches` `split` `replace` 는 다음 자리에서 다시 찾을 때도 글 전체를 본다. 그래서 `^` 와 `\b` 는 그
자리 앞의 바이트를 안다.]

#ex("spec", "16-rex.pnx")
#ex("spec", "16-rex-blocks.pnx")
#ex("spec", "16-rex-compile.pnx")

#part("제약")

#para("7")[`rx` 의 패턴은 문자열 리터럴이거나 `const` 여야 한다. 돌 때 만든 패턴은 `compile` 을 쓴다.]
#ex("spec", "16-rex-variable.pnx")

#para("8")[모양이 틀린 패턴과 없는 기능(역참조 등)은 거절된다.]
#ex("spec", "16-rex-bad.pnx")
#ex("spec", "16-rex-backref.pnx")

#para("9")[`regex` 자리에 `str` 를 주면 거절된다. 블록의 묶음 매개변수는 `str` 이어야 한다.]
#ex("spec", "16-rex-str.pnx")
#ex("spec", "16-rex-group-type.pnx")

#part("결함")

#para("10")[찾는 데 드는 작업 공간은 부를 때마다 구역의 끝에서 빌린다. 구역이 모자라면 결함 `full` 이다. 아래는 구역을
거의 다 채운 뒤에 찾는다.]
#ex("spec", "16-rex-full.pnx")

#part("진단")
#diag(
  ([`rx` 에 상수가 아닌 패턴], "rx takes a string literal or a const name; a pattern made while running is rex.compile"),
  ([거꾸로 된 횟수], "a counted repetition goes downwards"),
  ([역참조], "back references are not supported"),
  ([`regex` 자리의 `str`], "this word takes a str and a regex: s re rex.match"),
  ([`str` 가 아닌 묶음 매개변수], "the groups are named str parameters"),
)
