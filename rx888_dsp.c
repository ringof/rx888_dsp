/*
 * rx888_dsp (stdin -> stdout/FIFO)
 *
 * Purpose
 *   Stream-processing decimator intended for RX888-class SDR front-ends.
 *   Input:   16-bit signed real samples on stdin at Fs = 135 MS/s (typical).
 *   Output:  complex 32-bit float IQ at Fs/4 after two halfband decimation stages.
 *
 * Pipeline overview (4 DSP stages)
 *   Stage 1: int16 real -> float IQ with -Fs/4 frequency shift (real->complex conversion).
 *   Stage 2: Halfband FIR #1, decimate-by-2 (Fs/2).
 *   Stage 3: +Fs/4 frequency shift (compensates Stage 1 mixing to place spectrum at baseband).
 *   Stage 4: Halfband FIR #2, decimate-by-2 (Fs/4).
 *
 * Performance notes
 *   - Stage 4 uses folded halfband symmetry (only 59 coefficient-pairs for a 235-tap HB),
 *     and an AVX2/FMA inner loop on separate I/Q arrays (SoA) for sustained throughput.
 *   - AVX2 is used where it provides clear benefit; other stages remain straightforward.
 *
 * I/O / robustness
 *   - Uses a fixed-size buffer pool and SPSC queues (free -> filled -> ready -> free).
 *   - Reads exactly one full block per buffer (handles short reads on pipes).
 *   - Output to stdout by default, or to a FIFO path (-o). FIFO disconnects/reconnects are handled.
 *
 * Build
 *   gcc -O3 -mavx2 -mfma -std=c11 -o rx888_dsp rx888_dsp.c -lm -lpthread -Wall
 *   (You may also use -march=native; consider disabling AVX-512 on laptops if it downclocks.)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <complex.h>
#include <immintrin.h>

// Buffer sizes
#define INPUT_SAMPLES       262144
#define STAGE1_SAMPLES      262144
#define STAGE2_SAMPLES      131072
#define OUTPUT_SAMPLES       65536
#define NUM_BUFFERS 64

// Halfband 1: 19 taps
#define HB1_LEN 19
#define HB1_CENTER 9
static const float h1[HB1_LEN] = {
    0.001268f, 0.0f, -0.0076082f, 0.0f, 0.027067f, 0.0f, -0.078846f, 0.0f,
    0.30815f, 0.5f, 0.30815f, 0.0f, -0.078846f, 0.0f, 0.027067f, 0.0f,
    -0.0076082f, 0.0f, 0.001268f
};

// Halfband 2: 235-tap halfband FIR (embedded coefficients)
// Notes: odd taps are zero except the center tap (0.5). Even taps are symmetric.
// This enables the folded halfband form used in Stage 4 (59 coefficient-pairs).
#define HB2_LEN 235
#define HB2_CENTER 117
#define HB2_PAIRS 59  // (235-1)/4 = 58.5 → 59

static const float h2[HB2_LEN] = {
    6.0483e-05f, 0.0f, -3.6537e-05f, 0.0f, 4.7397e-05f, 0.0f, -6.0258e-05f, 0.0f,
    7.5357e-05f, 0.0f, -9.2947e-05f, 0.0f, 0.0001133f, 0.0f, -0.00013671f, 0.0f,
    0.00016347f, 0.0f, -0.00019391f, 0.0f, 0.00022839f, 0.0f, -0.00026725f, 0.0f,
    0.00031088f, 0.0f, -0.00035967f, 0.0f, 0.00041406f, 0.0f, -0.00047449f, 0.0f,
    0.00054143f, 0.0f, -0.00061537f, 0.0f, 0.00069683f, 0.0f, -0.00078636f, 0.0f,
    0.00088456f, 0.0f, -0.00099203f, 0.0f, 0.0011094f, 0.0f, -0.0012375f, 0.0f,
    0.0013769f, 0.0f, -0.0015286f, 0.0f, 0.0016933f, 0.0f, -0.0018721f, 0.0f,
    0.002066f, 0.0f, -0.0022762f, 0.0f, 0.0025038f, 0.0f, -0.0027505f, 0.0f,
    0.0030179f, 0.0f, -0.0033078f, 0.0f, 0.0036223f, 0.0f, -0.0039639f, 0.0f,
    0.0043355f, 0.0f, -0.0047405f, 0.0f, 0.005183f, 0.0f, -0.0056678f, 0.0f,
    0.0062009f, 0.0f, -0.0067895f, 0.0f, 0.0074427f, 0.0f, -0.0081718f, 0.0f,
    0.0089914f, 0.0f, -0.0099204f, 0.0f, 0.010984f, 0.0f, -0.012215f, 0.0f,
    0.013661f, 0.0f, -0.015388f, 0.0f, 0.017494f, 0.0f, -0.020128f, 0.0f,
    0.023533f, 0.0f, -0.028128f, 0.0f, 0.034703f, 0.0f, -0.044954f, 0.0f,
    0.06329f, 0.0f, -0.10588f, 0.0f, 0.31824f, 0.5f, 0.31824f, 0.0f,
    -0.10588f, 0.0f, 0.06329f, 0.0f, -0.044954f, 0.0f, 0.034703f, 0.0f,
    -0.028128f, 0.0f, 0.023533f, 0.0f, -0.020128f, 0.0f, 0.017494f, 0.0f,
    -0.015388f, 0.0f, 0.013661f, 0.0f, -0.012215f, 0.0f, 0.010984f, 0.0f,
    -0.0099204f, 0.0f, 0.0089914f, 0.0f, -0.0081718f, 0.0f, 0.0074427f, 0.0f,
    -0.0067895f, 0.0f, 0.0062009f, 0.0f, -0.0056678f, 0.0f, 0.005183f, 0.0f,
    -0.0047405f, 0.0f, 0.0043355f, 0.0f, -0.0039639f, 0.0f, 0.0036223f, 0.0f,
    -0.0033078f, 0.0f, 0.0030179f, 0.0f, -0.0027505f, 0.0f, 0.0025038f, 0.0f,
    -0.0022762f, 0.0f, 0.002066f, 0.0f, -0.0018721f, 0.0f, 0.0016933f, 0.0f,
    -0.0015286f, 0.0f, 0.0013769f, 0.0f, -0.0012375f, 0.0f, 0.0011094f, 0.0f,
    -0.00099203f, 0.0f, 0.00088456f, 0.0f, -0.00078636f, 0.0f, 0.00069683f, 0.0f,
    -0.00061537f, 0.0f, 0.00054143f, 0.0f, -0.00047449f, 0.0f, 0.00041406f, 0.0f,
    -0.00035967f, 0.0f, 0.00031088f, 0.0f, -0.00026725f, 0.0f, 0.00022839f, 0.0f,
    -0.00019391f, 0.0f, 0.00016347f, 0.0f, -0.00013671f, 0.0f, 0.0001133f, 0.0f,
    -9.2947e-05f, 0.0f, 7.5357e-05f, 0.0f, -6.0258e-05f, 0.0f, 4.7397e-05f, 0.0f,
    -3.6537e-05f, 0.0f, 6.0483e-05f
};

static float hb2_c[HB2_PAIRS];  // Folded even-tap coefficients, built at startup

// Processing buffer
typedef struct {
    int16_t input[INPUT_SAMPLES];

    // Stage 1 as SoA (I/Q separate) so Stage 2 can run without complex ops.
    float stage1I[STAGE1_SAMPLES];
    float stage1Q[STAGE1_SAMPLES];

    // Stage 2 is SoA (I/Q separate) to enable efficient AVX2 in Stage 4
    float stage2I[STAGE2_SAMPLES];
    float stage2Q[STAGE2_SAMPLES];

    // SoA output for SIMD stage4; also provide interleaved IQ floats for the writer.
    float outI[OUTPUT_SAMPLES];
    float outQ[OUTPUT_SAMPLES];
    float output[OUTPUT_SAMPLES * 2];  // interleaved [I0,Q0,I1,Q1,...]

    volatile int state;
} buffer_t;

// C11 atomic SPSC queue
typedef struct {
    buffer_t *buffers[NUM_BUFFERS];
    _Atomic unsigned int write_idx;
    _Atomic unsigned int read_idx;
    char padding[64 - 2*sizeof(unsigned int)];
} spsc_queue_t;

static buffer_t buffer_pool[NUM_BUFFERS] __attribute__((aligned(64)));

// Renamed queues by lifecycle stage
static spsc_queue_t free_queue;    // Available to fill
static spsc_queue_t filled_queue;  // Ready for processing
static spsc_queue_t ready_queue;   // Ready to output

static volatile sig_atomic_t stop_flag = 0;

/* Compromise vs. full refactor:
   Keep the DSP hot-path globals as-is, but group program configuration / output state. */
