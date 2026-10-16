#import "/tools/common.typ": *

= 글과 바이트의 형식 <formats>

#opening[5장(뷰, 버퍼), 7장(레코드), 8장(실패).][
- CSV 나 JSON 을 나무를 만들지 않고 어떻게 읽고 쓰는가?
- 레코드를 바이트로 보내고 다시 읽으려면?
- 해시와 Base64 같은 바꾸기는 어디에 있는가?
]

Pinax 에는 힙이 없으므로 JSON 을 객체의 나무로 읽지 않는다. 대신 #strong[한 방향으로] 읽는다: 토큰 하나, 필드 하나씩.
쓸 때는 버퍼에 바로 쓴다. 레코드와 맞추는 일은 레코드의 선언에서 만들어진 코드가 한다.

== CSV 와 JSON

`csv.unpack` 은 한 줄을 읽어 레코드의 필드에 차례로 넣고, `json.pack` 은 레코드를 필드 이름이 키인 객체로 쓴다.

#ex("manual", "11-csv-to-json.pnx")

줄을 먼저 더하고 `unpack` 이 실패하면 `before cities truncate` 로 그 줄을 되돌린다. `truncate` 는 목록의 길이를
더하기 전의 길이로 줄인다.

#mistake[레코드 목록에서도 `pop` 으로 마지막 줄을 뺀다][레코드의 목록에는 `pop` 이 없다(값의 목록에만 있다). 마지막
줄은 `truncate` 로 거둔다. 다만 그 목록의 줄을 찾아보기나 필드가 담고 있으면 `truncate` 도 거절된다. 그때는 줄을 더하기
전에 확인한다.
#ex("manual", "11-pop-records.pnx")]

JSON 을 토큰으로 읽으면 깊이와 상관없이 필요한 것만 고를 수 있다. `json.next` 는 토큰의 갈래, 값의 뷰, 나머지를 준다.

#ex("manual", "11-json-read.pnx")

`json.next` 가 주는 셋을 한 토큰씩 보자. `s json.next` 는 `(kind int, value str, rest str)` 를 준다: 토큰의 갈래, 그 토큰의
글(뷰), 그리고 그 뒤에 남은 글이다. 남은 글을 다시 `json.next` 에 주는 것을 되풀이하면 글 전체를 앞에서부터 한 번 훑는다.
글이 끝나면 `json.next` 가 실패하고, 루프는 그 실패로 끝난다.

#ex("manual", "11-json-tokens.pnx")

#trace(
  ("rest json.next", "json.object \"{\" …", [`{` 를 읽었다. 갈래 `json.object`, 남은 글은 `"name": …`.]),
  ([`or_else (e) break end`], "json.object \"{\" …", [실패하지 않았으므로 몸을 건너뛴다.]),
  ("to (kind, value, rest)", "—", [셋을 이름에 담는다. 오른쪽 `rest` 가 맨 위 값을 받는다.]),
)

갈래는 `json.object` 처럼 모듈의 상수이므로 `case` 가 아니라 `eq` 로 견준다(4장). 갈래마다 할 일만 고르면 되므로, 깊이가
얼마든 필요한 값만 뽑을 수 있다. `11-json-read.pnx` 는 `json.number` 만 골라 더했다.

== 바이트

모듈 `bytes` 는 정해진 폭과 바이트 순서로 쓰고 읽는다. 레코드에 `big_endian` 을 적으면 `bytes.pack` 이 큰 끝부터 쓴다.
`varint` 필드는 작은 수를 적은 바이트로 쓴다.

#ex("manual", "11-binary.pnx")

쓰는 낱말(`pack`, `put_…`, `encode.…_encode`)은 버퍼가 차면 실패한다. 그래서 뒤에 늘 지킴 낱말이 와야 한다.

#ex("manual", "11-pack-unguarded.pnx")

#qa[메모리 안의 레코드도 큰 끝으로 놓이는가?][아니다. `big_endian` 은 `pack` 과 `unpack` 이 쓰는 바이트의 차례만
정한다. 메모리 안의 모양은 처리기가 정하고, 스크립트는 그것을 볼 수 없다.]

== 그 밖의 바꾸기

`fmt` 는 수를 십육진, 이진, 0 을 채운 모양으로 바꾼다. `hash` 는 CRC-32, FNV-1a, SipHash 를, `encode` 는 Base64,
십육진, URL 퍼센트 부호화를 준다. 바꾼 결과를 버퍼에 쓰는 낱말은 버퍼가 차면 실패한다. 자세한 목록은 표준 16절에 있다.

#summary[
- `json.next` 와 `csv.fields` 는 한 토큰, 한 필드씩 읽는다. 나무를 만들지 않는다(표준 16.8, 16.9).
- `pack` 은 버퍼에 쓰고, `unpack` 은 모두 맞을 때만 줄에 쓴다(16.5, 16.8, 16.9).
- `big_endian` 은 바이트 순서만 정한다(16.5).
- 레코드의 목록에는 `pop` 이 없다(15.3).
]
