// embed
/* Running code you do not trust. A budget stops a script that never ends. While a run is
   suspended the state takes no other run; pinax_abort gives the run up, and the state goes on
   with what the script had stored. */
#include "pinax.h"
#include <stdio.h>
#include <string.h>

static unsigned char block[256 * 1024];

int main(void) {
    pinax_config config = {.struct_size = sizeof config, .budget = 50000};
    pinax_state *st;
    if (pinax_open((pinax_mem){block, sizeof block}, &config, &st) != PINAX_OK)
        return 1;
    static const char source[] =
        "global calls int\n"
        "word spin (gives int) calls 1 add to calls  0 to n loop n 1 add to n end end\n"
        "word quick (gives int) calls 1 add to calls  6 7 mul end\n"
        "word count_calls (gives int) calls end\n";
    if (pinax_load(st, "main", (pinax_str){(const unsigned char *)source, strlen(source)}, PINAX_LOAD_CODE) != PINAX_OK)
        return 1;

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
    printf("spin after %d slices: %s\n", slices, err == PINAX_SUSPENDED ? "still running" : "ended");
    pinax_find(st, "quick", "(gives int)", 0, &w);
    printf("another run while it is suspended: %s\n", pinax_run(st, w, &args) == PINAX_ERR_STATE ? "PINAX_ERR_STATE" : "?");

    pinax_abort(st);                                    /* give the spinning run up */
    err = pinax_run(st, w, &args);
    printf("after pinax_abort, quick: %lld\n", (long long)r);
    pinax_find(st, "count_calls", "(gives int)", 0, &w);
    pinax_run(st, w, &args);
    printf("calls stored so far: %lld\n", (long long)r);
    pinax_close(st);
    return 0;
}
