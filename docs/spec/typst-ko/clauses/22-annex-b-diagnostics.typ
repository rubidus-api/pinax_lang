#import "/tools/common.typ": *
// shape: collected from the clauses

#set heading(numbering: (..n) => if n.pos().len() == 1 { "부속서 B" } else { "B." + n.pos().slice(1).map(str).join(".") })
= 진단 목록 (참고) <annex-diagnostics>

#para("1")[이 부속서는 본문의 진단 표에 나온 메시지를 모은다. 메시지는 이 구현이 내는 글이다(4 (3)). 왼쪽은 그
메시지를 적은 조항이다.]

#all-diagnostics()
