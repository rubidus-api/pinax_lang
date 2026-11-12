// pinax_sqlite.c - the host module `sqlite` (RFC-0043): SQLite as words, written against pinax.h.
//
// A separate member of libpinax_host.a with its own register function (pinax_host_register_sqlite),
// so that a host that never calls it does not link SQLite. A connection and a statement are handles;
// a connection object counts its statements, so the order in which they are closed does not matter.
#include "pinax_host.h"

#include "proven/heap.h"
#include "sqlite3.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// ---- objects --------------------------------------------------------------------------------

typedef struct {
    sqlite3 *db;
    uint32_t stmts; // statements prepared on it and not yet finalized
    bool     closed; // sqlite.close was called; the database closes with the last statement
} sq_conn;

typedef struct {
    sqlite3_stmt *st;
    sq_conn      *conn;
    bool          row; // the last step gave a row
} sq_stmt;

static void *take(size_t bytes, size_t align) {
    proven_allocator_t                 heap = proven_heap_allocator();
    const proven_result_mem_mut_t room = heap.alloc_fn(heap.ctx, bytes, align);
    return room.err == PROVEN_OK ? room.value.ptr : NULL;
}
static void give(void *p) {
    proven_allocator_t heap = proven_heap_allocator();
    heap.free_fn(heap.ctx, p);
}

// The handle types of this state travel in the `user` pointer of the words: 16 bits each.
static uint32_t conn_type(void *user) { return (uint32_t)((uintptr_t)user & 0xffffu); }
static uint32_t stmt_type(void *user) { return (uint32_t)(((uintptr_t)user >> 16) & 0xffffu); }

static void conn_release(sq_conn *c) {
    if (c->closed && c->stmts == 0) {
        (void)sqlite3_close_v2(c->db); // rolls back a transaction left open
        give(c);
    }
}
static void conn_close_object(void *object, void *user) {
    (void)user;
    sq_conn *c = object;
    c->closed = true;
    conn_release(c);
}
static void stmt_close_object(void *object, void *user) {
    (void)user;
    sq_stmt *s = object;
    (void)sqlite3_finalize(s->st);
    sq_conn *c = s->conn;
    give(s);
    --c->stmts;
    conn_release(c);
}

static sq_conn *conn_of(pinax_call *call, void *user, int64_t h) {
    return pinax_handle_get(pinax_call_state(call), conn_type(user), (pinax_handle)h);
}
static sq_stmt *stmt_of(pinax_call *call, void *user, int64_t h) {
    return pinax_handle_get(pinax_call_state(call), stmt_type(user), (pinax_handle)h);
}

// ---- the last failure, and its mapping to err.* ----------------------------------------------

static _Thread_local int    last_code;
static _Thread_local char   last_text[256];
static _Thread_local size_t last_len;

static void note(sqlite3 *db, int rc) {
    last_code = db ? sqlite3_extended_errcode(db) : rc;
    const char *text = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    last_len = 0;
    while (text && text[last_len] && last_len < sizeof last_text - 1) {
        last_text[last_len] = text[last_len];
        ++last_len;
    }
}

// `parsing`: the failure came from preparing a statement, where a plain SQLITE_ERROR is text that
// is not SQL of this database (err.format); from running one it is err.failed.
static pinax_err fail_from(sqlite3 *db, int rc, bool parsing) {
    if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE)
        return PINAX_OK;
    note(db, rc);
    switch (rc & 0xff) {
    case SQLITE_NOMEM:
    case SQLITE_FULL:
    case SQLITE_TOOBIG:
        return PINAX_ERR_FULL;
    case SQLITE_CANTOPEN:
        return PINAX_ERR_NOT_FOUND;
    case SQLITE_RANGE:
        return PINAX_ERR_RANGE;
    case SQLITE_MISMATCH:
    case SQLITE_NOTADB:
    case SQLITE_CORRUPT:
        return PINAX_ERR_FORMAT;
    case SQLITE_MISUSE:
        return PINAX_ERR_STATE;
    case SQLITE_ERROR:
        return parsing ? PINAX_ERR_FORMAT : PINAX_ERR_FAILED;
    default:
        return PINAX_ERR_FAILED;
    }
}

