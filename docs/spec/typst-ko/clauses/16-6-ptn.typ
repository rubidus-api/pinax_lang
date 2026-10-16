#import "/tools/common.typ": *

== `ptn` --- 작은 패턴 <std-ptn>

#para("1")[모듈 `ptn` 은 Lua 의 패턴과 같은 크기의 #term("패턴", en: "pattern")으로 문자열을 찾고 바꾼다. 갈래
선택(`|`), 묶음의 되풀이, 역참조는 없다. 더 큰 문법은 `rex`(16.7)가 맡는다.]

=== 패턴의 글

#para("2")[패턴은 다음 항목의 나열이다. 바이트 단위로 맞춘다.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*항목*], [*맞는 것*],
  [보통 글자], [그 바이트],
  [`.`], [아무 바이트],
  [`%a` `%d` `%l` `%u` `%s` `%w` `%x` `%p` `%c`], [글자, 숫자, 소문자, 대문자, 공백, 글자나 숫자, 십육진 숫자, 구두점, 제어 문자(ASCII). 대문자로 쓰면 그 밖],
  [`%` 와 글자·숫자가 아닌 것], [그 바이트 자체(`%.` `%%` `%(`)],
  [`[…]` `[^…]`], [집합과 그 여집합. 안에 `a-z` 범위와 `%` 갈래를 쓴다. 여는 `[` 나 `[^` 바로 뒤의 `]` 는 글자다],
  [항목 뒤의 `*` `+` `-` `?`], [0 번 이상(길게), 1 번 이상(길게), 0 번 이상(짧게), 0 번이나 1 번],
  [`( … )`], [갈무리(capture). 아홉까지],
  [처음의 `^`, 끝의 `$`], [글의 처음, 글의 끝에 붙임],
)]

#ex("spec", "16-ptn-classes.pnx")

=== 낱말

#para("3")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`is_match`], [`(s str, pat gives bool)`], [어디든 맞는 곳이 있는가],
  [`match`], [`(s str, pat gives str or fail)`], [처음 맞은 부분의 뷰. 없으면 `err.not_found`],
  [`capture`], [`(n int gives str)`], [마지막 `match` 의 갈무리 `n` (1--9). 0 은 맞은 전체],
  [`matches`], [줄기], [맞은 부분을 차례로. 겹치지 않게 다음 자리에서 다시 찾는다],
  [`replace`], [`(s str, pat, rep str gives str)`], [맞은 곳을 모두 `rep` 로 바꾼 새 글. `rep` 의 `%0`--`%9` 는 갈무리, `%%` 는 `%`],
)]

#para("4")[`^` 로 시작하는 패턴은 처음 자리에서만 맞춰 본다. 그래서 `matches` 와 `replace` 도 많아야 한 번 맞는다.]

#para("5")[`capture` 가 읽는 마지막 맞음은 실행(작업)마다 따로 있다. 다른 `match` 를 부르면 바뀐다. 호스트가 시작한
실행은 맞음이 없는 채로 시작한다: 지난 실행의 맞음은 보이지 않는다. 그 갈무리는 지난 실행이 맞춘 글의 뷰이고, 그 글은
이미 없을 수 있기 때문이다.]

#ex("spec", "16-ptn.pnx")

#part("제약")

#para("6")[패턴은 문자열 리터럴이거나 그런 값의 `const` 여야 한다. 적재 때 검사하기 위해서다. 돌 때 만든 글로
찾으려면 `rex.compile`(16.7)을 쓴다.]
#ex("spec", "16-ptn-variable.pnx")

#para("7")[모양이 틀린 패턴(닫히지 않은 집합이나 갈무리, `%` 로 끝남, 열지 않은 `)`, 갈무리 열 개 이상)은
거절된다. `%b` `%f`, 역참조 `%1`, 위치 갈무리 `()` 도 거절된다.]
#ex("spec", "16-ptn-bad.pnx")
#ex("spec", "16-ptn-balance.pnx")

#part("결함")

#para("8")[`capture` 의 `n` 이 마지막 맞음에 없는 갈무리이거나 아직 맞음이 없으면 결함 `range` 다.]
#ex("spec", "16-ptn-capture-none.pnx")

#para("9")[맞추기는 되돌아가며 찾는다. 낱말 한 번에 2#super[24] 걸음을 넘으면 결함 `depth` 로 멈춘다. 그런 패턴은
짧은 되풀이(`-`)를 줄이거나 `rex` 를 쓴다(`rex` 는 되돌아가지 않는다).]
#ex("spec", "16-ptn-steps.pnx")

#part("진단")
#diag(
  ([패턴이 상수가 아님], "a pattern is a string literal or a const name, so that it is checked at load"),
  ([닫히지 않은 집합], "a set is not closed by ]"),
  ([`%b` `%f` 역참조], "%b, %f and back references are not supported"),
)
