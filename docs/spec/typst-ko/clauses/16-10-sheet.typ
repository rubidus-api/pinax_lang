#import "/tools/common.typ": *

== `sheet` --- 줄과 열의 표를 모은 작업책 <std-sheet>

#para("1")[모듈 `sheet` 는 프로그램이 선언한 버퍼(8.7) 안에 줄과 열의 표, 곧 #term("시트", en: "sheets")를 여럿 둔다. 버퍼 하나가 #term("작업책", en: "book")
하나다. 자료형이 아니라 라이브러리이고, 어댑터처럼 돌아간다: 프로그램이 작업책을 열고, 그 안에 시트를 만들고, 이름이나
번호로 하나를 고르면, 다른 모든 낱말은 그 #term("현재 시트", en: "current sheet")에 닿는다. `sheet` 의 모든 낱말은 모음 매개변수(9.9)처럼 버퍼를 맨
끝에 받는다. 칸에는 아무것도 없거나, 프로그램이 넣은 `int`, `bool`, `dec(N)`, 날짜(Unix 초, UTC), `str` 이 들어 있다. 칸의 종류는 모듈 안에서만
알고, 값은 그 형의 낱말(`int`, `str` ...)로만 칸에서 나온다. 그래서 모든 낱말의 서명은 정해져 있고, 스택에 형이 없는 값이
오르는 일이 없다. 칸과 풀과 색인이 버퍼 안에 어떻게 놓이는지는 인터페이스가 아니다.]

#para("2")[줄, 열, 시트는 0부터 센다. 줄은 `add_row` 가 끝에 더하고, 시트는 `create` 가 받은 `rmax` 줄까지 가진다. 열의 수는
`create` 가 정하고 `widen` 이 바꾼다. 칸에 넣는 값은 복사된다. 같은 값은 하나만 둔다. 작업책 전체가 풀 하나를 나눠 쓴다: 칸에
들어가지 않는 큰 수와 모든 글은 그것을 쥔 칸(어느 시트의 칸이든)의 수와 함께 풀에 놓이고, 덮어쓰거나 비우거나 잘려 나간
칸은 값을 돌려준다. 풀에 자리가 없으면 `err.full` 이고, 흩어진 글의 빈자리는 새 글이 필요로 할 때(와 `compact` 가 불릴 때)
모은다. `sheet` 는 번호로 읽고 쓰며 들어 있는 값으로 찾는 표를 위한 것이다. 이름으로 찾는 것은 색인(8.10)이 맡는다.]

=== 작업책과 시트

#para("3")[이 묶음이 아닌 낱말은 현재 시트가 있어야 하고, 없으면 결함 `state` 다. 작업책에 처음 만든 시트가 현재 시트가 된다. 이름은
1에서 255바이트이고 작업책에 한 번만 보관된다. 시트 번호는 만든 차례이며, 지운 시트의 번호는 다음에 만드는 시트가 다시 받는다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`open_book`], [`(nmax int, b buffer or fail)`], [버퍼를 시트가 최대 `nmax`(1에서 256)개인 작업책으로 만든다(처음엔 시트 0개). 버퍼가 모자라면 `err.full`. 그 뒤 버퍼는 가득 찬 것으로 읽혀, 작업책 위에 덧붙일 수 없다],
  [`create` `create_cols`], [`(name str, rmax int, cols int, b buffer gives int or fail)`], [줄이 최대 `rmax`, 열이 꼭 `cols` 인 시트를 만들고 그 번호를 준다. `create_cols` 는 열 단위로 저장한다: 한 열을 훑는 일이 몇 배 빠르고 한 줄을 통째로 읽는 일은 느리다. 이름이 이미 있으면 `err.format`, 자리나 번호가 없으면 `err.full`],
  [`activate` `activate_n`], [`(name str, b buffer or fail)` `(n int, b buffer or fail)`], [그 이름, 그 번호의 시트를 현재 시트로 한다. 없으면 `err.not_found`. 읽기 전용 작업책은 현재 시트를 바꿀 수 없다],
  [`current` `sheets`], [`(b buffer gives int)`], [현재 시트의 번호(없으면 -1), 쓰는 가장 큰 번호보다 하나 큰 수],
  [`sheet_name`], [`(n int, b buffer gives str or fail)`], [시트 `n` 의 이름을 보는 view. 작업책에 다음 쓰기 전까지 유효. 쓰지 않는 번호면 `err.not_found`],
  [`find_sheet`], [`(name str, b buffer gives int or fail)`], [그 이름의 시트 번호. 없으면 `err.not_found`],
  [`remove`], [`(name str, b buffer or fail)`], [시트를 색인과 함께 지우고 그 칸들이 쥔 값을 돌려준다. 현재 시트였다면 그 뒤에는 현재 시트가 없다],
  [`rename`], [`(n int, name str, b buffer or fail)`], [시트 `n` 에 다른 이름을 준다. 이름이 이미 있으면 `err.format`],
  [`copy_to`], [`(row1 int, col1 int, n int, row2 int, col2 int, b buffer or fail)`], [현재 시트의 칸을 시트 `n` 의 칸에 복사한다. 값은 나눠 쥔다. 시트 `n` 이 없으면 `err.not_found`],
)]

