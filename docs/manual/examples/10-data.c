// embed
#include "pinax.h"
#include <stdio.h>
#include <string.h>

/* A settings file the host reads as data: declarations and values, no code. */
static const char settings[] =
    "global port int\n"
    "global name str(16)\n"
    "8080 set port\n"
    "\"edge\" set name\n";
static const char hostile[] = "word boom () end\n";

int main(void) {
    static unsigned char block[1 << 20];
    pinax_config config = {.struct_size = sizeof config};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;
    if (pinax_load(st, "settings", (pinax_str){(const unsigned char *)settings, strlen(settings)}, PINAX_LOAD_DATA) != PINAX_OK)
        return 1;
    pinax_global g;
    pinax_global_find(st, "port", &g);
    printf("port: %lld\n", (long long)*(const int64_t *)g.ptr);
    if (pinax_load(st, "hostile", (pinax_str){(const unsigned char *)hostile, strlen(hostile)}, PINAX_LOAD_DATA) != PINAX_OK) {
        pinax_str why = pinax_error_text(st);
        printf("refused: %.*s\n", (int)why.len, (const char *)why.ptr);
    }
    pinax_close(st);
    return 0;
}
