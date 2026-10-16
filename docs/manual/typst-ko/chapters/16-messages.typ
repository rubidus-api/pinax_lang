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

== 이름

#table(columns: (1.3fr, 1fr, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*메시지*], [*뜻과 고치는 법*], [*장*]),
  [`this name is not declared before this place`],
  [없는 이름이거나, 그 이름을 쓰기 전에 선언하지 않았다. 철자와 선언의 자리를 본다. 서로 부르는 낱말은 `forward word`.],
  [1, 3],
  [`this path names neither a module in use nor a record`],
  [`모듈.낱말` 의 모듈을 열지 않았다. 파일 위에 `use 모듈` 을 쓴다.], [1, 10],
  [`this name is already visible here; a new name may not hide it`],
  [이미 보이는 이름(핵 낱말 `sum`, `count`, `first` 따위도)을 새 이름으로 썼다. 다른 이름을 고른다.], [3],
  [`outside a definition `+"`to`"+` makes no new local; a global is written with set`],
  [정의 밖에서 `to` 를 썼다. 정의 밖에는 지역 변수가 없다. 일을 낱말로 묶거나 전역에 `set` 으로 넣는다.], [4],
  [`this name is a global; a global is written with set: value set name`],
  [전역이나 전역 모음의 칸에 `to` 로 넣으려 했다. `값 set 이름`, 칸은 `값 i set 이름`.], [4, 6],
)

== 실패와 한 번만 쓰는 값

#table(columns: (1.3fr, 1fr, auto), inset: 5pt, stroke: 0.5pt + luma(170),
  table.header([*메시지*], [*뜻과 고치는 법*], [*장*]),
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
