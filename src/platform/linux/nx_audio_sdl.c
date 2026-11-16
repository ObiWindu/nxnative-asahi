// Linux PCM output for the SDK audio renderer (hle_audren.c). SDL3 is loaded at
// run time and uses PipeWire (or ALSA) underneath. If no device can be opened,
// a timed silent sink consumes the same buffers at the same rate, so the game's
// audio clock and buffer lifecycle keep working.
#include "platform/linux/nx_platform.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
// nn::audio::AudioOutBuffer (hle.h aout_buf): next, data, capacity, size, offset
typedef struct { void *next; void *ptr; uint64_t cap, size, offset; } aout_buf;

typedef struct SDL_AudioStream SDL_AudioStream;
typedef uint32_t SDL_AudioDeviceID;
typedef struct { int format; int channels; int freq; } SDL_AudioSpec;

struct pcmout {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    SDL_AudioStream *stream;
    int ch, rate, silent;
    aout_buf *q[64];       // pending
    uint64_t qend[64];     // cumulative byte end of each pending buffer
    int nq;
    aout_buf *rel[64];     // played, waiting for the renderer
    int nrel;
    uint64_t appended, played;
    pthread_t th;
    int run, started;
};

static void *g_sdl;
static int (*p_SDL_InitSubSystem)(uint32_t);
static SDL_AudioStream *(*p_SDL_OpenAudioDeviceStream)(SDL_AudioDeviceID, const SDL_AudioSpec *, void *, void *);
static int (*p_SDL_ResumeAudioStreamDevice)(SDL_AudioStream *);
static int (*p_SDL_PauseAudioStreamDevice)(SDL_AudioStream *);
static int (*p_SDL_PutAudioStreamData)(SDL_AudioStream *, const void *, int);
static int (*p_SDL_GetAudioStreamQueued)(SDL_AudioStream *);
static int (*p_SDL_ClearAudioStream)(SDL_AudioStream *);
static void (*p_SDL_DestroyAudioStream)(SDL_AudioStream *);

static int sdl_audio_load(void) {
    if (g_sdl) return 1;
    g_sdl = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!g_sdl) g_sdl = dlopen("libSDL3.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_sdl) return 0;
#define A(n) *(void **)(&p_##n) = dlsym(g_sdl, #n)
    A(SDL_InitSubSystem); A(SDL_OpenAudioDeviceStream); A(SDL_ResumeAudioStreamDevice);
    A(SDL_PauseAudioStreamDevice); A(SDL_PutAudioStreamData); A(SDL_GetAudioStreamQueued);
    A(SDL_DestroyAudioStream); A(SDL_ClearAudioStream);
#undef A
    return p_SDL_InitSubSystem && p_SDL_OpenAudioDeviceStream && p_SDL_PutAudioStreamData && p_SDL_GetAudioStreamQueued;
}

static void *silent_thread(void *arg) {
    struct pcmout *o = arg;
    while (__atomic_load_n(&o->run, __ATOMIC_ACQUIRE)) {
        aout_buf *b = NULL;
        pthread_mutex_lock(&o->mu);
        if (o->nq) { b = o->q[0]; memmove(o->q, o->q + 1, (size_t)(--o->nq) * sizeof o->q[0]); }
        pthread_mutex_unlock(&o->mu);
        if (!b) { struct timespec ts = {0, 2000000}; nanosleep(&ts, NULL); continue; }
        uint64_t bytes = b->size;
        uint64_t ns = bytes * 1000000000ull / (uint64_t)(o->rate * o->ch * 2);
        struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&o->mu);
        if (o->nrel < (int)(sizeof o->rel / sizeof o->rel[0])) o->rel[o->nrel++] = b;
        o->played += bytes;
        pthread_cond_broadcast(&o->cv);
        pthread_mutex_unlock(&o->mu);
    }
    return NULL;
}

void *dm_pcmout_open(int ch) {
    struct pcmout *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    pthread_mutex_init(&o->mu, NULL); pthread_cond_init(&o->cv, NULL);
    o->ch = ch > 0 ? ch : 2; o->rate = 48000;
    SDL_AudioSpec spec = {0x8010 /* SDL_AUDIO_S16LE */, o->ch, o->rate};
    if (sdl_audio_load() && p_SDL_InitSubSystem(0x10 /* SDL_INIT_AUDIO */)) {
        o->stream = p_SDL_OpenAudioDeviceStream(0xFFFFFFFFu /* default playback */, &spec, NULL, NULL);
    }
    if (!o->stream) { o->silent = 1; o->run = 1; pthread_create(&o->th, NULL, silent_thread, o); NX_LOGW("audio: no SDL audio device; timed silent sink (%d ch, %d Hz)", o->ch, o->rate); }
    else NX_LOGI("audio: SDL3 PCM output %d ch, %d Hz", o->ch, o->rate);
    return o;
}

