#import "/tools/common.typ": *

== `csv` --- CSV <std-csv>

#para("1")[모듈 `csv` 는 RFC 4180 꼴의 CSV 를 읽고 쓴다. 필드는 쉼표로 가르고, 쉼표, 따옴표, 줄바꿈을 담은
필드는 따옴표로 싸며, 안의 따옴표는 두 번 적는다.]

#para("2")[`s csv.fields` 는 줄 하나의 필드를 차례로 내는 줄기(10.6)다. 따옴표로 싼 필드는 따옴표를 떼고 겹친
따옴표를 풀어서 낸다. 닫히지 않은 따옴표는 남은 글 그대로를 마지막 필드로 낸다(줄기는 실패를 알리지 못한다).]

#para("3")[쓰는 낱말은 버퍼에 붙인다(`or fail`, 자리가 모자라면 `err.full`). `s buf csv.put`, `n buf
csv.put_int`, `d buf csv.put_dec`, `b buf csv.put_bool` 은 필드 하나를(앞에 필요한 쉼표와 함께), `buf csv.end_line` 은 줄 끝을
붙인다. 참거짓은 `true` `false` 로 쓴다.]

#para("4")[`줄 buf csv.pack` 은 레코드의 줄 하나를 한 줄로 붙인다. `s 줄 csv.unpack` 은 한 줄을 읽어 필드의
차례대로 넣고 그 줄 뒤의 나머지를 준다. 모든 필드가 맞을 때만 줄에 쓴다(`err.format`). `dec(N)` 필드는 `json`
처럼 읽고 쓴다(16.8).]

#ex("spec", "16-csv.pnx")

#part("제약")

#para("5")[`pack` 과 `unpack` 은 레코드의 줄만 받는다.]
#ex("spec", "16-csv-pack-int.pnx")

#part("진단")
#diag(
  ([레코드의 줄이 아닌 값], "pack takes a row of a table of records and a buffer: row buf csv.pack"),
)
