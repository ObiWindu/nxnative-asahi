// PCM sink lifecycle: append buffers through the same API the SDK audio
// renderer uses and take them back when played. Runs on SDL3/PipeWire when an
// audio device exists, otherwise on the timed silent sink.
#include "platform/linux/nx_platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { void *next; void *ptr; uint64_t cap, size, offset; } aout_buf;
void *dm_pcmout_open(int ch);
void dm_pcmout_start(void *k);
int dm_pcmout_append(void *k, void *b);
void *dm_pcmout_released(void *k);
void dm_pcmout_wait(void *k, int ms);
void dm_pcmout_close(void *k);

int main(void) {
    void *o = dm_pcmout_open(2);
    if (!o) { printf("FAIL: cannot open PCM sink\n"); return 1; }
    dm_pcmout_start(o);
    enum { N = 4, FRAMES = 240 };
    static int16_t pcm[N][FRAMES * 2];
    aout_buf b[N];
    for (int i = 0; i < N; i++) {
        b[i] = (aout_buf){NULL, pcm[i], sizeof pcm[i], sizeof pcm[i], 0};
        if (!dm_pcmout_append(o, &b[i])) { printf("FAIL: append %d\n", i); return 1; }
    }
    int got = 0;
    for (int i = 0; i < 400 && got < N; i++) {
        void *r = dm_pcmout_released(o);
        if (r) got++;
        else dm_pcmout_wait(o, 5);
    }
    dm_pcmout_close(o);
    printf("PCM sink: %d/%d buffers returned\n", got, N);
    if (got != N) { printf("PCM SINK TEST FAILED\n"); return 1; }
    printf("PCM SINK TEST PASSED\n");
    return 0;
}
