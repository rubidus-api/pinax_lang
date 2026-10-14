// embed
/* Running code you do not trust. A budget stops a script that never ends. While a run is
   suspended the state takes no other run, so the host keeps an image taken right after loading
   and goes back to it when it gives up. */
#include "pinax.h"
#include <stdio.h>
#include <string.h>

static unsigned char block[256 * 1024], saved[256 * 1024];
static size_t saved_len;

int main(void) {
    pinax_config config = {.struct_size = sizeof config, .budget = 50000};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;
    static const char source[] =
        "word spin (gives int) 0 to n loop n 1 add to n end end\n"
        "word quick (gives int) 6 7 mul end\n";
    if (pinax_load(st, "main", (pinax_str){(const unsigned char *)source, strlen(source)}, PINAX_LOAD_CODE) != PINAX_OK)
        return 1;
    pinax_image_save(st, (pinax_mem){saved, sizeof saved}, &saved_len);   /* the clean state */

    pinax_word w;
    int64_t r = 0;
    pinax_args args = {.ri = &r};
    pinax_find(st, "spin", "(gives int)", 0, &w);
    pinax_err err = pinax_run(st, w, &args);
    int slices = 0;
    while (err == PINAX_SUSPENDED && slices < 5) {     /* the host decides how long to wait */
        ++slices;
        err = pinax_resume(st, &args);
    }
    printf("spin after %d slices: %s\n", slices, err == PINAX_SUSPENDED ? "still running, given up" : "ended");
    pinax_find(st, "quick", "(gives int)", 0, &w);
    printf("another run now: %s\n", pinax_run(st, w, &args) == PINAX_ERR_STATE ? "PINAX_ERR_STATE" : "?");

    pinax_close(st);                                    /* back to the clean state */
    if (pinax_image_open((pinax_mem){block, sizeof block}, (pinax_str){saved, saved_len}, &config, &st) != PINAX_OK)
        return 1;
    pinax_find(st, "quick", "(gives int)", 0, &w);
    err = pinax_run(st, w, &args);
    printf("after restoring the image, quick: %lld\n", (long long)r);
    pinax_close(st);
    return 0;
}
