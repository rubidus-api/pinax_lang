#import "/tools/common.typ": *

== `sheet` --- 줄과 열의 표 <std-sheet>

#para("1")[모듈 `sheet` 는 프로그램이 선언한 버퍼(8.7) 안에 줄과 열의 표를 둔다. 자료형이 아니라 라이브러리다. 버퍼는
`init` 이 표로 만들고, `sheet` 의 모든 낱말은 모음 매개변수(9.9)처럼 그 버퍼를 맨 끝에 받는다. 칸에는 아무것도 없거나,
프로그램이 넣은 `int`, `bool`, `dec(N)`, `str` 이 들어 있다. 칸의 종류는 모듈 안에서만 알고, 값은 그 형의 낱말(`int`,
`str` ...)로만 칸에서 나온다. 그래서 모든 낱말의 서명은 정해져 있고, 스택에 형이 없는 값이 오르는 일이 없다.]

#para("2")[줄과 열은 0부터 센다. 줄은 `add_row` 가 끝에 더하고, 표는 `init` 이 받은 `Rmax` 줄까지 가진다. 열의 수는 `init` 이
정하고, `widen` 이 같은 버퍼 안에서 표를 다시 만들어 바꾼다. 칸에 넣는 값은 복사된다. 같은 값은 하나만 둔다: 칸에 들어가지
않는 큰 수와 모든 글은 그것을 쥔 칸의 수와 함께 풀에 놓이고, 덮어쓰거나 비우거나 잘려 나간 칸은 값을 돌려준다. 풀에 자리가
없으면 `err.full` 이고, 흩어진 글의 빈자리는 새 글이 필요로 할 때(와 `compact` 가 불릴 때) 모은다. `sheet` 는 번호로 읽고 쓰며
들어 있는 값으로 찾는 표를 위한 것이다. 이름으로 찾는 것은 색인(8.10)이 맡는다.]

=== 만들기와 늘리기

#para("3")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`init`], [`(rmax int, cols int, b buffer or fail)`], [버퍼를 줄이 최대 `rmax`, 열이 꼭 `cols` 인 표로 만든다(처음엔 줄 0개). 버퍼가 모자라면 `err.full`. 그 뒤 버퍼는 가득 찬 것으로 읽혀, 표 위에 덧붙일 수 없다],
  [`rows` `cols` `rows_max`], [`(b buffer gives int)`], [지금의 줄 수, 열 수, 최대 줄 수],
  [`add_row`], [`(b buffer gives int or fail)`], [빈 줄을 더하고 그 번호를 준다. `rmax` 에 닿으면 `err.full`],
  [`truncate_rows`], [`(n int, b buffer)`], [앞의 `n` 줄만 남기고 나머지가 쥔 값을 돌려준다. 있는 줄보다 많으면 결함 `range`],
  [`widen`], [`(cols int, b buffer or fail)`], [줄과 값을 지키며 열이 `cols` 인 표로 다시 만든다. 열을 늘리면 버퍼에 자리가 있어야 하고(`err.full`), 줄이면 잘리는 열이 모두 비었을 때만 된다(`err.format`)],
  [`clear` `compact`], [`(b buffer)`], [모든 줄을 지운다. 글의 빈자리를 모은다],
)]

=== 칸

#para("4")[줄과 열을 받는 낱말은 칸이 표 밖이면 결함 `range` 다. 표가 아닌 버퍼도 같은 결함이다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`put_int` `put_bool`], [`(v int, row int, col int, b buffer or fail)` `(v bool, row int, col int, b buffer or fail)`], [칸에 복사해 넣는다. 풀이 가득이면 `err.full`],
  [`put_dec`], [`(v dec(N), row int, col int, b buffer or fail)`], [자릿수와 함께 넣는다],
  [`put_str`], [`(s str, row int, col int, b buffer or fail)`], [글을 복사해 넣는다. 이 표 안을 보는 view 를 줘도 된다],
  [`clear_cell`], [`(row int, col int, b buffer)`], [칸을 비운다],
  [`copy_cell`], [`(row1 int, col1 int, row2 int, col2 int, b buffer)`], [칸을 다른 칸에 복사한다. 값은 나눠 쥔다],
  [`int` `bool`], [`(row int, col int, b buffer gives int or fail)` `(... gives bool or fail)`], [값. 다른 종류의 칸이면 `err.format`],
  [`dec`], [`(row int, col int, N int, b buffer gives dec(N) or fail)`], [자릿수 `N`(0에서 18의 상수)의 decimal. 종류나 자릿수가 다르면 `err.format`],
  [`str`], [`(row int, col int, b buffer gives str or fail)`], [표 안의 글을 보는 view. 그 표에 다음에 쓸 때까지 유효. 다른 종류면 `err.format`],
  [`kind` `is_empty`], [`(row int, col int, b buffer gives int)` `(... gives bool)`], [종류(`sheet.k_empty` 0, `k_int` 1, `k_bool` 2, `k_dec` 3, `k_str` 4). 비었는가],
)]