typedef struct {
    int verbose;         /* -v */
    int block_on_full;   /* --block-on-full */
    int output_fd;       /* FIFO fd when connected, else -1 */
    char *output_path;   /* -o PATH, else NULL => stdout */
} app_ctx_t;

static app_ctx_t g_app = {
    .verbose = 0,
    .block_on_full = 0,
    .output_fd = -1,
    .output_path = NULL
};

#define PROGRAM_NAME "rx888_dsp"

/* Logging: errors always go to stderr; info only with -v. */
#define LOG_ERR(fmt, ...)  fprintf(stderr, PROGRAM_NAME ": " fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) do { if (g_app.verbose) fprintf(stderr, PROGRAM_NAME ": " fmt, ##__VA_ARGS__); } while (0)

static unsigned long samples_processed = 0;
static unsigned long blocks_dropped = 0;

// History buffers for streaming FIR
static float *hb1_histI = NULL;
static float *hb1_histQ = NULL;
static float *hb2_histI = NULL;
static float *hb2_histQ = NULL;
static float *hb2_extI = NULL;  // Stage4 working buffer: extended I
static float *hb2_extQ = NULL;  // Stage4 working buffer: extended Q
static float *hb2_eI = NULL, *hb2_eQ = NULL; // even phase I/Q (contiguous)
static float *hb2_oI = NULL, *hb2_oQ = NULL; // odd phase I/Q (contiguous)

// Statistics
static struct {
    unsigned long total_blocks;
    double avg_time_ms;
    double min_time_ms;
    double max_time_ms;
} stats = {0, 0.0, 1e9, 0.0};

//=============================================================================
// SPSC Queue (C11 atomics - correct!)
//=============================================================================

static inline void spsc_init(spsc_queue_t *q) {
    atomic_store_explicit(&q->write_idx, 0u, memory_order_relaxed);
    atomic_store_explicit(&q->read_idx, 0u, memory_order_relaxed);
}

static inline int spsc_push(spsc_queue_t *q, buffer_t *buf) {
    unsigned w = atomic_load_explicit(&q->write_idx, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&q->read_idx, memory_order_acquire);
    unsigned next = (w + 1u) % NUM_BUFFERS;
    if (next == r) return 0;  // Full
    
    q->buffers[w] = buf;
    atomic_store_explicit(&q->write_idx, next, memory_order_release);
    return 1;
}

static inline buffer_t* spsc_pop(spsc_queue_t *q) {
    unsigned r = atomic_load_explicit(&q->read_idx, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&q->write_idx, memory_order_acquire);
    if (r == w) return NULL;  // Empty
    
    buffer_t *buf = q->buffers[r];
    atomic_store_explicit(&q->read_idx, (r + 1u) % NUM_BUFFERS, memory_order_release);
    return buf;
}

//=============================================================================
// Filter Coefficient Folding
//=============================================================================

static void init_hb2_coeffs(void) {
    // r=0 uses tap[116], r=58 uses tap[0]
    for (int r = 0; r < HB2_PAIRS; r++) {
        int idx = (HB2_CENTER - 1) - 2*r;  // 116, 114, ..., 0
        hb2_c[r] = h2[idx];
    }
}

static inline unsigned long get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000UL + tv.tv_usec;
}


