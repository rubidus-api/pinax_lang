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
것뿐이다.]

#para("3")[`use` 하는 모듈은 이미 있어야 한다: 표준 모듈, 호스트가 등록한 모듈, 또는 먼저 적재된 소스.
`pnx` 는 `use 이름` 을 만나면 프로그램 파일 옆의 `이름.pnx` 를 먼저 적재한다.]

#ex("spec", "13-use.pnx")
#ex("spec", "13-use-as.pnx")

#part("제약")

#para("4")[없는 모듈을 `use` 하면 거절된다.]
#ex("spec", "13-unknown-module.pnx")

#para("5")[모듈의 낱말을 맨 이름으로 쓰면 거절된다. 열지 않은 모듈의 경로도 거절된다.]
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
