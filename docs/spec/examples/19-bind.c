// embed
#include "pinax.h"
#include <stdio.h>

int main(void) {
    static unsigned char block[1 << 20];
    pinax_config config = {.struct_size = sizeof config};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;
    static const char source[] =
        "bound array samples int 5\n"
        "bound global scale int\n"
        "word scaled_sum (gives n int) samples sum scale mul end\n"
        "word clear_first () 0 0 to global.samples end\n";
    if (pinax_load(st, "main", (pinax_str){(const unsigned char *)source, sizeof source - 1}, PINAX_LOAD_CODE) != PINAX_OK)
        return 1;
    static int64_t samples[5] = {1, 2, 3, 4, 5};    /* host memory the script reads and writes in place */
    static int64_t scale = 10;
    pinax_bind(st, "samples", samples, sizeof samples);
    pinax_bind(st, "scale", &scale, sizeof scale);
    pinax_word w;
    int64_t n = 0;
    pinax_find(st, "scaled_sum", "(gives n int)", 0, &w);
    pinax_run(st, w, &(pinax_args){.ri = &n});
    printf("scaled_sum = %lld\n", (long long)n);
    samples[4] = 105;                                /* the host changes it; no copy to refresh */
    pinax_run(st, w, &(pinax_args){.ri = &n});
    printf("after the host wrote samples[4]: %lld\n", (long long)n);
    pinax_find(st, "clear_first", "()", 0, &w);
    pinax_run(st, w, NULL);
    printf("after the script wrote cell 0: samples[0] = %lld\n", (long long)samples[0]);
    pinax_close(st);
    return 0;
}