/*
 * read_full()
 * -----------
 * For high-rate streaming, especially from pipes (e.g., USB capture helpers),
 * read() is allowed to return *short* even when more data will arrive.
 * The original code treated any short read as a dropped block, which can
 * result in dropping everything when stdin is a pipe (e.g., `cat /dev/zero | ...`).
 *
 * This helper loops until the requested byte count is satisfied, EOF occurs
 * (returns 0 if nothing read), or an error occurs.
 */
static ssize_t read_full(int fd, void *buf, size_t bytes)
{
    uint8_t *p = (uint8_t*)buf;
    size_t off = 0;

    while (off < bytes) {
        ssize_t n = read(fd, p + off, bytes - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == 0) {
            // EOF
            return (off == 0) ? 0 : (ssize_t)off;
        }
        if (errno == EINTR) {
            continue; // retry
        }
        return -1; // error (errno set)
    }
    return (ssize_t)off;
}


//=============================================================================
// Stage 1: Convert int16 to complex with -Fs/4 shift
//=============================================================================

static inline void stage1_convert_and_shift(buffer_t *buf) {
    // Convert int16 real samples to complex baseband using a -Fs/4 shift.
    //
    // Multiply by exp(-j*pi/2*n) = [1, -j, -1, j] repeating.
    // For real input x[n], the resulting complex samples are:
    //   n mod 4 == 0: ( x,  0)
    //   n mod 4 == 1: ( 0, -x)
    //   n mod 4 == 2: (-x,  0)
    //   n mod 4 == 3: ( 0,  x)
    //
    // We store SoA: stage1I[n], stage1Q[n].
    //
    // "Fast+simple" SIMD:
    //  - Vectorize the expensive part (int16 -> float + scaling)
    //  - Keep the [-Fs/4] sign/zero pattern as simple scalar stores.
    // This keeps the code readable while still removing a lot of conversion overhead.
    const float scale = 1.0f / 32768.0f;

    // Process 16 int16 samples per iteration.
    // INPUT_SAMPLES is divisible by 16, so no tail path needed for normal operation.
    const int16_t *in = buf->input;
    float *Iptr = buf->stage1I;
    float *Qptr = buf->stage1Q;

    const __m256 scalev = _mm256_set1_ps(scale);
    float tmp[16] __attribute__((aligned(32)));

    for (int i = 0; i < INPUT_SAMPLES; i += 16) {
        // Load 16 x int16
        __m256i v16 = _mm256_loadu_si256((const __m256i *)(in + i));

        // Widen lower 8 and upper 8 to int32, then convert to float
        __m256i lo32 = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(v16));
        __m256i hi32 = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(v16, 1));

        __m256 flo = _mm256_mul_ps(_mm256_cvtepi32_ps(lo32), scalev);
        __m256 fhi = _mm256_mul_ps(_mm256_cvtepi32_ps(hi32), scalev);

        _mm256_store_ps(tmp + 0, flo);
        _mm256_store_ps(tmp + 8, fhi);

        // Apply [-Fs/4] pattern in groups of 4 samples
        // (this is cheap vs the conversion we just vectorized)
        for (int t = 0; t < 16; t += 4) {
            int n = i + t;
            float v0 = tmp[t + 0];
            float v1 = tmp[t + 1];
            float v2 = tmp[t + 2];
            float v3 = tmp[t + 3];

            Iptr[n + 0] =  v0; Qptr[n + 0] = 0.0f;   // * 1
            Iptr[n + 1] = 0.0f; Qptr[n + 1] = -v1;   // * (-j)
            Iptr[n + 2] = -v2; Qptr[n + 2] = 0.0f;   // * (-1)
            Iptr[n + 3] = 0.0f; Qptr[n + 3] =  v3;   // * ( j)
        }
    }
}

//=============================================================================
// Stage 2: HB1 with history (streaming FIR) producing SoA I/Q
//
// IMPORTANT: This is a *causal* streaming FIR:
//   y[m] = sum_{k=0..L-1} h[k] * x[2m - k]
//
// We build ext[] = [history(L-1) | current_block] so ext[H + i] == x[i].
// Then x[2m-k] maps to ext[H + 2m - k]. This avoids any look-ahead,
// eliminates bounds checks in the inner loop, and is correct across blocks.
//=============================================================================

static inline void stage2_hb1_stream_soa(buffer_t *buf, float *histI, float *histQ) {
    // HB1 decimate-by-2, causal streaming FIR on SoA data:
    //   y[m] = sum_{k=0..L-1} h[k] * x[2m - k]
    //
    // HB1 is a *halfband* filter (many zero taps, symmetric, center tap = 0.5).
    // For "fast+simple", we apply folded symmetry *without* AVX2:
    //   y = h[c]*x[p-c] + sum_{k=0..c-1} h[k]*(x[p-k] + x[p-(L-1-k)])
    // and we only include the non-zero even taps (k = 0,2,4,6,8) plus the center (k=9).
    //
    // This reduces HB1 from 19 MACs/output/channel to:
    //   1 center + 5 folded pairs  => 6 multiplies/output/channel
    // which is typically "in the noise" compared to the long HB2 stage.
    const int L = HB1_LEN;         // 19
    const int H = L - 1;           // 18 history samples
    const int C = HB1_CENTER;      // 9

    static float extI[(HB1_LEN - 1) + STAGE1_SAMPLES];
    static float extQ[(HB1_LEN - 1) + STAGE1_SAMPLES];

    memcpy(extI, histI, H * sizeof(float));
    memcpy(extQ, histQ, H * sizeof(float));
    memcpy(extI + H, buf->stage1I, STAGE1_SAMPLES * sizeof(float));
    memcpy(extQ + H, buf->stage1Q, STAGE1_SAMPLES * sizeof(float));

    // Pre-fetch the few non-zero coefficients we use (compiler will keep in regs)
    const float h1_0 = h1[0];
    const float h1_2 = h1[2];
    const float h1_4 = h1[4];
    const float h1_6 = h1[6];
    const float h1_8 = h1[8];
    const float h1_c = h1[C]; // should be 0.5f

    for (int m = 0; m < STAGE2_SAMPLES; m++) {
        int p = H + 2*m;  // ext[p] corresponds to x[2m]

        // Center tap (k=C=9): x[2m-9] => ext[p-9]
        float accI = h1_c * extI[p - C];
        float accQ = h1_c * extQ[p - C];

        // Folded symmetric pairs:
        // k=8 pairs with 10, k=6 with 12, k=4 with 14, k=2 with 16, k=0 with 18
        accI += h1_8 * (extI[p - 8] + extI[p - 10]);
        accQ += h1_8 * (extQ[p - 8] + extQ[p - 10]);

        accI += h1_6 * (extI[p - 6] + extI[p - 12]);
        accQ += h1_6 * (extQ[p - 6] + extQ[p - 12]);

        accI += h1_4 * (extI[p - 4] + extI[p - 14]);
        accQ += h1_4 * (extQ[p - 4] + extQ[p - 14]);

        accI += h1_2 * (extI[p - 2] + extI[p - 16]);
        accQ += h1_2 * (extQ[p - 2] + extQ[p - 16]);

        accI += h1_0 * (extI[p - 0] + extI[p - 18]);
        accQ += h1_0 * (extQ[p - 0] + extQ[p - 18]);

        buf->stage2I[m] = accI;
        buf->stage2Q[m] = accQ;
    }

    memcpy(histI, extI + STAGE1_SAMPLES, H * sizeof(float));
    memcpy(histQ, extQ + STAGE1_SAMPLES, H * sizeof(float));
}

