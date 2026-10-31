#import "/tools/common.typ": *

= 선언 <declarations>

== 일반 <decl-general>

#part("구문")
```ebnf
declaration = [ "export" ] ( [ "bound" ] bindable | "shared" shareable | decl ) ;
bindable    = "global" NAME type
            | "array"  NAME type capacity
            | "buffer" NAME capacity ;
shareable   = bindable
            | "list"   NAME type capacity
            | "index"  NAME NAME PATH ;
decl        = "const"  NAME literal
            | "list"   NAME type capacity
            | "queue"  NAME type capacity
            | "pool"   NAME NAME capacity
            | "record" NAME { field | "big_endian" } "end"
            | "type"   NAME base
            | "index"  NAME NAME PATH
            | "tasks"  NAME capacity [ size [ size [ size ] ] ] [ "apart" ]
            | "extern" c-name "(" effect ")" "from" STRING [ "as" NAME ]
            | "test"   NAME { body } "end" ;
capacity    = size | "host" ;
size        = INT | NAME ;                         (* NAME: a const *)
```

#para("1")[#term("선언", en: "declaration")은 이름과 그 이름이 가리키는 것(값, 저장소, 타입)을 만든다. 선언은
정의 밖, 소스의 맨 바깥에만 쓴다.]

#para("2")[처리기는 소스를 위에서 아래로 한 번 읽는다. 이름은 쓰는 자리보다 앞에서 선언돼 있어야 한다.
예외는 자기를 부르는 낱말과 `forward`(8.11)다.]

#para("3")[한 모듈 안에서 같은 이름을 두 번 짓지 못한다. 종류가 달라도 그렇다. 핵 낱말과 타입의 이름도 다시
짓지 못한다.]

#para("4")[용량(`size`)은 양의 정수 리터럴이거나 그런 값의 `const` 다. 용량은 적재 때 정해지고 돌 때 바뀌지
않는다. 모든 저장소는 적재 때 상태의 덩이 안에 자리를 잡는다.]

#para("4a")[배열, 버퍼, 목록, 큐, 풀, 작업 묶음의 용량은 `host` 로 쓸 수 있다. 그러면 그것은 호스트가 적재 전에 그 선언에
준 수다(19.4 (1d)). 적재는 그 수를 거기에 적은 것처럼 다룬다. 호스트가 수를 주지 않은 선언에서 `host` 를 만난 적재는
거절되고, 메시지가 그 선언의 이름을 댄다. `host` 는 그 자리에서만 선언의 낱말이다. 그 자리에서는 같은 이름의 상수를 찾지
않는다.]
#ex("spec", "08-host-capacity.pnx")

#para("5")[전역, 배열의 칸, 목록·큐·풀의 칸, 버퍼, 레코드의 필드는 0 으로 시작한다(7 (3)).]

#part("제약")

#para("6")[이미 있는 이름을 다시 지으면 거절된다.]
#ex("spec", "08-name-twice.pnx")

#para("7")[선언보다 앞에서 이름을 쓰면 거절된다.]
#ex("spec", "08-use-before.pnx")

== 상수 <decl-const>

#para("1")[`const 이름 리터럴` 은 #term("상수", en: "constant")를 만든다. 타입은 리터럴의 타입이다. 상수는
저장소가 없다 --- 쓰는 자리에 값이 들어간다. 용량, `case` 의 가지 값, `row` 의 값에 쓸 수 있다.]

#part("제약")

#para("2")[상수에는 넣지 못한다. 정의 안에서 `v to 상수` 는 같은 이름의 지역 변수를 새로 만들려는 것이 되어
가림 금지(9.5)로 거절된다. 정의 밖에서는 `to` 가 지역을 만들지 않으므로 거절된다. `set` 은 전역에만 쓴다(9.4).]
#ex("spec", "08-const-store.pnx")

== 전역 <decl-global>

