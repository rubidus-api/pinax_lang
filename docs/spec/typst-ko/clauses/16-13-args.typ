#import "/tools/common.typ": *

== `args` --- 여러 종류의 값 묶음 <std-args>

#part("구문")
```ebnf
args-value = "args" { body } "." ;              (* each body leaves one value, 9.8 *)
param      = NAME "args" ;                      (* a parameter that takes an args value *)
```

#para("1")[모듈 `args` 와 타입 `args` 는 핵에 든다. 이 절의 다른 모듈과 달리 `args` 의 프로시저는 `use` 없이 `args.프로시저` 로 쓴다.
#term("args 값", en: "args value")은 종류가 다른 값 몇 개를 담는다. 값의 수가 달라지는 프로시저는 그런 값 하나를 매개변수
`이름 args` 로 받는다(`def show label str, rest args gives str do … end`).]

#para("2")[`args 값1 값2 … .` 는 `args` 와 마침표 사이의 코드가 남기는 값들로 args 값을 만든다. 항목마다 값 하나를 남긴다.
값은 `int` `bool` `str` `dec N` `float` 이고, 다른 타입의 수나 레코드, 모음은 들지 못한다. args 값은 1 개에서 64 개까지 담는다.
값은 구역(12.2)에 복사된다: `str` 은 바이트까지 복사하므로 args 값은 글이 어디 있었는지에 기대지 않는다. `cat` 바로 앞에 쓰면
값을 만들지 않고 9.8 대로 `args a b . cat` 이 이어 붙인다.]

#para("3")[args 값은 호출 아래로만 산다: 매개변수다. 지역 변수나 전역에 담지 못하고, 결과로 돌려주지 못하고, 다른 args 의
값이 되지 못한다. `args` 를 받는 프로시저에는 그대로 넘기고(`rest inner`), `rest cat` 은 `args … . cat` 과 같게 값을 쓴다:
`str` 은 그대로, `int` 는 십진으로, `bool` 은 `true` `false` 로, `dec N` 은 `N` 자리를 모두, `float` 는 `str.to_str` 처럼(7.7).]

#ex("spec", "16-args.pnx")

#para("4")[아래 프로시저는 args 값을 읽는다. 값의 번호(0 이 처음)를 먼저, args 를 맨 뒤에 준다. 종류는 상수 `args.k_int` 1,
`args.k_bool` 2, `args.k_str` 3, `args.k_dec` 4, `args.k_float` 5 다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`len`], [`(a args gives int)`], [값의 수],
  [`kind`], [`(i int, a args gives int or fail)`], [`i` 번 값의 종류],
  [`int`], [`(i int, a args gives int or fail)`], [`i` 번 값이 `int` 이면 그 값],
  [`bool`], [`(i int, a args gives bool or fail)`], [`i` 번 값이 `bool` 이면 그 값],
  [`str`], [`(i int, a args gives str or fail)`], [`i` 번 값이 `str` 이면 그 글: args 값 안에 복사된 바이트의 뷰],
  [`dec`], [`(i int, N int, a args gives dec N or fail)`], [`i` 번 값이 `dec N` 이면 그 값. `N` 은 0 부터 18 의 상수],
  [`float`], [`(i int, a args gives float or fail)`], [`i` 번 값이 `float` 이면 그 값],
)]

#ex("spec", "16-args-read.pnx")
#ex("spec", "16-args-float.pnx")

#para("5")[값의 종류가 다르거나 `dec` 의 자리가 `N` 과 다르면 `err.format` 으로 실패한다. 번호가 음수이거나 `len` 이상이면
`err.not_found` 로 실패한다.]

#ex("spec", "16-args-pass.pnx")

#part("제약")

#para("6")[`args` 는 매개변수일 뿐이다. 지역 변수나 전역에 담거나, `gives` 로 돌려주거나, 다른 args 에 넣거나, 호스트 프로시저나
`extern` 에 주면 거절된다. 값이 없는 `args` 도 거절된다.]
#ex("spec", "16-args-stored.pnx")
#ex("spec", "16-args-nested.pnx")

#part("결함")

#para("7")[args 값은 구역에 만든다. 구역이 모자라면 결함 `full` 이다(12.2 (5)).]
#ex("spec", "16-args-full.pnx")

#part("진단")
#diag(
  ([args 를 담거나 돌려줌], "an args is only a parameter: it is not stored in a local or a global, and not given back"),
  ([args 안의 args], "an args holds no args: its values are int, bool, str, dec N and float"),
  ([int bool str dec N float 이 아닌 값], "an args holds int, bool, str, dec N and float values"),
  ([args 의 프로시저에 값의 차례가 거꾸로], "this procedure takes the number of the value, and then the args: 0 rest args.int"),
  ([`args.dec` 의 자리가 상수가 아님], "the digits of args.dec are a constant 0 to 18: 2 rest args.dec"),
  ([앞에 args 가 없는 `cat`], "cat takes args: args a b . cat, or an args parameter"),
)
