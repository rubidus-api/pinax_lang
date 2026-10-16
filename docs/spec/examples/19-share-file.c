// embed host
#include "pinax.h"
#include "pinax_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One program writes the data as a share file; any number of processes map it and read it in
   place. Here the same program maps it twice, as two processes would. */
static const char data[] =
    "record city\n  name str(12)\n  people int\nend\n"
    "list cities city 8\n"
    "index by_name cities city.name\n"
    "row cities (\"Seoul\", 9411)\n"
    "row cities (\"Busan\", 3317)\n";
static const char reader[] =
    "record city\n  name str(12)\n  people int\nend\n"
    "shared list cities city 8\n"
    "shared index by_name cities city.name\n"
    "word people_of (name str gives n int or fail) name by_name lookup or_fail city.people end\n";

static unsigned char blocks[3][1024 * 1024];

static int write_file(const char *path) {
    pinax_config config = {.struct_size = sizeof config};
    pinax_state *source;
    if (pinax_open((pinax_mem){blocks[0], sizeof blocks[0]}, &config, &source) != PINAX_OK ||
        pinax_load(source, "data", (pinax_str){(const unsigned char *)data, strlen(data)}, PINAX_LOAD_DATA) != PINAX_OK)
        return 1;
    size_t size = pinax_share_size(source), written = 0;
    unsigned char *bytes = malloc(size);
    if (!bytes || pinax_share_save(source, (pinax_mem){bytes, size}, &written) != PINAX_OK)
        return 1;
    FILE *f = fopen(path, "wb");
    int bad = !f || fwrite(bytes, 1, written, f) != written;
    bad |= f && fclose(f) != 0;
    printf("wrote %zu bytes\n", written);
    free(bytes);
    pinax_close(source); /* the file needs nothing of it */
    return bad;
}

static void read_file(const char *path, unsigned char *block, size_t bytes, const char *name) {
    pinax_config config = {.struct_size = sizeof config};
    pinax_state *st;
    pinax_str file;
    pinax_mapping *map;
    pinax_word w;
    int64_t out = 0;
    pinax_str in = {(const unsigned char *)name, strlen(name)};
    pinax_args args = {.s = &in, .ri = &out};
    if (pinax_map_file(path, &file, &map) != PINAX_OK)
        return;
    if (pinax_open((pinax_mem){block, bytes}, &config, &st) == PINAX_OK &&
        pinax_load(st, "main", (pinax_str){(const unsigned char *)reader, strlen(reader)}, PINAX_LOAD_CODE) == PINAX_OK &&
        pinax_share_file(st, "cities", file, "cities") == PINAX_OK &&      /* the table first */
        pinax_share_file(st, "by_name", file, "by_name") == PINAX_OK &&    /* then its index */
        pinax_find(st, "people_of", "(name str gives n int or fail)", 0, &w) == PINAX_OK &&
        pinax_run(st, w, &args) == PINAX_OK)
        printf("%s has %lld\n", name, (long long)out);
    pinax_close(st);
    pinax_unmap_file(map); /* after the state that read it */
}

int main(void) {
    const char *path = "cities.pnxs";
    if (write_file(path) != 0)
        return 1;
    read_file(path, blocks[1], sizeof blocks[1], "Seoul");
    read_file(path, blocks[2], sizeof blocks[2], "Busan");
    remove(path);
    return 0;
}
