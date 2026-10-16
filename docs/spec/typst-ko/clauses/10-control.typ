#import "/tools/common.typ": *

= 제어 <control>

#part("구문")
```ebnf
control = "if" { body } [ "else" { body } ] "end"
        | "loop" [ "(" NAME ")" ] { body } "end"
        | "case" { branch } [ "otherwise" { body } "end" ] "end" ;
branch  = values "of" { body } "end" ;
values  = case-value | "(" case-value { "," case-value } ")" ;
leave   = ( "while" | "until" | "break" ) [ "(" NAME ")" ] | "return" ;
block   = "do" [ "(" effect ")" ] { body } "end" ;
```

#para("1")[`end` 는 가장 안쪽에 열린 것 하나를 닫는다. 여는 낱말은 `word` `test` `record` `do` `if` `loop`
`case` `of` `otherwise` `or_else` `defer` 다. (텍스트 리터럴과 `note` 주석은 자기 꼬리표로 닫는다, 6.3, 6.5.)]

#para("2")[처리기는 적재 때 모든 갈래의 스택을 견준다. 갈래가 다시 만나는 자리에서 스택은 어느 갈래로 왔든
같아야 한다. #term("떠나는 갈래", en: "leaving branch") --- `return` `fail` `break` 로 끝나거나 끝나지 않는
루프로 끝나는 갈래 --- 는 견주지 않는다.]

== 갈림 <control-if>

#para("1")[`조건 if 몸 end` 는 맨 위의 `bool` 을 내려 참이면 몸을 돈다. `조건 if 몸 else 몸 end` 는 거짓이면
`else` 의 몸을 돈다.]

#para("2")[`else` 가 없는 `if` 의 몸은 스택을 들어올 때와 같게 둬야 한다. `else` 가 있으면 두 몸은 같은 스택을
남겨야 한다.]

#ex("spec", "10-if.pnx")

#part("제약")

#para("3")[두 갈래가 남기는 스택이 다르면 거절된다.]
#ex("spec", "10-if-unequal.pnx")

== 루프 <control-loop>

#para("1")[`loop 몸 end` 는 몸을 되풀이한다. 몸 안에서 다음 낱말이 루프를 떠난다.
- `조건 while` --- 조건이 거짓이면 떠난다.
- `조건 until` --- 조건이 참이면 떠난다.
- `break` --- 조건 없이 떠난다.
셋은 몇 번이든 쓸 수 있고, 루프의 몸에 바로 쓰거나 그 안의 갈래(`if` `else` `case` `or_else`) 안에 쓴다.
가장 안쪽 루프를 떠난다.]

#para("2")[한 바퀴는 스택을 들어올 때와 같게 둬야 한다. 떠나는 자리마다 스택은 루프에 들어올 때와 같아야
한다. 루프의 결과가 필요하면 지역 변수에 둔다.]

#ex("spec", "10-loop.pnx")

#para("3")[`loop (이름)` 은 루프에 이름을 붙인다. `while (이름)` `until (이름)` `break (이름)` 은 그 이름의
루프까지 한 번에 떠난다. 이름은 그 루프의 몸 안에서만 보이고 값이 아니다. 가림 금지(9.5)를 따른다.]

#ex("spec", "10-loop-named.pnx")

#para("4")[자기를 떠나는 `while` `until` `break` 가 하나도 없는 루프는 끝나지 않는 루프다. 끝나지 않는 루프는
떠나는 갈래로 센다. 그래서 그 뒤에 결과를 적지 않아도 낱말의 서명이 맞는다. 안쪽 루프의 `until` 은 바깥
루프를 떠나는 것으로 세지 않지만, 안쪽에 쓴 `until (바깥 이름)` 은 센다.]

#ex("spec", "10-forever.pnx")

#part("제약")

#para("5")[한 바퀴가 스택을 바꾸면 거절된다.]
#ex("spec", "10-loop-net.pnx")

#para("6")[`while` `until` `break` 를 루프 밖에 쓰면 거절된다. `do` 블록 안에서 바깥 루프를 떠나지도 못한다.]
#ex("spec", "10-while-outside.pnx")

#para("7")[떠나는 낱말(`return` `fail` `break`)이나 끝나지 않는 루프 바로 뒤에 같은 갈래의 코드가 더 있으면
닿지 않는 코드로 거절된다.]
#ex("spec", "10-unreachable.pnx")

== 고르기 <control-case>

#para("1")[`값 case 가지… end` 는 맨 위의 `int` 나 `str` 를 내려 가지의 값과 견주고, 같은 첫 가지의 몸을
돈다. 가지의 값은 리터럴이나 이 소스의 `const` 다. 한 가지에 값이 여럿이면 괄호에 쉼표로 적는다. 맞는 가지가 없으면
`otherwise` 의 몸을 돈다. 가지마다 같은 스택을 남겨야 한다.]