//=============================================================================
// Stage 3: Shift by +Fs/4 (in-place) on SoA
//
// For each sample index i:
//   multiply by [1, j, -1, -j] repeating.
// In SoA this is just sign flips and (I,Q) swaps.
//=============================================================================

static inline void stage3_shift_fs4_soa(buffer_t *buf) {
    // AVX2 version (SoA): multiply by repeating [1, j, -1, -j].
    // For each index i:
    //   i&3==0: (I,Q)
    //   i&3==1: (-Q, I)
    //   i&3==2: (-I,-Q)
    //   i&3==3: ( Q,-I)
    //
    // This is branchy in scalar form; SIMD turns it into:
    //   - optional lane-wise swap of I/Q
    //   - lane-wise sign flips via XOR with +/-0.0f
    //
    // Masks depend only on (base_index & 3), so we precompute 4 mask sets.
    static const uint32_t swap_u[4][8] = {
        // base&3 == 0: lanes p = 0,1,2,3,0,1,2,3 => swap on 1,3
        {0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu},
        // base&3 == 1: lanes p = 1,2,3,0,1,2,3,0 => swap on 1,3
        {0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u},
        // base&3 == 2: lanes p = 2,3,0,1,2,3,0,1 => swap on 1,3
        {0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu},
        // base&3 == 3: lanes p = 3,0,1,2,3,0,1,2 => swap on 1,3
        {0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u},
    };

    static const uint32_t signI_u[4][8] = {
        // negate I when p==1 or p==2
        {0u, 0x80000000u, 0x80000000u, 0u, 0u, 0x80000000u, 0x80000000u, 0u},
        {0x80000000u, 0x80000000u, 0u, 0u, 0x80000000u, 0x80000000u, 0u, 0u},
        {0x80000000u, 0u, 0u, 0x80000000u, 0x80000000u, 0u, 0u, 0x80000000u},
        {0u, 0u, 0x80000000u, 0x80000000u, 0u, 0u, 0x80000000u, 0x80000000u},
    };

    static const uint32_t signQ_u[4][8] = {
        // negate Q when p==2 or p==3
        {0u, 0u, 0x80000000u, 0x80000000u, 0u, 0u, 0x80000000u, 0x80000000u},
        {0u, 0x80000000u, 0x80000000u, 0u, 0u, 0x80000000u, 0x80000000u, 0u},
        {0x80000000u, 0x80000000u, 0u, 0u, 0x80000000u, 0x80000000u, 0u, 0u},
        {0x80000000u, 0u, 0u, 0x80000000u, 0x80000000u, 0u, 0u, 0x80000000u},
    };

    // Avoid naming collision with the complex.h macro `I`.
    float *pI = buf->stage2I;
    float *pQ = buf->stage2Q;

    int i = 0;
    for (; i + 7 < STAGE2_SAMPLES; i += 8) {
        const int s = i & 3;

        const __m256 vI = _mm256_loadu_ps(pI + i);
        const __m256 vQ = _mm256_loadu_ps(pQ + i);

        // Swap candidates
        const __m256 vI_sw = vQ;
        const __m256 vQ_sw = vI;

        // Select swapped lanes for phases 1 and 3.
        const __m256 swap_mask = _mm256_castsi256_ps(_mm256_loadu_si256((const __m256i*)swap_u[s]));
        __m256 outI = _mm256_blendv_ps(vI, vI_sw, swap_mask);
        __m256 outQ = _mm256_blendv_ps(vQ, vQ_sw, swap_mask);

        // Apply sign flips.
        const __m256 signI = _mm256_castsi256_ps(_mm256_loadu_si256((const __m256i*)signI_u[s]));
        const __m256 signQ = _mm256_castsi256_ps(_mm256_loadu_si256((const __m256i*)signQ_u[s]));
        outI = _mm256_xor_ps(outI, signI);
        outQ = _mm256_xor_ps(outQ, signQ);

        _mm256_storeu_ps(pI + i, outI);
        _mm256_storeu_ps(pQ + i, outQ);
    }

    // Scalar cleanup.
    for (; i < STAGE2_SAMPLES; i++) {
        float re = pI[i];
        float im = pQ[i];
        switch (i & 3) {
            case 0: break;
            case 1: pI[i] = -im; pQ[i] =  re; break;
            case 2: pI[i] = -re; pQ[i] = -im; break;
            case 3: pI[i] =  im; pQ[i] = -re; break;
        }
    }
}

