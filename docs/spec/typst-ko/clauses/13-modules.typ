#import "/tools/common.typ": *

= 모듈 <modules>

== 쓰기 <modules-use>

#part("구문")
```ebnf
import = "use" NAME [ "as" NAME ] ;
```

#para("1")[소스 하나는 *모듈*하나다. 호스트가 소스를 적재할 때 준 이름이 모듈의 이름이다(`pnx` 는
파일 이름에서 `.pnx` 를 뗀 것을 쓴다). 표준 모듈과 호스트 모듈(13.3)도 모듈이다.]

#para("2")[`use 모듈` 은 그 모듈을 이 소스에서 연다. 그 뒤로 모듈의 것은 *경로*`모듈.이름` 으로 쓴다.
`use 모듈 as 별명` 은 같은 모듈을 `별명.이름` 으로 쓰게 한다. 맨 이름으로 쓰는 것은 핵 낱말과 이 소스가 선언한
것, 그리고 찾는 순서가 이름 붙인 모듈의 낱말(13.5)뿐이다.]

#para("3")[`use` 하는 모듈은 이미 있어야 한다: 표준 모듈, 호스트가 등록한 모듈, 또는 먼저 적재된 소스.
`pnx` 는 `use 이름` 을 만나면 프로그램 파일 옆의 `이름.pnx` 를 먼저 적재한다.]

#ex("spec", "13-use.pnx")
#ex("spec", "13-use-as.pnx")

#part("제약")

#para("4")[없는 모듈을 `use` 하면 거절된다.]
#ex("spec", "13-unknown-module.pnx")

#para("5")[모듈의 낱말을 맨 이름으로 쓰면 거절된다. 찾는 순서(13.5)가 그 모듈을 이름 붙였으면 그렇지 않다. 열지 않은 모듈의
경로도 거절된다.]
#ex("spec", "13-bare-name.pnx")
#ex("spec", "13-no-use.pnx")

== 내보내기 <modules-export>

#para("1")[다른 소스가 쓸 수 있는 것은 `export` 를 붙여 선언하거나 정의한 것뿐이다. 낱말, 상수, 전역은
`모듈.이름` 으로 쓰고(내보낸 전역에 넣을 때는 `set 모듈.이름`), 내보낸 `record` 와 `type` 과 표의 줄 타입은 이름만으로
쓴다.]

#para("2")[아래 모듈 `geometry` 는 13.1 의 예가 쓴 것이다. `square` 는 내보내지 않았다.]

#ex("spec", "geometry.pnx")

#part("제약")

#para("3")[내보내지 않은 것을 쓰면 거절된다.]
#ex("spec", "13-not-exported.pnx")

== 표준 모듈과 호스트 모듈 <modules-kinds>

#para("1")[#term("표준 모듈", en: "standard module")은 처리기에 들어 있는 언어의 일부다: `str` `fmt` `hash`
`encode` `bytes` `ptn` `rex` `json` `csv`(16절). `use` 해야 보인다.]

#para("2")[#term("호스트 모듈", en: "host module")은 호스트가 등록한 낱말의 묶음이다. 호스트가 등록하지 않은
모듈은 없다. 스크립트가 할 수 있는 바깥일(파일, 시계, 환경)은 호스트가 등록한 낱말이 전부다. `pnx` 는 표준
호스트 모듈 `io` `file` `clock` `random` `env` 를 등록한다(17절).]

#para("3")[`err` 는 `use` 없이 경로로 쓰는 상수의 묶음이다(11.1).]

== 자료 적재 <modules-data>

#part("구문")
```ebnf
data-source = { data-item } ;
data-item   = [ "export" ] ( [ "bound" ] bindable | "const" NAME literal
              | "list" NAME type size | "queue" NAME type size | "pool" NAME NAME size
              | "record" NAME { field | "big_endian" } "end" | "type" NAME base
              | "index" NAME NAME PATH )
            | "forward" ( "array" | "list" | "pool" ) NAME
            | row-item | assign-item
            | data-value [ data-value ] "set" NAME              (* a global, or a cell *)
            | data-value NAME "to" PATH ;                      (* a field of a named row *)
data-value  = literal | NAME ;                      (* NAME: a const or a row name *)
```

#para("1")[호스트가 자료 적재로 읽는 소스는 선언과 값만 받는다. 낱말 정의, 낱말 부르기, `use`, `extern`,
`test`, 제어, 블록, `cat` 은 거절된다. 그래서 그런 소스는 아무 코드도 돌리지 못한다. 설정과 자료처럼 믿지 못하는
글을 읽을 때 쓴다.]

