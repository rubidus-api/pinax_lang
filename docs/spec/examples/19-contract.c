// embed
#include "pinax.h"
#include <stdio.h>
#include <string.h>

/* The three promises a host relies on, each checked in a few lines; run it on your own compiler and
   platform. It prints `ok` for each, or the check that failed, and exits with 0 only when all hold:
   1. the memory is the host's: a block that is too small is refused with PINAX_ERR_NO_ROOM (by the
      open or by the load), and pinax_needed names a size that is at least what was being taken;
   2. the run is the host's: the instruction budget bounds a slice, a run resumed to its end gives the
      result of a run with no budget at all, and a script that never ends is stopped by the host;
   3. the functions are the host's: a registered C function is called with its arguments, and its
      failure comes back as the code of the run. */

static int failed;
#define CHECK(what, ok) do { if (ok) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); failed = 1; } } while (0)

static pinax_err tenfold(pinax_call *call, void *user, const pinax_args *args) {
    (void)call; (void)user;
    if (args->i[0] < 0)
        return PINAX_ERR_ARGUMENT;                  /* a failure of the host function: the code of the run */
    args->ri[0] = args->i[0] * 10;
    return PINAX_OK;
}

static int load(pinax_state *st, const char *text) {
    if (pinax_load(st, "main", (pinax_str){(const unsigned char *)text, strlen(text)}, PINAX_LOAD_CODE) == PINAX_OK)
        return 1;
    pinax_str why = pinax_error_text(st);
    printf("     refused: %.*s\n", (int)why.len, (const char *)why.ptr);
    return 0;
}

/* resumes a run for as long as the host allows; returns the last code and the slices it took */
static pinax_err slices(pinax_state *st, pinax_word w, pinax_args *args, int allowed, int *used) {
    pinax_err err = pinax_run(st, w, args);
    *used = 1;
    while (err == PINAX_SUSPENDED && *used < allowed) {
        err = pinax_resume(st, args);
        ++*used;
    }
    return err;
}

int main(void) {
    /* 1. a block too small: the open or the load answers PINAX_ERR_NO_ROOM, and pinax_needed says at least how much */
    static unsigned char block[1 << 20];
    pinax_config config = {.struct_size = sizeof config, .budget = 1000};
    pinax_state *st = NULL;
    pinax_err err;
    size_t small = 4096;
    while (small < sizeof block && (err = pinax_open((pinax_mem){block, small}, &config, &st)) != PINAX_OK)
        small *= 2;                                              /* the smallest block (a power of 2) a state opens in */
    static char big[40000];                                      /* a source of many procedures: more than the block has left */
    size_t n = 0;
    for (int k = 0; k < 400 && n + 80 < sizeof big; ++k)
        n += (size_t)snprintf(big + n, sizeof big - n, "def p%d n int gives r int do n %d add end\n", k, k);
    err = pinax_load(st, "main", (pinax_str){(const unsigned char *)big, n}, PINAX_LOAD_CODE);
    CHECK("a load that does not fit is refused with PINAX_ERR_NO_ROOM", err == PINAX_ERR_NO_ROOM);
    const size_t needed = pinax_needed(st);
    CHECK("pinax_needed names a block size above the one that failed", needed > small);
    pinax_close(st);
    st = NULL;
    size_t again = needed;                                       /* pinax_needed is at least enough, and a load may take more: read it again */
    while (again <= sizeof block) {
        if (pinax_open((pinax_mem){block, again}, &config, &st) == PINAX_OK &&
            pinax_load(st, "main", (pinax_str){(const unsigned char *)big, n}, PINAX_LOAD_CODE) == PINAX_OK)
            break;
        if (st)
            pinax_close(st);
        st = NULL;
        again = again * 3 / 2;
    }
    CHECK("opened again at that size (or a little more), the same source loads", st != NULL);
    if (st)
        pinax_close(st);
    st = NULL;
    CHECK("a block of 1 MB opens", pinax_open((pinax_mem){block, sizeof block}, &config, &st) == PINAX_OK);
    if (!st)
        return 1;

    /* 3. a registered C function */
    static const pinax_word_def words[] = {{"tenfold", "(n int gives r int or fail)", tenfold, NULL, 0}};
    CHECK("a C function is registered", pinax_register(st, "calc", words, 1) == PINAX_OK);
    CHECK("a script that uses it loads",
          load(st, "use calc .\n"
                   "def answer n int gives r int or fail do n calc.tenfold or_fail 2 add end\n"
                   "def tally n int gives r int do 0 to s  1 n 1 add range do to i s i add to s end each  s end\n"
                   "def spin do loop end end\n"));
    pinax_word answer, count, spin;
    pinax_find(st, "answer", "(n int gives r int or fail)", 0, &answer);
    int64_t in = 4, out = 0;
    pinax_args args = {.i = &in, .ri = &out};
    int used;
    err = slices(st, answer, &args, 100, &used);
    CHECK("the C function is called with its argument: answer(4) = 42", err == PINAX_OK && out == 42);
    in = -1;
    err = slices(st, answer, &args, 100, &used);
    CHECK("its failure is the code of the run (PINAX_ERR_ARGUMENT)", err == PINAX_ERR_ARGUMENT);

    /* 2. the budget bounds a slice, not the run */
    pinax_find(st, "tally", "(n int gives r int)", 0, &count);
    in = 100000;
    err = slices(st, count, &args, 100000, &used);
    const int64_t sliced = out;
    CHECK("a run of a hundred thousand turns is suspended at the budget (many slices)", err == PINAX_OK && used > 10);
    pinax_state *second;
    static unsigned char block2[1 << 20];
    pinax_config none = {.struct_size = sizeof none};                 /* budget 0: no limit */
    CHECK("a second state opens with no budget", pinax_open((pinax_mem){block2, sizeof block2}, &none, &second) == PINAX_OK);
    CHECK("the same script loads there", load(second, "def tally n int gives r int do 0 to s  1 n 1 add range do to i s i add to s end each  s end\n"));
    pinax_word count2;
    pinax_find(second, "tally", "(n int gives r int)", 0, &count2);
    int64_t out2 = 0;
    pinax_args args2 = {.i = &in, .ri = &out2};
    err = slices(second, count2, &args2, 1, &used);
    CHECK("the result of a resumed run equals the result of a run with no budget", err == PINAX_OK && used == 1 && out2 == sliced);
    pinax_close(second);

    pinax_find(st, "spin", "()", 0, &spin);
    pinax_args none_args = {0};
    err = slices(st, spin, &none_args, 50, &used);
    CHECK("a script that never ends is stopped by the host after 50 slices", err == PINAX_SUSPENDED && used == 50);

    pinax_close(st);
    puts(failed ? "contract: FAILED" : "contract: ok");
    return failed;
}
