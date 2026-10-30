#import "/tools/common.typ": *

= 낱말 <words>

== 정의 <words-definition>

#part("구문")
```ebnf
definition = [ "export" ] "word" NAME "(" effect ")" { body } "end" ;
effect     = [ params ] [ "gives" params ] [ "or" "fail" ] ;
params     = param { "," param } ;
param      = [ NAME ] ( type | collection-type ) ;   (* collection-type: 9.9 *)
type       = [ "owned" ] ( NAME [ "(" size ")" ] | "word" "(" effect ")" ) ;
```

#para("1")[`word 이름 (서명) 몸 end` 는 *낱말*을 정의한다. 낱말은 정의 밖, 소스의 맨
바깥에서만 정의한다. 낱말 안에 낱말을 정의하지 못한다.]

#para("2")[낱말을 #term("부른다", en: "call")는 것은 몸에서 그 이름을 쓰는 것이다. 부르는 자리에서 스택의 맨
위 값들이 낱말의 입력이 되고, 낱말이 끝나면 결과가 그 자리에 놓인다. 핵 낱말, 표준 모듈의 낱말, 호스트
낱말도 같은 모양으로 부른다.]

== 서명 <words-signature>

#para("1")[모든 낱말에는 *서명*이 있다. 입력, `gives` 와 결과, 그리고 실패할 수 있으면 `or fail` 을
이 차례로 적는다. 입력도 결과도 없으면 `()` 다. 서명은 적재 때 검사된다: 부르는 자리의 스택은 입력과
맞아야 하고, 몸이 끝나는 자리의 스택은 결과와 맞아야 한다.]

#para("2")[입력에 이름을 쓰면(`w int`) 그 값은 그 이름의 지역 변수가 되고 몸은 빈 스택으로 시작한다. 타입만
쓰면(`int`) 값은 스택에 놓인 채 몸이 시작한다. 결과 쪽의 이름은 읽는 사람을 위한 표시일 뿐 지역 변수를
만들지 않는다. 입력은 모음을 받을 수도 있다(9.9).]

#ex("spec", "09-signatures.pnx")

#part("제약")

#para("3")[한 서명의 입력은 모두 이름이 있거나 모두 타입만 있어야 한다.]
#ex("spec", "09-signature-mixed.pnx")

#para("4")[몸이 끝나는 자리(그리고 `return` 하는 자리)에서 스택에는 결과가 정확히 그 차례와 타입으로 있어야
한다. 더 있어도 모자라도 거절된다.]
#ex("spec", "09-result-missing.pnx")

== 스택 <words-stack>

#para("1")[낱말과 낱말 사이의 값은 *스택*에 놓인다. 리터럴과 이름 읽기는 값 하나를 올리고, 낱말은
입력을 내리고 결과를 올린다. 처리기는 적재 때 몸의 모든 자리에서 스택의 깊이와 칸마다의 타입을 안다.]

#para("2")[스택을 고치는 핵 낱말은 다섯뿐이다.
#table(columns: (auto, auto, 1fr), inset: 5pt, stroke: 0.5pt + luma(170),
  [*낱말*], [*전*  →  *후*], [*뜻*],
  [`dup`], [`a` → `a a`], [맨 위를 하나 더],
  [`drop`], [`a` → ], [맨 위를 버림],
  [`swap`], [`a b` → `b a`], [위의 둘을 바꿈],
  [`over`], [`a b` → `a b a`], [둘째를 위로 하나 더],
  [`nip`], [`a b` → `b`], [둘째를 버림],
)
이보다 깊은 조작(`rot` `pick` `roll` `depth`)은 없다. 값이 셋 이상 얽히면 지역 변수를 쓴다.]

#ex("spec", "09-stack-words.pnx")