=== 만들기와 늘리기

#para("4")[이 묶음의 낱말은 현재 시트에 닿는다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`rows` `cols` `rows_max`], [`(b buffer gives int)`], [지금의 줄 수, 열 수, 최대 줄 수],
  [`add_row`], [`(b buffer gives int or fail)`], [빈 줄을 더하고 그 번호를 준다. `rmax` 에 닿으면 `err.full`],
  [`truncate_rows`], [`(n int, b buffer)`], [앞의 `n` 줄만 남기고 나머지가 쥔 값을 돌려준다. 있는 줄보다 많으면 결함 `range`],
  [`widen`], [`(cols int, b buffer or fail)`], [줄, 값, 색인을 지키며 열이 `cols` 인 시트로 다시 만든다. 열을 늘리면 작업책에 자리가 있어야 하고(`err.full`), 줄이면 잘리는 열이 모두 비었을 때만 된다(`err.format`). 옛 자리는 구멍으로 남고 `compact` 가 메운다],
  [`clear`], [`(b buffer)`], [모든 줄을 지운다],
  [`compact`], [`(b buffer)`], [글의 빈자리를 모으고 시트 사이의 구멍을 메운다],
)]

=== 칸

#para("5")[줄과 열을 받는 낱말은 칸이 표 밖이면 결함 `range` 다. 표가 아닌 버퍼도 같은 결함이다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`put_int` `put_bool`], [`(v int, row int, col int, b buffer or fail)` `(v bool, row int, col int, b buffer or fail)`], [칸에 복사해 넣는다. 풀이 가득이면 `err.full`],
  [`put_date`], [`(secs int, row int, col int, b buffer or fail)`], [날짜, 곧 Unix 초 `secs`(UTC)를 칸에 넣는다. 60비트에 안 드는 값(1~9999년의 어느 날짜든 들어간다)은 결함 `range`],
  [`put_dec`], [`(v dec(N), row int, col int, b buffer or fail)`], [자릿수와 함께 넣는다],
  [`put_str`], [`(s str, row int, col int, b buffer or fail)`], [글을 복사해 넣는다. 이 표 안을 보는 view 를 줘도 된다],
  [`clear_cell`], [`(row int, col int, b buffer)`], [칸을 비운다],
  [`copy_cell`], [`(row1 int, col1 int, row2 int, col2 int, b buffer)`], [칸을 다른 칸에 복사한다. 값은 나눠 쥔다],
  [`int` `bool`], [`(row int, col int, b buffer gives int or fail)` `(... gives bool or fail)`], [값. 다른 종류의 칸이면 `err.format`(날짜도 그렇다: 날짜는 `date` 로 읽는다)],
  [`date`], [`(row int, col int, b buffer gives int or fail)`], [날짜 칸의 Unix 초. 다른 종류의 칸이면 `err.format`],
  [`dec`], [`(row int, col int, N int, b buffer gives dec(N) or fail)`], [자릿수 `N`(0에서 18의 상수)의 decimal. 종류나 자릿수가 다르면 `err.format`],
  [`str`], [`(row int, col int, b buffer gives str or fail)`], [표 안의 글을 보는 view. 그 표에 다음에 쓸 때까지 유효. 다른 종류면 `err.format`],
  [`kind` `is_empty`], [`(row int, col int, b buffer gives int)` `(... gives bool)`], [종류(`sheet.k_empty` 0, `k_int` 1, `k_bool` 2, `k_dec` 3, `k_str` 4, `k_date` 5). 비었는가],
)]

