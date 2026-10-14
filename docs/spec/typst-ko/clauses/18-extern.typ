#import "/tools/common.typ": *

= C 함수 부르기 <extern>

#part("구문")
```ebnf
extern = [ "export" ] "extern" c-name "(" effect ")" "from" STRING [ "as" NAME ] ;
c-name = NAME | reserved-word | STRING ;
```

#para("1")[`extern 이름 (서명) from "라이브러리"` 는 C 함수 하나를 낱말로 삼는다. 처리기는 라이브러리를 스스로
열지 않는다. 적재할 때 호스트의 #term("해결 함수", en: "resolver")에 (라이브러리, 이름)을 묻고, 호스트가 준
함수를 부른다. 호스트가 해결 함수를 두지 않거나 함수를 주지 않으면 적재가 거절된다. 그래서 스크립트가 부를 수
있는 C 함수는 호스트가 정한다.]

#para("2")[끝에 `as 다른이름` 을 붙이면 스크립트는 그 이름으로 부르고, 호스트에게 묻는 이름은 그대로다. C 함수의
이름이 핵 낱말이나 예약 낱말과 같거나 이름의 글자가 아닌 글자를 담으면(따옴표로 적는다) `as` 가 있어야 한다.]

#para("3")[서명에 쓸 수 있는 것: 인자는 `int`(그리고 `bool`, 선언한 타입처럼 `int` 로 표현되는 값), 핸들, `str`,
`cstr`. 결과는 `int` 나 핸들 하나이거나 없다. `or fail` 은 핸들 결과에만 붙이며, 함수가 널 포인터를 주면
실패다. `cstr` 는 이 서명에만 쓰는 타입이다.]

#para("4")[부르는 방식: `int` 와 핸들의 객체 포인터는 `intptr_t` 인자 하나씩, `str` 는 `(const char *, size_t)`
두 인자(NUL 로 끝나지 않는다), `cstr` 는 NUL 로 끝나는 복사본의 `const char *` 하나다. C 인자는 여덟까지다.
결과는 `intptr_t` 하나로 읽는다. 다른 모양의 함수(실수 인자, `int` 결과, 구조체, 가변 인자)는 호스트 낱말로
감싼다.]

#ex("spec", "18-extern.c")

#para("5")[서명이 C 함수와 맞지 않으면 C 의 미정의 동작이다. 처리기는 그것을 알 수 없다. 믿지 못하는 스크립트를
돌리는 호스트는 해결 함수를 두지 않는다.]

#part("제약")

#para("6")[이미 있는 이름(핵 낱말 등)의 함수는 `as` 로 다른 이름을 줘야 한다.]
#ex("spec", "18-extern-as.pnx")

#para("7")[(3)이 허락하지 않는 서명은 거절된다.]
#ex("spec", "18-extern-result.pnx")

#part("진단")
#diag(
  ([`as` 없이 이미 있는 이름], "this name is already defined: give the function another with `as`"),
  ([허락하지 않는 서명], "extern: int, handle, str and cstr arguments (eight for C at most), one int or handle result; or fail only with a handle result"),
  ([호스트가 함수를 주지 않음], "extern: the host does not give this function"),
)