static pinax_err w_last_code(pinax_call *call, void *user, const pinax_args *args) {
    (void)call; (void)user;
    args->ri[0] = last_code;
    return PINAX_OK;
}
static pinax_err w_last_error(pinax_call *call, void *user, const pinax_args *args) {
    (void)user;
    const pinax_mem room = last_len ? pinax_call_alloc(call, last_len) : (pinax_mem){NULL, 0};
    if (last_len && !room.ptr)
        return PINAX_ERR_FULL;
    if (last_len)
        memcpy(room.ptr, last_text, last_len);
    args->rs[0] = (pinax_str){room.ptr, last_len};
    return PINAX_OK;
}

// ---- process-wide limits (RFC-0043 section 5.1) ------------------------------------------------

static _Atomic int64_t limit_soft = INT64_C(64) << 20;
static _Atomic int64_t limit_hard = INT64_C(256) << 20;

static void apply_limits(void) {
    (void)sqlite3_soft_heap_limit64(limit_soft);
    (void)sqlite3_hard_heap_limit64(limit_hard);
}

void pinax_host_sqlite_limits(int64_t soft_bytes, int64_t hard_bytes) {
    if (soft_bytes >= 0)
        limit_soft = soft_bytes;
    if (hard_bytes >= 0)
        limit_hard = hard_bytes;
    if (sqlite3_initialize() == SQLITE_OK)
        apply_limits();
}

// ---- connection ----------------------------------------------------------------------------------

static pinax_err open_db(pinax_call *call, void *user, pinax_str path, bool read_only, int64_t *out) {
    char *name = take(path.len + 1, 1);
    if (!name)
        return PINAX_ERR_FULL;
    if (path.len)
        memcpy(name, path.ptr, path.len);
    name[path.len] = '\0';
    sqlite3  *db = NULL;
    const int flags = (read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) | SQLITE_OPEN_NOMUTEX;
    const int rc = sqlite3_open_v2(name, &db, flags, NULL); // no SQLITE_OPEN_URI: a path is a path
    give(name);
    if (rc != SQLITE_OK) {
        const pinax_err err = fail_from(db, rc, false);
        if (db)
            (void)sqlite3_close_v2(db);
        return err;
    }
    // a script's database is not trusted to be well made, and loads no code
    (void)sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, (int *)NULL);
    (void)sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, (int *)NULL);
    (void)sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, 16 << 20);
    sq_conn *c = take(sizeof *c, _Alignof(sq_conn));
    if (!c) {
        (void)sqlite3_close_v2(db);
        return PINAX_ERR_FULL;
    }
    *c = (sq_conn){db, 0, false};
    pinax_handle   handle;
    const pinax_err err = pinax_handle_new(pinax_call_state(call), conn_type(user), c, &handle);
    if (err != PINAX_OK) {
        conn_close_object(c, NULL);
        return err;
    }
    *out = (int64_t)handle;
    return PINAX_OK;
}
static pinax_err w_open(pinax_call *call, void *user, const pinax_args *args) {
    return open_db(call, user, args->s[0], false, &args->ri[0]);
}
static pinax_err w_open_read(pinax_call *call, void *user, const pinax_args *args) {
    return open_db(call, user, args->s[0], true, &args->ri[0]);
}
static pinax_err w_close(pinax_call *call, void *user, const pinax_args *args) {
    sq_conn *c = conn_of(call, user, args->i[0]);
    if (!c)
        return PINAX_ERR_STALE; // a fault: the word has no `or fail`
    const pinax_err err = pinax_handle_release(pinax_call_state(call), conn_type(user), (pinax_handle)args->i[0]);
    conn_close_object(c, NULL);
    return err;
}

// One statement of `sql`; what follows it may only be space and comments (else err.format).
static pinax_err prepare_one(sqlite3 *db, pinax_str sql, sqlite3_stmt **out) {
    *out = NULL;
    if (sql.len > INT_MAX)
        return PINAX_ERR_FULL;
    const char *tail = NULL;
    int         rc = sqlite3_prepare_v2(db, (const char *)sql.ptr, (int)sql.len, out, &tail);
    if (rc != SQLITE_OK)
        return fail_from(db, rc, true);
    if (!*out)
        return PINAX_ERR_FORMAT; // nothing but space and comments
    const char *end = (const char *)sql.ptr + sql.len;
    if (tail && tail < end) {
        sqlite3_stmt *more = NULL;
        rc = sqlite3_prepare_v2(db, tail, (int)(end - tail), &more, NULL);
        if (more)
            (void)sqlite3_finalize(more);
        if (rc != SQLITE_OK || more) {
            (void)sqlite3_finalize(*out);
            *out = NULL;
            return rc != SQLITE_OK ? fail_from(db, rc, true) : PINAX_ERR_FORMAT;
        }
    }
    return PINAX_OK;
}