#ex("spec", "10-case.pnx")

#part("제약")

#para("2")[같은 값이 두 가지에 있으면 거절된다.]
#ex("spec", "10-case-twice.pnx")

#para("3")[모듈의 상수(`json.key`, `err.full` 같은 경로)는 가지의 값이 되지 못한다. 그런 값은 `eq` 로 견준다.]
#ex("spec", "10-case-path.pnx")

#part("결함")

#para("4")[`otherwise` 가 없고 맞는 가지도 없으면 결함 `range` 다.]
#ex("spec", "10-case-nomatch.pnx")

== 돌아가기 <control-return>

#para("1")[`return` 은 낱말을 그 자리에서 끝낸다. 그 자리의 스택은 결과와 맞아야 한다(9.2 (4)).]

#part("제약")

#para("2")[`return` 은 정의(낱말, 값이 되는 블록, 시험) 안에서만 쓴다. 정의 밖의 코드에 쓰면 거절된다.]
#ex("spec", "10-return-outside.pnx")

== 블록 <control-blocks>

#para("1")[`do … end` 는 #term("블록", en: "block")이다. 블록은 두 가지로 쓰인다. 모양은 같고, 블록을 받는
쪽이 가른다.
- #term("끼워 넣는 블록", en: "inline block") --- 조합 낱말(10.6)에 바로 준 블록. 그 자리에 펼쳐진다.
  그래서 바깥의 지역 변수를 읽고 쓸 수 있고, 안에서 `return` 하면 바깥 낱말을 떠난다.
- #term("값이 되는 블록", en: "block value") --- 지역 변수에 넣거나, 블록을 받는 매개변수(`word (서명)`
  타입)에 주는 블록. 이름 없는 낱말 하나가 되며 `call` 로 부른다. 바깥의 지역 변수를 보지 못한다(붙잡지
  않는다). 전역은 본다.]

#para("2")[`do` 바로 뒤에 서명을 쓸 수 있고, 그 이름들은 낱말 정의와 같이 블록의 지역 변수가 된다. 받는 쪽이
효과를 정해 주면 서명을 줄일 수 있다.]

#ex("spec", "10-inline-locals.pnx")
#ex("spec", "10-value-block.pnx")

#part("제약")

#para("3")[값이 되는 블록이 바깥의 지역 변수를 쓰면 거절된다(그 이름은 블록 안에 선언돼 있지 않다).]
#ex("spec", "10-value-block-outer.pnx")

#para("4")[루프로 펼쳐지는 블록(`each` `times` 등)은 스택을 들어올 때와 같게 둬야 한다.]
#ex("spec", "10-block-net.pnx")

== 줄기와 조합 낱말 <control-stems>

#para("1")[#term("줄기", en: "stem")는 차례로 값을 내는 것이다. 줄기는 값이 아니어서 지역 변수에 넣지 못하고,
바로 뒤의 조합 낱말이 받아야 한다. 둘이 합쳐 루프 하나가 된다. 줄기를 내는 것은 다음과 같다.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*줄기*], [*원소*],
  [배열, 목록의 이름], [칸의 값(레코드면 줄). 목록은 길이까지. 큐, 풀, 버퍼, 찾아보기는 줄기가 아니다],
  [`lo hi range`], [`lo` 부터 `hi` 앞까지의 `int`],
  [`s sep str.split`, `s str.lines`, `s str.words`, `s str.chars`], [`str` 뷰(16.1). `chars` 는 UTF-8 글자 하나씩],
  [`s str.bytes`], [`int`: 바이트(16.1)],
  [`s pat ptn.matches`, `s re rex.matches`, `s re rex.split`], [맞은 부분(16.6, 16.7)],
  [`s csv.fields`], [필드(16.9)],
)]

#para("2")[조합 낱말은 다음과 같다. "블록" 열은 블록이 받고 주는 것이다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*블록*], [*결과*],
  [`each`], [`(T)`], [없음],
  [`each_index`], [`(T, i int)`], [없음],
  [`fold`], [`(acc U, T gives U)`], [`U`. 첫 값을 줄기 앞에 둔다: `0 xs do … end fold`],
  [`count`], [`(T gives bool)`], [`int`],
  [`any` `all`], [`(T gives bool)`], [`bool`. 답이 정해지면 일찍 끝난다],
  [`first`], [`(T gives bool)`], [`T or fail`. 없으면 `err.not_found`],
  [`sum`], [없음], [`int`(`dec(N)` 의 줄기면 `dec(N)`). 넘치면 결함 `overflow`],
  [`minimum` `maximum`], [없음], [`int or fail`(`dec(N)` 도 같다). 빈 줄기면 `err.not_found`],
  [`map_into`], [`(T gives U)`], [뒤에 목적지. `or fail`],
  [`filter_into`], [`(T gives bool)`], [뒤에 목적지. `or fail`],
  [`n times`], [`()`], [없음. 줄기 없이 `n` 번],
)]

