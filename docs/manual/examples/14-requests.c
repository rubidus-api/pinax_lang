// embed
#include "pinax.h"
#include <stdio.h>

/* Requests as tasks. web.next gives the id of the next request (-1: no more). Each request is
 * served by a task of its own, which asks the host for a page with web.fetch. fetch waits: it
 * keeps a token and returns PINAX_PENDING, so the other tasks go on. The host answers the first
 * fetch, and gives up on the second as a timeout would: a failure, which the task takes. */
static const int64_t arriving[] = {1, 2, -1};
static int next_at;
static pinax_err web_next(pinax_call *call, void *user, const pinax_args *args) {
    (void)call; (void)user;
    args->ri[0] = arriving[next_at < 2 ? next_at++ : 2];
    return PINAX_OK;
}

static pinax_token fetching[2];
static int n_fetching;
static pinax_err web_fetch(pinax_call *call, void *user, const pinax_args *args) {
    (void)user; (void)args;
    fetching[n_fetching++] = pinax_call_pending(call);
    return PINAX_PENDING;
}

static pinax_err web_reply(pinax_call *call, void *user, const pinax_args *args) {
    (void)call; (void)user;
    printf("request %lld: %.*s\n", (long long)args->i[0], (int)args->s[0].len, (const char *)args->s[0].ptr);
    return PINAX_OK;
}

int main(void) {
    static unsigned char block[1 << 19];
    pinax_config config = {.struct_size = sizeof config, .budget = 100000};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;
    static const pinax_word_def words[] = {
        {"next", "(gives id int)", web_next, NULL, 0},
        {"fetch", "(id int gives page str or fail)", web_fetch, NULL, PINAX_WORD_AWAITABLE},
        {"reply", "(id int, body str)", web_reply, NULL, 0},
    };
    pinax_register(st, "web", words, 3);
    static const char source[] =
        "use web\n"
        "tasks requests 4\n"
        "queue incoming int 4\n"
        "word serve ()\n"
        "  incoming take or_return to id\n"
        "  id web.fetch or_else (e) cat (\"gave up: \", e err_text) end to page\n"
        "  id page web.reply\n"
        "end\n"
        "word start (id int)\n"
        "  do serve end requests spawn or_else (e) id \"busy\" web.reply return end drop\n"
        "  id incoming put or_else (e) end\n"
        "end\n"
        "word main ()\n"
        "  loop web.next to id  id 0 lt until  id start end\n"
        "  requests run_all\n"
        "end\n";
    if (pinax_load(st, "main", (pinax_str){(const unsigned char *)source, sizeof source - 1}, PINAX_LOAD_CODE) != PINAX_OK) {
        pinax_str why = pinax_error_text(st);
        fprintf(stderr, "%.*s\n", (int)why.len, (const char *)why.ptr);
        return 1;
    }
    pinax_word w;
    pinax_find(st, "main", "()", 0, &w);
    pinax_err err = pinax_run(st, w, &(pinax_args){0});
    printf("%s: %d requests wait for the host\n", err == PINAX_WAITING ? "PINAX_WAITING" : "?", n_fetching);

    pinax_str page = {(const unsigned char *)"page one", 8};
    pinax_complete(st, fetching[0], PINAX_OK, &(pinax_args){.rs = &page}); /* the first fetch came back */
    pinax_complete(st, fetching[1], PINAX_ERR_CANCELLED, NULL);             /* the second took too long */
    err = pinax_resume(st, &(pinax_args){0});
    printf("%s\n", err == PINAX_OK ? "PINAX_OK" : "?");
    pinax_close(st);
    return 0;
}
