#import "/tools/common.typ": *

= 제어 <control>

#part("구문")
```ebnf
control = "if" { body } [ "else" { body } ] "end"
        | "loop" { body } "end"
        | "match" { branch } [ "otherwise" { body } "end" ] "end" ;
branch  = "when" case-value { case-value } "do" { body } "end" ;
leave   = ( "break" | "continue" ) [ INT ] "." | "return" ;
block   = "do" { "to" NAME } { body } "end"
        | "def" effect "do" { body } "end"       (* a block value, 10.5 *)
        | "ref" NAME ;
```

#para("1")[`end` 는 가장 안쪽에 열린 것 하나를 닫는다. 여는 프로시저는 `def` `forward` `extern` `test` `record` `do` `if` `loop`
`match` `when` `otherwise` `or_else` `defer` 다. (`forward` 와 `extern` 은 서명 뒤의 `end` 로, `type NAME proc … end` 도 닫는다.) (텍스트 리터럴과 `note` 주석은 자기 꼬리표로 닫는다, 6.3, 6.5.)]

#para("2")[처리기는 적재 때 모든 갈래의 스택을 견준다. 갈래가 다시 만나는 자리에서 스택은 어느 갈래로 왔든
같아야 한다. #term("떠나는 갈래", en: "leaving branch") --- `return` `fail` `break` `continue` 로 끝나거나 끝나지 않는
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

#para("1")[`loop 몸 end` 는 몸을 되풀이한다. 몸 안에서 다음 문장이 루프를 떠난다.
- `break .` --- 루프를 떠나 `end` 뒤의 코드로 간다.
- `continue .` --- 한 바퀴의 나머지를 건너뛰고 다음 바퀴를 시작한다.
- `break N .` `continue N .` --- 안쪽에서 세어 `N` 번째 루프에 한다(`N` 은 1 이상의 정수 리터럴, 빼면 1). 안쪽 루프들은 한 번에 떠난다.
조건은 `if` 로 쓴다: `x 0 eq if break . end`. 문장은 마침표로 끝난다. `break` 와 `continue` 는 몇 번이든 쓸 수 있고, 루프의 몸에 바로 쓰거나 그 안의 갈래(`if` `else` `match` `or_else`) 안에 쓴다.]

#para("2")[한 바퀴는 스택을 들어올 때와 같게 둬야 한다. 떠나는 자리마다 스택은 루프에 들어올 때와 같아야
한다. 루프의 결과가 필요하면 지역 변수에 둔다.]

#ex("spec", "10-loop.pnx")

#para("3")[루프에는 이름이 없다. 바깥 루프를 떠나려면 세는 수를 쓴다: `break 2 .` 는 두 루프를 한 번에 떠난다.]

#ex("spec", "10-loop-named.pnx")

#para("4")[자기를 떠나는 `break` 가 하나도 없는 루프는 끝나지 않는 루프다. 끝나지 않는 루프는
떠나는 갈래로 센다. 그래서 그 뒤에 결과를 적지 않아도 프로시저의 서명이 맞는다. 안쪽 루프의 `break .` 는 바깥
루프를 떠나는 것으로 세지 않지만, 안쪽에 쓴 `break 2 .` 는 센다.]

#ex("spec", "10-forever.pnx")

#part("제약")

#para("5")[한 바퀴가 스택을 바꾸면 거절된다.]
#ex("spec", "10-loop-net.pnx")

#para("6")[`break` 와 `continue` 를 루프 밖에 쓰면 거절된다. `do` 블록 안에서 바깥 루프를 떠나지도 못한다.
`each`, `each_index`, `times` 의 블록 안에서는 그 블록을 떠난다(10.6 (4)).]
#ex("spec", "10-while-outside.pnx")

#para("7")[떠나는 프로시저(`return` `fail` `break` `continue`)이나 끝나지 않는 루프 바로 뒤에 같은 갈래의 코드가 더 있으면
닿지 않는 코드로 거절된다.]
#ex("spec", "10-unreachable.pnx")

== 고르기 <control-match>

