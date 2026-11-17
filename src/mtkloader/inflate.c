/*
 * Compact freestanding RFC 1951 raw DEFLATE decompressor.
 * Uses only byte-aligned loads/stores so it runs safely with MMU/caches off.
 */
#include "inflate.h"

#define MAX_BITS    15
#define MAX_LCODES  286
#define MAX_DCODES  30

struct bit_reader {
    const uint8_t *src;
    uint64_t len;
    uint64_t pos;
    uint32_t bitbuf;
    uint32_t bitcnt;
};

struct huff_table {
    uint16_t counts[MAX_BITS + 1];
    uint16_t symbols[288];
};

static int need_bits(struct bit_reader *br, uint32_t n)
{
    while (br->bitcnt < n) {
        if (br->pos >= br->len) {
            return -1;
        }
        br->bitbuf |= (uint32_t)br->src[br->pos++] << br->bitcnt;
        br->bitcnt += 8U;
    }
    return 0;
}

static int read_bits(struct bit_reader *br, uint32_t n, uint32_t *out)
{
    if (n == 0U) {
        *out = 0U;
        return 0;
    }
    if (need_bits(br, n) < 0) {
        return -1;
    }
    *out = br->bitbuf & ((1U << n) - 1U);
    br->bitbuf >>= n;
    br->bitcnt -= n;
    return 0;
}

static int build_huff(struct huff_table *t, const uint8_t *lengths, uint32_t nsyms)
{
    uint16_t offs[MAX_BITS + 1];
    for (uint32_t i = 0; i <= MAX_BITS; i++) {
        t->counts[i] = 0;
    }
    for (uint32_t i = 0; i < nsyms; i++) {
        if (lengths[i] > MAX_BITS) {
            return -2;
        }
        t->counts[lengths[i]]++;
    }
    if (t->counts[0] == nsyms) {
        return 0;
    }
    int left = 1;
    for (uint32_t len = 1; len <= MAX_BITS; len++) {
        left <<= 1;
        left -= (int)t->counts[len];
        if (left < 0) {
            return -2;
        }
    }
    offs[1] = 0;
    for (uint32_t len = 1; len < MAX_BITS; len++) {
        offs[len + 1] = (uint16_t)(offs[len] + t->counts[len]);
    }
    for (uint32_t sym = 0; sym < nsyms; sym++) {
        if (lengths[sym] != 0) {
            t->symbols[offs[lengths[sym]]++] = (uint16_t)sym;
        }
    }
    return 0;
}