//=============================================================================
// Stage 4: HB2 decimate-by-2 using folded halfband symmetry + AVX2 (SoA)
//
// Halfband properties for HB2 (235 taps):
//   - All odd taps are 0 except the center tap h[117] ~= 0.5
//   - Even taps are symmetric
//
// A causal streaming decimator is:
//   y[m] = sum_{k=0..L-1} h[k] * x[2m - k]
//
// With halfband folding (59 pairs) this becomes:
//   y[m] = 0.5 * x[2m - 117] + Σ_{r=0..58} c[r] * ( x[2m - kL] + x[2m - kR] )
//
// where:
//   kL = 116 - 2r   (even, moving left)
//   kR = 118 + 2r   (even, moving right)
//   c[r] = h[116 - 2r]  (we precompute hb2_c[r])
//
// To make AVX2 efficient, we split the extended input stream into even/odd
// phase arrays:
//   e[m] = x[2m]     (even samples, contiguous)
//   o[m] = x[2m + 1] (odd samples, contiguous)
//
// Then:
//   x[2m - 117] = o[m - 59]
//   x[2m - (116-2r)] = e[m - (58-r)]
//   x[2m - (118+2r)] = e[m - (59+r)]
//
// This yields contiguous loads for SIMD.
//=============================================================================

static inline void hb2_decim2_folded_avx2_soa(
    const float * restrict eI,
    const float * restrict eQ,
    const float * restrict oI,
    const float * restrict oQ,
    float * restrict outI,
    float * restrict outQ,
    int out_len)
{
    const int CENTER_O = 59;
    const int BASE = 117; // phase history offset (117 even + 117 odd history samples)
    // This kernel is only called with out_len == OUTPUT_SAMPLES.
    if (__builtin_expect(out_len != OUTPUT_SAMPLES, 0)) {
        __builtin_unreachable();
    }

    int m = 0;
    for (; m + 7 < out_len; m += 8) {
        __m256 accI = _mm256_mul_ps(_mm256_set1_ps(0.5f),
                                    _mm256_loadu_ps(oI + (BASE + m - CENTER_O)));
        __m256 accQ = _mm256_mul_ps(_mm256_set1_ps(0.5f),
                                    _mm256_loadu_ps(oQ + (BASE + m - CENTER_O)));

        // 59 folded pairs
        for (int r = 0; r < HB2_PAIRS; r++) {
            const float c = hb2_c[r];
            const int a = 58 - r;
            const int b = 59 + r;

            __m256 cvec = _mm256_set1_ps(c);

            __m256 sI = _mm256_add_ps(_mm256_loadu_ps(eI + (BASE + m - a)),
                                      _mm256_loadu_ps(eI + (BASE + m - b)));
            __m256 sQ = _mm256_add_ps(_mm256_loadu_ps(eQ + (BASE + m - a)),
                                      _mm256_loadu_ps(eQ + (BASE + m - b)));

        #ifdef __FMA__
            accI = _mm256_fmadd_ps(cvec, sI, accI);
            accQ = _mm256_fmadd_ps(cvec, sQ, accQ);
        #else
            accI = _mm256_add_ps(accI, _mm256_mul_ps(cvec, sI));
            accQ = _mm256_add_ps(accQ, _mm256_mul_ps(cvec, sQ));
        #endif
        }

        _mm256_storeu_ps(outI + m, accI);
        _mm256_storeu_ps(outQ + m, accQ);
    }

    // Scalar tail
    for (; m < out_len; m++) {
        // Use size_t indices to make it obvious to the compiler that we're in-bounds.
        const size_t mo = (size_t)(BASE + m);
        float acc_i = 0.5f * oI[mo - (size_t)CENTER_O];
        float acc_q = 0.5f * oQ[mo - (size_t)CENTER_O];

        for (int r = 0; r < HB2_PAIRS; r++) {
            const float c = hb2_c[r];
            const size_t a = (size_t)(58 - r);
            const size_t b = (size_t)(59 + r);

            acc_i += c * (eI[mo - a] + eI[mo - b]);
            acc_q += c * (eQ[mo - a] + eQ[mo - b]);
        }

        outI[m] = acc_i;
        outQ[m] = acc_q;
    }
}

