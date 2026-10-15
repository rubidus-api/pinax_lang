#import "/tools/common.typ": *
// shape: what this edition leaves out

#set heading(numbering: (..n) => if n.pos().len() == 1 { "부속서 D" } else { "D." + n.pos().slice(1).map(str).join(".") })
= 이 판에 없는 것 (참고) <annex-later>

#para("1")[아래는 설계에는 자리가 있으나 이 판의 처리기가 받지 않는 것이다. 쓰면 적재가 거절된다. 앞으로의 판이
정할 때까지 이 문서는 그 뜻을 정하지 않는다.
- 실수: 타입 `float`, 실수 리터럴(6.4 (8). 점만 있는 수는 `dec` 이므로(7.5) 실수 리터럴은 지수를 쓴다), 실수 칸, 모듈
  `float`, `fmt.fixed`, 레코드의 `f32` `f64` 필드.
- 사용자가 정하는 선언(`make`, 6.6 (5)).
- 레코드 안에 레코드를 넣는 필드와 필드별 배치 `columns`.
- 압축 모듈 `deflate`.]
