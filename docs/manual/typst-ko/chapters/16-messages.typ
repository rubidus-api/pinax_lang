#import "/tools/common.typ": *
// shape: a reference chapter: its examples are those of the chapters each row points to

= 자주 보는 메시지 <messages>

#opening[1장부터 15장까지. 찾아보는 장이다.][
- 처리기가 내 프로그램을 거절했다. 메시지는 무슨 뜻이고 어떻게 고치는가?
- 돌다가 멈췄다. 결함의 이름은 무슨 뜻인가?
]

메시지는 `파일:줄:칸: 까닭` 의 꼴이다. 줄과 칸은 1 부터 세고, 칸은 그 줄의 바이트 자리다. 처리기는 처음 만난 잘못 하나만
알리므로, 고친 뒤 다시 돌리면 다음 잘못이 보일 수 있다. 아래 표의 메시지는 처리기가 내는 그대로이고, 오른쪽 칸의 장에
그 메시지를 내는 예제가 있다.

== 스택과 타입

#table(columns: (1.3fr, 1fr, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*메시지*], [*뜻과 고치는 법*], [*장*]),
  [`an argument has another type than the word's signature asks for`],
  [낱말에 준 값의 타입이 서명과 다르다. 서명을 보고 값을 바꾸거나 차례를 고친다.], [3],
  [`this word takes a str`, `this word takes an int`],
  [그 낱말은 그 타입 하나만 받는다. 수를 글로 바꾸려면 `str.to_str`, 글을 수로 바꾸려면 `str.to_int`.], [5],
  [`the two values compared must have the same type`],
  [`eq` `lt` 같은 비교에 타입이 다른 둘이 왔다. 한쪽을 바꾼다. `int` 와 `dec` 도 다른 타입이다.], [2],
  [`two dec values of different scales: rescale one first (d 2 rescale)`],
  [`4.20` 과 `0.5` 처럼 자리 수가 다른 소수다. 한쪽을 `rescale` 하거나 같은 자리 수로 적는다.], [2],
  [`a declared type is added to and subtracted from its own type, and multiplied, divided or taken modulo by an int; for other arithmetic take raw`],
  [선언한 타입(`cents` 같은)에 허용되지 않는 산술이다. `raw` 로 바탕 정수를 꺼내 계산한다.], [10],
  [`the two branches of this if leave different stacks`],
  [`if` 의 두 가지가 다른 개수나 타입을 남긴다. 둘 다 같은 것을 남기게 한다.], [4],
  [`this block must leave the stack as it found it`],
  [`each` 같은 조합 낱말의 블록이 값을 남겼다. 블록 안에서 쓰거나 `drop` 한다.], [4],
  [`stored text is str(N); a str view lives only in locals, arguments and results`],
  [전역이나 칸이나 필드에 `str` 를 두려 했다. 오래 사는 자리에는 `str(N)` 을 쓴다.], [5],
  [`the stack does not hold enough values for this word`],
  [낱말이 가져갈 값이 스택에 모자란다. 값을 먼저 올렸는지, 차례가 맞는지 본다. `pnx see` 로 줄마다의 스택을 볼 수 있다.], [2, 15],
  [`values are left on the stack at the end of the source`],
  [프로그램 끝에 아무도 가져가지 않은 값이 있다. 출력하거나 `drop` 하거나, 빠뜨린 낱말을 넣는다.], [2],
  [`the stack does not hold exactly the results of the signature here`],
  [낱말의 몸이 끝날 때 남은 값이 서명의 결과와 다르다. 결과를 서명에 적거나, 남은 값을 정리한다.], [3],
  [`this word takes two int values, or two of the same declared type`],
  [산술 낱말에 다른 타입이 왔다. 글을 수로 바꾸려면 `str.to_int`, 수를 글로 바꾸려면 `str.to_str`.], [2],
  [`an if without else must leave the stack as it found it`],
  [`else` 없는 `if` 가 값을 남긴다. `else` 갈래를 더해 같은 타입의 값을 남긴다.], [4],
  [`one turn of a loop must leave the stack as it found it`],
  [루프의 한 바퀴가 값을 남기거나 가져간다. 만든 값은 `to 이름` 으로 지역 변수에 담는다.], [4],
  [`inputs are all named or all bare types, not a mix`],
  [서명에서 이름 있는 입력과 타입만 쓴 입력을 섞었다. 모두 이름을 붙인다.], [3],
)

