// embed host
/* Two states on two threads. The shop packs each order into bytes and sends it through the
   channel `orders`; the clerk unpacks and prints them. Nothing is shared but the messages. */
#include "pinax.h"
#include "pinax_host.h"
#include "pinax_loop.h"
#include <stdio.h>
#include <string.h>

static const char shop[] =
    "use orders\nuse bytes\n"
    "record order\n  item str(8)\n  qty int\nend\n"
    "array rows order 1\n"
    "buffer wire 32\n"
    "word send_order (name str, q int)\n"
    "  0 rows idx to r\n"
    "  name r to order.item  q r to order.qty\n"
    "  wire clear\n"
    "  r wire bytes.pack or_else (e) end\n"
    "  wire contents orders.send or_else (e) end\n"
    "end\n"
    "word open_shop (n int gives r int)\n"
    "  \"apple\" 3 send_order  \"pear\" 1 send_order  \"plum\" 12 send_order\n"
    "  orders.close\n"
    "  3\n"
    "end\n";

static const char clerk[] =
    "use orders\nuse bytes\nuse io\nuse str\n"
    "record order\n  item str(8)\n  qty int\nend\n"
    "array rows order 1\n"
    "word serve (n int gives r int)\n"
    "  0 to served\n"
    "  0 rows idx to r\n"
    "  loop\n"
    "    orders.receive or_else (e) \"\" end to m\n"
    "    m str.is_empty until\n"
    "    m r bytes.unpack or_else (e) \"\" end drop\n"
    "    cat (r order.item, \" x\", r order.qty) io.println\n"
    "    served 1 add to served\n"
    "  end\n"
    "  served\n"
    "end\n";

typedef struct side {
    unsigned char block[1 << 20];
    pinax_state *state;
    pinax_word word;
    int64_t in, out;
    pinax_args args;
} side;
static side shop_side, clerk_side;

static void done(void *user, pinax_state *state, pinax_err outcome) {
    (void)user; (void)state;
    if (outcome != PINAX_OK)
        printf("a run ended with %#x\n", (unsigned)outcome);
}

static int prepare(side *s, pinax_channel *orders, const char *source, const char *word) {
    pinax_config config = {.struct_size = sizeof config, .budget = 10000};
    if (pinax_open((pinax_mem){s->block, sizeof s->block}, &config, &s->state) != PINAX_OK ||
        pinax_channel_register(s->state, orders) != PINAX_OK || pinax_host_register(s->state, "io") != PINAX_OK ||
        pinax_load(s->state, "main", (pinax_str){(const unsigned char *)source, strlen(source)}, PINAX_LOAD_CODE) ||
        pinax_find(s->state, word, "(n int gives r int)", 0, &s->word) != PINAX_OK)
        return 1;
    s->args = (pinax_args){.i = &s->in, .ri = &s->out};
    return 0;
}

int main(void) {
    pinax_loop_config loop_config = {.struct_size = sizeof loop_config, .threads = 2};
    pinax_loop *loop;
    pinax_channel *orders;
    if (pinax_loop_open(&loop_config, &loop) != PINAX_OK || pinax_channel_open(loop, "orders", 2, 32, &orders) != PINAX_OK ||
        prepare(&shop_side, orders, shop, "open_shop") || prepare(&clerk_side, orders, clerk, "serve"))
        return 1;
    pinax_loop_start(loop, clerk_side.state, clerk_side.word, &clerk_side.args, done, NULL);
    pinax_loop_start(loop, shop_side.state, shop_side.word, &shop_side.args, done, NULL);
    pinax_loop_wait(loop);
    printf("sent %lld, served %lld\n", (long long)shop_side.out, (long long)clerk_side.out);
    pinax_loop_close(loop);
    pinax_close(shop_side.state);
    pinax_close(clerk_side.state);
    return 0;
}
