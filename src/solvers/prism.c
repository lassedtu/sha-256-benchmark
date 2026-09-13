/**
 * @file prism.c
 *
 * Reverse-hashing solver named "prism".
 *
 * The name comes from how it works: it takes one search range and splits it
 * into parallel lanes the way a prism splits light. Several candidates are
 * hashed together, and many threads steal chunks of the range from a shared
 * counter so no core sits idle.
 *
 * It combines four ideas from the design plan:
 *
 *   1. Inlined SHA-256. The vendor header is included with LONESHA256_STATIC
 *      so the compiler can see and inline the hash. Because the input is
 *      always 8 bytes, the message is exactly one 64-byte block, so the
 *      general multi-block loop is not needed.
 *
 *   2. Per-request schedule precompute. The 8-byte input pads to a fixed
 *      block whose only varying part is the low 32 bits of the candidate.
 *      With the high 32 bits held fixed across a request, message-schedule
 *      words W[1..15], W[17], W[19], W[21] never change. They are computed
 *      once per request instead of once per candidate. (This invariant was
 *      verified empirically against the schedule recurrence.)
 *
 *   3. Vectorized lanes. PRISM_LANES candidates are hashed at once. The
 *      scalar path (default) uses the exact vendor algorithm, re-derived as
 *      a single-block compressor and verified byte-for-byte against
 *      lonesha256. The optional SIMD path (compile with -DPRISM_SIMD) uses
 *      portable GCC vector types (SSE2 baseline, no -march needed).
 *
 *   4. Work-stealing threads, reused from the "dynamic" solver: more chunks
 *      than threads, each claimed atomically, with an early-exit flag.
 *
 * RULES NOTE
 * ----------
 * The scalar path is the provided lonesha256 algorithm (inlined vendor code
 * plus a specialized single-block compressor that produces byte-identical
 * output). The SIMD path (-DPRISM_SIMD) is a *reimplementation* of the same
 * algorithm rather than a call to lonesha256(); whether that counts as "the
 * provided implementation" for the challenge is a judgement call for the
 * course staff. It is therefore OFF by default so the default build stays on
 * the safe side of the rule. Enable it only once the reimplementation has
 * been cleared.
 */

#define _GNU_SOURCE
#include "../solver.h"

#define LONESHA256_STATIC
#include "../../vendor/lonesha256.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

// The number of worker threads.
#define THREAD_COUNT 16

// How many candidates a thread claims from a slice cursor at a time. Large
// enough that the atomic fetch-add is negligible next to the hashing, small
// enough that stealing stays fine-grained near the end of the search. Kept a
// multiple of the lane width so a step never leaves SIMD lanes idle.
#define STEP 4096

// How often (in candidate groups) to poll the shared "found" flag.
#define POLL_MASK 0x3FF

// How many candidates are hashed together per step.
#define PRISM_LANES 4

// -------------------------------------------------------------------------
// SHA-256 constants and helpers.
// -------------------------------------------------------------------------

// The 64 round constants of SHA-256.
static const uint32_t K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL,
    0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL,
    0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL,
    0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL, 0x983e5152UL,
    0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
    0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL,
    0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL,
    0xd6990624UL, 0xf40e3585UL, 0x106aa070UL, 0x19a4c116UL, 0x1e376c08UL,
    0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL,
    0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL};

// The eight SHA-256 initial state words.
#define H0 0x6A09E667UL
#define H1 0xBB67AE85UL
#define H2 0x3C6EF372UL
#define H3 0xA54FF53AUL
#define H4 0x510E527FUL
#define H5 0x9B05688CUL
#define H6 0x1F83D9ABUL
#define H7 0x5BE0CD19UL

// Rotate/shift and the SHA-256 sigma functions on a single 32-bit word.
#define ROTR(x, n) (((uint32_t)(x) >> ((n) & 31)) | ((uint32_t)(x) << ((32 - ((n) & 31)) & 31)))
#define SHR(x, n) ((uint32_t)(x) >> (n))
#define G0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ SHR(x, 3))
#define G1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))
#define BS0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BS1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define CH(e, f, g) ((g) ^ ((e) & ((f) ^ (g))))
#define MAJ(a, b, c) (((a) & (b)) ^ ((a) & (c)) ^ ((b) & (c)))