#note[스택을 처음 보는 사람을 위해 `2 3 add 4 mul` 을 한 토큰씩 따라가면 이렇다(맨 위는 오른쪽).
#trace(
  ("2", "2", [값을 올린다.]),
  ("3", "2 3", [값을 올린다.]),
  ("add", "5", [두 값을 내려 합을 올린다.]),
  ("4", "5 4", [값을 올린다.]),
  ("mul", "20", [두 값을 내려 곱을 올린다.]),
)
처리기는 적재 때 이 표의 둘째 칸(깊이와 타입)을 모든 자리에서 미리 안다. 그래서 (3) 과 (3a) 의 잘못은 돌기 전에 드러난다.]

#part("제약")

#para("3")[낱말이 받을 값이 스택에 모자라면 거절된다. 스택의 깊이는 돌 때 달라지지 않는다.]
#ex("spec", "09-stack-short.pnx")

#para("3a")[정의 밖의 코드(5.2)가 끝날 때, 곧 소스의 끝에서 스택에 값이 남으면 거절된다. 낱말의 몸이 끝날 때의 스택은
9.2 가 정한다.]
#ex("spec", "09-leftover.pnx")

#para("4")[정의되지 않은 이름을 쓰면 거절된다.]
#ex("spec", "09-unknown-word.pnx")

== 지역 변수 <words-locals>

#part("구문")
```ebnf
store = "to" ( NAME | PATH | "(" NAME { "," NAME } ")" ) ;
set   = "set" ( NAME | PATH ) ;
```

#para("1")[몸 안에서 `to 이름` 은 맨 위 값을 내려 그 이름에 넣는다. 그 이름이 처음이면 그 값의 타입으로
#term("지역 변수", en: "local variable")가 생긴다. 그 뒤의 `to` 는 같은 타입이어야 한다. `to` 는 지역 변수(와
`to r.필드` 의 필드)에만 넣는다.]

#para("1a")[전역에는 `set 이름` 으로 넣는다. `v set 이름` 은 스칼라 전역에, `v i set 이름` 은 배열과 목록의 칸에
넣는다. 다른 모듈이 내보낸 전역은 `set 모듈.이름` 이다. 정의 안팎이 같다. `to` 가 지역을 만드는 일과 전역을 바꾸는
일을 글자로 가르려는 규칙이다.]

#para("2")[`to (a, b, c)` 는 위의 값 셋을 한 번에 넣는다. 오른쪽 이름이 맨 위 값을 받는다 --- 서명에 적은
차례와 같다.]

#ex("spec", "09-locals.pnx")
#ex("spec", "09-store-global.pnx")

#para("3")[지역 변수가 보이는 범위는 처음 넣은 자리부터 그것을 둘러싼 가지, 루프, 블록의 끝까지다. 바깥에서
만든 지역 변수에는 안에서 넣을 수 있다.]

#part("제약")

#para("4")[지역 변수에 처음과 다른 타입의 값을 넣으면 거절된다.]
#ex("spec", "09-local-type.pnx")

#para("5")[가지 안에서 처음 만든 지역 변수는 가지 밖에서 읽지 못한다. 다른 가지로 왔다면 값이 없기 때문이다.]
#ex("spec", "09-local-scope.pnx")

#para("6")[`to` 의 이름이 전역이면 거절된다. 전역은 가릴 수 없으므로(9.5) 그 이름의 지역을 만들 수도 없고, 전역을
조용히 바꾸지도 않는다. 메시지가 `set` 을 알려 준다. `set` 의 이름이 전역이 아니면 거절된다.]
#ex("spec", "09-store-global-bare.pnx")

== 가림 금지 <words-shadowing>

#para("1")[새 이름(매개변수, 지역 변수, 루프의 이름)은 그 자리에서 보이는 이름 --- 낱말, 전역, 상수, 타입,
바깥의 지역 변수 --- 과 같을 수 없다. 이름 하나가 한 자리에서 늘 한 가지만 뜻하게 하려는 규칙이다.]

#part("제약")

