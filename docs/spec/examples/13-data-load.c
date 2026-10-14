// embed
#include "pinax.h"
#include <stdio.h>
#include <string.h>

static pinax_err load(pinax_state *st, const char *name, const char *text) {
    pinax_err err = pinax_load(st, name, (pinax_str){(const unsigned char *)text, strlen(text)}, PINAX_LOAD_DATA);
    if (err != PINAX_OK) {
        pinax_str why = pinax_error_text(st);
        printf("%s refused: %.*s\n", name, (int)why.len, (const char *)why.ptr);
    }
    return err;
}

int main(void) {
    static unsigned char block[1 << 20];
    pinax_config config = {.struct_size = sizeof config};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;

    /* A settings file: declarations and values only. */
    if (load(st, "settings",
             "global port int\n"
             "global name str(16)\n"
             "array allow str(16) 4\n"
             "8080 to port\n"
             "\"edge\" to name\n"
             "assign allow (\"10.0.0.1\", \"10.0.0.2\")\n") != PINAX_OK)
        return 1;
    pinax_global g;
    if (pinax_global_find(st, "port", &g) != PINAX_OK)
        return 1;
    printf("port = %lld\n", (long long)*(const int64_t *)g.ptr);

    /* The same mode refuses code: nothing in such a file can run. */
    load(st, "evil", "word boom () 1 0 div drop end\n");
    load(st, "evil2", "global n int\n1 2 add to n\n");
    pinax_close(st);
    return 0;
}
