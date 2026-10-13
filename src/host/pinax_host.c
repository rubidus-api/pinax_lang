// pinax_host.c - the standard host modules (see pinax_host.h). Written against pinax.h; the
// operating system is reached through the vendored proven.
#include "pinax_host.h"

#include "pinax_proven.h"

#include "proven/fs.h"
#include "proven/heap.h"
#include "proven/random.h"
#include "proven/sysio.h"
#include "proven/time.h"

#include <stdint.h>

static proven_u8str_view_t view_of(pinax_str s) {
    return (proven_u8str_view_t){s.ptr, s.len};
}

static pinax_err from(proven_err_t err) {
    return err == PROVEN_OK ? PINAX_OK : pinax_err_from_proven(err);
}

// ---- io -----------------------------------------------------------------------------------

static pinax_err write_to(proven_file_t file, pinax_str text, bool newline) {
    // a host word without `or fail` must not fail: a closed output is ignored, as printf does
    (void)proven_fs_write_all(file, (proven_mem_view_t){text.ptr, text.len});
    if (newline)
        (void)proven_fs_write_all(file, (proven_mem_view_t){(const proven_byte_t *)"\n", 1});
    return PINAX_OK;
}
static pinax_err io_print(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    return write_to(proven_sysio_stdout(), args->s[0], false);
}
static pinax_err io_println(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    return write_to(proven_sysio_stdout(), args->s[0], true);
}
static pinax_err io_eprint(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    return write_to(proven_sysio_stderr(), args->s[0], false);
}

// Reads one line (without its line break) into the caller's region. Fails with not_found at
// the end of the input and with full when the line does not fit.
static pinax_err line_from(pinax_call *call, proven_file_t file, pinax_str *out) {
    enum { MOST = 4096 };
    pinax_mem room = pinax_call_alloc(call, MOST);
    if (!room.ptr)
        return PINAX_ERR_FULL;
    unsigned char *line = room.ptr;
    size_t n = 0;
    for (;;) {
        proven_byte_t byte;
        const proven_result_size_t got = proven_fs_read(file, (proven_mem_mut_t){&byte, 1});
        if (got.err != PROVEN_OK && got.err != PROVEN_ERR_EOF)
            return from(got.err);
        if (got.err == PROVEN_ERR_EOF || got.value == 0) { // the end: a last line may lack its line break
            if (n == 0)
                return PINAX_ERR_NOT_FOUND;
            break;
        }
        if (byte == '\n')
            break;
        if (n == MOST)
            return PINAX_ERR_FULL;
        line[n++] = byte;
    }
    if (n && line[n - 1] == '\r')
        --n;
    out->ptr = line;
    out->len = n;
    return PINAX_OK;
}
static pinax_err io_read_line(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    return line_from(call, proven_sysio_stdin(), &args->rs[0]);
}

// ---- file ---------------------------------------------------------------------------------

// The handle type number of "file" in this state travels in the `user` pointer of the words.
static uint32_t file_type(void *user) {
    return (uint32_t)(uintptr_t)user;
}

static void file_close_object(void *object, void *user) {
    (void)user;
    proven_file_t *file = object;
    (void)proven_fs_close(*file);
    proven_allocator_t heap = proven_heap_allocator();
    heap.free_fn(heap.ctx, file);
}

