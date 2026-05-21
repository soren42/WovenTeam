#include "wt_security.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WT_SHA256_BLOCK_SIZE 64
#define WT_SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bitLen;
    unsigned char buffer[WT_SHA256_BLOCK_SIZE];
    size_t bufferLen;
} WtSha256;

static const uint32_t WT_SHA256_K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t rotr32(uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

static uint32_t loadBe32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void storeBe64(unsigned char *p, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        p[i] = (unsigned char)(value & 0xffU);
        value >>= 8;
    }
}

static void sha256Transform(WtSha256 *ctx, const unsigned char block[WT_SHA256_BLOCK_SIZE]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = loadBe32(block + (size_t)i * 4);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + WT_SHA256_K[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256Init(WtSha256 *ctx) {
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
    ctx->bitLen = 0;
    ctx->bufferLen = 0;
}

static void sha256Update(WtSha256 *ctx, const unsigned char *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->buffer[ctx->bufferLen++] = data[i];
        if (ctx->bufferLen == WT_SHA256_BLOCK_SIZE) {
            sha256Transform(ctx, ctx->buffer);
            ctx->bitLen += WT_SHA256_BLOCK_SIZE * 8U;
            ctx->bufferLen = 0;
        }
    }
}

static void sha256Final(WtSha256 *ctx, unsigned char digest[WT_SHA256_DIGEST_SIZE]) {
    ctx->bitLen += (uint64_t)ctx->bufferLen * 8U;
    ctx->buffer[ctx->bufferLen++] = 0x80U;
    if (ctx->bufferLen > 56) {
        while (ctx->bufferLen < WT_SHA256_BLOCK_SIZE) ctx->buffer[ctx->bufferLen++] = 0;
        sha256Transform(ctx, ctx->buffer);
        ctx->bufferLen = 0;
    }
    while (ctx->bufferLen < 56) ctx->buffer[ctx->bufferLen++] = 0;
    storeBe64(ctx->buffer + 56, ctx->bitLen);
    sha256Transform(ctx, ctx->buffer);

    for (int i = 0; i < 8; ++i) {
        digest[(size_t)i * 4] = (unsigned char)(ctx->state[i] >> 24);
        digest[(size_t)i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
        digest[(size_t)i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
        digest[(size_t)i * 4 + 3] = (unsigned char)ctx->state[i];
    }
}

void wtTokenHash(const char *token, char *out, size_t outSize) {
    if (!out || outSize == 0) return;
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[WT_SHA256_DIGEST_SIZE];
    WtSha256 ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, (const unsigned char *)(token ? token : ""), strlen(token ? token : ""));
    sha256Final(&ctx, digest);
    if (outSize < 72) {
        snprintf(out, outSize, "%s", "");
        return;
    }
    memcpy(out, "sha256:", 7);
    for (size_t i = 0; i < WT_SHA256_DIGEST_SIZE; ++i) {
        out[7 + i * 2] = hex[digest[i] >> 4];
        out[8 + i * 2] = hex[digest[i] & 0x0fU];
    }
    out[71] = '\0';
}

void wtTokenPreview(const char *token, char *out, size_t outSize) {
    if (!out || outSize == 0) return;
    size_t len = strlen(token ? token : "");
    if (len < 8) {
        snprintf(out, outSize, "%s", "[redacted]");
        return;
    }
    snprintf(out, outSize, "%.4s...[redacted]", token);
}

static int looksSecretAt(const char *s) {
    return strncmp(s, "wt_", 3) == 0 ||
           strncmp(s, "ghp_", 4) == 0 ||
           strncmp(s, "github_pat_", 11) == 0 ||
           strncmp(s, "AKIA", 4) == 0 ||
           strncmp(s, "-----BEGIN ", 11) == 0;
}

int wtRedactSecrets(const char *input, char *output, size_t outputSize) {
    if (!output || outputSize == 0) return -1;
    output[0] = '\0';
    const char *redacted = "[REDACTED]";
    size_t used = 0;
    for (const char *p = input ? input : ""; *p; ) {
        if (looksSecretAt(p)) {
            size_t n = strlen(redacted);
            if (used + n + 1 >= outputSize) return -1;
            memcpy(output + used, redacted, n);
            used += n;
            while (*p && !isspace((unsigned char)*p) && *p != '"' && *p != '\'' && *p != ',' && *p != '}') p++;
            continue;
        }
        if (used + 2 >= outputSize) return -1;
        output[used++] = *p++;
    }
    output[used] = '\0';
    return 0;
}
