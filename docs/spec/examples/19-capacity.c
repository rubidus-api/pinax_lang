// embed
#include "pinax.h"
#include <stdio.h>

/* The host knows at start how many requests it serves at once; the script leaves the number to it. */
int main(void) {
    static unsigned char block[1 << 20];
    pinax_config config = {.struct_size = sizeof config};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;
    static const char source[] =
        "tasks handlers host 32 8192 apart\n"
        "list seen int host\n"
        "word room (gives n int) seen capacity end\n";
    const uint32_t at_once = 48;
    pinax_capacity(st, "handlers", at_once + 1); /* one more for the task that takes the requests */
    pinax_capacity(st, "seen", at_once);
    if (pinax_load(st, "main", (pinax_str){(const unsigned char *)source, sizeof source - 1}, PINAX_LOAD_CODE) != PINAX_OK) {
        pinax_str why = pinax_error_text(st);
        fprintf(stderr, "%.*s\n", (int)why.len, (const char *)why.ptr);
        return 1;
    }
    pinax_word w;
    int64_t n = 0;
    pinax_find(st, "room", "(gives n int)", 0, &w);
    pinax_run(st, w, &(pinax_args){.ri = &n});
    printf("seen holds %lld\n", (long long)n);
    pinax_close(st);
    return 0;
}