== 글자와 리터럴

#table(columns: (1.3fr, 1fr, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*메시지*], [*뜻과 고치는 법*], [*장*]),
  [`this character is not allowed outside literals and remarks`],
  [`+` `;` `{` 같은 기호를 코드에 썼다. 산술과 구분은 모두 낱말이다: `add`, `mul`, 줄바꿈.], [1],
  [`a sign is attached to its number; arithmetic uses words (add, sub)`],
  [`3 - 1` 처럼 적었다. `-` 는 수에 붙는 부호일 뿐이다. `3 1 sub` 로 적는다.], [2],
  [`unknown escape; a raw string r"..." keeps backslashes as written`],
  [보통 문자열에 `\d` 같은 이스케이프가 있다. 패턴은 날 문자열 `r"…"` 에 적는다.], [12],
  [`rx takes a string literal or a const name; a pattern made while running is rex.compile`],
  [`rex.rx` 에 변수를 주었다. 패턴은 리터럴이나 `const` 다. 돌 때 만든 패턴은 `rex.compile`.], [12],
)

== 이름

#table(columns: (1.3fr, 1fr, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*메시지*], [*뜻과 고치는 법*], [*장*]),
  [`no module of this name is registered or loaded`],
  [`use` 한 모듈이 없다. 표준 모듈과 호스트 모듈의 이름을 보고, 파일 모듈이면 프로그램 옆에 `이름.pnx` 가 있는지 본다.], [10],
  [`the module exports nothing of this name`],
  [모듈에 그 이름이 없거나 `export` 하지 않았다. 모듈 파일에서 `export` 를 붙인다.], [10],
  [`this name is already defined`],
  [같은 이름의 낱말이나 선언이 이미 있다. 다른 이름을 쓴다. 대화에서는 `redefine word` 로 바꾼다.], [3, 15],
  [`this parameter takes one row of a table; a parameter that takes the whole table is written list RECORD or array RECORD`],
  [표의 이름을 매개변수 타입으로 쓰면 줄 하나다. 표 전체를 받으려면 `list 레코드` 나 `array 레코드`.], [6],
  [`the collection given here is of another kind, or holds another type, than the parameter says`],
  [낱말에 준 모음이 매개변수의 종류나 원소 타입과 다르다. `list int` 자리에 `array int` 나 `list str(8)` 을 줄 수 없다.], [6],
  [`this name is not declared before this place`],
  [없는 이름이거나, 그 이름을 쓰기 전에 선언하지 않았다. 철자와 선언의 자리를 본다. 서로 부르는 낱말은 `forward word`.],
  [1, 3],
  [`this path names neither a module in use nor a record`],
  [`모듈.낱말` 의 모듈을 열지 않았다. 파일 위에 `use 모듈` 을 쓴다.], [1, 10],
  [`this name is already visible here; a new name may not hide it`],
  [프로그램의 이미 보이는 이름(낱말, 전역, 상수, 바깥의 지역 변수)을 새 이름으로 썼다. 다른 이름을 고른다.], [3],
  [`NAME is a word of the language; choose another name`],
  [언어의 낱말 --- 예약어, `sum`, `count`, `first` 같은 핵 낱말, `sort` 같은 낱말, 타입 --- 을 새 이름으로 썼다. 어디서
   이름을 만들었든 같다. 다른 이름을 고른다.], [3],
  [`rows do not move: sort a list of their numbers by a field: order table sort_by record.field`],
  [`sort` 에 레코드의 표를 주었다. 줄(`list order cities 8`)이나 줄 번호로 목록을 채우고 `sort_by` 로 정렬한다.], [7],
  [`outside a definition `+"`to`"+` makes no new local; a global is written with set`],
  [정의 밖에서 `to` 를 썼다. 정의 밖에는 지역 변수가 없다. 일을 낱말로 묶거나 전역에 `set` 으로 넣는다.], [4],
  [`this name is a global; a global is written with set: value set name`],
  [전역이나 전역 모음의 칸에 `to` 로 넣으려 했다. `값 set 이름`, 칸은 `값 i set 이름`.], [4, 6],
)

== 실패와 한 번만 쓰는 값