void dm_pcmout_start(void *k) {
    struct pcmout *o = k; if (!o) return;
    pthread_mutex_lock(&o->mu); o->started = 1; pthread_mutex_unlock(&o->mu);
    if (o->stream) p_SDL_ResumeAudioStreamDevice(o->stream);
}
void dm_pcmout_close(void *k) {
    struct pcmout *o = k; if (!o) return;
    if (o->silent) { __atomic_store_n(&o->run, 0, __ATOMIC_RELEASE); pthread_join(o->th, NULL); }
    if (o->stream) p_SDL_DestroyAudioStream(o->stream);
    pthread_mutex_destroy(&o->mu); pthread_cond_destroy(&o->cv); free(o);
}
int dm_pcmout_append(void *k, void *b) {
    struct pcmout *o = k; aout_buf *buf = b;
    if (!o || !buf) return 0;
    if (o->stream) {
        pthread_mutex_lock(&o->mu);
        if (o->nq >= (int)(sizeof o->q / sizeof o->q[0])) { pthread_mutex_unlock(&o->mu); return 0; }
        o->q[o->nq] = buf; o->appended += buf->size; o->qend[o->nq] = o->appended; o->nq++;
        pthread_mutex_unlock(&o->mu);
        if (!p_SDL_PutAudioStreamData(o->stream, (const uint8_t *)buf->ptr + buf->offset, (int)buf->size)) return 0;
        return 1;
    }
    pthread_mutex_lock(&o->mu);
    if (o->nq >= (int)(sizeof o->q / sizeof o->q[0])) { pthread_mutex_unlock(&o->mu); return 0; }
    o->q[o->nq++] = buf;
    pthread_mutex_unlock(&o->mu);
    return 1;
}
void *dm_pcmout_released(void *k) {
    struct pcmout *o = k; if (!o) return NULL;
    if (o->stream) {
        int q = p_SDL_GetAudioStreamQueued(o->stream); if (q < 0) q = 0;
        uint64_t played = o->appended > (uint64_t)q ? o->appended - (uint64_t)q : 0;
        pthread_mutex_lock(&o->mu);
        if (played > o->played) o->played = played;
        while (o->nq > 0 && o->qend[0] <= o->played) {
            if (o->nrel < (int)(sizeof o->rel / sizeof o->rel[0])) o->rel[o->nrel++] = o->q[0];
            memmove(o->q, o->q + 1, (size_t)(--o->nq) * sizeof o->q[0]);
            memmove(o->qend, o->qend + 1, (size_t)o->nq * sizeof o->qend[0]);
        }
        pthread_mutex_unlock(&o->mu);
    }
    pthread_mutex_lock(&o->mu);
    aout_buf *b = o->nrel ? o->rel[0] : NULL;
    if (b) { memmove(o->rel, o->rel + 1, (size_t)(--o->nrel) * sizeof o->rel[0]); }
    pthread_mutex_unlock(&o->mu);
    return b;
}
void dm_pcmout_wait(void *k, int ms) {
    struct pcmout *o = k; if (!o) return;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&o->mu);
    pthread_cond_timedwait(&o->cv, &o->mu, &ts);
    pthread_mutex_unlock(&o->mu);
}

// Drop everything the device has not played yet and give every queued buffer
// back to its producer. Used by StopAudioOut, where the guest expects its blocks
// released promptly instead of hearing up to one sink lead of stale audio.
void dm_pcmout_purge(void *k) {
    struct pcmout *o = k;
    if (!o) return;
    pthread_mutex_lock(&o->mu);
    if (o->stream && p_SDL_ClearAudioStream) p_SDL_ClearAudioStream(o->stream);
    for (int i = 0; i < o->nq; i++)
        if (o->nrel < (int)(sizeof o->rel / sizeof o->rel[0])) o->rel[o->nrel++] = o->q[i];
    o->nq = 0;
    o->played = o->appended;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mu);
}