#para("1")[`값 match when 값… do 몸 end … otherwise 몸 end end` 는 맨 위의 `int`, 좁은 정수(7.6), `err` 나 `str` 를 내려
가지의 값과 견주고, 같은 첫 가지의 몸을 돈다. 가지는 `when` 으로 시작한다. 가지의 값은 리터럴, 이 소스의 `const`, 오류
코드(`err.range`), 또는 다른 모듈이 내보낸 상수나 실패 코드(`모듈.이름`, 8.14)다. 한 가지에 값이 여럿이면 `when` 뒤에
공백으로 늘어놓는다(쉼표나 괄호 없이). 값들 뒤의 `do` 가 몸을 연다. 맞는 가지가 없으면
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

#para("1")[`return` 은 프로시저를 그 자리에서 끝낸다. 그 자리의 스택은 결과와 맞아야 한다(9.2 (4)).]

#part("제약")

#para("2")[`return` 은 정의(프로시저, 값이 되는 블록, 시험) 안에서만 쓴다. 정의 밖의 코드에 쓰면 거절된다.]
#ex("spec", "10-return-outside.pnx")

== 블록 <control-blocks>

#para("1")[`do … end` 는 #term("블록", en: "block")이다. 블록은 조합 프로시저에 주는 것으로만 쓴다(`if` 의 몸처럼 프로시저
없이 홀로 있는 `do … end` 는 없다). 값이 되는 블록은 `def` 로 적는다(`def 서명 do … end`, (2a)).
블록은 두 가지로 쓰인다.
- #term("끼워 넣는 블록", en: "inline block") --- 조합 프로시저(10.6)에 바로 준 블록. 그 자리에 펼쳐진다.
  그래서 바깥의 지역 변수를 읽고 쓸 수 있고, 안에서 `return` 하면 바깥 프로시저를 떠난다.
- #term("값이 되는 블록", en: "block value") --- `def 서명 do … end` 로 적어 지역 변수에 넣거나, 블록을 받는
  매개변수(`proc … end` 타입, 7.4)에 주는 것. 이름 없는 프로시저 하나가 되며 `call` 로 부른다. 바깥의 지역 변수를
  보지 못한다(붙잡지 않는다). 전역은 본다.]

#para("2")[끼워 넣는 블록은 `do` 바로 뒤에 `to 이름` 을 하나씩 이어 써서 레일이 내는 값(과 `each_index` 의 번호)을 지역 변수로
받는다. 이름은 값이 놓인 차례의 거꾸로 받는다: `do to i to s … end each_index` 는 `s` 가 원소, `i` 가 번호다(레일의
원소가 먼저 놓이므로 맨 위가 `i`). 값이 되는 블록의 서명은 프로시저의 서명과 같고, 이름들은 지역 변수가 된다.]

#ex("spec", "10-inline-locals.pnx")
#ex("spec", "10-value-block.pnx")

#para("2a")[`ref 이름` 은 `def` 로 정의한 프로시저(이 소스의 것이든 쓰는 모듈이 내보낸 것이든) 자체를 값으로 준다(`ref by_len`,
`ref geo.area`). 타입은 그 프로시저의 서명을 가진 `proc` 이고, `call` 은 그 프로시저를 부른다. 언어의 다른 프로시저는
서명을 쓴 블록으로 준다. 타입만 쓴 입력은 스택에 남으므로(9.2 (2)) 블록의 몸은 그 프로시저 하나다:
`def str, str gives bool do str.starts_with end`.]

#ex("spec", "10-word-value.pnx")

#part("제약")

#para("3")[값이 되는 블록이 바깥의 지역 변수를 쓰면 거절된다(그 이름은 블록 안에 선언돼 있지 않다).]
#ex("spec", "10-value-block-outer.pnx")

#para("4")[루프로 펼쳐지는 블록(`each` `times` 등)은 스택을 들어올 때와 같게 둬야 한다.]
#ex("spec", "10-block-net.pnx")

