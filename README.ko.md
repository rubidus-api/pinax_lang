**한국어** | [English](README.md) — **Pinax v0.36.2** — [Spec PDF(ko)](https://github.com/rubidus-api/pinax_lang/releases/download/v0.36.2/pinax-spec-ko.pdf) · [Spec PDF(en)](https://github.com/rubidus-api/pinax_lang/releases/download/v0.36.2/pinax-spec-en.pdf) · [Manual PDF(ko)](https://github.com/rubidus-api/pinax_lang/releases/download/v0.36.2/pinax-manual-ko.pdf) · [Manual PDF(en)](https://github.com/rubidus-api/pinax_lang/releases/download/v0.36.2/pinax-manual-en.pdf) · [pinax.h](https://github.com/rubidus-api/pinax_lang/releases/download/v0.36.2/pinax.h) · [pinax.c](https://github.com/rubidus-api/pinax_lang/releases/download/v0.36.2/pinax.c) · [pinax_native.h](https://github.com/rubidus-api/pinax_lang/releases/download/v0.36.2/pinax_native.h) · [ZIP(Windows x64)](https://github.com/rubidus-api/pinax_lang/releases/download/v0.36.2/pinax-0.36.2-win64.zip) · [웹판](https://rubidus-api.github.io/pinax_lang/) · [플레이그라운드](https://rubidus-api.github.io/pinax_lang/playground/)

# Pinax

C 프로그램 안에 넣어 쓰는 작고 빠른 스크립트 언어입니다. 메모리는 고정되어 있고 평평합니다.

> **상태: 초기 단계이고, 돌아갑니다.** 소스 파일을 컴파일해 실행합니다. 명령 `pnx` 로도, C
> 프로그램 안에서도 됩니다. 언어는 아직 다 갖춰지지 않았습니다(실수가 아직
> 없습니다). 지금 릴리스는 위 줄의 것이고, 바이트코드 형식은 아직 약속이 아닙니다.
>
> **0.31.0 이전에 쓴 스크립트 옮기기:** 그때 문법이 바뀌었습니다(`word` 가 `def`, 서명에 괄호 없음,
> `case` 대신 `match`, `break N .`). 옛 꼴은 새 꼴을 알리는 메시지와 함께 거절되고,
> `CHANGELOG.md` `[0.31.0]` 에 바뀐 것이 모두 적혀 있습니다.

Pinax 는 호스트 프로그램 안에서 돌도록 만든 언어입니다. 호스트가 메모리 한 덩이를 주고,
부를 수 있는 함수를 등록하고, 그 안에서 스크립트를 실행합니다. 모든 값의 타입과 자리가
스크립트를 적재할 때 정해지므로, 실행 중에는 힙도 가비지 컬렉터도 포인터도 필요하지 않습니다.

## 목표

- **넣기 쉽습니다.** 상태 객체 하나, 메모리 한 덩이, 전역 상태 없음. C 와의 연결과 기존
  C 라이브러리 호출을 설계의 맨 앞에 둡니다.
- **정수가 빠릅니다.** 64비트 정수, 정의된 넘침 동작, 넉넉한 비트 연산을 갖춥니다.
- **문자열이 빠릅니다.** 자르기·분해·찾기는 복사가 아니라 뷰를 만듭니다.
- **직렬화가 빠릅니다.** 상태가 평평한 메모리이므로 그대로 저장하고 되살립니다. 레코드는
  선언에서 생성된 코드로 묶고 풉니다.
- **예측할 수 있습니다.** 용량은 미리 선언하고 적재할 때 검사합니다. 자리가 모자라면
  추락이 아니라 오류 값이 나오고, 수거로 인한 멈춤이 없습니다.
- **믿지 못하는 코드도 돌릴 수 있습니다.** 어떤 소스도, 적재기가 받아들인 어떤 바이트코드도 호스트가 준 메모리 밖을 읽거나
  쓰지 않습니다. 명령 예산을 주면 실행은 언제나 돌아옵니다. 틀린 프로그램은 적재 때 거절되거나 결함으로 멈출 뿐,
  호스트를 무너뜨리지 않습니다. 호스트가 등록한 C 함수는 호스트 자신의 코드라서 이 약속 밖에 있고, 스크립트는 호스트가
  등록한 프로시저에만 닿습니다.

## 목표가 아닌 것

시스템 프로그래밍, 가비지 컬렉션, 일반 힙 할당, 객체지향, 동적 타입.

## 이름

*Pinax*(그리스어 πίναξ, 라틴어에서도 씀)는 평평한 판이나 서판을 뜻하고, 거기서 목록이라는
뜻으로 넓어졌습니다. 이 언어는 자료를 평평하게 둡니다.

## 기반

순수 C23 입니다. 의존하는 것은 `vendor/proven/` 에 벤더링한
[proven_c_lib](https://github.com/rubidus-api/proven_c_lib)(MIT) 하나뿐입니다.
`pnx` 와 호스트 라이브러리는 호스트 모듈 `sqlite` 를 위해 `vendor/sqlite/` 에 [SQLite](https://www.sqlite.org/)(퍼블릭 도메인)도
담습니다. 코어, 한 파일 `pinax.c`, 웹 페이지에는 들어 있지 않습니다.

C23 컴파일러로 짓습니다.

```sh
cc -std=c23 -o nob nob.c
./nob build          # 명령 pnx 와 build/native/release/: libpinax.a, libpinax_host.a,
                     # libpinax_sqlite.a. 공개 이름(pinax_)만 전역이다
./nob dist           # build/dist/pinax.h 와 pinax.c: 핵 전체를 두 파일로
./nob wasm           # build/wasm/pinax.wasm 과 pinax.mjs: 브라우저와 node 용(clang, wasm-ld),
                     # 그리고 build/dist/pinax-playground.html: 디스크에서 바로 여는 Pinax 터미널 페이지
```

첫 스크립트 `hello.pnx` 입니다. 인자를 먼저 쓰고 그것을 받는 프로시저를 뒤에 씁니다. 리터럴 밖의
구두점은 `.` `(` `)` `,` 넷뿐입니다.

```text
use io .

def sum_to last int gives total int do
  0 to total
  1 last 1 add range do to i total i add to total end each
  total
end

args "1 to 100 is " 100 sum_to . cat io.println
```

```sh
build/native/pnx hello.pnx        # 1 to 100 is 5050
```

## C 프로그램에 넣기

호스트 인터페이스는 헤더 한 장 `include/pinax.h` 입니다. 호스트가 상태에 메모리를 주고,
스크립트가 부를 C 함수를 등록하고, 소스를 적재하고, 프로시저를 실행합니다.

```c
#include "pinax.h"
#include <stdio.h>

static pinax_err tenfold(pinax_call *call, void *user, const pinax_args *args) {
    (void)call; (void)user;
    if (args->i[0] > INT64_MAX / 10 || args->i[0] < INT64_MIN / 10)
        return PINAX_ERR_OVERFLOW;              /* as the script's own mul would: a fault */
    args->ri[0] = args->i[0] * 10;
    return PINAX_OK;
}

int main(void) {
    static unsigned char block[1 << 20];
    pinax_config config = {.struct_size = sizeof config, .budget = 100000};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;

    static const pinax_word_def words[] = {{"tenfold", "(n int gives r int)", tenfold, NULL, 0}};
    pinax_register(st, "calc", words, 1);

    static const char source[] =
        "use calc .\n"
        "def answer n int gives r int do n calc.tenfold 2 add end\n";
    pinax_str text = {(const unsigned char *)source, sizeof source - 1};
    if (pinax_load(st, "main", text, PINAX_LOAD_CODE) != PINAX_OK)
        return 1;

    pinax_word answer;
    pinax_find(st, "answer", "(n int gives r int)", 0, &answer);
    int64_t in = 4, out = 0;
    pinax_args args = {.i = &in, .ri = &out};
    pinax_err err = pinax_run(st, answer, &args);
    /* 예산은 한 조각을 막을 뿐 실행 전체를 막지 않습니다: 멈추는 것은 호스트의 몫입니다. 여기서는
       100 조각까지 허용합니다. 서버라면 조각 사이에 이벤트 루프로 돌아갈 자리입니다. */
    for (int slices = 1; err == PINAX_SUSPENDED && slices < 100; ++slices)
        err = pinax_resume(st, &args);
    if (err != PINAX_OK) {
        fprintf(stderr, "스크립트가 끝나지 않았습니다: 오류 %d\n", (int)err);
        return 1;
    }
    printf("%lld\n", (long long)out);        /* 42 */
    pinax_close(st);
    return 0;
}
```

```sh
cc -std=c99 -Iinclude host.c build/native/release/libpinax.a -lm -o host
```

호스트가 기대는 세 가지(모자란 블록은 거절되고, 예산은 한 조각을 막으며 실행은 이어져 결과에 이르고, 등록한 C 함수는 불리고 그
실패는 돌아온다)는 `./nob contract`(`docs/spec/examples/19-contract.c`)로 자기 컴파일러와 플랫폼에서 확인합니다. `pnx --stats
script.pnx` 는 스크립트가 쓴 것을 알려 주어 블록의 크기를 정하게 합니다. Lua 와 LuaJIT 에 견준 측정은
[`benchmarks/`](benchmarks/) 에 있습니다.

상태는 `block` 안에만 삽니다. Pinax 는 시스템에 메모리를 청하지 않습니다. 표준 호스트
모듈(`io` `file` `clock` `random` `env`)은 `src/host/` 에 있는 평범한 호스트 프로시저이고, 필요한
호스트만 링크합니다(`libpinax_host.a`. 모듈 `sqlite` 는 `libpinax_sqlite.a` 이고 그 안의 SQLite 는
감춰져 있어서, 호스트가 자기 SQLite 를 함께 링크해도 됩니다). 호스트 라이브러리에는 여러 상태를 몇 개의 스레드에서 돌리는
호스트 루프와, 상태 사이로 메시지를 나르는 채널도 있습니다(`src/host/pinax_loop.h`). 읽기만 하는
자료는 `shared` 로 선언하고 `pinax_share` 로 여러 상태에 복사 없이 빌려줍니다. 여러 프로세스라면
`pinax_share_save` 로 쓴 파일을 각자 `pinax_map_file` 로 사상해 함께 읽습니다.

## 문서

- [Pinax 매뉴얼](docs/manual/md-ko/README.md): 첫 프로그램부터 C 프로그램에 넣기까지, 돌아가는 예제와 거절되는 예제와
  함께 배우는 책입니다([영어판](docs/manual/md-en/README.md)).
- [Pinax 표준](docs/spec/md-ko/README.md): 언어의 정의입니다. 구문, 제약, 의미, 결함과 모든 진단을 번호 붙은 조항으로
  적었습니다([영어판](docs/spec/md-en/README.md)).

두 문서 모두 Typst 로 쓰고 `docs/tools/build.sh` 로 PDF, HTML, Markdown 을 짓습니다. 실린 프로그램은 모두 실제로 돌린
것이고, 출력도 그때 받은 그대로입니다. 웹판은 <https://rubidus-api.github.io/pinax_lang/> 에서 읽고, PDF 는
릴리스에서 받습니다.

## 라이선스

MIT. [LICENSE](LICENSE) 를 보십시오. `docs/spec/` 의 표준도 MIT 이고, `docs/manual/` 의 매뉴얼은 본문이
CC BY-NC-SA 4.0, 예제가 MIT 입니다(`docs/manual/LICENSE-NOTICE.md`).