static pinax_err w_exec(pinax_call *call, void *user, const pinax_args *args) {
    sq_conn *c = conn_of(call, user, args->i[0]);
    if (!c)
        return PINAX_ERR_STALE;
    const pinax_str sql = args->s[0];
    if (sql.len > INT_MAX)
        return PINAX_ERR_FULL;
    const char *p = (const char *)sql.ptr, *end = p + sql.len;
    while (p < end) {
        sqlite3_stmt *st = NULL;
        const char   *tail = NULL;
        int           rc = sqlite3_prepare_v2(c->db, p, (int)(end - p), &st, &tail);
        if (rc != SQLITE_OK)
            return fail_from(c->db, rc, true);
        if (st) {
            do
                rc = sqlite3_step(st);
            while (rc == SQLITE_ROW);
            if (rc != SQLITE_DONE) {
                rc = sqlite3_finalize(st); // the error of the step
                return fail_from(c->db, rc ? rc : SQLITE_ERROR, false);
            }
            (void)sqlite3_finalize(st);
        }
        if (!tail || tail <= p)
            break;
        p = tail;
    }
    return PINAX_OK;
}

// The first column of the first row of `sql`; the statement is finalized before the word returns,
// so a text is copied into the caller's region here.
static pinax_err scalar(pinax_call *call, void *user, const pinax_args *args, bool as_text) {
    sq_conn *c = conn_of(call, user, args->i[0]);
    if (!c)
        return PINAX_ERR_STALE;
    sqlite3_stmt   *st;
    pinax_err       err = prepare_one(c->db, args->s[0], &st);
    if (err != PINAX_OK)
        return err;
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE)
        err = PINAX_ERR_NOT_FOUND;
    else if (rc != SQLITE_ROW)
        err = fail_from(c->db, rc, false);
    else if (sqlite3_column_count(st) < 1)
        err = PINAX_ERR_FORMAT;
    else if (!as_text) {
        if (sqlite3_column_type(st, 0) != SQLITE_INTEGER)
            err = PINAX_ERR_FORMAT;
        else
            args->ri[0] = sqlite3_column_int64(st, 0);
    } else {
        const int type = sqlite3_column_type(st, 0);
        if (type == SQLITE_NULL || type == SQLITE_BLOB)
            err = PINAX_ERR_FORMAT;
        else {
            const unsigned char *text = sqlite3_column_text(st, 0);
            const int            n = sqlite3_column_bytes(st, 0);
            const pinax_mem      room = n > 0 ? pinax_call_alloc(call, (size_t)n) : (pinax_mem){NULL, 0};
            if (!text && n)
                err = PINAX_ERR_FULL;
            else if (n > 0 && !room.ptr)
                err = PINAX_ERR_FULL;
            else {
                if (n > 0)
                    memcpy(room.ptr, text, (size_t)n);
                args->rs[0] = (pinax_str){room.ptr, (size_t)(n > 0 ? n : 0)};
            }
        }
    }
    (void)sqlite3_finalize(st);
    return err;
}
static pinax_err w_scalar_int(pinax_call *call, void *user, const pinax_args *args) {
    return scalar(call, user, args, false);
}
static pinax_err w_scalar_str(pinax_call *call, void *user, const pinax_args *args) {
    return scalar(call, user, args, true);
}