=== 찾기와 고르기

#para("6")[찾기는 수는 수로, 글은 적힌 대로 견준다. 한 열을 아래로, 또는 한 줄을 옆으로 훑는다. 어느 칸에도 없는 글은 바로
없다고 알아낸다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`find_row_int` `find_row_str`], [`(key int, col int, b buffer gives int or fail)` `(key str, col int, ...)`], [`col` 열이 `key` 인 첫 줄. 없으면 `err.not_found`. 스프레드시트의 `MATCH` 다. 날짜 칸은 그 초로 찾는다(`where_int`, `narrow_int`, 색인, `sum_*` 도 그렇게 본다). 값은 그 뒤 `int`, `str`, `date` ... 로 읽는다(`VLOOKUP` 처럼 형 없는 값을 돌려주는 낱말은 없다)],
  [`find_col_int` `find_col_str`], [`(key int, row int, b buffer gives int or fail)`], [`row` 줄을 옆으로 훑어 첫 열],
  [`find_row_from_int` `find_row_from_str` `find_col_from_int` `find_col_from_str`], [`(key, line int, from int, b buffer gives int or fail)`], [같되 줄이나 열 `from` 부터],
)]

#para("7")[시트는 #term("선택", en: "selection")을 가진다: 줄의 목록이고, 처음엔 모든 줄이다. `where_*` 는 모든 줄에서 다시 시작해, 그 열의 값이 `v` 와
관계 `op`(`sheet.eq` `ne` `lt` `le` `gt` `ge`)인 줄만 남기고, `narrow_*` 는 지금 선택된 줄 가운데 그런 줄만 남긴다. 종류가
다른 칸과 빈 칸은 어떤 관계도 아니다. 낱말마다 시트의 선택에 일하므로 낱말을 늘어놓으면 곧 파이프다. 선택은 버퍼 안의 한 자리이고
작업책 안에 시트마다 보관된다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`where_int` `narrow_int`], [`(col int, op int, v int, b buffer)`], [수로 고르거나 좁힌다],
  [`where_str` `narrow_str`], [`(col int, op int, v str, b buffer)`], [글로(바이트 순서) 고르거나 좁힌다],
  [`select_all`], [`(b buffer)`], [모든 줄을 고른다],
  [`sel_len`], [`(b buffer gives int)`], [고른 줄의 수],
  [`sel_row`], [`(k int, b buffer gives int)`], [`k` 번째로 고른 줄의 번호. 범위 밖이면 결함 `range`],
  [`order_by` `order_by_desc`], [`(col int, b buffer or fail)`], [고른 줄을 `col` 열의 순서로, 작은(큰) 것부터 안정적으로 놓는다. 수(날짜는 그 초), 글, 나머지 차례. 수를 놓을 자리는 버퍼의 빈 끝에서 가져온다(`err.full`)],
)]

#para("8")[합계는 int 칸과 날짜 칸만 본다(날짜는 그 초로 센다). `sum_*` 는 합이 넘치면 결함 `overflow`, `min_*` 와 `max_*` 는 수가 없으면 `err.not_found` 로
실패한다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`sum_col` `min_col` `max_col` `count_col`], [`(col int, b buffer gives int)`], [`col` 열의 모든 줄. `min` 과 `max` 는 `or fail`],
  [`sum_row` `min_row` `max_row` `count_row`], [`(row int, b buffer gives int)`], [`row` 줄의 모든 열],
  [`sum_sel` `min_sel` `max_sel` `count_sel`], [`(col int, b buffer gives int)`], [고른 줄의 `col` 열],
)]

=== 순서 있는 색인