#para("4a")[서명이 없는 값이 되는 블록은 아무것도 받지 않는다. 그 안에서 값이 필요한 프로시저는 거절되고, 메시지가
(2a)의 두 꼴을 보인다.]
#ex("spec", "10-word-value-bare.pnx")

#para("4b")[`forward` 로 선언한 프로시저는 정의된 뒤에만 값으로 준다.]
#ex("spec", "10-word-value-forward.pnx")

== 레일과 조합 프로시저 <control-rails>

#para("1")[#term("레일", en: "rail")는 차례로 값을 내는 것이다. 레일은 값이 아니어서 지역 변수에 넣지 못하고,
바로 뒤의 조합 프로시저가 받아야 한다. 둘이 합쳐 루프 하나가 된다. 훑기 전에 길이를 알고 어느 자리의 원소든 읽을 수 있는 레일을
#term("닫힌 레일", en: "closed rail")이라 하고(`range`, 배열, 목록, `str.bytes`), 그렇지 않은 레일을 #term("열린 레일", en: "open rail")이라 한다: 원천에서
조각을 차례로 하나씩 꺼내고 한 번만 훑는다(`str.split` `str.lines` `str.words` `str.chars` `ptn.matches` `rex.matches` `rex.split` `csv.fields`, 풀).
레일은 게으르게 평가된다: 루프는 훑는 차례대로 원소를 하나씩 꺼내고, 어댑터의 블록은 거기에 닿은 원소에만 돈다. 일찍 끝나는
훑기(`head` `head_while` `first` `any` `break`)는 그 뒤를 읽지도 평가하지도 않고, 단계 사이에 저장하는 것이 없다.
레일을 내는 것은 다음과 같다.
#table(columns: (auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*레일*], [*원소*],
  [배열, 목록의 이름], [칸의 값(레코드면 줄). 목록은 길이까지. 큐, 버퍼, 찾아보기는 레일이 아니다],
  [풀의 이름], [살아 있는 줄, 칸의 차례로. `each` 에만(8.9 (3a))],
  [`lo hi range`], [`lo` 부터 `hi` 앞까지의 `int`],
  [`s sep str.split`, `s str.lines`, `s str.words`, `s str.chars`], [`str` 뷰(16.1). `chars` 는 UTF-8 글자 하나씩],
  [`s str.bytes`], [`u8`: 바이트(16.1)],
  [`s pat ptn.matches`, `s re rex.matches`, `s re rex.split`], [맞은 부분(16.6, 16.7)],
  [`s csv.fields`], [필드(16.9)],
)]

#para("2")[조합 프로시저는 다음과 같다. "블록" 열은 블록이 받고 주는 것이다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*프로시저*], [*블록*], [*결과*],
  [`each`], [`(T)`], [없음],
  [`each_index`], [`(T, i int)`], [없음],
  [`fold`], [`(acc U, T gives U)`], [`U`. 첫 값을 레일 앞에 둔다: `0 xs do … end fold`],
  [`count`], [`(T gives bool)`], [`int`],
  [`any` `all`], [`(T gives bool)`], [`bool`. 답이 정해지면 일찍 끝난다],
  [`first`], [`(T gives bool)`], [`T or fail`. 없으면 `err.not_found`],
  [`sum`], [없음], [`int`(`dec N` 의 레일이면 `dec N`). 넘치면 결함 `overflow`],
  [`minimum` `maximum`], [없음], [`int or fail`(`dec N` 도 같다). 빈 레일이면 `err.not_found`],
  [`map_into`], [`(T gives U)`], [뒤에 목적지. `or fail`],
  [`filter_into`], [`(T gives bool)`], [뒤에 목적지. `or fail`],
  [`n times`], [`()`], [없음. 레일 없이 `n` 번],
)]