static pinax_err w_changes(pinax_call *call, void *user, const pinax_args *args) {
    sq_conn *c = conn_of(call, user, args->i[0]);
    if (!c)
        return PINAX_ERR_STALE;
    args->ri[0] = sqlite3_changes64(c->db);
    return PINAX_OK;
}
static pinax_err w_last_id(pinax_call *call, void *user, const pinax_args *args) {
    sq_conn *c = conn_of(call, user, args->i[0]);
    if (!c)
        return PINAX_ERR_STALE;
    args->ri[0] = sqlite3_last_insert_rowid(c->db);
    return PINAX_OK;
}
static pinax_err w_in_transaction(pinax_call *call, void *user, const pinax_args *args) {
    sq_conn *c = conn_of(call, user, args->i[0]);
    if (!c)
        return PINAX_ERR_STALE;
    args->ri[0] = sqlite3_get_autocommit(c->db) ? 0 : 1;
    return PINAX_OK;
}
static pinax_err w_busy_ms(pinax_call *call, void *user, const pinax_args *args) {
    sq_conn *c = conn_of(call, user, args->i[1]);
    if (!c)
        return PINAX_ERR_STALE;
    if (args->i[0] < 0 || args->i[0] > INT_MAX)
        return PINAX_ERR_RANGE;
    return fail_from(c->db, sqlite3_busy_timeout(c->db, (int)args->i[0]), false);
}

// ---- statement ------------------------------------------------------------------------------------

static pinax_err w_prepare(pinax_call *call, void *user, const pinax_args *args) {
    sq_conn *c = conn_of(call, user, args->i[0]);
    if (!c)
        return PINAX_ERR_STALE;
    sqlite3_stmt *st;
    pinax_err     err = prepare_one(c->db, args->s[0], &st);
    if (err != PINAX_OK)
        return err;
    sq_stmt *s = take(sizeof *s, _Alignof(sq_stmt));
    if (!s) {
        (void)sqlite3_finalize(st);
        return PINAX_ERR_FULL;
    }
    *s = (sq_stmt){st, c, false};
    ++c->stmts;
    pinax_handle handle;
    err = pinax_handle_new(pinax_call_state(call), stmt_type(user), s, &handle);
    if (err != PINAX_OK) {
        stmt_close_object(s, NULL);
        return err;
    }
    args->ri[0] = (int64_t)handle;
    return PINAX_OK;
}
static pinax_err w_finalize(pinax_call *call, void *user, const pinax_args *args) {
    sq_stmt *s = stmt_of(call, user, args->i[0]);
    if (!s)
        return PINAX_ERR_STALE;
    const pinax_err err = pinax_handle_release(pinax_call_state(call), stmt_type(user), (pinax_handle)args->i[0]);
    stmt_close_object(s, NULL);
    return err;
}
static pinax_err w_reset(pinax_call *call, void *user, const pinax_args *args) {
    sq_stmt *s = stmt_of(call, user, args->i[0]);
    if (!s)
        return PINAX_ERR_STALE;
    (void)sqlite3_reset(s->st); // the error of the last step was told by the step
    s->row = false;
    return PINAX_OK;
}
static pinax_err w_clear_binds(pinax_call *call, void *user, const pinax_args *args) {
    sq_stmt *s = stmt_of(call, user, args->i[0]);
    if (!s)
        return PINAX_ERR_STALE;
    (void)sqlite3_clear_bindings(s->st);
    return PINAX_OK;
}