#para("1")[`global 이름 타입` 은 값 하나를 담는 #term("전역", en: "global")을 만든다. 타입은 `int` `bool`
`err` `dec(N)` `str(N)`, 선언한 타입, 줄의 타입(표의 이름), 핸들 타입이다. `str` 는 쓰지 못한다(7.3).]

#para("2")[전역을 읽으려면 이름을 쓰고, 넣으려면 `v set 이름` 을 쓴다(9.4). 상태가 살아 있는 동안 값이 남는다 ---
한 낱말의 실행이 끝나도, 다음 적재가 와도 그대로다.]

#ex("spec", "08-const-global.pnx")

== 배열 <decl-array>

#part("구문")
```ebnf
assign-item = "assign" NAME "(" value { "," value } ")" ;
```

#para("1")[`array 이름 타입 용량` 은 칸이 용량만큼 있는 #term("배열", en: "array")을 만든다. 배열은 늘
가득 차 있다: `len` 은 용량과 같다. 원소 타입은 `int` `bool` `err` `dec(N)` `str(N)`, 좁은 정수(7.6), 선언한 타입,
레코드의 이름, 또는 표의 이름이다. 표의 이름이면 칸은 그 표의 줄을 담는다(8.8 (5a)). 좁은 정수의 칸은 그 폭만큼의 바이트이고, 다른 값의 칸은 8 바이트다. 목록과 큐의 칸도 같다.]

#para("2")[`i 배열 idx` 는 칸 `i` 의 값을 읽는다. `v i set 배열` 은 칸 `i` 에 넣는다. 모음 매개변수(9.9)의 칸도 같은 꼴로 넣는다. 번호는 0 부터 센다.
레코드의 배열에서 `idx` 는 값이 아니라 줄(8.8)을 낸다.]

#para("3")[`assign 배열 (값, …)` 은 정의 밖에서 배열을 0 번 칸부터 채운다. 값이 칸보다 적으면 나머지 칸은
그대로다. 같은 배열에 다시 쓰면 0 번 칸부터 다시 넣는다. 자료 적재(13.4)에서도 쓸 수 있다.]

#para("4")[배열의 이름을 줄기로 쓰면 칸의 값을 0 번부터 차례로 낸다(10.6).]

#ex("spec", "08-array.pnx")

#part("제약")

#para("5")[`assign` 의 값이 칸보다 많으면 거절된다.]
#ex("spec", "08-assign-many.pnx")

#part("결함")

#para("6")[번호가 0 보다 작거나 용량과 같거나 크면 결함 `range` 다.]
#ex("spec", "08-array-range.pnx")

== 목록 <decl-list>

#para("1")[`list 이름 타입 용량` 은 길이가 0 에서 용량까지 자라는 #term("목록", en: "list")을 만든다. 칸을
읽고 쓰는 것은 배열과 같으나, 번호는 길이 안에서만 쓴다.]

#para("2")[`v 목록 push` 는 끝에 하나를 더하고, `목록 pop` 은 끝의 하나를 빼서 준다. 둘은 실패할 수 있다
(`or fail`): 가득 찬 목록에 `push` 하면 `err.full`, 빈 목록에서 `pop` 하면 `err.range` 다. `str(N)` 목록에 너무 긴
글을 `push` 하면 `err.range` 다(7.3 (4a)). 레코드의 목록에는
`목록 add_row` 로 빈 줄을 더하고 그 줄을 받는다. `목록 clear` 는 길이를 0 으로 한다.]

#ex("spec", "08-list.pnx")
#ex("spec", "08-list-full.pnx")

#para("2a")[`n 목록 truncate` 는 길이를 `n` 으로 줄인다. 정수, 글, 레코드의 목록 모두에 쓴다. `add_row` 로 더한 줄의
값이 틀린 것을 알았을 때 그 줄을 되돌리는 데 쓴다.]

#ex("spec", "08-list-truncate.pnx")

#part("제약")

