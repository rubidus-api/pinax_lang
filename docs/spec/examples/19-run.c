// embed
#include "pinax.h"
#include <stdio.h>
#include <string.h>

static void load(pinax_state *st, const char *text) {
    if (pinax_load(st, "main", (pinax_str){(const unsigned char *)text, strlen(text)}, PINAX_LOAD_CODE) != PINAX_OK) {
        pinax_str why = pinax_error_text(st);
        printf("refused: %.*s\n", (int)why.len, (const char *)why.ptr);
    }
}

int main(void) {
    static unsigned char block[1 << 20];
    pinax_config config = {.struct_size = sizeof config, .budget = 1000};   /* instructions per run or resume */
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;
    load(st,
         "word total (n int gives r int) 0 to s  1 n 1 add range do (i int) s i add to s end each  s end\n"
         "word ratio (a int, b int gives r int) a 100 mul b div end\n");

    pinax_word w;
    /* the signature asked for must be the word's */
    printf("find with another signature: %s\n",
           pinax_find(st, "total", "(gives r int)", 0, &w) == PINAX_ERR_SIGNATURE ? "PINAX_ERR_SIGNATURE" : "?");
    pinax_find(st, "total", "(n int gives r int)", 0, &w);
    int64_t n = 100000, r = 0;
    pinax_args args = {.i = &n, .ri = &r};
    int resumes = 0;
    pinax_err err = pinax_run(st, w, &args);
    while (err == PINAX_SUSPENDED) {                 /* the budget ran out: go on where it stopped */
        ++resumes;
        err = pinax_resume(st, &args);
    }
    printf("total(100000) = %lld after %s resumes\n", (long long)r, resumes > 10 ? "many" : "few");

    /* a fault comes back as the error code, with its place in the error text */
    pinax_find(st, "ratio", "(a int, b int gives r int)", 0, &w);
    int64_t ab[2] = {3, 0};
    err = pinax_run(st, w, &(pinax_args){.i = ab, .ri = &r});
    pinax_str why = pinax_error_text(st);
    printf("ratio(3, 0): %s, \"%.*s\"\n", err == PINAX_ERR_DIV_ZERO ? "PINAX_ERR_DIV_ZERO" : "?", (int)why.len, (const char *)why.ptr);

    /* the state is still usable */
    ab[1] = 4;
    err = pinax_run(st, w, &(pinax_args){.i = ab, .ri = &r});
    printf("ratio(3, 4) = %lld\n", (long long)r);

    /* one line at a time, as a session does */
    const char *line = "6 7 mul \"done\"";
    if (pinax_eval(st, (pinax_str){(const unsigned char *)line, strlen(line)}) == PINAX_OK) {
        for (uint32_t k = 0; k < pinax_eval_count(st); ++k) {
            pinax_value v;
            pinax_eval_value(st, k, &v);
            if (v.kind == PINAX_VALUE_INT)
                printf("value %u: %lld (%.*s)\n", k, (long long)v.i, (int)v.type.len, (const char *)v.type.ptr);
            else
                printf("value %u: %.*s (%.*s)\n", k, (int)v.s.len, (const char *)v.s.ptr, (int)v.type.len, (const char *)v.type.ptr);
        }
    }
    pinax_close(st);
    return 0;
}