#para("2")[값은 `값 set 이름` 으로, 칸은 `값 번호 set 이름` 으로(`"10.0.0.1" 0 set allow`), 이름 붙은 줄의 필드는 `값 줄 to
레코드.필드` 로 넣는다. 코드와 같다(8.8, 9.4). 자료 적재의 소스가
선언한 것은 모두 내보낸 것으로 본다.]

#ex("spec", "13-data-load.c")

#part("진단")
#diag(
  ([없는 모듈], "no module of this name is registered or loaded"),
  ([열지 않은 모듈이나 없는 레코드의 경로], "this path names neither a module in use nor a record"),
  ([내보내지 않은 이름], "the module exports nothing of this name"),
  ([자료 적재의 정의나 `use`], "a data source defines no words and uses no modules"),
  ([자료 적재의 낱말 부르기], "a data source calls no words; only literals, const names and stores"),
)

== 찾는 순서 <modules-search>

#part("구문")
```ebnf
order     = "set_order" "(" [ order-one { "," order-one } ] ")" | "get_order" ;
order-one = NAME | "get_order" ;
```

#para("1")[`set_order (io, str)` 는 #term("찾는 순서", en: "search order")를 준다: 맨 이름을 찾아볼 모듈들이다. 이름 붙인
모듈은 `use` 처럼 열린다(13.1 (2), (3)). `pnx` 는 `use` 때처럼 프로그램 옆의 파일 모듈을 먼저 적재한다. `set_order ()` 는
순서를 비운다. 찾는 순서가 없으면 어느 모듈에서도 찾지 않는다.]

#para("1a")[괄호 안의 `get_order` 는 그 자리의 찾는 순서를 뜻한다: `set_order (json, get_order)` 는 `json` 을 그 앞에,
`set_order (get_order, csv)` 는 그 뒤에 둔다. `get_order` 가 다시 데려온 모듈, 또는 `get_order` 가 데려온 뒤에 다시 이름
붙인 모듈은 처음 자리를 지킨다.]

#para("2")[코드의 맨 이름은 이 차례로 찾고, 처음 가진 것을 쓴다: 지역 변수와 매개변수, 이 소스(또는 세션)가 선언한 것, 핵
낱말, 그리고 찾는 순서의 모듈들을 왼쪽부터. 그래서 소스의 이름과 핵 낱말은 순서에 무엇이 있든 뜻이 바뀌지 않고, 순서에 더한
모듈이 전에 컴파일된 코드를 바꾸지 못한다. 핵 낱말과 이름이 같은 모듈의 낱말(`len` 곁의 `str.len`)은 경로로 쓴다. 경로는
전과 같이 읽는다.]

#ex("spec", "13-order.pnx")
#ex("spec", "13-order-core.pnx")

#para("3")[`set_order` 는 소스나 세션 줄의 맨 위 수준에 필요한 만큼 쓸 수 있다. 하나하나가 그 자리부터 소스 끝까지의 순서를
바꾸고, 세션은 다음 순서가 나올 때까지 뒤 줄들에 그것을 지닌다. 낱말은 컴파일될 때 그 이름들이 찾은 것을 쓴다.]

#para("3a")[맨 위 수준에 홀로 쓴 `get_order` 는 그 자리의 찾는 순서를 `set_order` 가 받는 꼴로, `order: (io, str)` 처럼
`config.diag` 에 줄 0 의 진단으로 알리고(19.4 (1)), 아무것도 바꾸지 않는다. 소스를 검사할 때 알린다: 파일에서는 파일의 코드가 돌기 전이다.]

#para("4")[찾는 순서로 닿는 이름은 이름 가리기 규칙(9.5)에 들지 않는다: 지역 변수는 순서에 있는 모듈의 낱말과 같은 이름일
수 있고, 그 낱말 안에서는 지역 변수를 뜻한다.]

#part("제약")

#para("5")[정의나 블록 안의 `set_order` 나 `get_order`, 모듈을 두 번 이름 붙이거나 16 개보다 많이 이름 붙인 순서, 없는
모듈을 이름 붙인 순서는 거절된다.]

#part("진단")
#diag(
  ([정의 안의 `set_order`], "set_order stands at the top level, outside definitions and blocks"),
  ([정의 안의 `get_order`], "get_order stands at the top level, or inside set_order (...)"),
  ([괄호가 없음], "set_order is followed by modules in parentheses: set_order (io, str)"),
  ([모듈 이름이 아님], "set_order names modules, separated by commas: set_order (io, str)"),
  ([모듈을 두 번], "this module is named twice in the order"),
  ([16 개보다 많음], "an order names at most 16 modules"),
  ([언어의 낱말이 아닌 `search (…)`], "search is now set_order: set_order (io, str)"),
)
