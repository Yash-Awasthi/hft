/* Independent ITCH 5.0 message counter in plain C, used to cross-check itch_count.
 * Shares no code with the C++ decoder and keeps its own length table.
 * Build: gcc -O2 -o count_itch research/count_itch.c
 * Usage: zcat day.gz | ./count_itch
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned msg_len(int type) {
    switch (type) {
        case 'S': case 'W': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': case 'N': return 20;
        case 'L': return 26;
        case 'V': case 'J': case 'U': return 35;
        case 'K': return 28;
        case 'h': return 21;
        case 'A': case 'C': return 36;
        case 'F': case 'Q': return 40;
        case 'E': return 31;
        case 'X': return 23;
        case 'D': case 'B': return 19;
        case 'P': return 44;
        case 'I': return 50;
        case 'O': return 48;
        default: return 0;
    }
}

#define CHUNK (1u << 22)
#define MAXFRAME (2u + 65535u)

int main(void) {
    unsigned long long counts[256] = {0}, frames = 0, bad = 0;
    unsigned char *buf = malloc(CHUNK + MAXFRAME);
    size_t have = 0;
    if (!buf) return 1;

    for (;;) {
        size_t got = fread(buf + have, 1, CHUNK, stdin);
        have += got;
        size_t off = 0;
        while (have - off >= 2) {
            size_t len = ((size_t)buf[off] << 8) | buf[off + 1];
            if (have - off < 2 + len) break;
            if (len == 0) {
                bad++;
            } else {
                int t = buf[off + 2];
                counts[t]++;
                if (msg_len(t) != len) bad++;
            }
            frames++;
            off += 2 + len;
        }
        memmove(buf, buf + off, have - off);
        have -= off;
        if (got == 0) break;
    }

    for (int t = 0; t < 256; t++) {
        if (counts[t]) printf("%c %llu\n", t, counts[t]);
    }
    printf("frames %llu\nbad %llu\ntrailing_bytes %zu\n", frames, bad, have);
    free(buf);
    return 0;
}
