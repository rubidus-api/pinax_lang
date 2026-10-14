// embed
#include "pinax.h"
#include <stdio.h>

/* A host word that finishes later: it starts the work, keeps a token and returns PINAX_PENDING. */
static pinax_token waiting[4];
static int64_t asked[4];
static int n_waiting;
static pinax_err fetch(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    asked[n_waiting] = args->i[0];
    waiting[n_waiting++] = pinax_call_pending(call);
    return PINAX_PENDING;
}

int main(void) {
    static unsigned char block[1 << 20];
    pinax_config config = {.struct_size = sizeof config};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;
    static const pinax_word_def words[] = {{"fetch", "(n int gives r int or fail)", fetch, NULL, PINAX_WORD_AWAITABLE}};
    pinax_register(st, "net", words, 1);
    static const char source[] =
        "use net\n"
        "tasks workers 2\n"
        "list got int 4\n"
        "word both (gives r int)\n"
        "  do 1 net.fetch or_else (e) -1 end got push or_else (e) end end workers spawn or_else (e) 0 return end drop\n"
        "  do 2 net.fetch or_else (e) -1 end got push or_else (e) end end workers spawn or_else (e) 0 return end drop\n"
        "  workers run_all\n"
        "  got sum\n"
        "end\n";
    if (pinax_load(st, "main", (pinax_str){(const unsigned char *)source, sizeof source - 1}, PINAX_LOAD_CODE) != PINAX_OK)
        return 1;
    pinax_word w;
    int64_t r = 0;
    pinax_args args = {.ri = &r};
    pinax_find(st, "both", "(gives r int)", 0, &w);
    pinax_err err = pinax_run(st, w, &args);
    printf("run: %s, %d operations started\n", err == PINAX_WAITING ? "PINAX_WAITING" : "?", n_waiting);
    /* the host finishes them in its own time, here the second first */
    for (int k = n_waiting - 1; k >= 0; --k) {
        int64_t result = asked[k] * 100;
        pinax_complete(st, waiting[k], PINAX_OK, &(pinax_args){.ri = &result});
    }
    err = pinax_resume(st, &args);
    printf("resume: %s, both = %lld\n", err == PINAX_OK ? "PINAX_OK" : "?", (long long)r);
    pinax_close(st);
    return 0;
}