#para("1a")[#term("어댑터", en: "adapter")는 레일을 받아 레일을 준다. `skip N` 은 처음 N 개를 건너뛰고, `head N` 은 처음 N 개만 남기고,
`step N` 은 N 개마다 하나(첫째부터)를, `rev` 는 끝에서 처음으로 훑는다. 레일 뒤에 쓴 차례대로 적용되어 어댑터가 몇이든 루프는 하나다:
`scores skip 2 head 3` 은 셋째부터 다섯째까지, `scores head 3 skip 1` 은 둘째와 셋째다. `N` 은 `int` 를 하나 남기는 코드이고 쉼표, `do`,
다음 어댑터, `sum` `minimum` `maximum` 앞에서 끝난다. 루프 앞에서 한 번만 계산한다. `N` 이 음수(`step` 은 1 보다 작음)이면 결함 `range` 다.
`step` 과 `rev` 는 닫힌 레일에만 쓴다. `skip` 과 `head` 는 열린 레일에도 쓰고, 거기서는 조각이 오는 대로 센다. `step` 은 맨 끝에만 온다. 한 레일에 열여섯까지 쓴다.
`each_index` 는 어댑터 뒤의 레일을 아직 세지 않는다. `each` `fold` `count` `any` `all` `first` `map_into` `filter_into` `sum` `minimum` `maximum` 이 받는다.]
#ex("spec", "10-rail-adapters.pnx")
#ex("spec", "10-rail-adapter-step.pnx")

#para("1b")[`filter` `map` `skip_while` `head_while` 은 바로 앞에 쓴 블록을 받는다(`do to s s 4 gt end filter`). 블록은 원소를 받아
`bool` 을 내고(`filter` 는 참인 원소만 남기고, `skip_while` 은 참인 동안 버리고, `head_while` 은 처음 거짓인 원소에서 훑기를 끝낸다),
`map` 은 같은 타입의 값을 내어 원소 자리를 대신한다. 블록은 루프 안에서 원소마다 적은 차례로 돌고, 앞의 것이 버린 원소에는 돌지 않는다:
루프는 하나이고 저장소도 부름도 없다. 블록 어댑터는 닫힌 레일에도 열린 레일(`str.words`, `str.lines` …)에도 쓴다. 닫힌 레일에서는 `skip` `head` `step` `rev` 가
이들보다 앞에 오고, 열린 레일에서는 어디에든 와서 자기에게 닿은 조각을 센다. 블록이 원소에 붙이는 이름은 마지막 블록의 이름과 같아도 된다. 블록은 값을 내고 루프를 떠나지 않는다: 그 안의 `break` 와 `continue` 는 적재 때 거절된다(블록 안에 둔 자기 루프는 쓸 수 있다). 원소를 버리려면 `filter` 로, 훑기를 끝내려면 `head_while` 로 한다.]
#ex("spec", "10-rail-open.pnx")
#ex("spec", "10-rail-open-rev.pnx")
#ex("spec", "10-rail-lazy.pnx")

#ex("spec", "10-rail-block-adapters.pnx")
#ex("spec", "10-rail-block-late.pnx")

#para("1c")[`xs ys zip` 은 배열이나 목록 둘을 나란히 짧은 쪽까지 훑는다. 한 바퀴가 쌍을 주고, `each` 의 블록은 이름 둘을 맨 위부터 받는다:
`do to y to x … end each` 에서 `x` 는 `xs` 의, `y` 는 `ys` 의 칸이다. 칸은 `int` `bool` `err` `dec N` 이다. 쌍은 `each` 만 받고, 어댑터가 붙은 레일이나
범위는 아직 묶지 못한다.]
#ex("spec", "10-rail-zip.pnx")
#ex("spec", "10-rail-zip-range.pnx")

#para("1e")[레일은 한 바퀴에 값 여럿을 줄 수 있고(`zip` `json.items` `json.members` `toml.items` `md.blocks`) `each` 의 블록은 레일이 주는 수만큼 이름을
맨 위부터 받는다. `skip` 과 `head` 는 쓸 수 있고 블록이 든 어댑터는 쓰지 못한다. 훑는 도중 실패하는 레일이 있다(`json` `toml` `md` 의 레일과 `sqlite.rows`):
좋은 원소 몇 개 뒤에 글이 깨질 수 있고 엔진이 실패할 수 있다. 실패는 루프를 끝내고 프로시저 `each` 의 실패가 되어 여느 실패처럼 다룬다: `each` 뒤에 `or_fail`
`or_return` `or_else` 가 온다. 원소가 끝난 것은 실패가 아니다. `sheet` 책의 줄은 레일이 따로 필요 없다: `0 g sheet.rows range` 와 `0 g sheet.cols range` 가 번호를
훑는다.]

