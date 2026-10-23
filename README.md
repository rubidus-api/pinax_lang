[한국어](README.ko.md) | **English** — **Pinax v0.9.1** — [ZIP(x64)](https://github.com/rubidus-api/pinax_lang/releases/download/v0.9.1/pinax-0.9.1-win64.zip) · [Spec PDF(ko)](https://github.com/rubidus-api/pinax_lang/releases/download/v0.9.1/pinax-spec-ko.pdf) · [Spec PDF(en)](https://github.com/rubidus-api/pinax_lang/releases/download/v0.9.1/pinax-spec-en.pdf) · [Manual PDF(ko)](https://github.com/rubidus-api/pinax_lang/releases/download/v0.9.1/pinax-manual-ko.pdf) · [Manual PDF(en)](https://github.com/rubidus-api/pinax_lang/releases/download/v0.9.1/pinax-manual-en.pdf) · [pinax.h](https://github.com/rubidus-api/pinax_lang/releases/download/v0.9.1/pinax.h) · [pinax.c](https://github.com/rubidus-api/pinax_lang/releases/download/v0.9.1/pinax.c) · [Web](https://rubidus-api.github.io/pinax_lang/)

# Pinax

A small, fast scripting language to embed in C programs, with fixed, flat memory.

> **Status: early, and it runs.** Source files compile and run, from the command `pnx` and
> from a C program. The language is not complete (no floating point
> yet), the current release is 0.9.1, and the bytecode format carries no promise.

Pinax is meant to live inside a host program. The host hands it one block of memory,
registers the functions it may call, and runs scripts in it. Every value has a type and a
place decided when a script is loaded, so the interpreter needs no heap, no garbage
collector and no pointers at run time.

## Goals

- **Easy to embed.** One state object, one memory block, no global state. Linking with C
  and calling existing C libraries comes first in the design.
- **Fast integers.** 64-bit integers with defined overflow behaviour and a full set of bit
  operations.
- **Fast strings.** Slicing, splitting and searching produce views, not copies.
- **Fast serialization.** State is flat memory, so it can be saved and restored as it is;
  records are packed and unpacked by code generated from their declaration.
- **Predictable.** Capacities are declared up front and checked at load. Running out of
  room is an error value, not a crash, and there are no collection pauses.
- **Safe to run untrusted code.** No source, and no bytecode the loader accepts, reads or writes outside the memory the
  host gave it; with an instruction budget every run comes back. A wrong program is refused at load or stops with a
  fault, and never brings the host down.

## Not goals

Systems programming, garbage collection, general heap allocation, object orientation,
dynamic typing.

## The name

*Pinax* (Greek *pinax*, also used in Latin) is a flat board or writing tablet, and by
extension a catalogue. The language keeps its data flat.

## Stack

Pure C23. The only dependency is a vendored copy of
[proven_c_lib](https://github.com/rubidus-api/proven_c_lib) (MIT) under `vendor/proven/`.

Build with any C23 compiler:

```sh
cc -std=c23 -o nob nob.c
./nob build          # build/native/libpinax.a, libpinax_host.a and the command pnx
./nob dist           # build/dist/pinax.h and pinax.c: the whole core as two files
./nob wasm           # build/wasm/pinax.wasm and pinax.mjs, for browsers and node (clang, wasm-ld)
```

A first script, `hello.pnx`. Arguments come before the word that takes them, and the only
punctuation outside literals is `.` `(` `)` `,`:

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

## Embedding

The host interface is one header, `include/pinax.h`. The host gives the state its memory,
registers the C functions scripts may call, loads source and runs words:

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
    while (err == PINAX_SUSPENDED)           /* the instruction budget ran out: go on */
        err = pinax_resume(st, &args);
    printf("%lld\n", (long long)out);        /* 42 */
    pinax_close(st);
    return 0;
}
```

```sh
cc -std=c99 -Iinclude host.c build/native/libpinax.a -lm -o host
```

The state lives entirely inside `block`; Pinax never asks the system for memory. The standard
host modules (`io`, `file`, `clock`, `random`, `env`) are ordinary host words in
`src/host/`, linked only by hosts that want them. The same library, `libpinax_host.a`, has a
host loop that runs many states on a few threads and channels that carry messages between
them (`src/host/pinax_loop.h`); data that is only read is declared `shared` and lent to many
states at once with `pinax_share`, without a copy; several processes map one file written with
`pinax_share_save` (`pinax_map_file`) and read it together.

## Documentation

- [The Pinax Manual](docs/manual/md-en/README.md): the language from the first program to embedding it in C, with
  running and refused examples ([Korean original](docs/manual/md-ko/README.md)).
- [The Pinax Standard](docs/spec/md-en/README.md): the definition in numbered clauses: syntax, constraints,
  semantics, faults and every diagnostic ([Korean original](docs/spec/md-ko/README.md)).

Both are written in Typst and built to PDF, HTML and Markdown by `docs/tools/build.sh`; every program they show is run
and its output captured. The Korean editions are the originals; the English editions have the same
chapters, clauses and examples. Read them on the web at
<https://rubidus-api.github.io/pinax_lang/>, or download the PDFs from the release.

## License

MIT. See [LICENSE](LICENSE). The standard in `docs/spec/` is MIT too; the manual in `docs/manual/` is
CC BY-NC-SA 4.0 with its examples under MIT (see `docs/manual/LICENSE-NOTICE.md`).
