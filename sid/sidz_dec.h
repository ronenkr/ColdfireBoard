/* SIDZ stream decoder (see sidz.py for the format). One decoder per register
 * group (voice 1, voice 2, voice 3, filter/volume). Reads straight from flash;
 * repeated passages are "calls" followed with a small stack, so no RAM history
 * is needed. Shared by the board player and the host-side test (sidz_test.c). */
#ifndef SIDZ_DEC_H
#define SIDZ_DEC_H
#include <stdint.h>

#define SIDZ_DEPTH 8

typedef struct {
    const uint8_t *base;
    uint32_t pos[SIDZ_DEPTH];
    uint16_t rem[SIDZ_DEPTH];     /* frames left at each call level (level 0 unused) */
    uint8_t  top;
    uint8_t  empty;               /* pending frames of an empty run */
    uint8_t  done;
} sidz_dec_t;

static const uint8_t sidz_base[4] = { 0, 7, 14, 21 };

static inline void sidz_init(sidz_dec_t *d, const uint8_t *stream)
{
    d->base = stream;
    d->pos[0] = 0;
    d->top = 0;
    d->empty = 0;
    d->done = 0;
}

/* Produce one frame: writes register numbers to reg[] and values to val[],
 * returns how many (0 = no change). Returns -1 at end of stream. */
static inline int sidz_next(sidz_dec_t *d, uint8_t *reg, uint8_t *val)
{
    int n = 0;
    uint8_t lvl;

    if (d->done)
        return -1;
    for (;;) {
        const uint8_t *p;
        uint8_t b;
        if (d->empty) {
            d->empty--;
            break;
        }
        p = d->base + d->pos[d->top];
        b = p[0];
        if (b == 0xFF) {
            d->done = 1;
            return -1;
        }
        if (b == 0x80) {                              /* call */
            if (d->top + 1 >= SIDZ_DEPTH) {           /* corrupt data: stop */
                d->done = 1;
                return -1;
            }
            d->pos[d->top] += 6;
            d->top++;
            d->pos[d->top] = p[1] | p[2] << 8 | (uint32_t)p[3] << 16;
            d->rem[d->top] = p[4] | p[5] << 8;
            continue;
        }
        if (b >= 0x10 && b <= 0x7F) {                 /* run of empty frames */
            d->pos[d->top]++;
            d->empty = b - 0x0E - 1;
            break;
        }
        if (b == 0) {                                 /* one empty frame */
            d->pos[d->top]++;
            break;
        }
        {                                             /* frame with values */
            const uint8_t *q = p + 1;
            uint8_t g;
            uint8_t m[4];
            uint8_t ng = 0;
            for (g = 0; g < 4; g++)
                if (b & (1 << g))
                    m[ng++] = *q++;
            ng = 0;
            for (g = 0; g < 4; g++) {
                if (b & (1 << g)) {
                    uint8_t mm = m[ng++], r;
                    for (r = 0; r < 7; r++)
                        if (mm & (1 << r)) {
                            reg[n] = sidz_base[g] + r;
                            val[n++] = *q++;
                        }
                }
            }
            d->pos[d->top] = (uint32_t)(q - d->base);
        }
        break;
    }
    /* one frame produced: count it at every call level, unwind finished ones */
    for (lvl = 1; lvl <= d->top; lvl++)
        d->rem[lvl]--;
    for (lvl = 1; lvl <= d->top; lvl++)
        if (d->rem[lvl] == 0) {
            d->top = lvl - 1;
            d->empty = 0;
            break;
        }
    return n;
}

#endif