static inline void stage4_hb2_stream_folded_avx2(buffer_t *buf) {
    const int L = HB2_LEN;
    const int H = L - 1; // 234

    // Build extended SoA buffers: extI/extQ = [history | current_block]
    memcpy(hb2_extI, hb2_histI, H * sizeof(float));
    memcpy(hb2_extQ, hb2_histQ, H * sizeof(float));
    memcpy(hb2_extI + H, buf->stage2I, STAGE2_SAMPLES * sizeof(float));
    memcpy(hb2_extQ + H, buf->stage2Q, STAGE2_SAMPLES * sizeof(float));

    const int ext_len = H + STAGE2_SAMPLES;

    // Materialize even/odd phase arrays (contiguous) for SIMD.
//
// This is bandwidth-bound. We use AVX2 to deinterleave ext[] into even/odd:
//   e[m] = ext[2m]
//   o[m] = ext[2m+1]
//
// ext_len is even here (234 + 131072 = 131306), so e_len == o_len == ext_len/2.
    {
        const int n8 = ext_len & ~7;      // round down to multiple of 8
        const __m256i idx_even = _mm256_setr_epi32(0,2,4,6, 0,2,4,6);
        const __m256i idx_odd  = _mm256_setr_epi32(1,3,5,7, 1,3,5,7);

        for (int i = 0; i < n8; i += 8) {
            const int m4 = i >> 1; // i/2: each 8 inputs produce 4 evens + 4 odds

            __m256 vI = _mm256_loadu_ps(hb2_extI + i);
            __m256 vQ = _mm256_loadu_ps(hb2_extQ + i);

            __m256 eI8 = _mm256_permutevar8x32_ps(vI, idx_even);
            __m256 oI8 = _mm256_permutevar8x32_ps(vI, idx_odd);
            __m256 eQ8 = _mm256_permutevar8x32_ps(vQ, idx_even);
            __m256 oQ8 = _mm256_permutevar8x32_ps(vQ, idx_odd);

            // low 128 contains the 4 desired lanes: [0,2,4,6] or [1,3,5,7]
            _mm_storeu_ps(hb2_eI + m4, _mm256_castps256_ps128(eI8));
            _mm_storeu_ps(hb2_oI + m4, _mm256_castps256_ps128(oI8));
            _mm_storeu_ps(hb2_eQ + m4, _mm256_castps256_ps128(eQ8));
            _mm_storeu_ps(hb2_oQ + m4, _mm256_castps256_ps128(oQ8));
        }

        // Scalar cleanup for any remaining samples (ext_len mod 8).
        for (int i = n8; i < ext_len; i++) {
            int m1 = i >> 1;
            if ((i & 1) == 0) {
                hb2_eI[m1] = hb2_extI[i];
                hb2_eQ[m1] = hb2_extQ[i];
            } else {
                hb2_oI[m1] = hb2_extI[i];
                hb2_oQ[m1] = hb2_extQ[i];
            }
        }
    }


    // Run AVX2 halfband decimator (writes SoA outI/outQ)
    hb2_decim2_folded_avx2_soa(hb2_eI, hb2_eQ, hb2_oI, hb2_oQ, buf->outI, buf->outQ, OUTPUT_SAMPLES);

    // Interleave output as [I,Q] floats for the writer.
    // This is bandwidth-bound housekeeping; SIMD reduces scalar store overhead.
    {
        float *dst = buf->output;
        const float *srcI = buf->outI;
        const float *srcQ = buf->outQ;

        int i = 0;
        for (; i + 7 < OUTPUT_SAMPLES; i += 8) {
            __m256 vI = _mm256_loadu_ps(srcI + i);
            __m256 vQ = _mm256_loadu_ps(srcQ + i);

            // [I0,Q0,I1,Q1,I2,Q2,I3,Q3]
            __m256 lo = _mm256_unpacklo_ps(vI, vQ);
            // [I4,Q4,I5,Q5,I6,Q6,I7,Q7]
            __m256 hi = _mm256_unpackhi_ps(vI, vQ);

            const size_t o = ((size_t)i) << 1; // 2*i
            _mm256_storeu_ps(dst + o + 0, lo);
            _mm256_storeu_ps(dst + o + 8, hi);
        }

        // Scalar cleanup.
        for (; i < OUTPUT_SAMPLES; i++) {
            const size_t o = ((size_t)i) << 1;
            dst[o]     = srcI[i];
            dst[o + 1] = srcQ[i];
        }
    }

    // Save last H samples of input (stage2 domain) for next block
    memcpy(hb2_histI, hb2_extI + STAGE2_SAMPLES, H * sizeof(float));
    memcpy(hb2_histQ, hb2_extQ + STAGE2_SAMPLES, H * sizeof(float));
}

//=============================================================================
// Processing Thread
//=============================================================================

static void* processing_thread(void *arg) {
    (void)arg;
    LOG_INFO("Processing thread started\n");
    
    // Allocate working buffers for Stage 4 (HB2) using posix_memalign.
// NOTE: aligned_alloc() requires size be a multiple of alignment; these sizes are not.
{
    const int H = HB2_LEN - 1;                // 234
    const int ext_len = H + STAGE2_SAMPLES;   // 131306
    const int e_len = (ext_len + 1) / 2;      // 65653
    const int o_len = ext_len / 2;            // 65653
    void *p = NULL;

    if (posix_memalign(&p, 64, ext_len * sizeof(float)) != 0) p = NULL;
    hb2_extI = (float*)p;

    if (posix_memalign(&p, 64, ext_len * sizeof(float)) != 0) p = NULL;
    hb2_extQ = (float*)p;

    if (posix_memalign(&p, 64, e_len * sizeof(float)) != 0) p = NULL;
    hb2_eI = (float*)p;

    if (posix_memalign(&p, 64, e_len * sizeof(float)) != 0) p = NULL;
    hb2_eQ = (float*)p;

    if (posix_memalign(&p, 64, o_len * sizeof(float)) != 0) p = NULL;
    hb2_oI = (float*)p;

    if (posix_memalign(&p, 64, o_len * sizeof(float)) != 0) p = NULL;
    hb2_oQ = (float*)p;

    if (!hb2_extI || !hb2_extQ || !hb2_eI || !hb2_eQ || !hb2_oI || !hb2_oQ) {
    LOG_ERR("FATAL: failed to allocate Stage4 working buffers (out of memory?)\n");
    free(hb2_extI); hb2_extI = NULL;
    free(hb2_extQ); hb2_extQ = NULL;
    free(hb2_eI);   hb2_eI   = NULL;
    free(hb2_eQ);   hb2_eQ   = NULL;
    free(hb2_oI);   hb2_oI   = NULL;
    free(hb2_oQ);   hb2_oQ   = NULL;
    stop_flag = 1; /* signal main thread to stop */
    return NULL;
}
}
    
    while (!stop_flag) {
        buffer_t *buf = spsc_pop(&filled_queue);
        if (!buf) {
            struct timespec ts = {0, 100000};  // 100 us
            nanosleep(&ts, NULL);
            continue;
        }
        
        unsigned long start = get_time_us();
        
        // Processing pipeline with streaming FIR
        stage1_convert_and_shift(buf);
        stage2_hb1_stream_soa(buf, hb1_histI, hb1_histQ);
        stage3_shift_fs4_soa(buf);
        stage4_hb2_stream_folded_avx2(buf);
        
        unsigned long elapsed = get_time_us() - start;
        double time_ms = elapsed / 1000.0;
        
        stats.total_blocks++;
        stats.avg_time_ms = (stats.avg_time_ms * 0.95) + (time_ms * 0.05);
        if (time_ms < stats.min_time_ms) stats.min_time_ms = time_ms;
        if (time_ms > stats.max_time_ms) stats.max_time_ms = time_ms;
        
        if (!spsc_push(&ready_queue, buf)) {
            // Ready queue full. In --block-on-full mode, wait for space.
            // Otherwise drop the processed block (keeps latency bounded).
            if (g_app.block_on_full) {
                while (!stop_flag && !spsc_push(&ready_queue, buf)) {
                    struct timespec ts = {0, 100000};
                    nanosleep(&ts, NULL);
                }
                if (stop_flag) {
                    spsc_push(&free_queue, buf);
                }
            } else {
                blocks_dropped++;
                spsc_push(&free_queue, buf);
            }
        }
    }
    
    free(hb2_extI);
    free(hb2_extQ);
    free(hb2_eI);
    free(hb2_eQ);
    free(hb2_oI);
    free(hb2_oQ);
    return NULL;
}