=== 찾기와 고르기

#para("5")[찾기는 수는 수로, 글은 적힌 대로 견준다. 한 열을 아래로, 또는 한 줄을 옆으로 훑는다. 어느 칸에도 없는 글은 바로
없다고 알아낸다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`find_row_int` `find_row_str`], [`(key int, col int, b buffer gives int or fail)` `(key str, col int, ...)`], [`col` 열이 `key` 인 첫 줄. 없으면 `err.not_found`. 스프레드시트의 `MATCH` 다. 값은 그 뒤 `int`, `str` ... 로 읽는다(`VLOOKUP` 처럼 형 없는 값을 돌려주는 낱말은 없다)],
  [`find_col_int` `find_col_str`], [`(key int, row int, b buffer gives int or fail)`], [`row` 줄을 옆으로 훑어 첫 열],
  [`find_row_from_int` `find_row_from_str` `find_col_from_int` `find_col_from_str`], [`(key, line int, from int, b buffer gives int or fail)`], [같되 줄이나 열 `from` 부터],
)]

#para("6")[표는 #term("선택", en: "selection")을 가진다: 줄의 목록이고, 처음엔 모든 줄이다. `where_*` 는 모든 줄에서 다시 시작해, 그 열의 값이 `v` 와
관계 `op`(`sheet.eq` `ne` `lt` `le` `gt` `ge`)인 줄만 남기고, `narrow_*` 는 지금 선택된 줄 가운데 그런 줄만 남긴다. 종류가
다른 칸과 빈 칸은 어떤 관계도 아니다. 낱말마다 표의 선택에 일하므로 낱말을 늘어놓으면 곧 파이프다. 선택은 버퍼 안의 한 자리이고
버퍼와 함께 사라진다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`where_int` `narrow_int`], [`(col int, op int, v int, b buffer)`], [수로 고르거나 좁힌다],
  [`where_str` `narrow_str`], [`(col int, op int, v str, b buffer)`], [글로(바이트 순서) 고르거나 좁힌다],
  [`select_all`], [`(b buffer)`], [모든 줄을 고른다],
  [`sel_len`], [`(b buffer gives int)`], [고른 줄의 수],
  [`sel_row`], [`(k int, b buffer gives int)`], [`k` 번째로 고른 줄의 번호. 범위 밖이면 결함 `range`],
  [`order_by` `order_by_desc`], [`(col int, b buffer or fail)`], [고른 줄을 `col` 열의 순서로, 작은(큰) 것부터 안정적으로 놓는다. 수, 글, 나머지 차례. 수를 놓을 자리는 버퍼의 빈 끝에서 가져온다(`err.full`)],
)]

#para("7")[합계는 int 칸만 본다. `sum_*` 는 합이 넘치면 결함 `overflow`, `min_*` 와 `max_*` 는 수가 없으면 `err.not_found` 로
실패한다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`sum_col` `min_col` `max_col` `count_col`], [`(col int, b buffer gives int)`], [`col` 열의 모든 줄. `min` 과 `max` 는 `or fail`],
  [`sum_row` `min_row` `max_row` `count_row`], [`(row int, b buffer gives int)`], [`row` 줄의 모든 열],
  [`sum_sel` `min_sel` `max_sel` `count_sel`], [`(col int, b buffer gives int)`], [고른 줄의 `col` 열],
)]

#ex("spec", "16-sheet.pnx")
#ex("spec", "16-sheet-pipe.pnx")

#part("제약")

#para("8")[마지막 인자는 버퍼이고, 나머지는 위 서명의 형이다.]
#ex("spec", "16-sheet-args.pnx")

#part("결함")

#para("9")[표 밖의 줄이나 열, `init` 으로 만들지 않았거나 그 뒤 바뀐 버퍼, 넘치는 합은 결함이다.]
#ex("spec", "16-sheet-range.pnx")

#part("진단")
#diag(
  ([`sheet` 의 낱말에 다른 형의 값], "this word of sheet takes int for row, column and number, str for text, bool for put_bool, dec(N) for put_dec"),
  ([`sheet` 의 낱말인데 끝에 버퍼가 없음], "the words of sheet take a buffer last: v row column grid sheet.put_int"),
)