// Positions are 0-based in Pinax (as for a sheet or a list); SQLite counts from 1.
static pinax_err position(int64_t i, int *out) {
    if (i < 0 || i >= INT_MAX)
        return PINAX_ERR_RANGE;
    *out = (int)(i + 1);
    return PINAX_OK;
}
static pinax_err bind_done(const sq_stmt *s, int rc) {
    return fail_from(sqlite3_db_handle(s->st), rc, false);
}
static pinax_err w_bind_null(pinax_call *call, void *user, const pinax_args *args) {
    sq_stmt *s = stmt_of(call, user, args->i[1]);
    int      at;
    if (!s)
        return PINAX_ERR_STALE;
    if (position(args->i[0], &at) != PINAX_OK)
        return PINAX_ERR_RANGE;
    return bind_done(s, sqlite3_bind_null(s->st, at));
}
static pinax_err w_bind_int(pinax_call *call, void *user, const pinax_args *args) {
    sq_stmt *s = stmt_of(call, user, args->i[2]);
    int      at;
    if (!s)
        return PINAX_ERR_STALE;
    if (position(args->i[1], &at) != PINAX_OK)
        return PINAX_ERR_RANGE;
    return bind_done(s, sqlite3_bind_int64(s->st, at, args->i[0]));
}
static pinax_err bind_bytes(pinax_call *call, void *user, const pinax_args *args, bool text) {
    sq_stmt *s = stmt_of(call, user, args->i[1]);
    int      at;
    if (!s)
        return PINAX_ERR_STALE;
    if (position(args->i[0], &at) != PINAX_OK)
        return PINAX_ERR_RANGE;
    if (args->s[0].len > INT_MAX)
        return PINAX_ERR_FULL;
    const void *bytes = args->s[0].len ? (const void *)args->s[0].ptr : "";
    const int   n = (int)args->s[0].len;
    // SQLITE_TRANSIENT: the string is a view that lives only for this call
    return bind_done(s, text ? sqlite3_bind_text(s->st, at, bytes, n, SQLITE_TRANSIENT)
                             : sqlite3_bind_blob(s->st, at, bytes, n, SQLITE_TRANSIENT));
}
static pinax_err w_bind_str(pinax_call *call, void *user, const pinax_args *args) {
    return bind_bytes(call, user, args, true);
}
static pinax_err w_bind_blob(pinax_call *call, void *user, const pinax_args *args) {
    return bind_bytes(call, user, args, false);
}
static pinax_err w_bind_name(pinax_call *call, void *user, const pinax_args *args) {
    sq_stmt *s = stmt_of(call, user, args->i[0]);
    if (!s)
        return PINAX_ERR_STALE;
    char name[130];
    if (args->s[0].len == 0 || args->s[0].len >= sizeof name)
        return PINAX_ERR_NOT_FOUND;
    memcpy(name, args->s[0].ptr, args->s[0].len);
    name[args->s[0].len] = '\0';
    const int at = sqlite3_bind_parameter_index(s->st, name); // the name as written: ":id", "@id", "$id"
    if (at == 0)
        return PINAX_ERR_NOT_FOUND;
    args->ri[0] = at - 1;
    return PINAX_OK;
}

static pinax_err w_step(pinax_call *call, void *user, const pinax_args *args) {
    sq_stmt *s = stmt_of(call, user, args->i[0]);
    if (!s)
        return PINAX_ERR_STALE;
    const int rc = sqlite3_step(s->st);
    s->row = rc == SQLITE_ROW;
    args->ri[0] = s->row ? 1 : 0;
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? PINAX_OK : fail_from(s->conn->db, rc, false);
}

static pinax_err w_col_count(pinax_call *call, void *user, const pinax_args *args) {
    sq_stmt *s = stmt_of(call, user, args->i[0]);
    if (!s)
        return PINAX_ERR_STALE;
    args->ri[0] = sqlite3_column_count(s->st);
    return PINAX_OK;
}
static bool in_range(const sq_stmt *s, int64_t i) {
    return i >= 0 && i < sqlite3_column_count(s->st);
}
static pinax_err w_col_name(pinax_call *call, void *user, const pinax_args *args) {
    sq_stmt *s = stmt_of(call, user, args->i[1]);
    if (!s)
        return PINAX_ERR_STALE;
    if (!in_range(s, args->i[0]))
        return PINAX_ERR_RANGE;
    const char *name = sqlite3_column_name(s->st, (int)args->i[0]);
    if (!name)
        return PINAX_ERR_FULL;
    const size_t n = strlen(name);
    const pinax_mem room = n ? pinax_call_alloc(call, n) : (pinax_mem){NULL, 0};
    if (n && !room.ptr)
        return PINAX_ERR_FULL;
    if (n)
        memcpy(room.ptr, name, n);
    args->rs[0] = (pinax_str){room.ptr, n};
    return PINAX_OK;
}
// col_kind and col_is_null have no `or fail`: a position outside the columns is a fault (range).
// With no current row a column is NULL.
static pinax_err w_col_kind(pinax_call *call, void *user, const pinax_args *args) {
    sq_stmt *s = stmt_of(call, user, args->i[1]);
    if (!s)
        return PINAX_ERR_STALE;
    if (!in_range(s, args->i[0]))
        return PINAX_ERR_RANGE;
    args->ri[0] = s->row ? sqlite3_column_type(s->st, (int)args->i[0]) : SQLITE_NULL;
    return PINAX_OK;
}
static pinax_err w_col_is_null(pinax_call *call, void *user, const pinax_args *args) {
    sq_stmt *s = stmt_of(call, user, args->i[1]);
    if (!s)
        return PINAX_ERR_STALE;
    if (!in_range(s, args->i[0]))
        return PINAX_ERR_RANGE;
    args->ri[0] = !s->row || sqlite3_column_type(s->st, (int)args->i[0]) == SQLITE_NULL ? 1 : 0;
    return PINAX_OK;
}
// The strict readers: a column of another kind is err.format, no row is err.not_found.
static pinax_err reader_check(const sq_stmt *s, int64_t i) {
    if (!in_range(s, i))
        return PINAX_ERR_RANGE;
    return s->row ? PINAX_OK : PINAX_ERR_NOT_FOUND;
}
static pinax_err w_col_int(pinax_call *call, void *user, const pinax_args *args) {
    sq_stmt *s = stmt_of(call, user, args->i[1]);
    if (!s)
        return PINAX_ERR_STALE;
    pinax_err err = reader_check(s, args->i[0]);
    if (err != PINAX_OK)
        return err;
    if (sqlite3_column_type(s->st, (int)args->i[0]) != SQLITE_INTEGER)
        return PINAX_ERR_FORMAT;
    args->ri[0] = sqlite3_column_int64(s->st, (int)args->i[0]);
    return PINAX_OK;
}

