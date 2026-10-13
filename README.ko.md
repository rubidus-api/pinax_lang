**한국어** | [English](README.md) — **Pinax**

# Pinax

C 프로그램 안에 넣어 쓰는 작고 빠른 스크립트 언어입니다. 메모리는 고정되어 있고 평평합니다.

> **상태: 초기 단계이고, 돌아갑니다.** 소스 파일을 컴파일해 실행합니다. 명령 `pnx` 로도, C
> 프로그램 안에서도 됩니다. 언어는 아직 다 갖춰지지 않았습니다(실수가 아직
> 없습니다). 릴리스한 것이 없고, 바이트코드 형식은 아직 약속이 아닙니다.

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

## 목표가 아닌 것

시스템 프로그래밍, 가비지 컬렉션, 일반 힙 할당, 객체지향, 동적 타입.

## 이름

*Pinax*(그리스어 πίναξ, 라틴어에서도 씀)는 평평한 판이나 서판을 뜻하고, 거기서 목록이라는
뜻으로 넓어졌습니다. 이 언어는 자료를 평평하게 둡니다.

## 기반

순수 C23 입니다. 의존하는 것은 `vendor/proven/` 에 벤더링한
[proven_c_lib](https://github.com/rubidus-api/proven_c_lib)(MIT) 하나뿐입니다.

C23 컴파일러로 짓습니다.

```sh
cc -std=c23 -o nob nob.c
./nob build          # build/native/libpinax.a, libpinax_host.a, 명령 pnx
./nob dist           # build/dist/pinax.h 와 pinax.c: 핵 전체를 두 파일로
```

첫 스크립트 `hello.pnx` 입니다. 인자를 먼저 쓰고 그것을 받는 낱말을 뒤에 씁니다. 리터럴 밖의
구두점은 `.` `(` `)` `,` 넷뿐입니다.

```text
use io

word sum_to (last int gives total int)
  0 to total
  1 last 1 add range do (i int) total i add to total end each
  total
end

cat ("1 to 100 is ", 100 sum_to) io.println
```

```sh
build/native/pnx hello.pnx        # 1 to 100 is 5050
```

## C 프로그램에 넣기

호스트 인터페이스는 헤더 한 장 `include/pinax.h` 입니다. 호스트가 상태에 메모리를 주고,
스크립트가 부를 C 함수를 등록하고, 소스를 적재하고, 낱말을 실행합니다.

```c
#include "pinax.h"
#include <stdio.h>

static pinax_err tenfold(pinax_call *call, void *user, const pinax_args *args) {
    (void)call; (void)user;
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
        "use calc\n"
        "word answer (n int gives r int) n calc.tenfold 2 add end\n";
    pinax_str text = {(const unsigned char *)source, sizeof source - 1};
    if (pinax_load(st, "main", text, PINAX_LOAD_CODE) != PINAX_OK)
        return 1;

    pinax_word answer;
    pinax_find(st, "answer", "(n int gives r int)", 0, &answer);
    int64_t in = 4, out = 0;
    pinax_args args = {.i = &in, .ri = &out};
    pinax_err err = pinax_run(st, answer, &args);
    while (err == PINAX_SUSPENDED)           /* 명령 예산이 다했습니다. 이어 돌립니다 */
        err = pinax_resume(st, &args);
    printf("%lld\n", (long long)out);        /* 42 */
    pinax_close(st);
    return 0;
}
```

```sh
cc -std=c99 -Iinclude host.c build/native/libpinax.a -lm -o host
```

상태는 `block` 안에만 삽니다. Pinax 는 시스템에 메모리를 청하지 않습니다. 표준 호스트
모듈(`io` `file` `clock` `random` `env`)은 `src/host/` 에 있는 평범한 호스트 낱말이고, 필요한
호스트만 링크합니다.

## 라이선스

MIT. [LICENSE](LICENSE) 를 보십시오.