#para("2b")[줄이 다른 곳에 남는 레코드의 목록은 `truncate` 하지 못한다. 그 목록의 줄을 담는 필드나 전역이나 모음, 그 목록에
`row` 로 이름 붙인 줄, 그 목록의 찾아보기(8.10)가 같은 소스 안에 하나라도 있으면 거절된다. 잘라 낸 자리에 새 줄이
들어서면 그것들이 엉뚱한 줄을 가리키기 때문이다. 지역에 든 줄은 쓸 때 검사한다(결함 `range`).]

#ex("spec", "08-list-truncate-kept.pnx")

#part("결함")

#para("3")[길이 밖의 번호는 용량 안이라도 결함 `range` 다. `truncate` 에 길이보다 큰 수나 음수를 주어도 결함
`range` 다.]
#ex("spec", "08-list-past-len.pnx")

#part("진단")
#diag(
  ([줄이 남는 목록을 `truncate`], "this list's rows are kept by a field, a global, a collection, a named row or an index; it cannot be truncated"),
)

== 큐 <decl-queue>

#para("1")[`queue 이름 타입 용량` 은 먼저 넣은 것이 먼저 나오는 #term("큐", en: "queue")를 만든다. 원소 타입은 `int`
`bool` `err` `dec(N)` 이다.
`v 큐 put` 은 뒤에 넣고, `큐 take` 는 앞에서 빼서 주며, `큐 peek` 은 빼지 않고 앞의 것을 준다. 셋 다 실패할
수 있다: 가득 찬 큐에 넣으면 `err.full`, 빈 큐에서 꺼내면 `err.range` 다.]

#ex("spec", "08-queue.pnx")

== 버퍼 <decl-buffer>

#para("1")[`buffer 이름 용량` 은 용량만큼의 바이트를 담는 #term("버퍼", en: "buffer")를 만든다. 버퍼는 고칠
수 있는 바이트의 열이다. 글을 조금씩 이어 붙일 때 쓴다.]

#para("2")[`s 버퍼 append`, `n 버퍼 append_int`(십진; `n` 은 `int` 나 좁은 정수), `b 버퍼 append_byte`(`b` 는 `u8`)
는 끝에 붙이고, 자리가 모자라면
`err.full` 로 실패한다(실패하면 아무것도 붙이지 않는다). `버퍼 contents` 는 지금 내용의 `str` 뷰를 준다.
`n 버퍼 truncate` 는 길이를 `n` 으로 줄인다. `버퍼 clear` 는 길이를 0 으로 한다.]

#ex("spec", "08-buffer.pnx")

#para("3")[`contents` 가 준 뷰는 버퍼의 바이트를 가리킨다. 버퍼를 고치면 뷰가 보는 바이트도 바뀐다.]

== 레코드와 줄 <decl-record>

#part("구문")
```ebnf
record   = "record" NAME { field | "big_endian" } "end" ;
field    = NAME width ;
width    = "u8" | "u16" | "u32" | "u64" | "i8" | "i16" | "i32" | "i64"
         | "int" | "bool" | "varint" | "dec" "(" INT ")" | "str" "(" size ")"
         | NAME ;                                         (* NAME: a table *)
row-item = [ "export" ] "row" NAME [ NAME ] "(" value { "," value } ")" ;
value    = literal | NAME ;                         (* NAME: a const, a row name, none *)
```

#para("1")[`record 이름 … end` 는 필드의 나열인 #term("레코드", en: "record") 타입을 만든다. 필드마다 폭을
적는다. `int` 와 `i64` 필드는 읽으면 `int`, `u8` … `i32` 필드는 그 좁은 정수(7.6), `bool` 필드는 `bool`, `dec(N)`
필드는 `dec(N)`(8 바이트), `str(N)` 필드는 `str` 뷰가 된다. 필드에는 그 타입의 값만 넣는다. 필드의 타입에 표의
이름을 쓰면 그 표의 줄을 담는다.]

