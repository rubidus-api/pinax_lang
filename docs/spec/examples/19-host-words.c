// embed
#include "pinax.h"
#include <stdio.h>
#include <string.h>

/* (s str gives r str): the result is written into the caller's region. */
static pinax_err shout(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    pinax_mem room = pinax_call_alloc(call, args->s[0].len + 1);
    if (!room.ptr)
        return PINAX_ERR_FULL;                       /* no `or fail`: this is a fault */
    unsigned char *out = room.ptr;
    for (size_t k = 0; k < args->s[0].len; ++k) {
        unsigned char c = args->s[0].ptr[k];
        out[k] = c >= 'a' && c <= 'z' ? (unsigned char)(c - 32) : c;
    }
    out[args->s[0].len] = '!';
    args->rs[0] = (pinax_str){out, args->s[0].len + 1};
    return PINAX_OK;
}

/* (s str gives n int or fail): an outcome other than PINAX_OK is the word's failure. */
static pinax_err digits(pinax_call *call, void *user, const pinax_args *args) {
    (void)call; (void)user;
    int64_t n = 0;
    if (args->s[0].len == 0)
        return PINAX_ERR_FORMAT;
    for (size_t k = 0; k < args->s[0].len; ++k) {
        unsigned char c = args->s[0].ptr[k];
        if (c < '0' || c > '9')
            return PINAX_ERR_FORMAT;
        n = n * 10 + (c - '0');
    }
    args->ri[0] = n;
    return PINAX_OK;
}

/* (b buffer or fail): writes straight into a buffer the script declared. */
static pinax_err stamp(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    pinax_mem room = pinax_call_buffer(call, args->i[0], 0);
    if (room.len < 5)
        return PINAX_ERR_FULL;
    memcpy(room.ptr, "[ok] ", 5);
    pinax_call_buffer(call, args->i[0], 5);
    return PINAX_OK;
}

int main(void) {
    static unsigned char block[1 << 20];
    pinax_config config = {.struct_size = sizeof config};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;
    static const pinax_word_def words[] = {
        {"shout", "(s str gives r str)", shout, NULL, 0},
        {"digits", "(s str gives n int or fail)", digits, NULL, 0},
        {"stamp", "(b buffer or fail)", stamp, NULL, 0},
    };
    pinax_register(st, "app", words, 3);
    static const char source[] =
        "use app\n"
        "buffer line 32\n"
        "word demo (gives s str)\n"
        "  line clear\n"
        "  line app.stamp or_else (e) end\n"
        "  \"12x\" app.digits or_else (e) -1 end to n\n"
        "  cat (\"hello\" app.shout, \" \", \"42\" app.digits or_else (e) -1 end, \" \", n) line append or_else (e) end\n"
        "  line contents\n"
        "end\n";
    if (pinax_load(st, "main", (pinax_str){(const unsigned char *)source, sizeof source - 1}, PINAX_LOAD_CODE) != PINAX_OK)
        return 1;
    pinax_word w;
    pinax_str out;
    if (pinax_find(st, "demo", "(gives s str)", 0, &w) != PINAX_OK)
        return 1;
    pinax_run(st, w, &(pinax_args){.rs = &out});
    printf("%.*s\n", (int)out.len, (const char *)out.ptr);
    pinax_close(st);
    return 0;
}