#para("9")[열에는 #term("색인", en: "index")을 둘 수 있다: 그 열의 수와 글을 순서대로(수가 먼저, 다음 글은 바이트 순서, 같으면
줄 번호 순) 늘어놓은 구조이며, 작업책의 빈 끝에서 가져온 쪽(page)들로 된 B+트리다. `index` 가 만들고, 그 열에 쓰면 함께
고쳐지며, `unindex` 가 쪽을 돌려준다. 색인이 있으면 `find_row_*`(열을 따라 내려가기), `ne` 를 뺀 모든 관계의 `where_*`,
모든 줄을 고른 `order_by` 가 훑지 않고 답한다. 걸리는 시간은 줄 수의 로그와 찾은 줄 수에 따라 자라고, 답은 색인이 없을 때와
같다(줄은 줄 번호 순). 색인은 쓰기에 값을 치르고, 쪽과 시트가 버퍼를 채운 작업책은 새 색인을 `err.full` 로 거절한다. 없는
쪽이 필요한 쓰기도 `err.full` 이며 아무것도 바꾸지 않는다. 색인은 수단이지 뜻이 아니다: 쓰지 않는 프로그램은 전과 같이 돈다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*Word*], [*Signature*], [*Meaning*],
  [`index`], [`(col int, b buffer or fail)`], [현재 시트의 `col` 열에 색인을 만든다(이미 있으면 그대로). 작업책에 쪽이 없으면 `err.full`],
  [`unindex`], [`(col int, b buffer)`], [색인을 없앤다],
  [`indexed`], [`(col int, b buffer gives bool)`], [그 열에 색인이 있는가],
)]

=== 파일

