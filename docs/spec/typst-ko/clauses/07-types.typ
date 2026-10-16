#import "/tools/common.typ": *

= 타입과 값 <types>

#para("1")[모든 값에는 적재 때 정해지는 타입이 있다. 돌 때 값이 타입을 들고 다니지 않고, 돌 때 타입을
묻는 낱말도 없다. 타입이 맞지 않는 프로그램은 적재 때 거절된다.]

#para("2")[타입은 다음과 같다.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*타입*], [*값*],
  [`int`], [64 비트 2 의 보수 정수(7.1)],
  [`dec(N)`], [소수점 아래 N 자리의 십진 소수, N 은 0 부터 18(7.5)],
  [`bool`], [`true`, `false`(7.2)],
  [`str`], [바이트의 뷰(7.3). 지역 변수, 인자, 결과에만 쓴다],
  [`str(N)`], [N 바이트까지 담는 저장된 글(7.3). 전역, 배열의 칸, 레코드의 필드에 쓴다],
  [`err`], [실패의 오류 번호(11.1)],
  [선언한 타입], [`type 이름 바탕` 으로 만든, 바탕과 같은 표현의 다른 타입(7.4)],
  [줄], [레코드를 담은 표(배열, 목록, 풀)의 한 줄. 타입의 이름은 표의 이름이다(8.8)],
  [핸들], [호스트가 준 객체를 가리키는 번호. 타입의 이름은 호스트가 정한다(19.6)],
  [블록], [`word (서명)` 으로 적는 낱말 값(10.5)],
  [`regex`], [정규 표현식 프로그램(16.7). `use rex` 가 있는 소스에서만 보인다],
  [`buffer`], [매개변수 타입. 스크립트가 선언한 버퍼(8.7)를 받는다(9.9, 19.3)],
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
base      = "int" | "bool" | "str" | "dec" "(" INT ")" ;
```

#para("1")[`type meters int` 는 #term("선언한 타입", en: "declared type") `meters` 를 만든다. 값의 표현은
`int` 와 같고 실행에 드는 비용도 없지만, 적재 때는 다른 타입이다. 단위가 다른 두 수를 섞는 실수를
막으려고 쓴다.]

#para("2")[바탕 타입의 값 뒤에 타입의 이름을 쓰면 그 타입의 값이 된다(`1500 meters`). 낱말 `raw` 는 선언한
타입의 값을 바탕 타입의 값으로 돌린다.]

#para("3")[같은 선언한 타입의 두 값은 `add` 와 `sub` 로 더하고 뺄 수 있고, 결과도 그 타입이다. 비교 낱말과
`min` `max` 도 같은 타입끼리 쓴다. 다른 산술은 `raw` 로 바꾼 뒤에 한다.]

#ex("spec", "07-type.pnx")

#para("3a")[`int` 위의 선언한 타입 `D` 의 값에 `int` 를 곱하거나(`D int mul`, `int D mul`) `int` 로 나누거나
나머지를 구하면(`D int div`, `D int mod`) 결과는 `D` 다. `neg` 와 `abs` 도 `D` 를 준다. 수를 몇 배 하거나 몇으로
나눠도 단위는 그대로이기 때문이다. 두 `D` 의 곱과 몫은 `D` 가 아니므로 이 규칙에 들지 않는다.]

#ex("spec", "07-type-scale.pnx")

#part("제약")

#para("4")[선언한 타입의 값을 다른 타입의 자리에 주면 거절된다. 아래는 두 인자의 차례를 바꾼 것이다.]
#ex("spec", "07-type-mixed.pnx")

#para("5")[선언한 타입의 두 값에 `add` `sub` 밖의 산술 낱말을 쓰면 거절된다. 선언한 타입과 `int` 에 (3a)
밖의 산술 낱말을 써도 거절된다.]
#ex("spec", "07-type-arith.pnx")

== 십진 소수 <types-dec>

#part("구문")
```ebnf
dec-type = "dec" "(" INT ")" ;   (* 0 부터 18 *)
DEC      = [ sign ] digits "." digits ;   (* 지수 없음: 6.4 *)
```

#para("1")[`dec(N)` 의 값은 소수점 아래 `N` 자리의 십진수다. 10#super[N] 배 한 `int` 로 담는다: `12.50` 은 1250 으로
담긴 `dec(2)` 다. 어느 처리기에서나 결과가 같다. 범위는 `int` 의 범위를 10#super[N] 으로 나눈 것이다.]

#para("2")[점 앞뒤에 숫자가 있고 지수가 없는 수는 `dec(N)` 리터럴이다. `N` 은 점 아래 숫자의 수다: `12.50` 은
`dec(2)`, `0.5` 는 `dec(1)`, `-0.05` 는 `dec(2)` 다.]

#para("3")[`dec(N)` 은 선언한 타입(7.4)처럼 `int` 위의 수 타입이다: 같은 `dec(N)` 둘은 더하고 빼고 비교하고
`min` `max` 에 준다. `dec(N)` 에 `int` 를 곱하고, `int` 로 나누고(`div` 처럼 0 쪽으로 자름), 나머지를 구한다.
`neg` `abs` 는 그대로 둔다. 그에 더해 `dec(A) dec(B) mul` 은 `A+B` 가 18 이하일 때 `dec(A+B)` 를 정확히 준다.
`sum`, `minimum`, `maximum`(10.6)은 `dec(N)` 의 줄기를 받아 `dec(N)` 을 준다. `chk_add` `chk_sub` `chk_mul`
`chk_div` 는 `add` `sub` `mul` `div` 처럼 `dec(N)` 을 받는다. 넘치면 결함 `overflow` 다.]

#para("4")[`n dec(N)` 은 `int` `n` 을 점 아래 0 이 `N` 개인 수로 만든다(`5 dec(2)` 는 `5.00`). `d raw` 는 담긴
`int` 를 준다(`12.50 raw` 는 1250). `d M rescale` 은 `d` 를 `dec(M)` 으로 준다. `M` 은 0 부터 18 의 상수다. 자리가
늘면 정확하고, 줄면 0 에서 먼 쪽으로 반올림한다(`2.345` 는 `2.35`, `-2.345` 는 `-2.35`).]

#ex("spec", "07-dec.pnx")
#ex("spec", "07-dec-round.pnx")

#para("5")[`type price dec(2)` 는 `dec(2)` 위의 타입을 선언한다. `9.99 price` 가 그 값을 만들고 `raw` 가
`dec(2)` 를 준다. `dec(N)` 은 `int` 처럼 전역, 필드(8 바이트), 배열과 목록의 칸, 큐의 값, 매개변수(9.9)의 타입이 된다.
`str.to_str` 과 `cat` 은 자리를 모두 써서 적고(16.1), `str.to_dec` 은 읽는다.]

#part("제약")

#para("6")[자리 수가 다른 `dec` 둘은 더하거나 비교하지 않는다. 하나를 먼저 `rescale` 한다. `dec` 를 `dec` 로 나누지
않는다.]
#ex("spec", "07-dec-scales.pnx")
#ex("spec", "07-dec-div.pnx")

#part("진단")
#diag(
  ([조건 자리에 `bool` 이 아닌 값], "a bool is expected here"),
  ([산술 낱말의 두 값의 타입이 다르거나 `int` 가 아님], "this word takes two int values, or two of the same declared type"),
  ([선언한 타입에 (3)과 (3a) 밖의 산술], "a declared type is added to and subtracted from its own type, and multiplied, divided or taken modulo by an int; for other arithmetic take raw"),
  ([인자의 타입이 서명과 다름], "an argument has another type than the word's signature asks for"),
  ([전역 등에 `str`], "stored text is str(N); a str view lives only in locals, arguments and results"),
  ([레코드의 필드에 `str`], "a field holds str(N); a str view lives only in locals, arguments and results"),
  ([자리 수가 다른 `dec` 둘], "two dec values of different scales: rescale one first (d 2 rescale)"),
  ([`dec` 를 `dec` 로 나눔], "a dec is divided by an int, not by a dec; for a share, rescale first and divide by the int"),
  ([`dec(A) dec(B) mul` 이 18 자리를 넘음], "dec(A) times dec(B) is dec(A+B), and A+B is at most 18; rescale one first"),
  ([0 부터 18 밖의 자리 수], "dec(N) has 0 to 18 digits after the point"),
)