#table(columns: (1.3fr, 1fr, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*메시지*], [*뜻과 고치는 법*], [*장*]),
  [`an owned value in a local is not given on every path (close, free or store it)`],
  [한 번만 쓰는 값(열린 파일, 풀의 줄, 표시)을 어느 길에서 넘겨주지 않았다. 실패해서 떠나는 길과 `return` 도 본다. `defer` 가 쉽다.], [9],
  [`this local gave its owned value away; it holds nothing to read`],
  [넘겨준 뒤에 그 이름을 다시 썼다. 닫은 파일은 다시 쓰지 못한다.], [9],
  [`a defer gives this owned value when the block is left; here it can only be lent`],
  [`defer` 에 맡긴 값을 몸에서 또 넘겨주었다(닫았다). 닫는 일은 `defer` 에 맡기고 몸에서는 빌려 쓴다.], [9],
  [`this str was made after a mark that was released since; the region may hold other bytes there now`],
  [`release` 로 되돌린 뒤에 그 사이 만든 글을 읽었다. 남겨야 할 글은 되돌리기 전에 `str(N)` 이나 버퍼에 넣는다.], [5, 9],
  [``or_fail passes the failure on, so this word needs `or fail` in its signature``],
  [`or_fail` 을 쓴 낱말의 서명에 `or fail` 이 없다. 서명에 적거나, `or_else` 로 그 자리에서 다룬다.], [8],
  [`this list's rows are kept by a field, a global, a collection, a named row or an index; it cannot be truncated`],
  [줄을 누가 들고 있는 목록은 `truncate` 하지 못한다. 더하기 전에 확인하는 쪽으로 바꾼다.], [11],
  [`a word that can fail is followed by or_fail, or_return or or_else`],
  [실패할 수 있는 낱말(`push`, `str.to_int`, 서명에 `or fail` 이 있는 낱말) 뒤에 지킴 낱말이 없다. 무시하려 해도
   `or_else (e) end` 라고 적는다.], [6, 8],
  [`the body of or_else must leave what the word would have given, or leave the word or the loop`],
  [`or_else` 의 몸이 성공했을 때의 결과와 다른 값을 남긴다. 같은 타입의 대신할 값을 남기거나, `return`·`break` 로 떠난다.],
  [8],
  [`an owned value is kept in a local or given on at once; this word would lose it`],
  [`owned` 값을 `drop` 하거나 스택에 둔 채 잃으려 한다. 지역 변수에 담고, 끝에 닫는다(`defer` 를 쓰면 쉽다).], [9],
  [`a field holds str(N); a str view lives only in locals, arguments and results`],
  [필드나 전역에 뷰 `str` 를 두려 했다. `str(N)` 으로 선언한다.], [5],
)

== 돌다가 멈출 때: 결함

적재를 지난 프로그램도 돌다가 #strong[결함]으로 멈출 수 있다. `pnx` 는 결함의 까닭과 줄, 그 낱말의 지역 변수를 보인다.

#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*결함*], [*`pnx` 가 쓰는 말*], [*뜻*]),
  [`overflow`], [`integer overflow`], [정수가 64 비트를 넘었다(2장). `chk_` 낱말은 이것을 실패로 준다.],
  [`div_zero`], [`division by zero`], [0 으로 나눴다.],
  [`range`], [`index out of range`], [칸의 번호가 길이 밖이거나, "없음" 줄의 필드를 썼다(6, 7장).],
  [`full`], [`a declared capacity is used up`], [구역이나 저장된 글의 자리가 모자란다(5장).],
  [`depth`], [`call depth limit`], [부름이 너무 깊다. 끝나지 않는 되부름인지 본다(3장).],
  [`stale`], [`a row or handle was used after it was given back`], [돌려준 풀의 줄이나 닫은 핸들을 썼다(7, 9장).],
  [`deadlock`], [`deadlock: waiting for a queue that nothing can change`], [모든 작업이 큐를 기다린다(13장).],
)

#qa[결함을 스크립트 안에서 잡을 수는 없는가?][없다. 결함은 프로그램을 고쳐야 할 자리를 알리는 것이다. 바깥에서 온 자료
때문에 일어날 수 있는 일이면, 결함 대신 실패를 주는 낱말(`chk_mul`, `str.to_int` 따위)을 쓰고 `or_else` 로 다룬다(8장).]

#summary[
- 거절 메시지는 처음 만난 잘못 하나를 `파일:줄:칸` 과 함께 알린다(표준 4).
- 스택, 이름, 실패, `owned` 의 잘못은 모두 돌기 전에 드러난다(표준 4).
- 결함은 돌 때에만 알 수 있는 잘못이고, 실행을 멈춘다(11.3).
]