// The bytes of a column as a text (text, int, real) or as they are (blob).
static pinax_err column_bytes(const sq_stmt *s, int64_t i, bool blob, const unsigned char **ptr, size_t *len) {
    const pinax_err err = reader_check(s, i);
    if (err != PINAX_OK)
        return err;
    const int type = sqlite3_column_type(s->st, (int)i);
    if (blob ? type != SQLITE_BLOB : (type == SQLITE_NULL || type == SQLITE_BLOB))
        return PINAX_ERR_FORMAT;
    const void *data = blob ? sqlite3_column_blob(s->st, (int)i) : (const void *)sqlite3_column_text(s->st, (int)i);
    const int   n = sqlite3_column_bytes(s->st, (int)i);
    if (!data && n)
        return PINAX_ERR_FULL; // SQLite ran out of memory making the text
    *ptr = data;
    *len = n > 0 ? (size_t)n : 0;
    return PINAX_OK;
}
static pinax_err col_value(pinax_call *call, void *user, const pinax_args *args, bool blob) {
    sq_stmt *s = stmt_of(call, user, args->i[1]);
    if (!s)
        return PINAX_ERR_STALE;
    const unsigned char *ptr;
    size_t               len;
    const pinax_err      err = column_bytes(s, args->i[0], blob, &ptr, &len);
    if (err != PINAX_OK)
        return err;
    args->rs[0] = (pinax_str){ptr, len}; // valid until the next step; the caller copies it
    return PINAX_OK;
}
static pinax_err col_value_into(pinax_call *call, void *user, const pinax_args *args, bool blob) {
    sq_stmt *s = stmt_of(call, user, args->i[1]);
    if (!s)
        return PINAX_ERR_STALE;
    const unsigned char *ptr;
    size_t               len;
    const pinax_err      err = column_bytes(s, args->i[0], blob, &ptr, &len);
    if (err != PINAX_OK)
        return err;
    const pinax_mem room = pinax_call_buffer(call, args->i[2], 0);
    if (len > room.len)
        return PINAX_ERR_FULL;
    if (len)
        memcpy(room.ptr, ptr, len);
    (void)pinax_call_buffer(call, args->i[2], len);
    return PINAX_OK;
}
static pinax_err w_col_str(pinax_call *call, void *user, const pinax_args *args) { return col_value(call, user, args, false); }
static pinax_err w_col_blob(pinax_call *call, void *user, const pinax_args *args) { return col_value(call, user, args, true); }
static pinax_err w_col_str_into(pinax_call *call, void *user, const pinax_args *args) { return col_value_into(call, user, args, false); }
static pinax_err w_col_blob_into(pinax_call *call, void *user, const pinax_args *args) { return col_value_into(call, user, args, true); }

// The kinds of a column, as SQLite numbers them: the pure words k_null ... k_blob.
static pinax_err w_kind_const(pinax_call *call, void *user, const pinax_args *args) {
    (void)call;
    args->ri[0] = (int64_t)(uintptr_t)user;
    return PINAX_OK;
}

// ---- registration --------------------------------------------------------------------------------