static pinax_err file_open(pinax_call *call, void *user, const pinax_args *args, proven_fs_mode_t mode) {
    proven_allocator_t heap = proven_heap_allocator();
    const proven_result_file_t opened = proven_fs_open(heap, view_of(args->s[0]), mode);
    if (opened.err != PROVEN_OK)
        return from(opened.err);
    const proven_result_mem_mut_t room = heap.alloc_fn(heap.ctx, sizeof(proven_file_t), _Alignof(proven_file_t));
    if (room.err != PROVEN_OK) {
        (void)proven_fs_close(opened.value);
        return PINAX_ERR_FULL;
    }
    proven_file_t *file = (proven_file_t *)(void *)room.value.ptr;
    *file = opened.value;
    pinax_handle handle;
    const pinax_err err = pinax_handle_new(pinax_call_state(call), file_type(user), file, &handle);
    if (err != PINAX_OK) {
        file_close_object(file, NULL);
        return err;
    }
    args->ri[0] = (int64_t)handle;
    return PINAX_OK;
}
static pinax_err file_open_read(pinax_call *call, void *user, const pinax_args *args) {
    return file_open(call, user, args, PROVEN_FS_READ);
}
static pinax_err file_open_write(pinax_call *call, void *user, const pinax_args *args) {
    return file_open(call, user, args, (proven_fs_mode_t)(PROVEN_FS_WRITE | PROVEN_FS_CREATE | PROVEN_FS_TRUNC));
}
static pinax_err file_write(pinax_call *call, void *user, const pinax_args *args) {
    proven_file_t *file = pinax_handle_get(pinax_call_state(call), file_type(user), (pinax_handle)args->i[0]);
    if (!file)
        return PINAX_ERR_STALE;
    return from(proven_fs_write_all(*file, (proven_mem_view_t){args->s[0].ptr, args->s[0].len}));
}
static pinax_err file_read_line(pinax_call *call, void *user, const pinax_args *args) {
    proven_file_t *file = pinax_handle_get(pinax_call_state(call), file_type(user), (pinax_handle)args->i[0]);
    if (!file)
        return PINAX_ERR_STALE;
    return line_from(call, *file, &args->rs[0]);
}
static pinax_err file_close(pinax_call *call, void *user, const pinax_args *args) {
    pinax_state *state = pinax_call_state(call);
    proven_file_t *file = pinax_handle_get(state, file_type(user), (pinax_handle)args->i[0]);
    if (!file)
        return PINAX_ERR_STALE; // a fault: the word has no `or fail`
    const pinax_err err = pinax_handle_release(state, file_type(user), (pinax_handle)args->i[0]);
    file_close_object(file, NULL);
    return err;
}
// The whole file as one string in the caller's region.
static pinax_err file_read_all(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    proven_allocator_t heap = proven_heap_allocator();
    const proven_result_file_t opened = proven_fs_open(heap, view_of(args->s[0]), PROVEN_FS_READ);
    if (opened.err != PROVEN_OK)
        return from(opened.err);
    pinax_err err = PINAX_OK;
    const proven_result_size_t size = proven_fs_size(opened.value);
    pinax_mem room = {NULL, 0};
    if (size.err != PROVEN_OK)
        err = from(size.err);
    else if (size.value && !(room = pinax_call_alloc(call, size.value)).ptr)
        err = PINAX_ERR_FULL;
    size_t have = 0;
    while (err == PINAX_OK && have < room.len) {
        const proven_result_size_t got =
            proven_fs_read(opened.value, (proven_mem_mut_t){(proven_byte_t *)room.ptr + have, room.len - have});
        if (got.err == PROVEN_ERR_EOF || (got.err == PROVEN_OK && got.value == 0))
            break;
        else if (got.err != PROVEN_OK)
            err = from(got.err);
        else
            have += got.value;
    }
    (void)proven_fs_close(opened.value);
    if (err != PINAX_OK)
        return err;
    args->rs[0].ptr = room.ptr;
    args->rs[0].len = have;
    return PINAX_OK;
}

// ---- clock --------------------------------------------------------------------------------

static pinax_err clock_now_ms(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    args->ri[0] = proven_time_now() / 1000000;
    return PINAX_OK;
}
static pinax_err clock_monotonic_ns(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    args->ri[0] = proven_time_monotonic_now();
    return PINAX_OK;
}
static pinax_err clock_sleep(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    if (args->i[0] < 0 || args->i[0] > UINT32_MAX)
        return PINAX_ERR_RANGE;
    proven_time_sleep((proven_u32)args->i[0]);
    return PINAX_OK;
}

// ---- random -------------------------------------------------------------------------------

static proven_xoshiro256ss_t generator;
static bool                  generator_seeded;

static proven_rng_t rng(void) {
    if (!generator_seeded) {
        proven_xoshiro256ss_seed(&generator, proven_random_u64());
        generator_seeded = true;
    }
    return proven_xoshiro256ss_rng(&generator);
}
static pinax_err random_next(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    args->ri[0] = (int64_t)proven_rng_u64(rng());
    return PINAX_OK;
}
static pinax_err random_below(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    if (args->i[0] <= 0)
        return PINAX_ERR_RANGE;
    args->ri[0] = (int64_t)proven_rng_below(rng(), (proven_u64)args->i[0]);
    return PINAX_OK;
}
static pinax_err random_seed(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    proven_xoshiro256ss_seed(&generator, (proven_u64)args->i[0]);
    generator_seeded = true;
    return PINAX_OK;
}

// ---- env ----------------------------------------------------------------------------------

static int                host_arg_count;
static const char *const *host_args;

void pinax_host_set_args(int count, const char *const *args) {
    host_arg_count = count;
    host_args = args;
}

