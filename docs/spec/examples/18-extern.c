// embed
#include "pinax.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Two C functions the script names. Every integer is an intptr_t; a str is (pointer, length). */
static intptr_t gcd(intptr_t a, intptr_t b) {
    while (b) { intptr_t t = a % b; a = b; b = t; }
    return a;
}
static intptr_t count_vowels(const char *s, size_t n) {
    intptr_t k = 0;
    for (size_t i = 0; i < n; ++i)
        k += strchr("aeiou", s[i]) != NULL && s[i] != '\0';
    return k;
}

/* The host decides what "mathlib" means: here, a table of its own functions. */
static pinax_extern_fn resolve(void *user, pinax_str library, pinax_str symbol) {
    (void)user;
    if (library.len != 7 || memcmp(library.ptr, "mathlib", 7) != 0)
        return NULL;
    if (symbol.len == 3 && memcmp(symbol.ptr, "gcd", 3) == 0)
        return (pinax_extern_fn)gcd;
    if (symbol.len == 12 && memcmp(symbol.ptr, "count_vowels", 12) == 0)
        return (pinax_extern_fn)count_vowels;
    return NULL;
}

static void try_load(pinax_state *st, const char *name, const char *text) {
    if (pinax_load(st, name, (pinax_str){(const unsigned char *)text, strlen(text)}, PINAX_LOAD_CODE) != PINAX_OK) {
        pinax_str why = pinax_error_text(st);
        printf("%s: %.*s\n", name, (int)why.len, (const char *)why.ptr);
    }
}

int main(void) {
    static unsigned char block[1 << 20];
    pinax_config config = {.struct_size = sizeof config};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;
    pinax_set_resolver(st, resolve, NULL);
    try_load(st, "main",
             "extern gcd (a int, b int gives int) from \"mathlib\"\n"
             "extern count_vowels (s str gives int) from \"mathlib\" as vowels\n"
             "word answer (gives int) 84 36 gcd \"embeddable\" vowels add end\n");
    pinax_word w;
    int64_t out = 0;
    pinax_find(st, "answer", "(gives int)", 0, &w);
    pinax_run(st, w, &(pinax_args){.ri = &out});
    printf("gcd(84, 36) + vowels(\"embeddable\") = %lld\n", (long long)out);

    try_load(st, "other", "extern sqrt (x int gives int) from \"libm\"\n");   /* the host gives no such thing */
    pinax_close(st);
    return 0;
}
