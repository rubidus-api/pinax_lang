// embed
#include "pinax.h"
#include <stdio.h>
#include <string.h>

/* One state holds the data; two others read it in place. Nothing is copied. */
static const char data[] =
    "record city\n  name str(12)\n  people int\nend\n"
    "list cities city 8\n"
    "index by_name cities city.name\n"
    "row cities (\"Seoul\", 9411)\n"
    "row cities (\"Busan\", 3317)\n"
    "row cities (\"Incheon\", 3001)\n";
static const char reader[] =
    "record city\n  name str(12)\n  people int\nend\n"
    "shared list cities city 8\n"
    "shared index by_name cities city.name\n"
    "word people_of (name str gives n int or fail) name by_name lookup or_fail city.people end\n"
    "word total (gives n int)\n"
    "  0 to t  0 to i\n"
    "  loop i cities len lt while  t i cities idx city.people add to t  i 1 add to i end\n"
    "  t\n"
    "end\n";

static pinax_state *open_in(unsigned char *block, size_t bytes) {
    pinax_config config = {.struct_size = sizeof config};
    pinax_state *st;
    return pinax_open((pinax_mem){block, bytes}, &config, &st) == PINAX_OK ? st : NULL;
}

static int64_t call(pinax_state *st, const char *word, const char *signature, const char *name, pinax_err *err) {
    pinax_word w;
    int64_t out = 0;
    pinax_str in = {(const unsigned char *)name, name ? strlen(name) : 0};
    pinax_args args = {.s = &in, .ri = &out};
    *err = pinax_find(st, word, signature, 0, &w);
    if (*err == PINAX_OK)
        *err = pinax_run(st, w, &args);
    return out;
}

int main(void) {
    static unsigned char blocks[3][1024 * 1024];
    pinax_state *source = open_in(blocks[0], sizeof blocks[0]);
    pinax_state *a = open_in(blocks[1], sizeof blocks[1]), *b = open_in(blocks[2], sizeof blocks[2]);
    if (!source || !a || !b || pinax_load(source, "data", (pinax_str){(const unsigned char *)data, strlen(data)},
                                          PINAX_LOAD_DATA) != PINAX_OK)
        return 1;
    pinax_err err;
    pinax_state *readers[2] = {a, b};
    for (int k = 0; k < 2; ++k) {
        if (pinax_load(readers[k], "main", (pinax_str){(const unsigned char *)reader, strlen(reader)},
                       PINAX_LOAD_CODE) != PINAX_OK)
            return 1;
    }
    call(a, "total", "(gives n int)", NULL, &err);
    printf("before sharing: %#x (not found)\n", (unsigned)err);

    for (int k = 0; k < 2; ++k) /* the table first, then its index */
        if (pinax_share(readers[k], "cities", source, "cities") != PINAX_OK ||
            pinax_share(readers[k], "by_name", source, "by_name") != PINAX_OK)
            return 1;
    printf("a: Busan has %lld\n", (long long)call(a, "people_of", "(name str gives n int or fail)", "Busan", &err));
    printf("b: total %lld\n", (long long)call(b, "total", "(gives n int)", NULL, &err));

    pinax_global mine, theirs;
    pinax_global_find(source, "cities", &theirs);
    pinax_global_find(b, "cities", &mine);
    printf("same bytes: %s\n", mine.ptr == theirs.ptr ? "yes" : "no");
    err = pinax_load(source, "more", (pinax_str){(const unsigned char *)"global x int\n", 13}, PINAX_LOAD_CODE);
    printf("the source loads again: %#x (state)\n", (unsigned)err);
    pinax_close(a);
    pinax_close(b);
    pinax_close(source);
    return 0;
}