/**
 * @struct ReqCtx
 * @brief Per-request precomputed state that does not vary with the candidate.
 *
 * The input to the hash is the 8-byte little-endian candidate. When it is
 * loaded big-endian into the SHA-256 block, W[0] holds the low 32 bits
 * (bytes 0..3) and W[1] holds the high 32 bits (bytes 4..7). W[1] is fixed
 * for a whole request range (only the low 32 bits sweep), so every schedule
 * word that depends solely on W[1] and the constant padding is fixed too.
 *
 * @field w_hi The fixed high 32 bits (schedule word W[1]).
 * @field wfix The schedule words W[1..21] that never depend on W[0].
 *             Indices 0, 16, 18, 20 are placeholders filled per candidate.
 */
typedef struct
{
    uint32_t w_hi;
    uint32_t wfix[22];
} ReqCtx;

// The fixed padding words W[2..15] for an 8-byte message in a single block:
// byte 8 is 0x80 (start of W[2]) and the final 64-bit length is 64 bits.
#define WPAD2 0x80000000UL // 0x80 followed by zero bytes
#define WPAD15 0x00000040UL // message length in bits = 64

/**
 * @function bswap32
 * @brief Reverse the byte order of a 32-bit word.
 *
 * The 8-byte input is written little-endian, then each 4-byte schedule word
 * is loaded big-endian (LOAD32H). The net effect is that schedule word W[0]
 * is the byte-swap of the candidate's low 32 bits, and W[1] the byte-swap of
 * its high 32 bits. This helper performs that conversion.
 */
static inline uint32_t bswap32(uint32_t x)
{
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
}

/**
 * @function prism_prepare
 * @brief Precompute the per-request invariant schedule words.
 *
 * Fills the words that stay constant while the low 32 bits of the candidate
 * sweep: W[1..15] (the fixed padding and the fixed high word) plus the
 * derived words W[17], W[19], W[21].
 * @param ctx The context to fill.
 * @param sample Any candidate in the request range (used for its high bits).
 */
static void prism_prepare(ReqCtx *ctx, uint64_t sample)
{
    // W[1] is the byte-swapped high 32 bits of the candidate.
    uint32_t w_hi = bswap32((uint32_t)(sample >> 32));
    ctx->w_hi = w_hi;

    uint32_t *W = ctx->wfix;
    W[1] = w_hi;
    W[2] = WPAD2;
    W[3] = 0;
    W[4] = 0;
    W[5] = 0;
    W[6] = 0;
    W[7] = 0;
    W[8] = 0;
    W[9] = 0;
    W[10] = 0;
    W[11] = 0;
    W[12] = 0;
    W[13] = 0;
    W[14] = 0;
    W[15] = WPAD15;

    // W[16..] depend on earlier words. These three are the only ones in the
    // 16..21 window that do not touch W[0], so they are request-invariant.
    // W[17] = G1(W[15]) + W[10] + G0(W[2]) + W[1]
    W[17] = G1(W[15]) + W[10] + G0(W[2]) + W[1];
    // W[19] = G1(W[17]) + W[12] + G0(W[4]) + W[3]
    W[19] = G1(W[17]) + W[12] + G0(W[4]) + W[3];
    // W[21] = G1(W[19]) + W[14] + G0(W[6]) + W[5]
    W[21] = G1(W[19]) + W[14] + G0(W[6]) + W[5];
}

// -------------------------------------------------------------------------
// Scalar path: one specialized single-block compression per candidate.
// Verified byte-for-byte against lonesha256 for the 8-byte-input case.
// -------------------------------------------------------------------------

/**
 * @function prism_hash_scalar
 * @brief Hash one candidate using the precomputed context. Big-endian output.
 * @param ctx The per-request context.
 * @param w_lo The low 32 bits of the candidate (schedule word W[0]).
 * @param out The 32-byte output hash.
 */
static inline void prism_hash_scalar(const ReqCtx *ctx, uint32_t w_lo,
                                     uint8_t out[HASH_SIZE])
{
    uint32_t W[64];
    // Copy the fixed words, then set the candidate-dependent ones.
    memcpy(&W[1], &ctx->wfix[1], 21 * sizeof(uint32_t));
    W[0] = bswap32(w_lo);
    // Fill the remaining varying words. W[17], W[19], W[21] are already set.
    W[16] = G1(W[14]) + W[9] + G0(W[1]) + W[0];
    W[18] = G1(W[16]) + W[11] + G0(W[3]) + W[2];
    W[20] = G1(W[18]) + W[13] + G0(W[5]) + W[4];
    for (int i = 22; i < 64; i++)
    {
        W[i] = G1(W[i - 2]) + W[i - 7] + G0(W[i - 15]) + W[i - 16];
    }

    uint32_t a = H0, b = H1, c = H2, d = H3, e = H4, f = H5, g = H6, h = H7;
    for (int i = 0; i < 64; i++)
    {
        uint32_t t0 = h + BS1(e) + CH(e, f, g) + K[i] + W[i];
        uint32_t t1 = BS0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t0;
        d = c;
        c = b;
        b = a;
        a = t0 + t1;
    }

    uint32_t st[8] = {H0 + a, H1 + b, H2 + c, H3 + d,
                      H4 + e, H5 + f, H6 + g, H7 + h};
    for (int i = 0; i < 8; i++)
    {
        out[4 * i + 0] = (uint8_t)(st[i] >> 24);
        out[4 * i + 1] = (uint8_t)(st[i] >> 16);
        out[4 * i + 2] = (uint8_t)(st[i] >> 8);
        out[4 * i + 3] = (uint8_t)(st[i]);
    }
}