#para("2")[레코드는 값으로 다루지 않는다. 레코드를 원소로 하는 배열·목록·풀(#term("표", en: "table"))을
선언하고, 그 한 칸인 #term("줄", en: "row")을 다룬다. 줄의 타입은 표의 이름이다. 줄은 표의 번호 하나를 담고,
쓸 때마다 범위를 검사한다(풀이면 세대도, 8.9).]

#para("3")[필드는 경로 `레코드.필드` 로 적는다. `줄 레코드.필드` 는 읽고, `v 줄 to 레코드.필드` 는 넣는다.]

#para("4")[`row 표 (값, …)` 는 정의 밖에서 목록에 줄 하나를 더한다. 값은 필드의 차례로 적고, 뒤쪽 필드는
줄일 수 있다(0 이 된다). `row 표 이름 (…)` 은 그 줄에 이름을 붙인다. 줄의 이름은 뒤의 `row` 값과 코드에서
그 줄로 쓴다.]

#ex("spec", "08-record.pnx")

#para("5")[줄을 담는 필드와 전역의 처음 값은 "없음" 이다. `줄 is_none` 은 없음인지 묻고, `표 none` 은 그 표의
없음 줄을 낸다. `줄 row_index` 는 줄의 번호를 준다.]

#para("5a")[원소 타입이 표의 이름인 배열이나 목록은 그 표의 줄을 8 바이트 칸에 담는다. 칸에는 줄의 번호에 1 을 더한 값이
들고, 풀의 줄이면 그 세대가 위쪽 32 비트에 든다. 0 은 없음이고, 칸은 모두 없음으로 시작한다. 칸은 다른 칸처럼 읽고, 넣고,
`push`·`pop` 하고, 돌며, 들어가는 값은 그 표의 줄이어야 한다. 칸에서 읽은 줄은 다른 줄처럼 쓰는 자리에서 검사한다. 큐는
줄을 담지 않는다. `alloc` 이 막 준 줄(8.9)은 `set` 으로 칸에 넣는다. `set` 은 필드처럼 그 줄을 넘겨받는다. `push` 는 그
줄을 거부한다. 실패한 `push` 는 그 줄을 잃기 때문이다.]

#ex("spec", "08-row-cells.pnx")

#para("6")[`big_endian` 은 16.5 의 `bytes.pack` 과 `bytes.unpack` 이 쓰는 바이트 순서만 정한다. 메모리 안의
모양은 바꾸지 않는다.]

#part("제약")

#para("7")[`row` 의 값은 필드의 차례와 타입에 맞아야 한다.]
#ex("spec", "08-row-types.pnx")

#part("결함")

#para("8")[좁은 정수 필드에 넣을 `int` 는 먼저 그 타입으로 바꾼다(`n u8`); 값이 폭에 맞지 않으면 그 변환이 결함
`overflow` 다(7.6). 자르지 않는다 --- 잘라 넣으려면 `low_u8` 처럼 아래 비트를 받는 낱말을 쓴다. 없음 줄의 필드를
읽거나 쓰면 결함 `range` 다.]
#ex("spec", "08-record-narrow.pnx")

== 풀 <decl-pool>

#para("1")[`pool 이름 레코드 용량` 은 줄을 얻고 돌려주는 #term("풀", en: "pool")을 만든다. `풀 alloc` 은 빈
줄 하나를 얻고(`or fail`, 남은 줄이 없으면 `err.full`), `줄 풀 free` 는 돌려준다. `줄 풀 is_alive` 는 그 줄이
아직 살아 있는지 묻는다.]

#para("2")[풀의 줄은 번호와 함께 #term("세대", en: "generation")를 담는다. 줄을 돌려주면 그 칸의 세대가
바뀐다. 그래서 돌려준 줄을 다시 쓰면 --- 그 사이 같은 칸을 다른 줄이 얻었더라도 --- 알아챈다.]