#para("9a")[CSV와 xlsx 파일을 현재 시트로 읽고 현재 시트에서 쓴다. 파일에 대한 권한은 없다: 프로그램이 파일을 `buffer` 로 읽고(`file.read_all_into`)
버퍼를 내보낸다(`contents` 를 `file.write`). 이 낱말들은 #term("버퍼 둘", en: "two buffers")을 받는다. 파일이 먼저, 작업책이 마지막이다(`0 blob book sheet.load_csv`).
둘은 서로 다른 버퍼이고, 작업책은 참조가 아닌 버퍼 자신이어야 한다(파일은 컬렉션 매개변수(9.9)여도 된다). 파일을 받는 낱말은 그 버퍼의 바이트를
바꿀 수 있다(작업 공간이다. 공유 버퍼는 거절한다). 읽기는 현재 시트의 마지막 줄 뒤에 줄을 더하고 더한 수를 주며, 실패하면 시트는 처음 그대로다.
`rows_max` 를 넘는 줄이나 시트보다 많은 열은 `err.full`, CSV가 아닌 글이나 xlsx가 아닌 파일은 `err.format` 이다. `mode` 는 0, 1, 2이고 다른 값은 오류(fault)다.]
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*서명*], [*뜻*],
  [`load_csv`], [`(mode int, f buffer, b buffer gives n int or fail)`], [`f` 의 글을 읽는다. 줄은 LF나 CRLF로 끝나고 필드는 쉼표로 나뉘며, 따옴표 필드는 쉼표·줄바꿈·겹따옴표를 담을 수 있다(그 자리에서 풀어 쓴다). 빈 줄과 UTF-8 BOM은 건너뛴다. mode 0: 정규형 정수(`-?(0|[1-9][0-9]*)`, int에 들어가는 것)는 `int`, 소수점 뒤 1~18자리인 정규형 수(들어가는 것)는 그 자릿수의 `dec`(`1.50` 은 `dec(2)`), 빈 필드는 빈 칸, 나머지는 글(`007`, `-0` 도). mode 2: mode 0과 같고, 실제로 있는 날짜인 필드(`YYYY-MM-DD`, 또는 `T`나 공백과 `hh:mm:ss`가 붙고 `Z`가 올 수도 있다)는 날짜 칸이 된다(UTC, 날짜만 있으면 자정). mode 1: 모두 글],
  [`load_xlsx`], [`(mode int, n int, f buffer, b buffer gives rows int or fail)`], [`f` 의 xlsx에서 `n` 번 시트(0부터, 통합 문서 순서, 없으면 `err.not_found`)를 읽는다. 숫자는 CSV 규칙을 따른다(소수 18자리를 넘는 자리는 반올림, 지수는 적용, 들어가지 않으면 글). 공유·인라인·수식 결과 문자열은 글(서식 있는 글은 이어 붙이고 발음 표기는 버린다), `t="b"` 는 `bool`, 오류값(`#N/A`)은 글, 수식은 저장된 결과를 준다. 숫자 서식이 날짜나 시간인 칸(내장 14~22, 27~36, 45~47, 50~58, 71~81, 또는 따옴표·대괄호 밖에 `y`, `d`, `h`, `s`, `m` 이 있는 사용자 서식, `[h]`, `[mm]` 포함)은 mode 0에서 Unix 초(UTC, 파일의 1900/1904 날짜 체계, 시각만 있는 값은 자정부터)의 `int`, mode 1에서는 일련번호 그대로의 `dec`, mode 2에서는 그 초를 가진 날짜 칸(다른 칸은 mode 0과 같다). 파일이 비운 줄과 열은 빈 칸],
  [`save_csv`], [`(dst buffer, b buffer gives n int or fail)`], [`dst` 의 내용을 현재 시트의 CSV로 바꾼다(쓴 마지막 줄과 열까지, 쉼표·따옴표·줄바꿈이 든 글은 따옴표로 감싼다, `dec` 는 자릿수대로, `bool` 은 `true`/`false`, 날짜는 `YYYY-MM-DD`이고 자정이 아니면 `Thh:mm:ss`가 붙는다. 0~9999년 밖이면 초로 쓴다). 쓴 바이트 수를 준다. `dst` 가 작으면 `err.full`(그때 `dst` 는 비어 있다)],
  [`save_xlsx`], [`(dst buffer, b buffer gives n int or fail)`], [`dst` 의 내용을 현재 시트의 xlsx로 바꾼다(시트 하나, 이름은 시트 이름대로 31자까지, `[]:*?/\` 는 뺀다. 글은 인라인 문자열, 수·`dec`·`bool` 은 그대로이고, 날짜는 엑셀 날짜로, 곧 1900 체계의 일련번호에 서식 `yyyy-mm-dd hh:mm:ss`를 붙여 쓴다. 날짜가 있는 시트에만 스타일 부분을 쓰며, mode 2로 읽으면 같은 초가 돌아온다. 1900-03-01 이전이나 9999-12-31 이후는 서식이 말할 수 없으니 초를 그냥 수로 쓴다). ZIP은 압축하지 않고 저장만 한다],
)
#para("9b")[#term("작업 공간", en: "work area") 아무것도 할당하지 않는다(12절). `load_xlsx` 는 파일이 든 버퍼의 마지막 바이트 뒤 남은 용량을 쓴다: 약
130 KB(inflate의 창과 훑개의 버퍼)에 풀어낸 공유 문자열(하나에 4바이트와 그 글)을 더한 만큼이다. 버퍼는 파일보다 크게 선언해야 하고 자리가 모자라면 `err.full` 이다.
파일의 부품은 ZIP의 중앙 디렉터리로 찾는다. ZIP64, 암호화된 부품, stored와 deflate 외의 방식은 `err.format`, 풀었을 때 256 MiB를 넘는 부품도 같다.
풀어낸 바이트마다 명령 예산에서 차감한다.]
#ex("spec", "16-sheet-files.pnx")

#ex("spec", "16-sheet-date.pnx")

#ex("spec", "16-sheet.pnx")
#ex("spec", "16-sheet-pipe.pnx")
#ex("spec", "16-sheet-book.pnx")
#ex("spec", "16-sheet-index.pnx")

#part("제약")

#para("10")[마지막 인자는 버퍼이고, 나머지는 위 서명의 형이다.]
#ex("spec", "16-sheet-args.pnx")

#part("결함")

#para("11")[시트 밖의 줄이나 열, 현재 시트가 없는데 현재 시트가 필요한 낱말(`state`), `open_book` 으로 만들지 않았거나 그 뒤 바뀐 버퍼, 넘치는 합은 결함이다.]
#ex("spec", "16-sheet-range.pnx")

#part("진단")
#diag(
  ([`sheet` 의 낱말에 다른 형의 값], "this word of sheet takes int for row, column and number, str for text, bool for put_bool, dec(N) for put_dec"),
  ([`sheet` 의 낱말인데 끝에 버퍼가 없음], "the words of sheet take a buffer last: v row column grid sheet.put_int"),
  ([파일을 받는 낱말에 참조인 작업책이나 버퍼가 아닌 파일], "this word of sheet takes the buffer of the file, then the book: 0 file book sheet.load_csv"),
)
