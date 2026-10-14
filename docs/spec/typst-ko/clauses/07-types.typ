#import "/tools/common.typ": *

= 타입과 값 <types>

#para("1")[모든 값에는 적재 때 정해지는 타입이 있다. 돌 때 값이 타입을 들고 다니지 않고, 돌 때 타입을
묻는 낱말도 없다. 타입이 맞지 않는 프로그램은 적재 때 거절된다.]

#para("2")[타입은 다음과 같다.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*타입*], [*값*],
  [`int`], [64 비트 2 의 보수 정수(7.1)],
  [`bool`], [`true`, `false`(7.2)],
  [`str`], [바이트의 뷰(7.3). 지역 변수, 인자, 결과에만 쓴다],
  [`str(N)`], [N 바이트까지 담는 저장된 글(7.3). 전역, 배열의 칸, 레코드의 필드에 쓴다],
  [`err`], [실패의 오류 번호(11.1)],
  [선언한 타입], [`type 이름 바탕` 으로 만든, 바탕과 같은 표현의 다른 타입(7.4)],
  [줄], [레코드를 담은 표(배열, 목록, 풀)의 한 줄. 타입의 이름은 표의 이름이다(8.8)],
  [핸들], [호스트가 준 객체를 가리키는 번호. 타입의 이름은 호스트가 정한다(19.6)],
  [블록], [`word (서명)` 으로 적는 낱말 값(10.5)],
  [`regex`], [정규 표현식 프로그램(16.7). `use rex` 가 있는 소스에서만 보인다],
  [`buffer`], [호스트 낱말의 서명에만 쓰는 매개변수 타입. 스크립트가 선언한 버퍼(8.7)를 받는다(19.3)],
)]

#para("3")[전역, 배열의 칸, 레코드의 필드는 처음에 0 이다: `int` 는 0, `bool` 은 `false`, `str(N)` 은
빈 글, 줄은 "없음"(8.8).]

== 정수 <types-int>

#para("1")[`int` 의 값은 −2#super[63] 부터 2#super[63]−1 까지의 정수다. 모든 처리기에서 같다.]

#para("2")[산술 낱말 `add` `sub` `mul` `div` `mod` `neg` `abs` 는 결과가 범위를 벗어나면 결함
`overflow` 로 멈춘다(11.3). 값을 감아 돌리는 낱말(`wrap_add` …), 끝에서 멈추는 낱말(`sat_add` …),
넘침을 실패로 알리는 낱말(`chk_add` …)이 따로 있다(15.1).]

#para("3")[`div` 는 0 쪽으로 버리고, `mod` 의 결과는 나뉘는 수의 부호를 따른다(`a b div b mul a b mod add`
가 `a` 다). `floor_div` 와 `floor_mod` 는 아래로 버린다. 0 으로 나누면 결함 `div_zero` 다.]

#ex("spec", "07-int.pnx")

#part("결함")

#para("4")[넘침은 그 자리에서 실행을 끝낸다. 아래 예에서 넘친 줄 뒤의 코드는 돌지 않는다.]
#ex("spec", "07-int-overflow.pnx")

== 참거짓 <types-bool>

#para("1")[`bool` 의 값은 `true` 와 `false` 둘이다. 비교 낱말(`eq` `lt` …)이 내고, `if` `while` `until` 과
논리 낱말(`and` `or` `not`)이 받는다.]

#ex("spec", "07-bool.pnx")

#part("제약")

#para("2")[`bool` 과 `int` 는 서로 바뀌지 않는다. 조건 자리에 `int` 를, 산술 낱말에 `bool` 을 줄 수 없다.]
#ex("spec", "07-bool-not-int.pnx")
#ex("spec", "07-int-not-bool.pnx")

== 문자열 <types-str>

#para("1")[#term("문자열", en: "string")은 바이트의 열이다. UTF-8 이어야 한다는 규칙은 없다 --- 바이트를
다루는 낱말은 바이트로, 글자를 다루는 낱말(`str.char_count` 등)은 UTF-8 로 읽는다.]

#para("2")[문자열의 타입은 둘이다.
- `str` 는 *뷰*다. 어딘가에 있는 바이트(리터럴, 전역의 글, 버퍼, 구역)의 시작과 길이를 가리키며,
  만들 때 바이트를 복사하지 않는다. 지역 변수, 인자, 결과, 스택에만 있다.
- `str(N)` 는 #term("저장된 글", en: "stored text")이다. N 바이트의 자리를 차지하고, 넣을 때 바이트를
  복사한다. 전역, 배열과 목록의 칸, 레코드의 필드에만 쓴다. 읽으면 그 자리를 가리키는 `str` 뷰가 된다.]

#ex("spec", "07-str.pnx")

#part("제약")

#para("3")[전역, 배열, 레코드의 필드에 `str` 를 쓰면 거절된다. 뷰가 가리키는 바이트가 그 자리보다 먼저
사라질 수 있기 때문이다.]
#ex("spec", "07-str-global.pnx")

#part("결함")

#para("4")[`str(N)` 자리에 N 바이트보다 긴 글을 넣으면 결함 `range` 로 멈춘다. 자르지 않는다.]
#ex("spec", "07-str-too-long.pnx")

#para("5")[구역에 만든 뷰는 그 구역이 되감기면 쓸 수 없다. 이 규칙은 12.3 이 정한다.]

== 선언한 타입 <types-declared>

#part("구문")
```ebnf
type-decl = "type" NAME base ;
base      = "int" | "bool" | "str" ;
```

#para("1")[`type meters int` 는 #term("선언한 타입", en: "declared type") `meters` 를 만든다. 값의 표현은
`int` 와 같고 실행에 드는 비용도 없지만, 적재 때는 다른 타입이다. 단위가 다른 두 수를 섞는 실수를
막으려고 쓴다.]

#para("2")[바탕 타입의 값 뒤에 타입의 이름을 쓰면 그 타입의 값이 된다(`1500 meters`). 낱말 `raw` 는 선언한
타입의 값을 바탕 타입의 값으로 돌린다.]

#para("3")[같은 선언한 타입의 두 값은 `add` 와 `sub` 로 더하고 뺄 수 있고, 결과도 그 타입이다. 비교 낱말도
같은 타입끼리 쓴다. 다른 산술은 `raw` 로 바꾼 뒤에 한다.]

#ex("spec", "07-type.pnx")

#part("제약")

#para("4")[선언한 타입의 값을 다른 타입의 자리에 주면 거절된다. 아래는 두 인자의 차례를 바꾼 것이다.]
#ex("spec", "07-type-mixed.pnx")

#para("5")[선언한 타입의 두 값에 `add` `sub` 밖의 산술 낱말을 쓰면 거절된다.]
#ex("spec", "07-type-arith.pnx")

#part("진단")
#diag(
  ([조건 자리에 `bool` 이 아닌 값], "a bool is expected here"),
  ([산술 낱말의 두 값의 타입이 다르거나 `int` 가 아님], "this word takes two int values, or two of the same declared type"),
  ([선언한 타입에 `add` `sub` 밖의 산술], "values of a declared type are added and subtracted; for other arithmetic take raw"),
  ([인자의 타입이 서명과 다름], "an argument has another type than the word's signature asks for"),
  ([전역 등에 `str`], "stored text is str(N); a str view lives only in locals, arguments and results"),
)
