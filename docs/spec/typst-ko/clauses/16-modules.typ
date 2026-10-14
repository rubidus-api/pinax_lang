#import "/tools/common.typ": *
// shape: an introduction; the modules are the files it includes

= 표준 모듈 <std>

#para("1")[표준 모듈은 처리기에 들어 있다. `use 모듈` 로 열고 `모듈.낱말` 로 쓴다(13.1). 표준 모듈의 낱말은
호스트의 허락 없이 바깥과 닿지 못한다 --- 파일도, 시계도, 환경도 쓰지 않는다.]

#para("2")[표준 모듈의 낱말은 새 저장소를 만들지 않는다. 결과는 다음 가운데 하나다.
- 받은 바이트를 가리키는 뷰. 복사가 없다.
- 구역(12.2)에 새로 쓴 바이트의 뷰.
- 부른 쪽이 준 버퍼에 붙인 바이트. 이런 낱말은 이름이 `_into` 로 끝나거나 버퍼를 받고, 자리가 모자라면
  `err.full` 로 실패한다.]

#include "16-1-str.typ"
#include "16-2-fmt.typ"
#include "16-3-hash.typ"
#include "16-4-encode.typ"
#include "16-5-bytes.typ"
