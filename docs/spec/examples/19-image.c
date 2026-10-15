// embed
#include "pinax.h"
#include <stdio.h>
#include <stdlib.h>

static pinax_err twice(pinax_call *call, void *user, const pinax_args *args) {
    (void)call; (void)user;
    args->ri[0] = args->i[0] * 2;
    return PINAX_OK;
}
static const pinax_word_def words[] = {{"twice", "(n int gives r int)", twice, NULL, 0}};

static int64_t call_next(pinax_state *st) {
    pinax_word w;
    int64_t r = 0;
    if (pinax_find(st, "next", "(gives r int)", 0, &w) != PINAX_OK)
        return -1;
    pinax_err err = pinax_run(st, w, &(pinax_args){.ri = &r});
    return err == PINAX_OK ? r : -(int64_t)err;
}

int main(void) {
    static _Alignas(16) unsigned char a[1 << 20], b[1 << 20], c[1 << 20];
    pinax_config config = {.struct_size = sizeof config};
    pinax_state *st;
    if (pinax_open((pinax_mem){a, sizeof a}, &config, &st) != PINAX_OK)
        return 1;
    pinax_register(st, "host", words, 1);
    static const char source[] =
        "use host\n"
        "global calls int\n"
        "word next (gives r int) calls 1 add set calls  calls host.twice end\n";
    if (pinax_load(st, "main", (pinax_str){(const unsigned char *)source, sizeof source - 1}, PINAX_LOAD_CODE) != PINAX_OK)
        return 1;
    call_next(st);
    printf("in the first state: %lld\n", (long long)call_next(st));

    /* save the whole state: code, globals, everything */
    size_t size = pinax_image_size(st), written = 0;
    unsigned char *image = malloc(size);
    pinax_image_save(st, (pinax_mem){image, size}, &written);

    /* open it in another block; host words are given again by name */
    pinax_state *copy;
    if (pinax_image_open((pinax_mem){b, sizeof b}, (pinax_str){image, written}, &config, &copy) != PINAX_OK)
        return 1;
    printf("before the host word is given again: %s\n", call_next(copy) == -PINAX_ERR_NOT_FOUND ? "PINAX_ERR_NOT_FOUND" : "?");
    pinax_register(copy, "host", words, 1);
    printf("the copy goes on: %lld\n", (long long)call_next(copy));
    printf("the first state is its own: %lld\n", (long long)call_next(st));

    /* a clone is the same in one call */
    pinax_state *twin;
    if (pinax_clone(copy, (pinax_mem){c, sizeof c}, &twin) != PINAX_OK)
        return 1;
    pinax_register(twin, "host", words, 1);
    printf("the clone of the copy: %lld\n", (long long)call_next(twin));
    free(image);
    pinax_close(twin);
    pinax_close(copy);
    pinax_close(st);
    return 0;
}
