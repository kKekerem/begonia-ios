/*
 * Freestanding RFC 1951 raw DEFLATE decoder for bare-metal Device memory.
 */
#ifndef INFLATE_H
#define INFLATE_H

#include <stdint.h>

/*
 * Decompress raw DEFLATE stream at [in, in + in_len) into [out, out + *out_len).
 * On entry *out_len holds output buffer capacity; on exit it holds bytes written.
 * Returns 0 on success, negative error code on failure.
 */
int inflate_raw(const uint8_t *in, uint64_t in_len, volatile uint8_t *out, uint64_t *out_len);

#endif /* INFLATE_H */
