#import "/tools/common.typ": *

== `fmt` --- 수를 글로 <std-fmt>

#para("1")[`cat` 은 `int` 를 십진으로 넣는다(9.8). 다른 모양은 모듈 `fmt` 의 낱말로 `str` 을 만든다. 형식을 적는
작은 언어(`%08x` 같은 것)는 없다. 결과는 구역에 쓴다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`hex` `bin`], [`(int gives str)`], [십육진(소문자), 이진. 음수는 64 비트 2 의 보수로],
  [`hex_pad`], [`(n int, width int gives str)`], [십육진을 앞에 0 을 채워 `width` 자리로],
  [`zero_pad`], [`(n int, width int gives str)`], [십진을 앞에 0 을 채워 `width` 자리로. 부호도 자리를 차지한다],
  [`append_hex`], [`(n int, buf or fail)`], [십육진을 버퍼에 붙인다],
)
십진은 `str.to_str`, 버퍼에 붙이는 십진은 `append_int`(15.3)다. `hex_pad` 와 `zero_pad` 의 `width` 가 음수이거나
2#super[32] 이상이면 결함 `range`, 구역이 모자라면 결함 `full` 이다.]

#ex("spec", "16-fmt.pnx")

#part("제약")

#para("2")[`fmt` 의 낱말은 `int` 를 받는다. 다른 타입의 값은 거절된다. `dec(N)` 은 `str.to_str` 과 `cat` 이 적는다(7.5).]
#ex("spec", "16-fmt-type.pnx")

#part("결함")

#para("3")[`hex_pad` 와 `zero_pad` 의 `width` 가 음수이거나 2#super[32] 이상이면 결함 `range` 다.]
#ex("spec", "16-fmt-width.pnx")

#para("4")[결과가 구역에 들어가지 않으면 결함 `full` 이다. 아래의 폭은 허용되는 수이지만 구역보다 크다.]
#ex("spec", "16-fmt-full.pnx")

#part("진단")
#diag(
  ([`int` 가 아닌 값], "this word takes an int"),
)
