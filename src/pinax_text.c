// pinax_text.c - scanning and parsing of text, on the vendored proven (D0017).
#include "pinax_internal.h"

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