#para("2")[보이는 이름과 같은 매개변수나 지역 변수는 거절된다.]
#ex("spec", "09-shadow.pnx")
#ex("spec", "09-shadow-word.pnx")

#para("3")[언어의 낱말 --- 예약어, 핵심 낱말, 모듈 없이 쓰는 15절과 16절의 낱말, 타입 --- 은 어디서도 새 이름이 될 수
없다. 낱말, 선언, 매개변수, 지역 변수, 루프의 이름이 모두 그렇다. 예약어는 필드의 이름도 될 수 없다. 다른 언어의 낱말은
필드의 이름이 될 수 있다. 필드는 늘 레코드 뒤에 쓰기 때문이다(8.8). 메시지는 그것이 어느 낱말인지 말한다.]
#ex("spec", "09-name-taken.pnx")

== 되부름과 꼬리 부름 <words-recursion>

#para("1")[낱말은 자기를 부를 수 있다. 앞선 선언은 필요 없다.]

#para("2")[낱말의 마지막 일로 다른 낱말을 부르는 #term("꼬리 부름", en: "tail call")은 부른 낱말의 자리를
다시 쓴다. 그래서 꼬리 부름으로 도는 되부름은 깊이가 자라지 않는다. 따로 표시하지 않는다.]

#ex("spec", "09-recursion.pnx")

#part("결함")

#para("3")[꼬리 부름이 아닌 부름이 쌓여 호출 깊이의 한계(부속서 C)를 넘으면 결함 `depth` 다.]
#ex("spec", "09-depth.pnx")

== 앞선 낱말 선언 <words-forward>

#para("1")[`forward word 이름 (서명)` 은 뒤에서 정의할 낱말을 미리 알린다. 서로를 부르는 두 낱말에 쓴다.]

#ex("spec", "09-forward-word.pnx")

#part("제약")

#para("2")[정의의 서명은 미리 알린 서명과 같아야 한다. 같은 모듈에서 정의되지 않으면 거절된다.]
#ex("spec", "09-forward-other.pnx")

== 글 잇기 <words-cat>

#part("구문")
```ebnf
cat    = "cat" "(" phrase { "," phrase } ")" ;
phrase = body { body } ;                   (* code that leaves one value *)
```

#para("1")[`cat ( … )` 은 쉼표로 가른 항목을 이어 `str` 하나를 낸다. 항목마다 값 하나를 남기는 코드다.
`str` 은 그대로, `int` 는 십진으로, `bool` 은 `true` `false` 로 들어간다. 다른 모양(십육진 등)은 그 모양을
만드는 낱말(16.2)로 먼저 `str` 을 만든다.]

#para("2")[결과는 구역(12.2)에 만든 뷰다. 항목의 수는 글에 보이는 만큼이고, 처리기는 길이를 먼저 더해 한 번에
쓴다.]

#ex("spec", "09-cat.pnx")

== 모음 매개변수 <words-collections>

#part("구문")
```ebnf
collection-type = ( "array" | "list" | "queue" ) cell | "buffer" ;
cell            = NAME [ "(" size ")" ] ;   (* int, bool, err, dec(N), str(N), 레코드, 또는 표: 그 줄 *)
```

#para("1")[타입이 `array T`, `list T`, `queue T` 나 `buffer` 인 입력은 모음을 받는다. 부르는 쪽은 그 자리에 모음의
이름을 쓴다(`scores total`). 몸 안에서 매개변수는 그런 모음의 이름이 들어가는 모든 자리에 쓴다 --- `l len`,
`i l idx`, `v l push`, `v i set l`, `l do … end each`, `… end l map_into`, `q take`, `s b append` 따위. 칸은 매개변수가 말하는 타입이어야
한다: `int` `bool` `err`, `N` 이 같은 `dec(N)` 과 `str(N)`, 또는 레코드. 용량은 타입에 들지 않는다. 어떤 용량이든 받고,
`l capacity` 가 그 용량을 준다.]

#ex("spec", "09-coll-param.pnx")

