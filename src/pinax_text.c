// pinax_text.c - scanning and parsing of text, on the vendored proven (D0017).
#include "pinax_internal.h"

#include "proven/fmt.h"
#include "proven/scan.h"
#include "proven/u8str.h"

bool pnx_parse_int(const uint8_t *bytes, uint32_t length, int64_t *out) {
    // proven_scan_i64 skips leading white space; here the whole view must be the number.
    if (length == 0 || !(bytes[0] == '-' || bytes[0] == '+' || (bytes[0] >= '0' && bytes[0] <= '9')))
        return false;
    proven_scan_t scan = proven_scan_init((proven_u8str_view_t){bytes, length});
    const proven_result_i64_t r = proven_scan_i64(&scan);
    if (r.err != PROVEN_OK || scan.cursor != length)
        return false;
    *out = r.val;
    return true;
}

uint32_t pnx_find_byte(const uint8_t *bytes, uint32_t length, uint8_t byte) {
    if (length == 0)
        return UINT32_MAX;
    const proven_size_t at =
        proven_u8str_view_find((proven_u8str_view_t){bytes, length}, 0, (proven_u8str_view_t){&byte, 1});
    return at == PROVEN_INDEX_NOT_FOUND ? UINT32_MAX : (uint32_t)at;
}

uint32_t pnx_format_int(int64_t value, uint8_t out[24]) {
    proven_u8str_t text = proven_u8str_borrow(out, 24);
    const proven_fmt_result_t r = proven_u8str_append_fmt(&text, "{}", proven_arg_i64(value));
    return r.err == PROVEN_OK ? (uint32_t)r.written : 0;
}

uint32_t pnx_find(const uint8_t *bytes, uint32_t length, const uint8_t *part, uint32_t part_length) {
    if (part_length == 0)
        return 0;
    if (part_length > length)
        return UINT32_MAX;
    const proven_size_t at =
        proven_u8str_view_find((proven_u8str_view_t){bytes, length}, 0, (proven_u8str_view_t){part, part_length});
    return at == PROVEN_INDEX_NOT_FOUND ? UINT32_MAX : (uint32_t)at;
}

// Error names, as err_text gives them. The order follows pinax_err.
#define PNX_ERR_NAMES(X)                                                                           \
    X(PINAX_ERR_FAILED, "failed") X(PINAX_ERR_OVERFLOW, "overflow") X(PINAX_ERR_DIV_ZERO, "div_zero")  \
    X(PINAX_ERR_RANGE, "range") X(PINAX_ERR_FULL, "full") X(PINAX_ERR_DEPTH, "depth")              \
    X(PINAX_ERR_STALE, "stale") X(PINAX_ERR_CANCELLED, "cancelled")                                \
    X(PINAX_ERR_INTERRUPTED, "interrupted") X(PINAX_ERR_ARGUMENT, "argument")                      \
    X(PINAX_ERR_NO_ROOM, "no_room") X(PINAX_ERR_SYNTAX, "syntax") X(PINAX_ERR_TYPE, "type")        \
    X(PINAX_ERR_NOT_FOUND, "not_found") X(PINAX_ERR_SIGNATURE, "signature")                        \
    X(PINAX_ERR_FORMAT, "format") X(PINAX_ERR_STATE, "state")

const char pnx_static_text[] =
#define PNX_ERR_TEXT(code, text) text "\0"
    PNX_ERR_NAMES(PNX_ERR_TEXT)
#undef PNX_ERR_TEXT
    "error";

void pnx_err_name(int64_t err, uint32_t *offset, uint32_t *length) {
    uint32_t at = 0;
#define PNX_ERR_AT(code, text)                                                                     \
    if (err == (code)) {                                                                           \
        *offset = at;                                                                              \
        *length = sizeof(text) - 1;                                                                \
        return;                                                                                    \
    }                                                                                              \
    at += sizeof(text);
    PNX_ERR_NAMES(PNX_ERR_AT)
#undef PNX_ERR_AT
    *offset = at; // any other number
    *length = 5;
}
