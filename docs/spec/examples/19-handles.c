// embed
#include "pinax.h"
#include <stdio.h>

typedef struct { int value; int open; } counter;
static counter pool_[4];
static uint32_t counter_type;

static void close_counter(void *object, void *user) {   /* called for handles the script never closed */
    (void)user;
    ((counter *)object)->open = 0;
    printf("closed by the state: counter at %d\n", ((counter *)object)->value);
}
static pinax_err c_new(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    for (int k = 0; k < 4; ++k)
        if (!pool_[k].open) {
            pool_[k] = (counter){0, 1};
            pinax_handle h;
            pinax_err err = pinax_handle_new(pinax_call_state(call), counter_type, &pool_[k], &h);
            args->ri[0] = (int64_t)h;
            return err;
        }
    return PINAX_ERR_FULL;
}
static pinax_err c_bump(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    counter *c = pinax_handle_get(pinax_call_state(call), counter_type, (pinax_handle)args->i[0]);
    if (!c)
        return PINAX_ERR_STALE;
    args->ri[0] = ++c->value;
    return PINAX_OK;
}
static pinax_err c_free(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    counter *c = pinax_handle_get(pinax_call_state(call), counter_type, (pinax_handle)args->i[0]);
    if (c) {
        printf("freed by the script: counter at %d\n", c->value);
        c->open = 0;
    }
    return pinax_handle_release(pinax_call_state(call), counter_type, (pinax_handle)args->i[0]);
}

int main(void) {
    static unsigned char block[1 << 20];
    pinax_config config = {.struct_size = sizeof config};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;
    pinax_handle_type(st, "counter", close_counter, NULL, &counter_type);
    static const pinax_word_def words[] = {
        {"new", "(gives owned counter or fail)", c_new, NULL, 0},
        {"bump", "(c counter gives n int)", c_bump, NULL, 0},
        {"free", "(c owned counter)", c_free, NULL, 0},
    };
    pinax_register(st, "ctr", words, 3);
    static const char source[] =
        "use ctr\n"
        "word twice (gives n int or fail)\n"
        "  ctr.new or_fail to c\n"
        "  c ctr.bump drop  c ctr.bump to n\n"
        "  c ctr.free\n"
        "  n\n"
        "end\n"
        "word broken (gives n int or fail)\n"
        "  ctr.new or_fail to c\n"
        "  c ctr.bump 0 div to n\n"            /* a fault while the counter is open */
        "  c ctr.free\n"
        "  n\n"
        "end\n";
    if (pinax_load(st, "main", (pinax_str){(const unsigned char *)source, sizeof source - 1}, PINAX_LOAD_CODE) != PINAX_OK)
        return 1;
    pinax_word w;
    int64_t n = 0;
    pinax_find(st, "twice", "(gives n int or fail)", 0, &w);
    pinax_run(st, w, &(pinax_args){.ri = &n});
    printf("twice = %lld\n", (long long)n);
    pinax_find(st, "broken", "(gives n int or fail)", 0, &w);
    pinax_err err = pinax_run(st, w, &(pinax_args){.ri = &n});
    printf("broken: %s\n", err == PINAX_ERR_DIV_ZERO ? "PINAX_ERR_DIV_ZERO" : "?");
    pinax_close(st);
    return 0;
}
