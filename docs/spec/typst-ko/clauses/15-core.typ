#import "/tools/common.typ": *

= 핵 낱말 <core>

#para("1")[#term("핵 낱말", en: "core word")은 `use` 없이 맨 이름으로 쓰는 낱말이다. 이 절의 표는 서명을 9.2 의
꼴로 적는다. `T` 는 "어느 타입이든 하나" 를 뜻하며 내장 낱말의 서명에만 쓰는 표기다. 스택 낱말(`dup` `drop`
`swap` `over` `nip`)은 9.3 이, 줄기를 쓰는 조합 낱말은 10.6 이 정한다.]

== 정수 <core-int>

#para("1")[산술. 넘치거나 0 으로 나누면 결함이다(11.3).
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`add` `sub` `mul`], [`(int, int gives int)`], [더하기, 빼기, 곱하기],
  [`div` `mod`], [`(int, int gives int)`], [0 쪽으로 버리는 몫과 그 나머지(나머지는 나뉘는 수의 부호)],
  [`floor_div` `floor_mod`], [`(int, int gives int)`], [아래로 버리는 몫과 그 나머지(나머지는 나누는 수의 부호)],
  [`neg` `abs`], [`(int gives int)`], [부호 바꾸기, 절댓값. 가장 작은 `int` 에서는 넘침],
  [`min` `max`], [`(int, int gives int)`], [작은 쪽, 큰 쪽],
  [`clamp`], [`(n int, lo int, hi int gives int)`], [`n` 을 `lo` 와 `hi` 사이로],
  [`rescale`], [`(d dec(N), M gives dec(M))`], [점 아래 `M` 자리의 소수로(`M` 은 상수). 자리가 줄면 0 에서 먼 쪽으로 반올림(7.5)],
)]

#para("2")[넘침을 다르게 다루는 낱말.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`wrap_add` `wrap_sub` `wrap_mul`], [`(int, int gives int)`], [2#super[64] 를 법으로 감는다],
  [`sat_add` `sat_sub` `sat_mul`], [`(int, int gives int)`], [범위의 끝에서 멈춘다],
  [`chk_add` `chk_sub` `chk_mul` `chk_div`], [`(int, int gives int or fail)`], [넘치면 `err.overflow`, 0 으로 나누면 `err.div_zero` 로 실패],
)]

#para("3")[부호 없는 수로 보는 낱말. 64 비트를 0 부터 2#super[64]−1 까지의 수로 읽는다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`u_div` `u_mod`], [`(int, int gives int)`], [부호 없는 몫과 나머지. 0 으로 나누면 결함],
  [`u_lt` `u_le` `u_gt` `u_ge`], [`(int, int gives bool)`], [부호 없는 비교],
  [`mul_hi` `u_mul_hi`], [`(int, int gives int)`], [128 비트 곱의 위 64 비트(부호 있음, 없음)],
)]

#para("4")[비트. 시프트와 회전의 수는 64 로 나눈 나머지(아래 6 비트)를 쓴다. `shl` 은 넘침을 검사하지 않는다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`bit_and` `bit_or` `bit_xor`], [`(int, int gives int)`], [비트끼리],
  [`bit_not`], [`(int gives int)`], [모든 비트를 뒤집음],
  [`shl` `shr` `u_shr`], [`(n int, k int gives int)`], [왼쪽, 부호 있는 오른쪽, 0 을 채우는 오른쪽],
  [`rotl` `rotr`], [`(n int, k int gives int)`], [회전],
  [`count_ones` `leading_zeros` `trailing_zeros`], [`(int gives int)`], [1 인 비트의 수, 앞과 뒤의 0 의 수(0 이면 64)],
  [`byte_swap`], [`(int gives int)`], [8 바이트의 차례를 뒤집음],
)]

#para("5")[좁은 정수(7.6)에서 위 낱말은 같은 타입 둘을 받아 그 타입을 준다. 넘침과 감기와 멈춤의 기준은 그 타입의
범위다. 부호 없는 수로 보는 낱말과 `floor_div` `floor_mod` 는 부호 없는 타입에서 보통 낱말과 같고 부호 있는 타입은
받지 않는다. 비트 낱말은 폭 안에서 한다. `neg` `abs` `min` `max` `clamp` 는 받지 않고, `rotl` `rotr` `mul_hi`
`u_mul_hi` 는 `u64` 만 받는다.]

#ex("spec", "15-integers.pnx")

#part("제약")

