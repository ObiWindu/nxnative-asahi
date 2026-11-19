// AAC (movie audio) for the movie decoder (hle_movie.c): AudioToolbox on Apple,
// libavcodec on Linux (hle_movie_ffmpeg.c). The demuxer hands over raw AAC
// frames (1024 samples each) and the AudioSpecificConfig from Configure's
// csd-0.
#pragma once
#include <stdint.h>

typedef struct maac maac;

// rate and channels come from the configuration (or the defaults the caller
// picked); asc may be NULL/0, in which case an AAC-LC configuration is made
// from them. NULL when the platform cannot make an AAC decoder.
maac *maac_new(int rate, int ch, const uint8_t *asc, int asc_n);
void maac_free(maac *a);

// Decode one packet into interleaved PCM16, at most max_frames frames (each
// frame is rate/ch samples). Returns the frames written: 0 when the decoder
// is still filling its start-up delay or the packet gave nothing.
int maac_decode(maac *a, const uint8_t *p, int n, int16_t *out, int max_frames);
