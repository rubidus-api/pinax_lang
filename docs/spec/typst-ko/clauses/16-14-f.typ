#import "/tools/common.typ": *

== `f` --- 부동소수점 수의 계산 <std-f>

#para("1")[모듈 `f` 는 `float`(7.7)의 프로시저다. `use f .` 로 연다. 산술 네 프로시저는 정확한 값을 가장 가까운 `float` 로
한 번 반올림한다(가운데이면 짝수 쪽). 어느 프로시저도 결함을 내지 않는다(`f.to_int` 만 뺀다): NaN 과 무한대는 결과로
넘어간다(7.7 (2)).
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*프로시저*], [*서명*], [*뜻*],
  [`add` `sub` `mul` `div`], [`(a float, b float gives float)`], [`a + b`, `a - b`, `a × b`, `a ÷ b`. 0 으로 나누면 무한대나 NaN],
  [`neg`], [`(a float gives float)`], [부호를 바꾼다. NaN 은 NaN],
  [`abs`], [`(a float gives float)`], [부호를 없앤다],
  [`near`], [`(a float, b float, eps float gives bool)`], [`a eq b` 이거나 `a b f.sub f.abs` 가 `eps` 이하이면 참. `a` 나 `b` 가 NaN 이면 거짓],
  [`is_nan`], [`(a float gives bool)`], [`a` 가 NaN 이면 참],
  [`is_finite`], [`(a float gives bool)`], [`a` 가 무한대도 NaN 도 아니면 참],
  [`from_int`], [`(n int gives float)`], [`n` 에 가장 가까운 `float`(가운데이면 짝수 쪽)],
  [`to_int`], [`(a float gives int)`], [0 쪽으로 자른 `int`. NaN 이나 범위 밖이면 결함 `overflow`],
)]

#ex("spec", "16-f.pnx")

#part("진단")
#diag(
  ([`float` 가 아닌 값의 `f.neg` `f.abs`], "f.neg and f.abs take a float"),
  ([`float` 가 아닌 값의 `f.near`], "f.near takes three float values: a b eps f.near"),
  ([`float` 가 아닌 값의 `f.is_nan` `f.is_finite`], "f.is_nan and f.is_finite take a float"),
  ([`int` 가 아닌 값의 `f.from_int`], "f.from_int takes an int (str.to_float reads a str)"),
  ([`float` 가 아닌 값의 `f.to_int`], "f.to_int takes a float (str.to_int reads a str)"),
)
