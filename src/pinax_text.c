// pinax_text.c - scanning and parsing of text, on the vendored proven (D0017).
#include "pinax_internal.h"

#include "proven/encode.h"
#include "proven/fmt.h"
#include "proven/hash.h"
#include "proven/scan.h"
#include "proven/u8str.h"
#include "proven/url.h"

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

const int64_t pnx_pow10[19] = {1,
                               10,
                               100,
                               1000,
                               10000,
                               100000,
                               1000000,
                               10000000,
                               100000000,
                               1000000000,
                               10000000000,
                               100000000000,
                               1000000000000,
                               10000000000000,
                               100000000000000,
                               1000000000000000,
                               10000000000000000,
                               100000000000000000,
                               1000000000000000000};

uint32_t pnx_format_dec(int64_t value, uint32_t scale, uint8_t out[24]) {
    if (scale == 0)
        return pnx_format_int(value, out);
    const uint64_t a = value < 0 ? 0u - (uint64_t)value : (uint64_t)value, p = (uint64_t)pnx_pow10[scale];
    uint64_t whole = a / p, frac = a % p;
    uint8_t  digits[20];
    uint32_t n = 0, k = 0;
    if (value < 0)
        out[n++] = '-';
    do {
        digits[k++] = (uint8_t)('0' + whole % 10);
        whole /= 10;
    } while (whole);
    while (k)
        out[n++] = digits[--k];
    out[n++] = '.';
    for (uint32_t j = scale; j-- > 0;) {
        out[n + j] = (uint8_t)('0' + frac % 10);
        frac /= 10;
    }
    return n + scale;
}

bool pnx_parse_dec(const uint8_t *bytes, uint32_t length, uint32_t scale, int64_t *out) {
    uint32_t i = 0;
    const bool negative = length && bytes[0] == '-';
    if (length && (bytes[0] == '-' || bytes[0] == '+'))
        ++i;
    uint64_t v = 0;
    uint32_t whole = 0, frac = 0;
    for (; i < length && bytes[i] >= '0' && bytes[i] <= '9'; ++i, ++whole) {
        if (ckd_mul(&v, v, 10u) || ckd_add(&v, v, (uint64_t)(bytes[i] - '0')))
            return false;
    }
    if (!whole)
        return false;
    if (i < length && bytes[i] == '.') {
        for (++i; i < length && bytes[i] >= '0' && bytes[i] <= '9'; ++i, ++frac) {
            if (frac == scale || ckd_mul(&v, v, 10u) || ckd_add(&v, v, (uint64_t)(bytes[i] - '0')))
                return false; // more digits than the scale: not rounded quietly
        }
        if (!frac)
            return false;
    }
    if (i != length || ckd_mul(&v, v, (uint64_t)pnx_pow10[scale - frac]))
        return false;
    if (negative ? v > (uint64_t)INT64_MAX + 1u : v > (uint64_t)INT64_MAX)
        return false;
    *out = negative ? (int64_t)(0u - v) : (int64_t)v;
    return true;
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
    X(PINAX_ERR_INTERRUPTED, "interrupted") X(PINAX_ERR_DEADLOCK, "deadlock")                       \
    X(PINAX_ERR_ARGUMENT, "argument")                                                              \
    X(PINAX_ERR_NO_ROOM, "no_room") X(PINAX_ERR_SYNTAX, "syntax") X(PINAX_ERR_TYPE, "type")        \
    X(PINAX_ERR_NOT_FOUND, "not_found") X(PINAX_ERR_SIGNATURE, "signature")                        \
    X(PINAX_ERR_FORMAT, "format") X(PINAX_ERR_STATE, "state")

const char pnx_static_text[] =
#define PNX_ERR_TEXT(code, text) text "\0"
    PNX_ERR_NAMES(PNX_ERR_TEXT)
#undef PNX_ERR_TEXT
    "error";
const uint32_t pnx_static_len = sizeof pnx_static_text;

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

uint64_t pnx_hash(const uint8_t key[16], const uint8_t *bytes, uint32_t length) {
    return proven_hash_keyed((proven_mem_view_t){bytes, length}, key);
}

uint32_t pnx_find_last(const uint8_t *bytes, uint32_t length, const uint8_t *part, uint32_t part_length) {
    if (part_length == 0)
        return length;
    if (part_length > length)
        return UINT32_MAX;
    const proven_size_t at =
        proven_u8str_view_find_last((proven_u8str_view_t){bytes, length}, (proven_u8str_view_t){part, part_length});
    return at == PROVEN_INDEX_NOT_FOUND ? UINT32_MAX : (uint32_t)at;
}

uint64_t pnx_hash_kind(uint32_t kind, const uint8_t *bytes, uint32_t length) {
    static const uint8_t zero[16] = {0};
    const proven_mem_view_t data = {bytes, length};
    return kind == PNX_HASH_CRC32   ? proven_crc32(data)
         : kind == PNX_HASH_FNV1A ? proven_hash_bytes(data)
                                  : proven_hash_keyed(data, zero); // hash64: SipHash-2-4, key 0
}

int pnx_encode(uint32_t kind, const uint8_t *in, uint32_t length, uint8_t *out, uint64_t cap, uint64_t *written) {
    const proven_mem_view_t view = {in, length};
    const proven_mem_mut_t  room = {out, (proven_size_t)cap};
    proven_size_t           n = 0;
    proven_err_t            err;
    switch (kind) {
    case PNX_ENC_BASE64: err = proven_base64_encode(view, out, room.size, &n); break;
    case PNX_ENC_BASE64_DEC: err = proven_base64_decode(view, out, room.size, &n); break;
    case PNX_ENC_HEX: err = proven_hex_encode(view, out, room.size, &n); break;
    case PNX_ENC_HEX_DEC: err = proven_hex_decode(view, out, room.size, &n); break;
    case PNX_ENC_URL: err = proven_url_encode_component(view, room, &n); break;
    default: err = proven_url_percent_decode((proven_u8str_view_t){in, length}, room, &n); break;
    }
    *written = n;
    return err == PROVEN_OK ? 0 : err == PROVEN_ERR_OUT_OF_BOUNDS || err == PROVEN_ERR_OVERFLOW ? 1 : 2;
}
