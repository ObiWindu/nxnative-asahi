// nn::audio::AudioOut HLE service on Linux: open the default output, append
// four PCM blocks, start playback, and wait for the sink to return all four
// through the release queue (the event/poll path games use).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct { const char *name; void *fn; const char *why; const char *to; } hle_force_ent;
extern const hle_force_ent __start_swforce[], __stop_swforce[];
typedef struct { void *next; void *ptr; uint64_t cap, size, offset; } aout_buf;

static void *lookup(const char *name) {
    for (const hle_force_ent *e = __start_swforce; e < __stop_swforce; e++)
        if (!strcmp(e->name, name)) return e->fn;
    return NULL;
}
static long ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void) {
    void (*init)(void *) = lookup("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE");
    int (*open)(void *, void *, const void *) = lookup("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE");
    int (*start)(void *) = lookup("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE");
    void (*stop)(void *) = lookup("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE");
    void (*close)(void *) = lookup("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE");
    int (*state)(const void *) = lookup("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE");
    int (*rate)(const void *) = lookup("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE");
    int (*chans)(const void *) = lookup("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE");
    unsigned (*append)(void *, aout_buf *) = lookup("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE");
    aout_buf *(*released)(void *) = lookup("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE");
    if (!init || !open || !start || !stop || !close || !state || !rate || !chans || !append || !released) {
        printf("FAIL: AudioOut service symbols are missing\n"); return 1;
    }
    uint8_t param[8] = {0};
    init(param);
    *(int32_t *)param = 48000; *(uint16_t *)(param + 4) = 2;
    uint8_t out[0x100] = {0};
    int r = open(out, NULL, param);
    if (r != 0) { printf("FAIL: OpenDefaultAudioOut returned %d\n", r); return 1; }
    if (state(out) != 1) { printf("FAIL: state after open is %d, want 1 (stopped)\n", state(out)); return 1; }
    if (rate(out) != 48000 || chans(out) != 2) { printf("FAIL: format %d Hz %d ch\n", rate(out), chans(out)); return 1; }
    if (start(out) != 0) { printf("FAIL: StartAudioOut\n"); return 1; }
    if (state(out) != 0) { printf("FAIL: state after start is %d, want 0 (started)\n", state(out)); return 1; }
    enum { N = 4, FRAMES = 480 };
    static int16_t pcm[N][FRAMES * 2];
    aout_buf b[N];
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < FRAMES * 2; j++) pcm[i][j] = (int16_t)((i + 1) * 1000);
        b[i] = (aout_buf){NULL, pcm[i], sizeof pcm[i], sizeof pcm[i], 0};
        if (!append(out, &b[i])) { printf("FAIL: append %d\n", i); return 1; }
    }
    int got = 0;
    long t0 = ms();
    while (got < N && ms() - t0 < 3000) {
        aout_buf *x = released(out);
        if (x) got++;
        else { struct timespec ts = {0, 2000000}; nanosleep(&ts, NULL); }
    }
    stop(out);
    close(out);
    printf("AudioOut: %d/%d buffers released in %ld ms\n", got, N, ms() - t0);
    if (got != N) { printf("AUDIO OUT TEST FAILED\n"); return 1; }
    printf("AUDIO OUT TEST PASSED\n");
    return 0;
}
