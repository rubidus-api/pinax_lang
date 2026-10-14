#import "/tools/common.typ": *

= 낱말 <words>

== 정의 <words-definition>

#part("구문")
```ebnf
definition = [ "export" ] "word" NAME "(" effect ")" { body } "end" ;
effect     = [ params ] [ "gives" params ] [ "or" "fail" ] ;
params     = param { "," param } ;
param      = [ NAME ] type ;
type       = [ "owned" ] ( NAME [ "(" size ")" ] | "word" "(" effect ")" ) ;
```

#para("1")[`word 이름 (서명) 몸 end` 는 #term("낱말", en: "word")을 정의한다. 낱말은 정의 밖, 소스의 맨
바깥에서만 정의한다. 낱말 안에 낱말을 정의하지 못한다.]

#para("2")[낱말을 #term("부른다", en: "call")는 것은 몸에서 그 이름을 쓰는 것이다. 부르는 자리에서 스택의 맨
위 값들이 낱말의 입력이 되고, 낱말이 끝나면 결과가 그 자리에 놓인다. 핵 낱말, 표준 모듈의 낱말, 호스트
낱말도 같은 모양으로 부른다.]

== 서명 <words-signature>

#para("1")[모든 낱말에는 #term("서명")이 있다. 입력, `gives` 와 결과, 그리고 실패할 수 있으면 `or fail` 을
이 차례로 적는다. 입력도 결과도 없으면 `()` 다. 서명은 적재 때 검사된다: 부르는 자리의 스택은 입력과
맞아야 하고, 몸이 끝나는 자리의 스택은 결과와 맞아야 한다.]

#para("2")[입력에 이름을 쓰면(`w int`) 그 값은 그 이름의 지역 변수가 되고 몸은 빈 스택으로 시작한다. 타입만
쓰면(`int`) 값은 스택에 놓인 채 몸이 시작한다. 결과 쪽의 이름은 읽는 사람을 위한 표시일 뿐 지역 변수를
만들지 않는다.]

#ex("spec", "09-signatures.pnx")

#part("제약")

#para("3")[한 서명의 입력은 모두 이름이 있거나 모두 타입만 있어야 한다.]
#ex("spec", "09-signature-mixed.pnx")

#para("4")[몸이 끝나는 자리(그리고 `return` 하는 자리)에서 스택에는 결과가 정확히 그 차례와 타입으로 있어야
한다. 더 있어도 모자라도 거절된다.]
#ex("spec", "09-result-missing.pnx")

== 스택 <words-stack>

#para("1")[낱말과 낱말 사이의 값은 #term("스택")에 놓인다. 리터럴과 이름 읽기는 값 하나를 올리고, 낱말은
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

#part("제약")

#para("3")[낱말이 받을 값이 스택에 모자라면 거절된다. 스택의 깊이는 돌 때 달라지지 않는다.]
#ex("spec", "09-stack-short.pnx")

#para("4")[정의되지 않은 이름을 쓰면 거절된다.]
#ex("spec", "09-unknown-word.pnx")

== 지역 변수 <words-locals>

#part("구문")
```ebnf
store = "to" ( NAME | PATH | "(" NAME { "," NAME } ")" ) ;
```

#para("1")[몸 안에서 `to 이름` 은 맨 위 값을 내려 그 이름에 넣는다. 그 이름이 처음이면 그 값의 타입으로
#term("지역 변수", en: "local variable")가 생긴다. 그 뒤의 `to` 는 같은 타입이어야 한다. 이름이 전역이면 그
전역에 넣는다(지역 변수를 만들지 않는다).]

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

== 가림 금지 <words-shadowing>

#para("1")[새 이름(매개변수, 지역 변수, 루프의 이름)은 그 자리에서 보이는 이름 --- 낱말, 전역, 상수, 타입,
바깥의 지역 변수 --- 과 같을 수 없다. 이름 하나가 한 자리에서 늘 한 가지만 뜻하게 하려는 규칙이다.]

#part("제약")

#para("2")[보이는 이름과 같은 매개변수나 지역 변수는 거절된다.]
#ex("spec", "09-shadow.pnx")
#ex("spec", "09-shadow-word.pnx")

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

#part("진단")
#diag(
  ([입력에 이름 있는 것과 없는 것이 섞임], "inputs are all named or all bare types, not a mix"),
  ([끝나는 자리의 스택이 결과와 다름], "the stack does not hold exactly the results of the signature here"),
  ([스택의 값이 모자람], "the stack does not hold enough values for this word"),
  ([정의되지 않은 이름], "this name is not declared before this place"),
  ([지역 변수의 타입이 바뀜], "this local variable has another type than the value stored into it"),
  ([보이는 이름을 가림], "this name is already visible here; a new name may not hide it"),
  ([앞선 선언과 다른 서명], "the definition has another signature than its forward declaration"),
)
