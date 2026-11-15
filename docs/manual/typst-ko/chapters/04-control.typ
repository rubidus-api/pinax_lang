#import "/tools/common.typ": *

= 지역 변수와 흐름 <control>

#opening[3장(서명, 이름 있는 입력).][
- 지역 변수는 언제 생기고 어디까지 보이는가?
- `if`, `loop`, `match` 는 어떻게 쓰고, 스택에 대해 무엇을 지켜야 하는가?
- 배열을 훑는 루프를 짧게 쓰는 길은?
- 낱말을 값으로 주려면?
]

== 지역 변수

몸 안에서 `to 이름` 은 맨 위 값을 그 이름에 넣는다. 처음 쓰는 이름이면 그 값의 타입으로 지역 변수가 생긴다. 따로
선언하는 줄은 없다.

#ex("manual", "04-locals.pnx")

`to` 는 스택의 맨 위 값을 #strong[내려서] 이름에 담는다. 이름을 쓰면 그 값이 다시 올라간다. 값을 스택에 오래 들고 있지
않고 이름에 넣어 두었다가 필요할 때 꺼내 쓰는 것이다.

#trace(
  ("a b add", "12", [`a` 는 3, `b` 는 9.]),
  ("c add", "18", [`c` 는 6.]),
  ("to total", "—", [18 을 내려 새 지역 변수 `total` 에 담는다. 스택이 빈다.]),
  ("total 3 div", "6", [`total` 을 쓰면 18 이 올라가고, 3 으로 나눈다.]),
  ("to mean", "—", [6 을 `mean` 에 담는다.]),
)

지역 변수가 보이는 범위는 처음 넣은 자리부터 그것을 감싼 가지, 루프, 블록의 끝까지다. 가지 안에서 처음 만든 이름은
가지 밖에서 읽지 못한다. 다른 가지로 왔다면 값이 없기 때문이다.

#ex("manual", "04-scope.pnx")

전역에 넣을 때는 `to` 가 아니라 `set` 을 쓴다. `to` 는 지역 변수에만 넣는다.

#ex("manual", "04-store-global.pnx")

#mistake[`to total` 로 전역에 넣는다][`to` 의 이름이 전역이면 거절되고, 메시지가 `set` 을 알려 준다. 전역은 가릴 수
없으므로 그 이름의 지역을 만들 수도 없고, 지역을 만들려던 `to` 가 전역을 조용히 바꾸지도 않게 하려는 규칙이다.
#ex("manual", "04-store-global-bare.pnx")]

== 갈림

`조건 if … else … end` 는 맨 위의 `bool` 을 보고 한쪽을 돈다. 두 갈래는 같은 스택을 남겨야 한다. `return` 으로
떠나는 갈래는 맞추지 않아도 된다.

#ex("manual", "04-if.pnx")

`if` 는 맨 위의 `bool` 을 #strong[내리고] 갈 길을 고른다. 두 갈래가 같은 스택을 남겨야 하는 까닭은, `end` 뒤의 코드가
어느 갈래에서 왔는지 모르기 때문이다. 어느 길로 왔든 스택이 같아야 처리기가 그다음을 검사할 수 있다.

#ex("manual", "04-sign.pnx")

#trace(
  ("n 0 lt", "true", [`n` 이 -4 이면 비교가 `true` 를 남긴다.]),
  ("if", "—", [`bool` 을 내리고 첫 갈래로 간다.]),
  ("\"negative\"", "\"negative\"", [이 갈래는 `str` 하나를 남긴다.]),
  ("end", "\"negative\"", [`else` 갈래도 `str` 하나를 남기므로 두 길이 맞는다.]),
)

`else` 가 없는 `if` 는 스택을 그대로 둬야 한다. 값 하나를 남기는 `if` 에 `else` 를 빠뜨리면 거절된다.

#ex("manual", "04-if-unequal.pnx")

== 루프

`loop … end` 는 되풀이한다. 안의 `break .` 는 루프를 떠나고 `continue .` 는 다음 바퀴로 간다. 조건은 `if` 로 쓴다:
`i n gt if break . end`. 한 바퀴는 스택을 그대로 둬야 한다 --- 그래서 루프가 만드는 값은 지역 변수에 모은다.

#ex("manual", "04-loop.pnx")

한 바퀴가 스택을 그대로 둬야 하는 까닭도 같다. 루프의 머리는 처음 들어올 때와 한 바퀴 돌고 돌아올 때 모두 같은 스택을
봐야 한다. 바퀴마다 값이 하나씩 쌓이면 몇 바퀴 돌지 모르는 처리기가 스택의 깊이를 정할 수 없다. 그래서 루프가 만드는
값은 지역 변수에 모은다.

#ex("manual", "04-loop-sum.pnx")
#ex("manual", "04-loop-leaves.pnx")

`count_up` 은 바퀴마다 `i 1 add` 로 값을 하나 만들지만 아무도 가져가지 않는다. `to i` 를 붙이면 그 값이 `i` 에 담기고
스택은 바퀴의 처음과 같아진다.

안쪽 루프에서 바깥 루프까지 한 번에 떠나려면 세는 수를 쓴다: `break 2 .` 는 두 루프를 떠난다.

#ex("manual", "04-named-loop.pnx")

#qa[루프 이름은 왜 없는가?][루프에 이름을 붙이는 형식은 없앴다. 몇 겹을 떠날지 수로 적는다(`break 2 .`). 루프를 한 겹 더
감싸면 수가 달라질 수 있으니, 겹이 둘 이상인 코드는 프로시저로 나누는 편이 낫다.]

== 고르기