#para("3")[`alloc` 이 준 줄은 `owned` 값(12.4)이다. 모든 경로에서 정확히 한 번, 필드나 전역에 넣거나 `free`
에 줘야 한다. 넣은 뒤에는 세대 검사가 그 줄을 지킨다.]

#ex("spec", "08-pool.pnx")

#part("제약")

#para("4")[얻은 줄을 넣지도 돌려주지도 않는 경로가 있으면 거절된다.]
#ex("spec", "08-pool-leak.pnx")

#part("결함")

#para("5")[돌려준 줄을 쓰면 결함 `stale` 이다. `pnx` 는 결함 아래에 그 낱말의 지역 변수를 함께 보인다.]
#ex("spec", "08-pool-stale.pnx")

== 찾아보기 <decl-index>

#para("1")[`index 이름 표 레코드.필드` 는 레코드의 표를 한 필드로 찾는 #term("찾아보기", en: "index")를
만든다. 키 필드는 `int` 이거나 `str(N)` 이다. 찾아보기는 표의 용량에 맞춘 고정 크기의 해시 표이고 줄의 번호만
담는다. 자료는 표에 있다.]

#para("2")[`키 찾아보기 lookup` 은 그 키의 줄을 준다(`or fail`, 없으면 `err.not_found`).
`줄 찾아보기 insert` 는 줄을 넣고(`or fail`, 같은 키가 이미 있으면 실패), `줄 찾아보기 remove` 는 뺀다.
키 필드를 바꾸려면 빼고, 고치고, 다시 넣는다.]

#para("3")[적재 때 `row` 로 적은 줄은 찾아보기에 들어 있다. 돌 때 `add_row` 로 더한 줄은 `insert` 해야
찾아진다.]

#ex("spec", "08-index.pnx")
#ex("spec", "08-index-insert.pnx")

#para("4")[해시는 키가 있는 해시(SipHash-2-4)다. 키는 호스트가 준다(19.2). 밖에서 온 글을 키로 쓰는 호스트는
무작위 키를 줘야 한다 --- 키를 아는 쪽은 충돌을 몰아 찾기를 느리게 만들 수 있다.]

== 앞선 선언 <decl-forward>

#part("구문")
```ebnf
forward = "forward" ( "array" | "list" | "pool" ) NAME
        | "forward" "word" NAME "(" effect ")" ;
```

#para("1")[`forward list 이름` 은 뒤에서 선언할 표를 미리 알린다. 레코드가 자기를 담을 표의 줄을 필드로 가질
때 쓴다. 미리 알린 표의 이름은 타입 자리에만 쓴다. `forward word` 는 9.7 이 정한다.]

#ex("spec", "08-forward.pnx")

#part("제약")

#para("2")[미리 알린 표는 같은 모듈에서 같은 종류로 선언돼야 한다.]
#ex("spec", "08-forward-missing.pnx")

== 묶은 저장소 <decl-bound>

#para("1")[`bound` 를 앞에 붙인 `global` `array` `buffer` 는 저장소를 호스트가 준다. 스크립트에서는 다른
전역과 같이 쓴다. 호스트가 묶기 전에는 상태 안의 자리를 쓴다. 묶는 법은 19.7 이 정한다.]

== 공유 저장소 <decl-shared>

#para("1")[`shared` 를 앞에 붙인 `global` `array` `list` `buffer` `index` 는 다른 상태의 저장소를 읽기만 한다.
선언한 상태에는 저장소가 없고, 호스트가 다른 상태의 같은 선언을 `pinax_share` 로 준다(19.7). 같은 자료를 여러
상태가 여러 스레드에서 한꺼번에 읽어도 복사도 잠금도 없다.]

#part("제약")