#ex("spec", "10-combinators.pnx")
#ex("spec", "10-first.pnx")

#para("3")[`map_into` 와 `filter_into` 는 새 저장소를 만들지 않고 목적지에 쓴다. 목적지가 목록이면 끝에
`push` 하고 아무것도 주지 않는다. 목적지가 배열이면 0 번 칸부터 쓰고 쓴 칸의 수를 준다(`gives n int or
fail`). 목적지가 차면 `err.full` 로 실패하고, 그때까지 쓴 것은 남는다. 목적지는 모음 매개변수여도 된다(9.9).]

#ex("spec", "10-map-into.pnx")

== 미룬 코드 <control-defer>

#part("구문")
```ebnf
defer = "defer" { body } "end" ;
```

#para("1")[`defer 코드 end` 는 둘러싼 블록을 떠날 때 돌 코드를 적는다. 블록은 낱말의 몸, 루프의 몸(한 바퀴마다),
조합 낱말의 `do` 블록(한 번 돌 때마다)이다. 그 블록을 떠나는 모든 길에서 돈다: 블록의 `end`, 루프의 한 바퀴 끝,
`break`, 루프를 떠나는 `while`·`until`, `return`, `or_return`, `or_fail`, `fail`. 여럿이면 적은 차례의 거꾸로
돈다. 결함으로 끝날 때는 돌지 않는다(그때는 상태가 핸들을 닫는다, 19.6).]

#ex("spec", "10-defer.pnx")

#para("2")[미룬 코드가 소유 값을 소비하면(`m release`, `f file.close`) 그 값은 미룬 코드가 맡는다. 블록을 떠나는
모든 길에서 소비된 것으로 치므로 실패 경로마다 닫는 코드를 따로 쓸 일이 없다. 블록 안에서는 그 값을 빌려 쓸 수
있다.]

#ex("spec", "10-defer-owned.pnx")

#part("제약")

#para("3")[`defer` 는 낱말의 몸, 루프의 몸, `do` 블록에 바로 쓴다. `if`·`case`·`or_else` 의 가지 안이나 정의 밖에
쓰면 거절된다.]
#ex("spec", "10-defer-branch.pnx")

#para("4")[미룬 코드는 스택을 받은 그대로 두고 아무것도 남기지 않는다. `return` `fail` `break` `while` `until` `to` `set`
`loop` `do` `case` `defer` 와 `or_fail` `or_return` 을 쓸 수 없다. 실패하는 낱말은 그 안에서 `or_else` 로 다룬다.]

#para("5")[미룬 코드가 맡은 소유 값을 블록 안에서 소비하면 거절된다.]
#ex("spec", "10-defer-taken.pnx")

#part("진단")
#diag(
  ([두 갈래의 스택이 다름], "the two branches of this if leave different stacks"),
  ([한 바퀴가 스택을 바꿈], "one turn of a loop must leave the stack as it found it"),
  ([루프 밖의 `while` `until` `break`], "while, until and break belong inside a loop"),
  ([닿지 않는 코드], "this code cannot be reached: the code before it always leaves"),
  ([`case` 의 같은 값], "this value is already in an earlier branch of the case"),
  ([펼쳐지는 블록이 스택을 바꿈], "this block must leave the stack as it found it"),
  ([정의 밖의 `return`], "return belongs inside a definition"),
  ([상수가 아닌 가지의 값], "a case value is a literal or the name of a const"),
  ([줄기가 아닌 것에 조합 낱말], "only an array or a list is a stem (not a queue, a pool, a buffer or an index)"),
  ([가지 안의 `defer`], "defer is written right in a word's body, a loop's body or a do block, not in a branch"),
  ([정의 밖의 `defer`], "defer belongs inside a definition"),
  ([미룬 코드가 스택을 바꿈], "a defer leaves the stack as it found it"),
  ([미룬 코드 안의 떠나는 낱말], "a defer holds no return, fail, break, while, until, to, set, loop, do, case or defer"),
  ([미룬 코드 안의 `or_fail` `or_return`], "a defer does not leave the word; a failure in it is taken with or_else"),
  ([미룬 코드가 맡은 값을 소비], "a defer gives this owned value when the block is left; here it can only be lent"),
)