static pinax_err env_arg_count(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    args->ri[0] = host_arg_count;
    return PINAX_OK;
}
static pinax_err env_arg(pinax_call *call, void *user, const pinax_args *args) {
    (void)call, (void)user;
    if (args->i[0] < 0 || args->i[0] >= host_arg_count)
        return PINAX_ERR_RANGE;
    const char *text = host_args[args->i[0]];
    size_t n = 0;
    while (text[n])
        ++n;
    args->rs[0].ptr = (const unsigned char *)text; // KEEP_STR: the arguments outlive the state
    args->rs[0].len = n;
    return PINAX_OK;
}
static pinax_err env_getenv(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    proven_allocator_t heap = proven_heap_allocator();
    proven_result_u8str_t got = proven_env_get(heap, view_of(args->s[0]));
    if (got.err != PROVEN_OK)
        return from(got.err);
    const proven_u8str_view_t value = proven_u8str_as_view(&got.value);
    pinax_mem room = {NULL, 0};
    pinax_err err = PINAX_OK;
    if (value.size && !(room = pinax_call_alloc(call, value.size)).ptr)
        err = PINAX_ERR_FULL;
    for (size_t k = 0; err == PINAX_OK && k < value.size; ++k)
        ((unsigned char *)room.ptr)[k] = value.ptr[k];
    proven_u8str_destroy(heap, &got.value);
    if (err != PINAX_OK)
        return err;
    args->rs[0].ptr = room.ptr;
    args->rs[0].len = value.size;
    return PINAX_OK;
}

// ---- registration -------------------------------------------------------------------------

static bool same(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

pinax_err pinax_host_register(pinax_state *state, const char *module) {
    if (!state || !module)
        return PINAX_ERR_ARGUMENT;
    if (same(module, "io")) {
        const pinax_word_def words[] = {
            {"print", "(s str)", io_print, NULL, 0},
            {"println", "(s str)", io_println, NULL, 0},
            {"eprint", "(s str)", io_eprint, NULL, 0},
            {"read_line", "(gives line str or fail)", io_read_line, NULL, 0},
        };
        return pinax_register(state, "io", words, sizeof words / sizeof words[0]);
    }
    if (same(module, "file")) {
        uint32_t type;
        const pinax_err err = pinax_handle_type(state, "file", file_close_object, NULL, &type);
        if (err != PINAX_OK)
            return err;
        void *tag = (void *)(uintptr_t)type;
        const pinax_word_def words[] = {
            {"open_read", "(path str gives owned file or fail)", file_open_read, tag, 0},
            {"open_write", "(path str gives owned file or fail)", file_open_write, tag, 0},
            {"write", "(s str, f file or fail)", file_write, tag, 0},
            {"read_line", "(f file gives line str or fail)", file_read_line, tag, 0},
            {"read_all", "(path str gives body str or fail)", file_read_all, tag, 0},
            {"close", "(f owned file)", file_close, tag, 0},
        };
        return pinax_register(state, "file", words, sizeof words / sizeof words[0]);
    }
    if (same(module, "clock")) {
        const pinax_word_def words[] = {
            {"now_ms", "(gives int)", clock_now_ms, NULL, 0},
            {"monotonic_ns", "(gives int)", clock_monotonic_ns, NULL, 0},
            {"sleep", "(ms int)", clock_sleep, NULL, 0},
        };
        return pinax_register(state, "clock", words, sizeof words / sizeof words[0]);
    }
    if (same(module, "random")) {
        const pinax_word_def words[] = {
            {"random", "(gives int)", random_next, NULL, 0},
            {"random_below", "(n int gives int)", random_below, NULL, 0},
            {"seed", "(n int)", random_seed, NULL, 0},
        };
        return pinax_register(state, "random", words, sizeof words / sizeof words[0]);
    }
    if (same(module, "env")) {
        const pinax_word_def words[] = {
            {"arg_count", "(gives int)", env_arg_count, NULL, 0},
            {"arg", "(i int gives s str or fail)", env_arg, NULL, PINAX_WORD_KEEP_STR},
            {"getenv", "(name str gives s str or fail)", env_getenv, NULL, 0},
        };
        return pinax_register(state, "env", words, sizeof words / sizeof words[0]);
    }
    return PINAX_ERR_NOT_FOUND;
}

pinax_err pinax_host_register_all(pinax_state *state) {
    static const char *const names[] = {"io", "file", "clock", "random", "env"};
    for (unsigned k = 0; k < sizeof names / sizeof names[0]; ++k) {
        const pinax_err err = pinax_host_register(state, names[k]);
        if (err != PINAX_OK)
            return err;
    }
    return PINAX_OK;
}
