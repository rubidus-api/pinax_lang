/*
 * pinax_proven.h - conversions between the Pinax host interface and proven_c_lib types.
 *
 * pinax.h exposes only standard C types, so that any C program can embed Pinax. A host that
 * is itself written on proven_c_lib includes this header as well and passes its values
 * across without copying. Nothing here allocates; the views keep pointing at the same bytes.
 *
 * Needs proven_c_lib on the include path (C23).
 */
#ifndef PINAX_PROVEN_H
#define PINAX_PROVEN_H

#include "pinax.h"

#include "proven/memory.h"
#include "proven/types.h"
#include "proven/u8str.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline pinax_mem pinax_mem_from_proven(proven_mem_mut_t memory) {
    pinax_mem out = {memory.ptr, memory.size};
    return out;
}

static inline proven_mem_mut_t pinax_mem_to_proven(pinax_mem memory) {
    proven_mem_mut_t out = {(proven_byte_t *)memory.ptr, memory.len};
    return out;
}

static inline pinax_str pinax_str_from_proven(proven_u8str_view_t text) {
    pinax_str out = {text.ptr, text.size};
    return out;
}

static inline proven_u8str_view_t pinax_str_to_proven(pinax_str text) {
    proven_u8str_view_t out = {text.ptr, text.len};
    return out;
}

/*
 * What a host word written on proven returns to the script. Codes with a close Pinax
 * meaning keep it; every other proven failure becomes PINAX_ERR_FAILED.
 */
static inline pinax_err pinax_err_from_proven(proven_err_t err) {
    switch (err) {
    case PROVEN_OK:                   return PINAX_OK;
    case PROVEN_ERR_NOMEM:            return PINAX_ERR_FULL;
    case PROVEN_ERR_OUT_OF_BOUNDS:    return PINAX_ERR_RANGE;
    case PROVEN_ERR_OVERFLOW:         return PINAX_ERR_OVERFLOW;
    case PROVEN_ERR_INVALID_ARG:      return PINAX_ERR_ARGUMENT;
    case PROVEN_ERR_NOT_FOUND:        return PINAX_ERR_NOT_FOUND;
    case PROVEN_ERR_INVALID_STATE:    return PINAX_ERR_STATE;
    case PROVEN_ERR_INVALID_ENCODING: return PINAX_ERR_FORMAT;
    case PROVEN_ERR_INVALID_FORMAT:   return PINAX_ERR_FORMAT;
    default:                          return PINAX_ERR_FAILED;
    }
}

/*
 * What a proven-based host reports after a Pinax call. Outcomes that are not failures
 * (PINAX_SUSPENDED, PINAX_WAITING, PINAX_PENDING) become PROVEN_ERR_AGAIN: the call has to
 * be continued, so test for them before converting.
 */
static inline proven_err_t pinax_err_to_proven(pinax_err err) {
    switch (err) {
    case PINAX_OK:            return PROVEN_OK;
    case PINAX_SUSPENDED:
    case PINAX_WAITING:
    case PINAX_PENDING:       return PROVEN_ERR_AGAIN;
    case PINAX_ERR_OVERFLOW:  return PROVEN_ERR_OVERFLOW;
    case PINAX_ERR_RANGE:     return PROVEN_ERR_OUT_OF_BOUNDS;
    case PINAX_ERR_FULL:
    case PINAX_ERR_DEPTH:
    case PINAX_ERR_NO_ROOM:   return PROVEN_ERR_NOMEM;
    case PINAX_ERR_DIV_ZERO:
    case PINAX_ERR_ARGUMENT:  return PROVEN_ERR_INVALID_ARG;
    case PINAX_ERR_NOT_FOUND: return PROVEN_ERR_NOT_FOUND;
    case PINAX_ERR_SYNTAX:
    case PINAX_ERR_TYPE:
    case PINAX_ERR_SIGNATURE:
    case PINAX_ERR_FORMAT:    return PROVEN_ERR_INVALID_FORMAT;
    default:                  return PROVEN_ERR_INVALID_STATE;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* PINAX_PROVEN_H */