//=============================================================================
// Output Thread (with partial write handling)
//=============================================================================

static void* output_thread(void *arg) {
    (void)arg;
    LOG_INFO("Output thread started\n");

    /* If -o PATH was provided, require it to be a FIFO.
       This avoids accidentally writing IQ to a regular file (disk fill) and
       provides clearer errors when the FIFO is missing. */
    if (g_app.output_path) {
        struct stat st;
        if (stat(g_app.output_path, &st) != 0) {
            LOG_ERR("FATAL: output path '%s' not found (create with: mkfifo %s)\n",
                    g_app.output_path, g_app.output_path);
            stop_flag = 1;
            return NULL;
        }
        if (!S_ISFIFO(st.st_mode)) {
            LOG_ERR("FATAL: output path '%s' is not a FIFO\n", g_app.output_path);
            stop_flag = 1;
            return NULL;
        }
    }
    
    while (!stop_flag) {
        buffer_t *buf = spsc_pop(&ready_queue);
        if (!buf) {
            struct timespec ts = {0, 100000};
            nanosleep(&ts, NULL);
            continue;
        }
        
        // Open FIFO if needed. Use O_NONBLOCK for the open() probe so we don't
        // stall the output thread before a reader exists. Once connected, flip
        // the fd to blocking mode so writes are all-or-nothing (no partial-frame
        // corruption on EAGAIN).
        if (g_app.output_fd < 0 && g_app.output_path) {
            int fd = open(g_app.output_path, O_WRONLY | O_NONBLOCK);
            if (fd >= 0) {
                int flags = fcntl(fd, F_GETFL);
                if (flags >= 0) (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                g_app.output_fd = fd;
                LOG_INFO("Connected to FIFO: %s\n", g_app.output_path);
            } else {
                /* ENXIO: FIFO exists but no reader yet (normal). */
                if (errno != ENXIO && errno != EINTR) {
                    LOG_ERR("FIFO open failed: %s\n", strerror(errno));
                }
            }
        }
        
        // Write a whole block (blocking on FIFO backpressure to preserve framing)
        size_t bytes = OUTPUT_SAMPLES * 2 * sizeof(float);
        int fd = (g_app.output_fd >= 0) ? g_app.output_fd : STDOUT_FILENO;
        if (g_app.output_path && g_app.output_fd < 0) {
            /* No reader yet; avoid blocking the whole pipeline.
               We drop this processed block (no output) and try to reconnect.
               In normal use, start the FIFO consumer (e.g. GQRX or mbuffer relay)
               before the producer to avoid drops during startup. */
            blocks_dropped++;
            spsc_push(&free_queue, buf);
            struct timespec ts = {0, 2000000}; // 2 ms
            nanosleep(&ts, NULL);
            continue;
        }
        size_t off = 0;
        
        while (off < bytes) {
            ssize_t w = write(fd, (char*)buf->output + off, bytes - off);
            if (w > 0) {
                off += (size_t)w;
                continue;
            }
            
            if (w < 0 && errno == EINTR) {
                if (stop_flag) break;
                continue;
            }
            if (w < 0 && errno == EPIPE) {
                close(g_app.output_fd);
                g_app.output_fd = -1;
                break;
            }
            if (w < 0) {
                LOG_ERR("write failed: %s\n", strerror(errno));
                stop_flag = 1;
                break;
            }
        }
        
        if (off == bytes) {
            samples_processed += OUTPUT_SAMPLES;
        }
        
        // Return buffer to free pool
        buf->state = 0;
        spsc_push(&free_queue, buf);
    }
    
    if (g_app.output_fd >= 0) {
        close(g_app.output_fd);
        g_app.output_fd = -1;
    }
    return NULL;
}

//=============================================================================
// Signal Handler
//=============================================================================

static void signal_handler(int sig) {
    (void)sig;
    stop_flag = 1;
}



static void usage(FILE *out) {
    fprintf(out,
        "Usage: %s [OPTIONS]\n"
        "High-rate rx888 decimator: stdin int16 real @135e6 -> stdout/FIFO complex float32 IQ @33.75e6.\n\n"
        "Options:\n"
        "  -o PATH           Write IQ to PATH (FIFO recommended); default stdout\n"
        "  -v                Verbose\n"
        "  --block-on-full   Block (no drops) when queues are full (benchmark/backpressure mode)\n"
        "  -h, --help        Show this help\n",
        PROGRAM_NAME);
}

//=============================================================================
// Main
//=============================================================================

int main(int argc, char **argv) {
    
    for (int i = 1; i < argc; i++) {
    if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0)) {
        usage(stdout);
        return 0;
    } else if (strcmp(argv[i], "-o") == 0 && i+1 < argc) {
        g_app.output_path = argv[++i];
    } else if (strcmp(argv[i], "-v") == 0) {
        g_app.verbose = 1;
    } else if (strcmp(argv[i], "--block-on-full") == 0) {
        g_app.block_on_full = 1;
    } else {
        LOG_ERR("Unknown option: %s\n", argv[i]);
        usage(stderr);
        return 2;
    }
} 

    /* Fail fast on an invalid output path. If -o is supplied we require a FIFO.
       (The output thread repeats a similar check to handle runtime changes.) */
    if (g_app.output_path) {
        struct stat st;
        if (stat(g_app.output_path, &st) != 0) {
            LOG_ERR("FATAL: output path '%s' not found (create with: mkfifo %s)\n",
                    g_app.output_path, g_app.output_path);
            return 2;
        }
        if (!S_ISFIFO(st.st_mode)) {
            LOG_ERR("FATAL: output path '%s' is not a FIFO\n", g_app.output_path);
            return 2;
        }
    }

    /* Signal handling: use sigaction() for reliability (avoid SA_RESTART). */
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    /* Broken pipe is expected if the FIFO consumer exits. */
    signal(SIGPIPE, SIG_IGN);
}    // Initialize folded coefficients for HB2
    init_hb2_coeffs();
    
    // Allocate history buffers
    hb1_histI = calloc(HB1_LEN - 1, sizeof(float));
    hb1_histQ = calloc(HB1_LEN - 1, sizeof(float));
    hb2_histI = calloc(HB2_LEN - 1, sizeof(float));
    hb2_histQ = calloc(HB2_LEN - 1, sizeof(float));
    
    if (!hb1_histI || !hb1_histQ || !hb2_histI || !hb2_histQ) {
        LOG_ERR("FATAL: failed to allocate history buffers\n");
        free(hb1_histI); free(hb1_histQ); free(hb2_histI); free(hb2_histQ);
        return 1;
    }
    
    // Initialize queues
    spsc_init(&free_queue);
    spsc_init(&filled_queue);
    spsc_init(&ready_queue);
    
    // Push all buffers to free queue
    for (int i = 0; i < NUM_BUFFERS; i++) {
        buffer_pool[i].state = 0;
        spsc_push(&free_queue, &buffer_pool[i]);
    }
    
    LOG_INFO("=== rx888-decimate ===\n");
    LOG_INFO("Architecture: buffer pool + streaming FIR\n");
    LOG_INFO("HB2: folded halfband (59 pairs)\n");
    LOG_INFO("Output: %s\n\n", g_app.output_path ? g_app.output_path : "stdout");
    
    // Start worker threads