#ex("spec", "10-combinators.pnx")
#ex("spec", "10-first.pnx")

#para("3")[`map_into` 와 `filter_into` 는 새 저장소를 만들지 않고 목적지에 쓴다. 목적지가 목록이면 끝에
`push` 하고 아무것도 주지 않는다. 목적지가 배열이면 0 번 칸부터 쓰고 쓴 칸의 수를 준다(`gives n int or
fail`). 목적지가 차면 `err.full` 로 실패하고, 그때까지 쓴 것은 남는다. 목적지는 모음 매개변수여도 된다(9.9).]

#ex("spec", "10-map-into.pnx")

#para("4")[레일과 조합 프로시저는 루프 하나를 이루고, `each`, `each_index`, `times` 의 블록 안에서 `break .` 는 그 루프를
떠나고 `continue .` 는 다음 원소로 간다. 레일은 멈추고 조합 프로시저 뒤의 코드가 돈다. 루프를 떠나는
규칙이 그대로다: 스택은 블록이 시작할 때와 같고(10.2 (2)), 블록의 미룬 코드가 돈다(10.7). 블록 안의 `loop` 에 쓴
`break .` 는 그 루프를 떠난다. `break N .` 와 `continue N .` 는 이런 블록과 `loop` 를 모두 한 단계로 센다(10.2 (1)): 안쪽 블록의
연산 값들은 함께 떠난다. 모든 원소로 결과를 만드는 다른 조합 프로시저의 블록은 일찍 떠나지 못하고, 그 블록을 건너 떠나지도 못한다.]
#ex("spec", "10-leave-each.pnx")
#ex("spec", "10-leave-two.pnx")

== 미룬 코드 <control-defer>

#part("구문")
```ebnf
defer = "defer" { body } "end" ;
```

#para("1")[`defer 코드 end` 는 둘러싼 블록을 떠날 때 돌 코드를 적는다. 블록은 프로시저의 몸, 루프의 몸(한 바퀴마다),
조합 프로시저의 `do` 블록(한 번 돌 때마다)이다. 그 블록을 떠나는 모든 길에서 돈다: 블록의 `end`, 루프의 한 바퀴 끝,
`break`, `continue`, `return`, `or_return`, `or_fail`, `fail`. 여럿이면 적은 차례의 거꾸로
돈다. 결함으로 끝날 때는 돌지 않는다(그때는 상태가 핸들을 닫는다, 19.6).]

#ex("spec", "10-defer.pnx")

#para("2")[미룬 코드가 소유 값을 소비하면(`m release`, `f file.close`) 그 값은 미룬 코드가 맡는다. 블록을 떠나는
모든 길에서 소비된 것으로 치므로 실패 경로마다 닫는 코드를 따로 쓸 일이 없다. 블록 안에서는 그 값을 빌려 쓸 수
있다.]

#ex("spec", "10-defer-owned.pnx")

#part("제약")

#para("3")[`defer` 는 프로시저의 몸, 루프의 몸, `do` 블록에 바로 쓴다. `if`·`match`·`or_else` 의 가지 안이나 정의 밖에
쓰면 거절된다.]
#ex("spec", "10-defer-branch.pnx")

#para("4")[미룬 코드는 스택을 받은 그대로 두고 아무것도 남기지 않는다. `return` `fail` `break` `continue` `to` `set`
`loop` `do` `match` `defer` 와 `or_fail` `or_return` 을 쓸 수 없다. 실패하는 프로시저는 그 안에서 `or_else` 로 다룬다.]

