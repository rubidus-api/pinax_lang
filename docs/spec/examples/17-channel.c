// embed host
#include "pinax.h"
#include "pinax_loop.h"
#include <stdio.h>
#include <string.h>

/* One state sends the numbers 1 to 100 through the channel `jobs`; two others take them and
   add them up. The loop runs the three on two threads. */
static const char producer[] =
    "use jobs\nuse str\n"
    "word produce (n int gives r int)\n"
    "  1 to i\n"
    "  loop\n"
    "    i n gt until\n"
    "    i str.to_str jobs.send or_else (e) -1 return end\n"
    "    i 1 add to i\n"
    "  end\n"
    "  jobs.close\n"
    "  n\n"
    "end\n";
static const char consumer[] =
    "use jobs\nuse str\n"
    "word consume (n int gives r int)\n"
    "  0 to total\n"
    "  loop\n"
    "    jobs.receive or_else (e) \"\" end to m      rem not_found: closed and empty\n"
    "    m str.is_empty until\n"
    "    m str.to_int or_else (e) 0 end total add to total\n"
    "  end\n"
    "  total\n"
    "end\n";

typedef struct worker {
    unsigned char block[1 << 20];
    pinax_state *state;
    pinax_word word;
    int64_t in, out;
    pinax_args args;
    pinax_err outcome;
} worker;
static worker w[3];

static void done(void *user, pinax_state *state, pinax_err outcome) {
    (void)state;
    ((worker *)user)->outcome = outcome;
}

static int start(pinax_loop *loop, pinax_channel *jobs, worker *x, const char *source, const char *word) {
    pinax_config config = {.struct_size = sizeof config, .budget = 1000};
    if (pinax_open((pinax_mem){x->block, sizeof x->block}, &config, &x->state) != PINAX_OK ||
        pinax_channel_register(x->state, jobs) != PINAX_OK ||
        pinax_load(x->state, "main", (pinax_str){(const unsigned char *)source, strlen(source)}, PINAX_LOAD_CODE) ||
        pinax_find(x->state, word, "(n int gives r int)", 0, &x->word) != PINAX_OK)
        return 1;
    x->args = (pinax_args){.i = &x->in, .ri = &x->out};
    return pinax_loop_start(loop, x->state, x->word, &x->args, done, x) != PINAX_OK;
}

int main(void) {
    pinax_loop_config loop_config = {.struct_size = sizeof loop_config, .threads = 2};
    pinax_loop *loop;
    pinax_channel *jobs;
    if (pinax_loop_open(&loop_config, &loop) != PINAX_OK || pinax_channel_open(loop, "jobs", 4, 20, &jobs) != PINAX_OK)
        return 1;
    w[0].in = 100;
    if (start(loop, jobs, &w[1], consumer, "consume") || start(loop, jobs, &w[2], consumer, "consume") ||
        start(loop, jobs, &w[0], producer, "produce"))
        return 1;
    pinax_loop_wait(loop);
    printf("sent %lld, received %lld\n", (long long)w[0].out, (long long)(w[1].out + w[2].out));

    /* the channel is closed and empty now: one more receiver ends at once */
    w[1].in = 0;
    if (pinax_loop_start(loop, w[1].state, w[1].word, &w[1].args, done, &w[1]) != PINAX_OK)
        return 1;
    pinax_loop_wait(loop);
    printf("after close: %lld\n", (long long)w[1].out);

    /* a receiver on a channel nobody sends to: every run left waits, so the loop ends it */
    pinax_channel *idle;
    pinax_close(w[2].state);
    if (pinax_channel_open(loop, "jobs", 1, 20, &idle) != PINAX_OK || start(loop, idle, &w[2], consumer, "consume"))
        return 1;
    pinax_loop_wait(loop);
    printf("nobody sends: %s\n", w[2].outcome == PINAX_ERR_DEADLOCK ? "deadlock" : "?");
    pinax_loop_close(loop);
    for (int k = 0; k < 3; ++k)
        pinax_close(w[k].state);
    return 0;
}