#para("2")[매개변수 `l` 이 레코드 표인 낱말에서는, 선언된 표의 이름처럼(8.8) 이름 `l` 이 그 줄의 타입이기도 하다.
`l` 로 얻은 줄은 그 낱말 안에서 쓴다: 지역 변수, 필드, 고리의 블록에서.]

#ex("spec", "09-coll-param-rows.pnx")

#para("2a")[`map_into` 와 `filter_into`(10.6)는 매개변수가 가리키는 목록이나 배열에 담고, 매개변수가 가리키는 표에서
줄을 베낀다.]

#ex("spec", "09-coll-param-fill.pnx")

#para("3")[넘기는 것은 모음 그 자체이고 복사본이 아니다. 모음은 전역이므로 매개변수를 `yield` 와 14.3 의 기다림을
건너 쥐고 있어도 된다.]

#part("제약")

#para("4")[모음은 그 종류와 칸이 매개변수의 것과 같은 자리에 주어야 한다.]
#ex("spec", "09-coll-param-kind.pnx")

#para("5")[모음 매개변수는 값이 아니다: `to` 로 담지 못하고, 낱말의 결과가 되지 못하고, 블록(`word (…)`)의 매개변수가
되지 못한다. 풀과 찾아보기는 받지 않는다.]
#ex("spec", "09-coll-param-result.pnx")

#para("6")[목록 매개변수를 자르는(8.5 (2a)) 낱말, 또는 자르는 낱말에 그것을 넘기는 낱말은 받은 목록을 자른다. 그러면
그 낱말을 부르는 자리에서 그 목록에 8.5 (2b) 의 규칙이 적용된다.]
#ex("spec", "09-coll-param-truncate.pnx")

#para("7")[`filter_into` 는 두 표가 모두 매개변수일 때 줄을 베끼지 않는다. 한쪽을 `each` 로 돌며 `add_row` 로 담는다.]
#ex("spec", "09-coll-param-copy.pnx")

#part("진단")
#diag(
  ([입력에 이름 있는 것과 없는 것이 섞임], "inputs are all named or all bare types, not a mix"),
  ([끝나는 자리의 스택이 결과와 다름], "the stack does not hold exactly the results of the signature here"),
  ([스택의 값이 모자람], "the stack does not hold enough values for this word"),
  ([소스의 끝에 값이 남음], "values are left on the stack at the end of the source"),
  ([정의되지 않은 이름], "this name is not declared before this place"),
  ([지역 변수의 타입이 바뀜], "this local variable has another type than the value stored into it"),
  ([`to` 의 이름이 전역], "this name is a global; a global is written with set: value set name"),
  ([`to 모듈.이름` 으로 전역에], "a global of a module is written with set: value set module.name"),
  ([`set` 의 이름이 지역], "set writes a global; a local is written with to"),
  ([`set` 의 이름이 전역이 아님], "set writes a declared global; there is none of this name"),
  ([보이는 이름을 가림], "this name is already visible here; a new name may not hide it"),
  ([언어의 낱말을 새 이름으로 씀(메시지에 그 낱말)], "NAME is a word of the language; choose another name"),
  ([앞선 선언과 다른 서명], "the definition has another signature than its forward declaration"),
  ([종류나 칸이 다른 모음], "the collection given here is of another kind, or holds another type, than the parameter says"),
  ([모음 자리에 값], "this parameter takes an array, list, queue or buffer: give its name"),
  ([결과로 모음], "a word gives no collection; it takes one as a parameter"),
  ([모음 매개변수에 `to`], "this is a collection parameter; a cell of it is stored as: value index set name"),
  ([남이 쥔 목록을 자르는 낱말에 줌], "this list's rows are kept by a field, a global, a collection, a named row or an index; the word it is given to truncates it"),
  ([두 표가 모두 매개변수인 줄 베끼기], "filter_into does not copy rows between two tables parameters name; walk one and add_row"),
)
