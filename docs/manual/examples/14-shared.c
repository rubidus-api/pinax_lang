// embed host
/* A word list loaded once and read by two states on two threads at the same time. The states
   declare it `shared`; the host lends them the dictionary state's storage. */
#include "pinax.h"
#include "pinax_loop.h"
#include <stdio.h>
#include <string.h>

static const char dictionary[] =
    "record pair\n  term str(12)\n  meaning str(12)\nend\n"
    "list pairs pair 64\n"
    "index by_term pairs pair.term\n"
    "row pairs (\"sagwa\", \"apple\")\n"
    "row pairs (\"bae\", \"pear\")\n"
    "row pairs (\"podo\", \"grape\")\n";

static const char translator[] =
    "use str\n"
    "record pair\n  term str(12)\n  meaning str(12)\nend\n"
    "shared list pairs pair 64\n"
    "shared index by_term pairs pair.term\n"
    "word known (w str gives int) w by_term lookup or_else (e) 0 return end drop 1 end\n"
    "word count_known (n int gives r int)\n"
    "  n 0 eq if \"sagwa\" known \"kiwi\" known add return end\n"
    "  \"bae\" known \"podo\" known add \"sagwa\" known add\n"
    "end\n";

typedef struct worker {
    unsigned char block[1 << 20];
    pinax_state *state;
    pinax_word word;
    int64_t in, out;
    pinax_args args;
} worker;
static unsigned char dict_block[1 << 20];
static worker w[2];

static void done(void *user, pinax_state *state, pinax_err outcome) {
    (void)user; (void)state;
    if (outcome != PINAX_OK)
        printf("a run ended with %#x\n", (unsigned)outcome);
}

int main(void) {
    pinax_config config = {.struct_size = sizeof config, .budget = 10000};
    pinax_state *dict;
    if (pinax_open((pinax_mem){dict_block, sizeof dict_block}, &config, &dict) != PINAX_OK ||
        pinax_load(dict, "dictionary", (pinax_str){(const unsigned char *)dictionary, strlen(dictionary)},
                   PINAX_LOAD_DATA) != PINAX_OK)
        return 1;
    pinax_loop_config loop_config = {.struct_size = sizeof loop_config, .threads = 2};
    pinax_loop *loop;
    if (pinax_loop_open(&loop_config, &loop) != PINAX_OK)
        return 1;
    for (int k = 0; k < 2; ++k) {
        if (pinax_open((pinax_mem){w[k].block, sizeof w[k].block}, &config, &w[k].state) != PINAX_OK ||
            pinax_load(w[k].state, "main", (pinax_str){(const unsigned char *)translator, strlen(translator)},
                       PINAX_LOAD_CODE) != PINAX_OK ||
            pinax_share(w[k].state, "pairs", dict, "pairs") != PINAX_OK ||     /* the table first */
            pinax_share(w[k].state, "by_term", dict, "by_term") != PINAX_OK || /* then its index */
            pinax_find(w[k].state, "count_known", "(n int gives r int)", 0, &w[k].word) != PINAX_OK)
            return 1;
        w[k].in = k;
        w[k].args = (pinax_args){.i = &w[k].in, .ri = &w[k].out};
        pinax_loop_start(loop, w[k].state, w[k].word, &w[k].args, done, NULL);
    }
    pinax_loop_wait(loop);
    printf("first knows %lld of 2, second knows %lld of 3\n", (long long)w[0].out, (long long)w[1].out);
    pinax_loop_close(loop);
    for (int k = 0; k < 2; ++k)
        pinax_close(w[k].state);
    pinax_close(dict); /* last: the others read its storage */
    return 0;
}
