// embed
#include "pinax.h"
#include <stdio.h>

/* A host word: the script calls it as calc.tenfold. */
static pinax_err tenfold(pinax_call *call, void *user, const pinax_args *args) {
    (void)call; (void)user;
    args->ri[0] = args->i[0] * 10;
    return PINAX_OK;
}

int main(void) {
    static unsigned char block[1 << 20];          /* the whole state lives in here */
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
    if (pinax_load(st, "main", text, PINAX_LOAD_CODE) != PINAX_OK) {
        pinax_str why = pinax_error_text(st);
        fprintf(stderr, "%.*s\n", (int)why.len, (const char *)why.ptr);
        return 1;
    }

    pinax_word answer;
    pinax_find(st, "answer", "(n int gives r int)", 0, &answer);
    int64_t in = 4, out = 0;
    pinax_args args = {.i = &in, .ri = &out};
    pinax_err err = pinax_run(st, answer, &args);
    while (err == PINAX_SUSPENDED)                 /* the instruction budget ran out */
        err = pinax_resume(st, &args);
    printf("answer(4) = %lld\n", (long long)out);
    pinax_close(st);
    return 0;
}