`match` 는 `int` 나 `str` 를 가지의 값과 견준다. 가지는 `when 값 do … end` 이고, 가지 하나에 값이 여럿이면 `when` 뒤에
공백으로 늘어놓는다. 맞는 가지가 없을 때 할 일은
`otherwise` 에 적는다. `otherwise` 없이 맞는 가지도 없으면 결함이다.

#ex("manual", "04-case.pnx")

가지의 값은 리터럴이거나 이 소스의 `const` 여야 한다. `err.full` 이나 `json.key` 같은 모듈의 상수는 `match` 에 쓰지
못하고 `eq` 로 견준다.

== 레일과 블록

배열이나 목록을 훑는 일은 루프 대신 #strong[레일]과 #strong[조합 낱말]로 쓴다. 배열의 이름이 레일이고, 뒤에 오는
`do … end` 블록과 `each` `count` `fold` 같은 낱말이 그것을 받는다. 블록은 그 자리에 펼쳐지므로 바깥의 지역 변수를
쓸 수 있다.

#ex("manual", "04-rails.pnx")

`lo hi range` 도 레일이다(`hi` 는 들지 않는다). `fold` 는 첫 값을 레일 앞에 두고, 블록이 (지금까지의 값, 원소)를 받아
새 값을 준다.

`each`, `each_index`, `times` 의 블록은 루프처럼 `break .` 로 일찍 떠나고 `continue .` 로 다음 원소로 간다. `do to 이름 … end` 의 `to` 가 원소를 이름에 받는다. 레일은 멈추고 그 낱말 뒤의
코드가 돈다. 처음 맞는 것에서 멈추는 찾기는 이렇게 쓴다.

#ex("manual", "04-leave-each.pnx")

레일은 어댑터로 다듬는다: `skip N` `head N` `step N` `rev`. 쓴 차례대로 적용되고 루프는 여전히 하나다. `N` 이 식이면 쉼표로 끝낸다.

#ex("manual", "04-rail-adapters.pnx")

`scores skip 2 head 3` 은 셋째부터 다섯째까지이고, `scores head 3 skip 1` 은 둘째와 셋째다. 순서가 뜻을 바꾼다. `step` 과 `rev` 는 #strong[닫힌 레일] (길이를 알고 아무 자리나 읽는 `range`, 배열, 목록)에만 쓰고,
`skip` 과 `head` 는 #strong[열린 레일] (조각을 차례로 꺼내는 `str.words`, `str.lines` …)에도 쓴다. `step` 은 맨 끝에 온다.

블록이 든 어댑터도 있다: `do to s s 4 gt end filter`(남김), `skip_while`, `head_while`, `map`(같은 타입의 값으로 바꿈). 루프 안에서 원소마다 돌고, 열린 레일에도 쓴다.
`scores do to s s 4 gt end filter do to s s 100 mul end map sum` 처럼 이어 붙인다. 배열 둘은 `xs ys zip do to y to x … end each` 로 나란히 훑는다.

== 값인 프로시저

조합 낱말에 주지 않는 블록은 `def 서명 do … end` 로 쓰고, #strong[값]이다. 지역 변수에 넣거나, 돌려주거나, `proc` 타입
(`type 이름 proc 서명 end`)의 매개변수에 줄 수 있고, `call` 로 부른다. 이런 블록은 바깥의 지역 변수를 보지 못한다.
프로그램의 프로시저를 값으로 주려면 `ref 이름` 이라고 쓴다. 그 값은 그 프로시저이고, 서명도 그 프로시저의 것이다.

#ex("manual", "04-word-values.pnx")

#qa[`str.starts_with` 같은 언어의 낱말은 어떻게 값으로 주는가?][블록의 서명을 타입만으로 쓴다:
`def str, str gives bool do str.starts_with end`. 타입만 쓴 입력은 스택에 남으므로 낱말이 그대로 받는다. 서명이 없으면
블록은 아무것도 받지 않고, 메시지가 두 꼴을 보여 준다.]

#mistake[`match` 가지에 낱말 자체를 둘 수 있다: `when "prefix" do str.starts_with end`][가지는 낱말을 실행한다. 값으로
만들지 않는다. 거기서 `str.starts_with` 가 불리고, 스택에 값이 모자란다. 블록으로 값을 만든다:
`when "prefix" do def str, str gives bool do str.starts_with end end`, 프로그램의 프로시저면 `ref 이름`.
#ex("manual", "04-word-values-branch.pnx")]

#qa[`if` 의 두 가지가 다른 타입을 남기고 싶으면?][못 한다. `if` 뒤의 코드는 어느 가지로 왔는지 모르므로, 두 가지는 같은
개수·같은 타입을 남겨야 한다. 다른 것을 남기고 싶다면 그것은 "값이 있거나 없거나" 이고, 그때는 실패(8장)나 `is_none`
으로 묻는 줄(7장)을 쓴다.]

#summary[
- `to 이름` 은 지역 변수에, `set 이름` 은 전역에 넣는다(표준 9.4).
- 가지 안에서 만든 이름은 가지 밖에서 보이지 않는다(9.4).
- `to` 는 맨 위 값을 내려 이름에 담고, 이름을 쓰면 값이 다시 올라간다(9.4).
- `if` 의 두 갈래와 `match` 의 가지는 같은 스택을, 루프의 한 바퀴는 들어올 때의 스택을 남긴다(10).
- 루프를 여럿 한 번에 떠나려면 `break 2 .` 처럼 수를 쓴다(10.2).
- 배열과 목록은 레일로 훑는다: `each` `each_index` `count` `fold` `sum` …(10.6). `each`, `each_index`, `times` 의
  블록은 `break .` 로 떠난다(10.6 (4)).
- `ref 이름` 은 프로시저 `이름` 을 값으로 준다. 다른 낱말은 `def 서명 do 낱말 end` 로 준다(10.5).
]