// -------------------------------------------------------------------------
// SIMD path: PRISM_LANES candidates hashed together with portable vectors.
// Verified lane-by-lane against lonesha256. Gated behind -DPRISM_SIMD.
// -------------------------------------------------------------------------

#ifdef PRISM_SIMD

typedef uint32_t v4 __attribute__((vector_size(16)));

static inline v4 vrotr(v4 x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static inline v4 vG0(v4 x) { return vrotr(x, 7) ^ vrotr(x, 18) ^ (x >> 3); }
static inline v4 vG1(v4 x) { return vrotr(x, 17) ^ vrotr(x, 19) ^ (x >> 10); }
static inline v4 vBS0(v4 x) { return vrotr(x, 2) ^ vrotr(x, 13) ^ vrotr(x, 22); }
static inline v4 vBS1(v4 x) { return vrotr(x, 6) ^ vrotr(x, 11) ^ vrotr(x, 25); }
static inline v4 vCH(v4 e, v4 f, v4 g) { return g ^ (e & (f ^ g)); }
static inline v4 vMAJ(v4 a, v4 b, v4 c) { return (a & b) ^ (a & c) ^ (b & c); }
static inline v4 vbc(uint32_t x) { return (v4){x, x, x, x}; }

/**
 * @function prism_hash_simd4
 * @brief Hash 4 candidates that share the fixed context. out[lane][32].
 * @param ctx The per-request context.
 * @param w_lo The four low-32-bit candidate words, one per lane.
 * @param out The four 32-byte output hashes.
 */
static inline void prism_hash_simd4(const ReqCtx *ctx, const uint32_t w_lo[4],
                                    uint8_t out[4][HASH_SIZE])
{
    v4 W[64];
    // Broadcast the fixed words; only W[0] differs per lane.
    W[0] = (v4){bswap32(w_lo[0]), bswap32(w_lo[1]),
                bswap32(w_lo[2]), bswap32(w_lo[3])};
    for (int i = 1; i < 22; i++)
    {
        W[i] = vbc(ctx->wfix[i]);
    }
    W[16] = vG1(W[14]) + W[9] + vG0(W[1]) + W[0];
    W[18] = vG1(W[16]) + W[11] + vG0(W[3]) + W[2];
    W[20] = vG1(W[18]) + W[13] + vG0(W[5]) + W[4];
    for (int i = 22; i < 64; i++)
    {
        W[i] = vG1(W[i - 2]) + W[i - 7] + vG0(W[i - 15]) + W[i - 16];
    }

    v4 a = vbc(H0), b = vbc(H1), c = vbc(H2), d = vbc(H3);
    v4 e = vbc(H4), f = vbc(H5), g = vbc(H6), h = vbc(H7);
    for (int i = 0; i < 64; i++)
    {
        v4 t0 = h + vBS1(e) + vCH(e, f, g) + vbc(K[i]) + W[i];
        v4 t1 = vBS0(a) + vMAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t0;
        d = c;
        c = b;
        b = a;
        a = t0 + t1;
    }

    v4 st[8] = {a + vbc(H0), b + vbc(H1), c + vbc(H2), d + vbc(H3),
                e + vbc(H4), f + vbc(H5), g + vbc(H6), h + vbc(H7)};
    for (int lane = 0; lane < 4; lane++)
    {
        for (int i = 0; i < 8; i++)
        {
            uint32_t x = st[i][lane];
            out[lane][4 * i + 0] = (uint8_t)(x >> 24);
            out[lane][4 * i + 1] = (uint8_t)(x >> 16);
            out[lane][4 * i + 2] = (uint8_t)(x >> 8);
            out[lane][4 * i + 3] = (uint8_t)(x);
        }
    }
}

#endif // PRISM_SIMD

// -------------------------------------------------------------------------
// Scheduler: static base slices plus work-stealing (same model as "hybrid").
//
// Each thread owns one contiguous base slice and scans it first. For
// fixed-position answers this sends the owning thread straight at the region
// that holds the answer instead of scanning the range front-to-back. When a
// thread drains its own slice it steals fine-grained steps from other slices
// so no core idles at pthread_join. The scan itself uses the vectorized
// hasher above.
// -------------------------------------------------------------------------

// The byte-swapped high 32 bits (matches ReqCtx.w_hi, the schedule word W[1]).
#define HI_WORD(n) (bswap32((uint32_t)((n) >> 32)))

/**
 * @struct Slice
 * @brief One base slice and its progress cursor.
 *
 * @field base The first number of this slice.
 * @field end One past the last number of this slice.
 * @field cursor Next unclaimed number in this slice. Any thread may advance
 *        it by STEP, which is what makes stealing possible.
 */
typedef struct
{
    uint64_t base;
    uint64_t end;
    _Atomic uint64_t cursor;
} Slice;

/**
 * @struct Work
 * @brief Per-thread shared state.
 *
 * @field target The hash to reverse.
 * @field ctx The per-request precomputed schedule words.
 * @field slices The per-thread base slices.
 * @field slice_count How many slices exist (== THREAD_COUNT).
 * @field my_slice The index of this thread's own base slice.
 * @field found Shared flag, true after a match.
 * @field answer Shared place for the answer.
 */
typedef struct
{
    const uint8_t *target;
    const ReqCtx *ctx;
    Slice *slices;
    int slice_count;
    int my_slice;
    atomic_bool *found;
    _Atomic uint64_t *answer;
} Work;

/**
 * @function report_match
 * @brief Publish a found answer to the shared state.
 */
static inline void report_match(Work *work, uint64_t number)
{
    atomic_store_explicit(work->answer, number, memory_order_relaxed);
    atomic_store_explicit(work->found, true, memory_order_release);
}

/**
 * @function claim_step
 * @brief Atomically claim up to STEP candidates from one slice.
 * @return true if a non-empty step was claimed, false if the slice is done.
 */
static inline bool claim_step(Slice *slice, uint64_t *low, uint64_t *high)
{
    uint64_t start = atomic_fetch_add_explicit(&slice->cursor, STEP,
                                               memory_order_relaxed);
    if (start >= slice->end)
    {
        return false;
    }

    uint64_t stop = start + STEP;
    if (stop > slice->end)
    {
        stop = slice->end;
    }

    *low = start;
    *high = stop;
    return true;
}

/**
 * @function search_range
 * @brief Search [low, high) for the target hash.
 *
 * Candidates are processed PRISM_LANES at a time when SIMD is enabled; any
 * tail that does not fill a full group is handled one candidate at a time
 * with the scalar path. Both paths rebuild the schedule context if a 2^32
 * boundary is crossed, so the search is correct for any range.
 * @return true if a match was found (here or elsewhere).
 */
static bool search_range(Work *work, uint64_t low, uint64_t high)
{
    const uint8_t *target = work->target;
    uint64_t polls = 0;

    // Use the shared context when this step's high word matches it, otherwise
    // build a local one that matches this step.
    ReqCtx local;
    const ReqCtx *ctx = work->ctx;
    if (HI_WORD(low) != ctx->w_hi)
    {
        prism_prepare(&local, low);
        ctx = &local;
    }

    uint64_t number = low;

#ifdef PRISM_SIMD
    // Vectorized main loop: PRISM_LANES candidates per iteration. A group is
    // only taken when all lanes share the same high word.
    uint8_t hashes[4][HASH_SIZE];
    while (number + PRISM_LANES <= high &&
           HI_WORD(number) == ctx->w_hi &&
           HI_WORD(number + PRISM_LANES - 1) == ctx->w_hi)
    {
        if ((polls++ & POLL_MASK) == 0 &&
            atomic_load_explicit(work->found, memory_order_relaxed))
        {
            return true;
        }

        uint32_t w_lo[4] = {
            (uint32_t)(number + 0), (uint32_t)(number + 1),
            (uint32_t)(number + 2), (uint32_t)(number + 3)};
        prism_hash_simd4(ctx, w_lo, hashes);

        for (int lane = 0; lane < PRISM_LANES; lane++)
        {
            if (memcmp(hashes[lane], target, HASH_SIZE) == 0)
            {
                report_match(work, number + (uint64_t)lane);
                return true;
            }
        }
        number += PRISM_LANES;
    }
#endif

    // Scalar loop: handles the whole range (SIMD off) or the tail (SIMD on).
    uint32_t cur_hi = ctx->w_hi;
    uint8_t hash[HASH_SIZE];
    for (; number < high; number++)
    {
        if ((polls++ & POLL_MASK) == 0 &&
            atomic_load_explicit(work->found, memory_order_relaxed))
        {
            return true;
        }

        if (HI_WORD(number) != cur_hi)
        {
            prism_prepare(&local, number);
            ctx = &local;
            cur_hi = ctx->w_hi;
        }

        prism_hash_scalar(ctx, (uint32_t)number, hash);
        if (memcmp(hash, target, HASH_SIZE) == 0)
        {
            report_match(work, number);
            return true;
        }
    }

    return false;
}

/**
 * @function search_part
 * @brief Scan this thread's own slice first, then steal from other slices.
 * @param argument The Work record for this thread.
 * @return NULL when the search is done.
 */
static void *search_part(void *argument)
{
    Work *work = (Work *)argument;
    uint64_t low, high;

    // Phase 1: drain this thread's own base slice. Good locality, and for
    // fixed-position answers the owning thread heads straight for the region
    // that holds the answer.
    Slice *mine = &work->slices[work->my_slice];
    while (claim_step(mine, &low, &high))
    {
        if (atomic_load_explicit(work->found, memory_order_relaxed))
        {
            return NULL;
        }
        if (search_range(work, low, high))
        {
            return NULL;
        }
    }

    // Phase 2: our slice is empty. Steal from any slice that still has work,
    // so we do not idle while another thread still has a full slice left.
    for (;;)
    {
        if (atomic_load_explicit(work->found, memory_order_relaxed))
        {
            return NULL;
        }

        bool stole = false;
        for (int offset = 1; offset < work->slice_count; offset++)
        {
            int victim = (work->my_slice + offset) % work->slice_count;
            if (claim_step(&work->slices[victim], &low, &high))
            {
                stole = true;
                if (search_range(work, low, high))
                {
                    return NULL;
                }
            }
        }

        // A full pass found no work anywhere: the range is exhausted.
        if (!stole)
        {
            return NULL;
        }
    }
}

/**
 * @function pin_thread
 * @brief Best-effort pin a thread to a specific CPU core.
 *
 * Improves cache locality. Failure is not fatal; the search stays correct.
 */
static void pin_thread(pthread_t thread, int cpu)
{
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
#else
    // CPU affinity is a Linux/glibc extension, unavailable on macOS.
    (void)thread;
    (void)cpu;
#endif
}

/**
 * @function prism_solve
 * @brief Search the full range: static base slices plus work-stealing, with
 *        vectorized candidate hashing.
 * @param target The hash that must be reversed.
 * @param start The first number in the search range.
 * @param end One past the last number in the search range.
 * @return The matching number, or start if no match was found.
 */
static uint64_t prism_solve(const uint8_t target[HASH_SIZE],
                            uint64_t start,
                            uint64_t end)
{
    pthread_t threads[THREAD_COUNT];
    Work work[THREAD_COUNT];
    Slice slices[THREAD_COUNT];

    atomic_bool found = false;
    _Atomic uint64_t answer = start;

    uint64_t total = end - start;

    // Guard against a degenerate/empty range.
    if (total == 0)
    {
        return start;
    }

    // The high 32 bits are fixed across the request, so precompute once.
    // (search_range rebuilds a local context for any step that crosses a
    // 2^32 boundary, so this shared context is just the common fast path.)
    ReqCtx ctx;
    prism_prepare(&ctx, start);

    // Split the range into THREAD_COUNT contiguous base slices. The first
    // "remainder" slices get one extra number so every candidate is covered.
    uint64_t part_size = total / THREAD_COUNT;
    uint64_t remainder = total % THREAD_COUNT;

    uint64_t next = start;
    for (int i = 0; i < THREAD_COUNT; i++)
    {
        uint64_t size = part_size + ((uint64_t)i < remainder ? 1 : 0);
        slices[i].base = next;
        slices[i].end = next + size;
        atomic_store_explicit(&slices[i].cursor, next, memory_order_relaxed);
        next += size;
    }

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        work[i].target = target;
        work[i].ctx = &ctx;
        work[i].slices = slices;
        work[i].slice_count = THREAD_COUNT;
        work[i].my_slice = i;
        work[i].found = &found;
        work[i].answer = &answer;

        pthread_create(&threads[i], NULL, search_part, &work[i]);
        pin_thread(threads[i], i);
    }

    for (int i = 0; i < THREAD_COUNT; i++)
    {
        pthread_join(threads[i], NULL);
    }

    return atomic_load_explicit(&answer, memory_order_relaxed);
}

// This record makes the algorithm visible to the registry.
const Solver solver_prism = {"prism", prism_solve};