#para("5")[정수 낱말은 `int`(또는 같은 선언한 타입, 7.4)만 받는다.]
#ex("spec", "15-max-str.pnx")

#part("결함")

#para("6")[`neg` 과 `abs` 는 −2#super[63] 에서 넘친다.]
#ex("spec", "15-neg-min.pnx")

== 비교와 논리 <core-logic>

#para("1")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`eq` `ne`], [`(T, T gives bool)`], [같다, 다르다. `int` `bool` `str` 선언한 타입, 줄, 오류 번호],
  [`lt` `le` `gt` `ge`], [`(T, T gives bool)`], [`int`(부호 있는 수로), `str`(바이트 차례로), 선언한 타입],
  [`and` `or`], [`(bool, bool gives bool)`], [논리곱, 논리합. 두 값을 모두 계산한다],
  [`not`], [`(bool gives bool)`], [부정],
  [`select`], [`(bool, T, T gives T)`], [참이면 앞의 값, 거짓이면 뒤의 값. 분기하지 않는다],
)]

#ex("spec", "15-compare.pnx")

#part("제약")

#para("2")[비교하는 두 값은 타입이 같아야 한다. `select` 의 두 값도 그렇다.]
#ex("spec", "15-compare-mixed.pnx")
#ex("spec", "15-select-types.pnx")

== 모음 <core-collections>

#para("1")[8절이 선언하는 저장소를 다루는 낱말이다. `c` 는 그 저장소의 이름이다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*쓰는 곳*], [*뜻*],
  [`c len`], [배열, 목록, 큐, 버퍼], [지금 담긴 수(배열은 용량, 버퍼는 바이트 수). 풀은 줄을 세지 않는다],
  [`c capacity`], [배열, 목록, 큐, 버퍼], [선언한 용량],
  [`c is_empty`], [배열, 목록, 큐, 버퍼], [비었는가],
  [`c is_full`], [배열, 목록, 큐], [찼는가],
  [`c clear`], [배열, 목록, 큐, 버퍼], [비운다(배열은 모든 칸을 0 으로). 풀에는 쓰지 못한다: 줄을 하나씩 `free` 해서 비운다],
  [`i c idx`], [배열, 목록], [칸 `i`. 범위 밖이면 결함 `range`],
  [`v c push` / `c pop`], [목록], [끝에 넣기 / 끝에서 빼기(`or fail`)],
  [`c add_row`], [레코드의 목록], [빈 줄을 더하고 준다(`or fail`)],
  [`v c put` / `c take` / `c peek`], [큐], [뒤에 넣기 / 앞에서 빼기 / 앞을 보기(`or fail`)],
  [`v c wait_put` / `c wait_take`], [큐], [넣기 / 빼기, 큐가 찬 / 빈 동안은 양보(`or fail`, 14.3)],
  [`c alloc` / `r c free` / `r c is_alive`], [풀], [줄 얻기(`owned`, `or fail`) / 돌려주기 / 살아 있는가],
  [`s c append` `n c append_int` `b c append_byte`], [버퍼], [붙이기(`or fail`)],
  [`c contents`], [버퍼], [내용의 뷰],
  [`n c truncate`], [버퍼, 목록], [길이를 `n` 으로(목록은 8.5 의 (2b))],
  [`c none` / `r is_none` / `r row_index`], [레코드의 표], [없음 줄 / 없음인가 / 줄의 번호],
  [`k x lookup` / `r x insert` / `r x remove`], [찾아보기], [찾기(`or fail`) / 넣기(`or fail`) / 빼기],
  [`c sort` / `c sort_desc`], [배열, 목록], [칸을 차례로 놓는다. 가장 작은 것 / 가장 큰 것부터((3))],
  [`o t sort_by r.f` / `o t sort_by_desc r.f`], [`int` 의 배열이나 목록, 레코드의 표], [줄 번호를 그 줄의 필드 `f` 의 차례로 놓는다((4))],
)]

#para("2")[보기는 8절에 있다.]