static int decode_sym(struct bit_reader *br, const struct huff_table *t, uint32_t *sym)
{
    int code = 0;
    int first = 0;
    int index = 0;
    for (uint32_t len = 1; len <= MAX_BITS; len++) {
        uint32_t bit = 0;
        if (read_bits(br, 1U, &bit) < 0) {
            return -1;
        }
        code |= (int)bit;
        int count = (int)t->counts[len];
        if (code - count < first) {
            *sym = t->symbols[index + (code - first)];
            return 0;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -3;
}

static int decode_block(struct bit_reader *br, const struct huff_table *lit_t,
                        const struct huff_table *dist_t, volatile uint8_t *out,
                        uint64_t cap, uint64_t *written)
{
    static const uint16_t lbase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t lextra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t dbase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };
    static const uint8_t dextra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    for (;;) {
        uint32_t sym = 0;
        if (decode_sym(br, lit_t, &sym) < 0) {
            return -4;
        }
        if (sym < 256U) {
            if (*written >= cap) {
                return -5;
            }
            out[(*written)++] = (uint8_t)sym;
        } else if (sym == 256U) {
            return 0;
        } else {
            uint32_t lidx = sym - 257U;
            if (lidx >= 29U) {
                return -6;
            }
            uint32_t extra = 0;
            if (read_bits(br, lextra[lidx], &extra) < 0) {
                return -4;
            }
            uint32_t length = lbase[lidx] + extra;

            uint32_t dsym = 0;
            if (decode_sym(br, dist_t, &dsym) < 0 || dsym >= MAX_DCODES) {
                return -6;
            }
            uint32_t dex = 0;
            if (read_bits(br, dextra[dsym], &dex) < 0) {
                return -4;
            }
            uint32_t dist = dbase[dsym] + dex;
            if (dist == 0U || (uint64_t)dist > *written || *written + length > cap) {
                return -5;
            }
            for (uint32_t i = 0; i < length; i++) {
                out[*written] = out[*written - dist];
                (*written)++;
            }
        }
    }
}

static int build_fixed_tables(struct huff_table *lit_t, struct huff_table *dist_t)
{
    uint8_t lengths[288];
    uint32_t i = 0;
    for (; i < 144U; i++) lengths[i] = 8;
    for (; i < 256U; i++) lengths[i] = 9;
    for (; i < 280U; i++) lengths[i] = 7;
    for (; i < 288U; i++) lengths[i] = 8;
    if (build_huff(lit_t, lengths, 288U) < 0) {
        return -7;
    }
    for (i = 0; i < MAX_DCODES; i++) lengths[i] = 5;
    return build_huff(dist_t, lengths, MAX_DCODES);
}

static int build_dynamic_tables(struct bit_reader *br, struct huff_table *lit_t, struct huff_table *dist_t)
{
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint32_t hlit = 0, hdist = 0, hclen = 0;
    if (read_bits(br, 5U, &hlit) < 0 || read_bits(br, 5U, &hdist) < 0 || read_bits(br, 4U, &hclen) < 0) {
        return -4;
    }
    hlit += 257U;
    hdist += 1U;
    hclen += 4U;
    if (hlit > MAX_LCODES || hdist > MAX_DCODES) {
        return -6;
    }

    uint8_t clens[19];
    for (uint32_t i = 0; i < 19U; i++) {
        clens[i] = 0;
    }
    for (uint32_t i = 0; i < hclen; i++) {
        uint32_t v = 0;
        if (read_bits(br, 3U, &v) < 0) {
            return -4;
        }
        clens[order[i]] = (uint8_t)v;
    }
    struct huff_table cl_t;
    if (build_huff(&cl_t, clens, 19U) < 0) {
        return -6;
    }

    uint8_t lengths[MAX_LCODES + MAX_DCODES];
    uint32_t total = hlit + hdist;
    uint32_t idx = 0;
    while (idx < total) {
        uint32_t sym = 0;
        if (decode_sym(br, &cl_t, &sym) < 0) {
            return -4;
        }
        if (sym < 16U) {
            lengths[idx++] = (uint8_t)sym;
        } else {
            uint8_t val = 0;
            uint32_t rep = 0;
            uint32_t extra = 0;
            if (sym == 16U) {
                if (idx == 0 || read_bits(br, 2U, &extra) < 0) return -6;
                val = lengths[idx - 1U];
                rep = 3U + extra;
            } else if (sym == 17U) {
                if (read_bits(br, 3U, &extra) < 0) return -4;
                rep = 3U + extra;
            } else if (sym == 18U) {
                if (read_bits(br, 7U, &extra) < 0) return -4;
                rep = 11U + extra;
            } else {
                return -6;
            }
            if (idx + rep > total) {
                return -6;
            }
            for (uint32_t k = 0; k < rep; k++) {
                lengths[idx++] = val;
            }
        }
    }
    if (build_huff(lit_t, lengths, hlit) < 0) {
        return -6;
    }
    return build_huff(dist_t, lengths + hlit, hdist);
}

int inflate_raw(const uint8_t *in, uint64_t in_len, volatile uint8_t *out, uint64_t *out_len)
{
    struct bit_reader br = { in, in_len, 0, 0, 0 };
    uint64_t cap = *out_len;
    uint64_t written = 0;
    struct huff_table lit_t, dist_t;
    uint32_t bfinal = 0;

    do {
        uint32_t btype = 0;
        if (read_bits(&br, 1U, &bfinal) < 0 || read_bits(&br, 2U, &btype) < 0) {
            return -1;
        }
        if (btype == 0U) {
            /* Stored block: discard partial bits in current byte */
            br.bitbuf = 0;
            br.bitcnt = 0;
            if (br.pos + 4ULL > br.len) {
                return -1;
            }
            uint16_t len = (uint16_t)(br.src[br.pos] | ((uint16_t)br.src[br.pos + 1] << 8));
            uint16_t nlen = (uint16_t)(br.src[br.pos + 2] | ((uint16_t)br.src[br.pos + 3] << 8));
            br.pos += 4ULL;
            if ((uint16_t)(len ^ 0xffffU) != nlen || br.pos + len > br.len || written + len > cap) {
                return -5;
            }
            for (uint16_t i = 0; i < len; i++) {
                out[written++] = br.src[br.pos++];
            }
        } else if (btype == 1U) {
            if (build_fixed_tables(&lit_t, &dist_t) < 0) return -7;
            int rc = decode_block(&br, &lit_t, &dist_t, out, cap, &written);
            if (rc < 0) return rc;
        } else if (btype == 2U) {
            if (build_dynamic_tables(&br, &lit_t, &dist_t) < 0) return -6;
            int rc = decode_block(&br, &lit_t, &dist_t, out, cap, &written);
            if (rc < 0) return rc;
        } else {
            return -8;
        }
    } while (!bfinal);

    *out_len = written;
    return 0;
}
