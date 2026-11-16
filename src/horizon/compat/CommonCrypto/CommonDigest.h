#pragma once
#include <stddef.h>
#include <stdint.h>
#include "horizon/linux_compat.h"
#define CC_SHA256_DIGEST_LENGTH 32
typedef NxSha256Ctx CC_SHA256_CTX;
typedef uint32_t CC_LONG;
#define CC_SHA256(data, len, out) nx_sha256((data), (len), (out))
#define CC_SHA256_Init(ctx) nx_sha256_init(ctx)
#define CC_SHA256_Update(ctx, data, len) nx_sha256_update((ctx), (data), (len))
#define CC_SHA256_Final(out, ctx) nx_sha256_final((ctx), (out))