pthread_t proc_thread, out_thread;
int rc;

rc = pthread_create(&proc_thread, NULL, processing_thread, NULL);
if (rc != 0) {
    LOG_ERR("FATAL: pthread_create(processing_thread) failed: %s\n", strerror(rc));
    free(hb1_histI); free(hb1_histQ); free(hb2_histI); free(hb2_histQ);
    return 1;
}

rc = pthread_create(&out_thread, NULL, output_thread, NULL);
if (rc != 0) {
    LOG_ERR("FATAL: pthread_create(output_thread) failed: %s\n", strerror(rc));
    stop_flag = 1;
    pthread_join(proc_thread, NULL);
    free(hb1_histI); free(hb1_histQ); free(hb2_histI); free(hb2_histQ);
    return 1;
}
    
    // Input loop: pop from free, read, push to filled
    while (!stop_flag) {
        buffer_t *buf = spsc_pop(&free_queue);
        if (!buf) {
            // No free buffers available.
            // In --block-on-full mode (benchmarking / backpressurable sources), wait.
            // Otherwise count this as a dropped block (what would happen with a non-backpressurable SDR source).
            if (!g_app.block_on_full) {
                blocks_dropped++;
            }
            struct timespec ts = {0, 100000};
            nanosleep(&ts, NULL);
            continue;
        }
        
        ssize_t n = read_full(STDIN_FILENO, buf->input, INPUT_SAMPLES * sizeof(int16_t));
        
        if (n == (ssize_t)(INPUT_SAMPLES * sizeof(int16_t))) {
            buf->state = 1;
            if (!spsc_push(&filled_queue, buf)) {
                // Filled queue full (should be rare with proper sizing).
                if (g_app.block_on_full) {
                    while (!stop_flag && !spsc_push(&filled_queue, buf)) {
                        struct timespec ts = {0, 100000};
                        nanosleep(&ts, NULL);
                    }
                    if (stop_flag) {
                        spsc_push(&free_queue, buf);
                    }
                } else {
                    blocks_dropped++;
                    spsc_push(&free_queue, buf);  // Give back if can't queue
                }
            }
        } else if (n == 0) {
            // EOF
            spsc_push(&free_queue, buf);
            break;
        } else if (n < 0) {
            perror("read");
            spsc_push(&free_queue, buf);
            break;
        } else {
            // Partial block: treat as end-of-stream (e.g., producer exited).
            LOG_ERR("Input stream ended mid-block (%zd bytes).\n", n);
            spsc_push(&free_queue, buf);
            break;
        }
    }
    
    stop_flag = 1;
    pthread_join(proc_thread, NULL);
    pthread_join(out_thread, NULL);
    
    if (g_app.verbose) {
    fprintf(stderr, "\n=== Statistics ===\n");
    fprintf(stderr, "Blocks processed: %lu\n", stats.total_blocks);
    
{
    const unsigned long total = stats.total_blocks + blocks_dropped;
    if (total > 0) {
        fprintf(stderr, "Blocks dropped:   %lu (%.2f%%)\n", blocks_dropped,
                100.0 * (double)blocks_dropped / (double)total);
    } else {
        fprintf(stderr, "Blocks dropped:   %lu (no data processed)\n", blocks_dropped);
    }
}
    fprintf(stderr, "Time/block: avg %.2f ms (min %.2f, max %.2f)\n",
            stats.avg_time_ms, stats.min_time_ms, stats.max_time_ms);
    fprintf(stderr, "Budget @ 135 MSPS: 1.94 ms/block\n");
    if (stats.avg_time_ms > 0) {
        fprintf(stderr, "Headroom: %.1f%%\n",
                100.0 * (1.94 - stats.avg_time_ms) / 1.94);
    }
    fprintf(stderr, "Samples output: %lu\n", samples_processed);
}

    
    if (g_app.output_fd >= 0) close(g_app.output_fd);
    free(hb1_histI);
    free(hb1_histQ);
    free(hb2_histI);
    free(hb2_histQ);
    
    return 0;
}