#para("2")[공유 전역은 아무것도 바꾸지 못한다: 넣기, `set`, `push`, `pop`, `clear`, `truncate`, `add_row`, 필드에
넣기, `insert`, `remove`, 버퍼에 붙이는 낱말, `map_into` 와 `filter_into` 의 목적지가 모두 거절된다. 처리기는
공유 전역을 바꾸는 바이트코드도 적재 때 거절하고, 매개변수로 받은 공유 전역을 바꾸려는 바이트코드는 실행 때 결함
`state` 로 끝낸다.]
#ex("spec", "08-shared-write.pnx")

#para("3")[공유 모음은 그 매개변수를 읽기만 하는 낱말의 모음 매개변수(9.9)로 넘길 수 있다. 낱말이 매개변수를
바꾸는 명령을 쓰거나, 매개변수를 바꾸는 낱말이나 호스트 낱말의 `buffer` 매개변수로 넘기면 그 매개변수를 바꾸는
낱말이다. 처리기는 원본을 다 읽은 뒤에 판단하므로 `forward` 로 알린 낱말에도, 앞선 적재의 낱말에도 같다.]
#ex("spec", "08-shared-param.pnx")
#ex("spec", "08-shared-change.pnx")

#para("3a")[공유 모음은 호스트 낱말의 `buffer` 매개변수와 블록 값이 부르는 낱말(`call`)의 매개변수로는 넘기지 않는다.
받는 쪽이 무엇을 하는지 적재 때 모르기 때문이다. `redefine` 은 읽기만 하던 매개변수를 바꾸는 낱말로 만들지 못한다.]

#para("4")[`shared` 는 `queue` `pool` `tasks` 와 `const` `record` 앞에 오지 않는다.]
#ex("spec", "08-shared-queue.pnx")

#part("의미")

#para("5")[호스트가 주기 전에는 비어 있다: 목록과 배열의 길이는 0 이고, 찾아보기는 아무것도 찾지 못하며, 스칼라와
버퍼는 0 과 빈 바이트다. 적재의 코드는 그대로 돈다. 호스트의 실행은 공유 전역을 모두 받을 때까지 거절된다(19.7).]

#part("진단")
#diag(
  ([이미 있는 이름], "this name is already defined"),
  ([선언 앞에서 쓴 이름], "this name is not declared before this place"),
  ([호스트가 수를 주지 않은 `host` 용량(메시지에 선언의 이름)], "the host gave no capacity for NAME (pinax_capacity)"),
  ([정의 밖의 `to` 가 전역이 아닌 이름에], "outside a definition `to` makes no new local; a global is written with set"),
  ([`assign` 의 값이 칸보다 많음], "assign has more values than the array has cells"),
  ([큐의 원소 타입이 `int` `bool` `err` `dec(N)` 밖], "a queue holds int, bool, err or dec(N) values"),
  ([`alloc` 이 막 준 줄의 `push`(8.8 (5a))], "push can fail and lose an owned row; store it with set, into a field or into a global"),
  ([`row` 의 값이 필드와 다름], "a value has another type than its field"),
  ([`owned` 줄을 넘기지 않는 경로], "an owned value in a local is not given on every path (close, free or store it)"),
  ([미리 알린 표가 없음], "a table announced by forward is never declared"),
  ([공유 전역을 바꿈], "a shared global is read-only: nothing here may change it"),
  ([공유 모음을 그것을 바꾸는 낱말에 넘김], "a shared collection is read-only: the word it is given to changes this parameter"),
  ([공유 모음을 블록 값의 낱말에 넘김], "a shared collection is read-only: it is not given to the word a block value names, which may change it"),
  ([`redefine` 이 읽던 매개변수를 바꿈], "redefine cannot make the word change a collection parameter the old one only read: shared collections may have been given to it"),
  ([공유 버퍼를 호스트 낱말에 넘김], "a shared buffer is read-only: it is not given to a host word, which may write it"),
  ([`shared` 뒤에 다른 선언], "shared stands before global, array, list, buffer or index"),
)