#para("3")[`c sort` 는 배열의 칸을, 목록이면 길이까지의 칸을 가장 작은 것부터 차례로 놓는다. `c sort_desc` 는 가장 큰
것부터 놓는다. 칸은 제자리에서 바뀌고 낱말은 아무것도 주지 않는다. 칸은 `int`, 좁은 정수, `dec(N)`, `bool`(`false` 가
먼저), `str(N)` 이거나, 그중 하나 위에 선언한 타입이어야 한다. `u64` 칸은 부호 없이 견주고, `str(N)` 칸은 `str.compare`
의 바이트 순서로 견준다(16.1). 모음 매개변수(9.9)도 똑같이 정렬한다. 레코드의 목록이나 배열은 거절된다. 찾아보기, 필드,
전역, 이름 붙은 줄이 그 줄을 붙들고 있을 수 있기 때문이다. 그 줄은 (4)로, 줄 번호의 목록을 거쳐 차례로 놓는다.]
#ex("spec", "15-sort.pnx")
#ex("spec", "15-sort-records.pnx")

#para("4")[`o t sort_by r.f` 는 `o`, `t`, 필드 `r.f` 를 받는다. `o` 는 `int` 의 배열이나 목록이고 칸마다 `t` 의 줄
번호다. `t` 는 레코드의 배열이나 목록이다. `r.f` 는 `t` 가 담는 레코드의 필드이고, `to` 와 `set` 이 이름을 받듯 낱말
뒤에 쓴다. 이 낱말은 `o` 의 칸을 그 칸이 가리키는 줄의 그 필드의 차례로, 가장 작은 것부터 놓는다. `sort_by_desc` 는
가장 큰 것부터 놓는다. 필드는 (3)의 칸이 될 수 있는 타입이어야 한다. 줄은 움직이지 않으므로 찾아보기와 붙들린 줄은
그대로 맞다. `t` 의 줄 번호가 아닌 칸은 결함 `range` 이고, 어느 칸도 움직이기 전에 찾는다. 그러면 `o` 는 그대로 남는다.
`o` 와 `t` 가운데 하나만 매개변수일 수 있다.]
#ex("spec", "15-sort-by.pnx")
#ex("spec", "15-sort-by-range.pnx")

#para("5")[두 정렬은 모두 안정하다. 키가 같은 칸은 차례를 지킨다. 여러 키로 정렬하려면 여러 번 정렬한다. 덜 중요한
키부터 한다.]

#para("6")[정렬은 실패하지 않는다. 정렬하는 적재는 프로그램에 작업 영역을 준다. 크기는 정렬하는 가장 큰 모음의 칸마다
8바이트이고, 모음 매개변수를 정렬하면 가장 큰 배열이나 목록의 칸마다 8바이트다. 작업 영역은 상태의 전역 하나로 세고,
블록에 그만한 자리가 있어야 한다. $n$ 칸의 정렬은 예산에 명령 $n dot.c ceil(log_2 n)$ 개로 친다(19.5).]

== 그 밖 <core-other>

#para("1")[
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`mark`], [`(gives owned mark)`], [구역의 지금 자리(12.2)],
  [`release`], [`(owned mark)`], [구역을 그 자리로 되돌림(12.2)],
  [`region_free`], [`(gives int)`], [구역에 남은 바이트],
  [`call`], [블록의 서명대로], [값이 되는 블록을 부름(10.5)],
  [`raw`], [`(D gives 바탕)`], [선언한 타입의 값을 바탕 타입으로(7.4)],
  [`err_text`], [`(err gives str)`], [오류 번호의 이름(11.1)],
  [`assert`], [`(bool)`], [거짓이면 결함 `failed`],
  [`spawn` `run_all` `resume` `is_done` `cancel` `yield` `task_tag`], [14절], [작업],
)]

#part("진단")
#diag(
  ([정수 낱말에 `int` 가 아닌 값], "this word takes two int values"),
  ([비교하는 두 값의 타입이 다름], "the two values compared must have the same type"),
  ([`select` 의 값의 타입이 다름], "select takes a bool and two values of one type: flag a b select"),
  ([레코드의 배열이나 목록의 `sort`], "rows do not move: sort a list of their numbers by a field: order table sort_by record.field"),
  ([다른 타입의 칸의 `sort`], "sort orders cells of int, a narrow integer, dec(N), bool or str(N)"),
  ([`int` 의 목록과 표가 없는 `sort_by`], "sort_by takes a list of row numbers and a table of records: order table sort_by record.field"),
  ([뒤에 필드가 없는 `sort_by`], "sort_by is followed by the field: order table sort_by record.field"),
  ([표의 레코드가 아닌 레코드의 필드], "the field is one of the record the table holds"),
  ([다른 타입의 필드], "sort_by orders by a field of int, a narrow integer, dec(N), bool or str(N)"),
  ([목록과 표가 모두 매개변수], "sort_by takes the list or the table from a parameter, not both"),
)
