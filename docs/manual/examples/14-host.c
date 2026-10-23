// embed
/* A host that lets users write pricing rules in Pinax. The host gives one word (the stock
   level), loads the user's rules, and asks them for a price; a broken rule never takes the
   host down. */
#include "pinax.h"
#include <stdio.h>
#include <string.h>

static int64_t stock[3] = {120, 4, 0};

static pinax_err stock_of(pinax_call *call, void *user, const pinax_args *args) {
    (void)call; (void)user;
    if (args->i[0] < 0 || args->i[0] >= 3)
        return PINAX_ERR_NOT_FOUND;                  /* (item int gives n int or fail) */
    args->ri[0] = stock[args->i[0]];
    return PINAX_OK;
}

static void report(pinax_state *st, const char *what, pinax_err err) {
    pinax_str why = pinax_error_text(st);
    printf("%s: error %#x, %.*s\n", what, (unsigned)err, (int)why.len, (const char *)why.ptr);
}

int main(void) {
    static unsigned char block[256 * 1024]; /* the state takes about 150 KB, a load its working memory */
    pinax_config config = {.struct_size = sizeof config, .region_bytes = 4096, .budget = 10000};
    pinax_state *st;
    pinax_err err = pinax_open((pinax_mem){block, sizeof block}, &config, &st);
    if (err != PINAX_OK) {
        printf("block too small: %zu bytes needed\n", pinax_needed(st));
        return 1;
    }
    static const pinax_word_def words[] = {{"stock_of", "(item int gives n int or fail)", stock_of, NULL, 0}};
    pinax_register(st, "shop", words, 1);

    /* the user's rules: scarce items cost more */
    static const char rules[] =
        "use shop\n"
        "export word price (item int, base int gives p int)\n"
        "  item shop.stock_of or_else (e) 0 end to left\n"
        "  left 0 eq if 0 return end\n"
        "  left 10 lt if base 3 mul 2 div else base end\n"
        "end\n";
    err = pinax_load(st, "rules", (pinax_str){(const unsigned char *)rules, strlen(rules)}, PINAX_LOAD_CODE);
    if (err != PINAX_OK) {
        report(st, "rules refused", err);
        return 1;
    }
    pinax_word price;
    if (pinax_find(st, "price", "(item int, base int gives p int)", 0, &price) != PINAX_OK) {
        report(st, "no price word", PINAX_ERR_NOT_FOUND);
        return 1;
    }
    for (int64_t item = 0; item < 4; ++item) {
        int64_t in[2] = {item, 1000}, out = 0;
        pinax_args args = {.i = in, .ri = &out};
        err = pinax_run(st, price, &args);
        while (err == PINAX_SUSPENDED)
            err = pinax_resume(st, &args);
        if (err != PINAX_OK)
            report(st, "price failed", err);
        else
            printf("item %lld: %lld\n", (long long)item, (long long)out);
    }

    /* a second set of rules with a mistake is refused; the first stays loaded */
    static const char broken[] = "word discount (p int gives int) p \"10\" sub end\n";
    err = pinax_load(st, "broken", (pinax_str){(const unsigned char *)broken, strlen(broken)}, PINAX_LOAD_CODE);
    if (err != PINAX_OK)
        report(st, "broken rules refused", err);
    pinax_close(st);
    return 0;
}
