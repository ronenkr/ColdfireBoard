/* Host test: decode the 4 SIDZ streams of a file and print one line per frame
 * ("reg=val ..."), to compare with the Python reference decoder. */
#include <stdio.h>
#include <stdlib.h>
#include "sidz_dec.h"

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    static uint8_t buf[1 << 20];
    size_t len = fread(buf, 1, sizeof buf, f);
    uint32_t off[4];
    sidz_dec_t d[4];
    int g, frames = atoi(argv[2]);
    (void)len;
    for (g = 0; g < 4; g++) {
        off[g] = buf[4 * g] | buf[4 * g + 1] << 8 | buf[4 * g + 2] << 16 | (uint32_t)buf[4 * g + 3] << 24;
        sidz_init(&d[g], buf + off[g]);
    }
    for (int i = 0; i < frames; i++) {
        for (g = 0; g < 4; g++) {
            uint8_t reg[8], val[8];
            int n = sidz_next(&d[g], reg, val);
            for (int k = 0; k < n; k++)
                printf("%d=%d ", reg[k], val[k]);
        }
        printf("\n");
    }
    return 0;
}