#para("5")[미룬 코드가 맡은 소유 값을 블록 안에서 소비하면 거절된다.]
#ex("spec", "10-defer-taken.pnx")

#part("진단")
#diag(
  ([두 갈래의 스택이 다름], "the two branches of this if leave different stacks"),
  ([한 바퀴가 스택을 바꿈], "one turn of a loop must leave the stack as it found it"),
  ([루프 밖의 `break` `continue`], "break and continue belong inside a loop"),
  ([결과를 주는 프로시저의 블록 안의 `break`], "only the block of each, each_index or times is left early; this procedure gives a result made from every element"),
  ([`do` 블록 밖 루프로의 `break N .`], "a named loop outside this block is not left from inside it; break leaves the block"),
  ([닿지 않는 코드], "this code cannot be reached: the code before it always leaves"),
  ([`match` 의 같은 값], "this value is already in an earlier branch of the case"),
  ([펼쳐지는 블록이 스택을 바꿈], "this block must leave the stack as it found it"),
  ([정의 밖의 `return`], "return belongs inside a definition"),
  ([상수가 아닌 가지의 값], "a case value is a literal or the name of a const"),
  ([레일이 아닌 것에 조합 프로시저], "only an array, a list or a pool is a rail (not a queue, a buffer or an index)"),
  ([가지 안의 `defer`], "defer is written right in a procedure's body, a loop's body or a do block, not in a branch"),
  ([정의 밖의 `defer`], "defer belongs inside a definition"),
  ([미룬 코드가 스택을 바꿈], "a defer leaves the stack as it found it"),
  ([미룬 코드 안의 떠나는 프로시저], "a defer holds no return, fail, break, continue, to, set, loop, do, match or defer"),
  ([미룬 코드 안의 `or_fail` `or_return`], "a defer does not leave the procedure; a failure in it is taken with or_else"),
  ([미룬 코드가 맡은 값을 소비], "a defer gives this owned value when the block is left; here it can only be lent"),
  ([서명이 없는 값이 되는 블록에서 값이 모자람], "a block without a signature takes nothing; a procedure of the program is given as ref NAME, any other procedure with the block's signature: def str, str gives bool do str.starts_with end"),
  ([정의 전의 forward 프로시저를 값으로], "a procedure declared with forward is given as a value only after its definition"),
)

== 문장의 끝 <control-ends>

#para("1")[코드에 쓴 쉼표나 점(서명이나 `args … .` 안이 아닌 곳)은 #term("문장", en: "statement")을 끝낸다:
마지막 쉼표나 점 뒤의 코드다. 둘 다 스택을 바꾸지 않고, 둘 다 꼭 써야 하는 것은 아니다. 둘이 없는 코드는 전과 같은 뜻이다.]

#para("2")[쉼표는 아무것도 기다리지 않는다: 다음 문장이 시작할 때 이 문장의 효과가 아직 진행 중일 수 있다.]

#para("3")[점은 #term("장벽", en: "barrier")이다: 점 앞에서 시작한 모든 효과가 점 뒤의 무엇보다 먼저 끝난다. 처리기의
몫은 호스트가 `pinax_on_barrier`(19.4 (3))로 준 함수를 부르는 것이고, 호스트의 몫은 호스트 프로시저가 붙들고 있는 것(표준
출력, 파일에 쓴 것)을 내보내고 뒤에서 시작한 일을 기다리는 것이다. 적재한 코드의 끝과 세션 줄의 끝은 점이다. 결함으로
끝나도 그렇다. 데이터로 적재한 소스에서 둘은 문장을 나눌 뿐 아무것도 부르지 않는다.]

#para("4")[문장이 결과를 써야 하는 효과(읽기, 명령의 출력)는 닿은 자리에서 끝낸다. 두 점 사이에서 같은 자원에 대한 효과는
프로그램의 순서를 지키고, 다른 자원에 대한 효과는 겹칠 수 있다. 이 판은 모든 효과를 프로그램의 순서대로 돌린다. 겹침은 뒤의
판이 쓸 수 있는 여지다.]

#ex("spec", "10-statements.pnx")