pinax_err pinax_host_register_sqlite(pinax_state *state) {
    if (!state)
        return PINAX_ERR_ARGUMENT;
    if (sqlite3_initialize() != SQLITE_OK)
        return PINAX_ERR_FAILED;
    apply_limits();
    uint32_t  conn, stmt;
    pinax_err err = pinax_handle_type(state, "sqlite", conn_close_object, NULL, &conn);
    if (err == PINAX_OK)
        err = pinax_handle_type(state, "stmt", stmt_close_object, NULL, &stmt);
    if (err != PINAX_OK)
        return err;
    if (conn > 0xffffu || stmt > 0xffffu)
        return PINAX_ERR_FULL;
    void *tag = (void *)(uintptr_t)(conn | (stmt << 16));
    const pinax_word_def words[] = {
        {"open", "(path str gives owned sqlite or fail)", w_open, tag, 0},
        {"open_read", "(path str gives owned sqlite or fail)", w_open_read, tag, 0},
        {"close", "(db owned sqlite)", w_close, tag, 0},
        {"exec", "(sql str, db sqlite or fail)", w_exec, tag, 0},
        {"scalar_int", "(sql str, db sqlite gives n int or fail)", w_scalar_int, tag, 0},
        {"scalar_str", "(sql str, db sqlite gives s str or fail)", w_scalar_str, tag, 0},
        {"changes", "(db sqlite gives n int)", w_changes, tag, 0},
        {"last_id", "(db sqlite gives n int)", w_last_id, tag, 0},
        {"in_transaction", "(db sqlite gives bool)", w_in_transaction, tag, 0},
        {"busy_ms", "(ms int, db sqlite or fail)", w_busy_ms, tag, 0},
        {"prepare", "(sql str, db sqlite gives owned stmt or fail)", w_prepare, tag, 0},
        {"finalize", "(s owned stmt)", w_finalize, tag, 0},
        {"reset", "(s stmt or fail)", w_reset, tag, 0},
        {"clear_binds", "(s stmt)", w_clear_binds, tag, 0},
        {"bind_null", "(i int, s stmt or fail)", w_bind_null, tag, 0},
        {"bind_int", "(v int, i int, s stmt or fail)", w_bind_int, tag, 0},
        {"bind_str", "(v str, i int, s stmt or fail)", w_bind_str, tag, 0},
        {"bind_blob", "(v str, i int, s stmt or fail)", w_bind_blob, tag, 0},
        {"bind_name", "(name str, s stmt gives i int or fail)", w_bind_name, tag, 0},
        {"step", "(s stmt gives more bool or fail)", w_step, tag, 0},
        {"col_count", "(s stmt gives n int)", w_col_count, tag, 0},
        {"col_name", "(i int, s stmt gives name str or fail)", w_col_name, tag, 0},
        {"col_kind", "(i int, s stmt gives k int)", w_col_kind, tag, 0},
        {"col_is_null", "(i int, s stmt gives bool)", w_col_is_null, tag, 0},
        {"col_int", "(i int, s stmt gives n int or fail)", w_col_int, tag, 0},
        {"col_str", "(i int, s stmt gives v str or fail)", w_col_str, tag, 0},
        {"col_str_into", "(i int, s stmt, buf buffer or fail)", w_col_str_into, tag, 0},
        {"col_blob", "(i int, s stmt gives v str or fail)", w_col_blob, tag, 0},
        {"col_blob_into", "(i int, s stmt, buf buffer or fail)", w_col_blob_into, tag, 0},
        {"last_code", "(gives n int)", w_last_code, NULL, PINAX_WORD_READS},
        {"last_error", "(gives msg str)", w_last_error, NULL, PINAX_WORD_READS},
        {"k_null", "(gives k int)", w_kind_const, (void *)(uintptr_t)SQLITE_NULL, PINAX_WORD_PURE},
        {"k_int", "(gives k int)", w_kind_const, (void *)(uintptr_t)SQLITE_INTEGER, PINAX_WORD_PURE},
        {"k_real", "(gives k int)", w_kind_const, (void *)(uintptr_t)SQLITE_FLOAT, PINAX_WORD_PURE},
        {"k_text", "(gives k int)", w_kind_const, (void *)(uintptr_t)SQLITE_TEXT, PINAX_WORD_PURE},
        {"k_blob", "(gives k int)", w_kind_const, (void *)(uintptr_t)SQLITE_BLOB, PINAX_WORD_PURE},
    };
    return pinax_register(state, "sqlite", words, sizeof words / sizeof words[0]);
}
