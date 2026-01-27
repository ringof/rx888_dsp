# rx888_dsp Usage Guide

This project turns **16‑bit real samples at 135 MS/s** (RX888 family) into **complex float32 IQ at 33.75 MS/s** suitable for SDR software such as **GQRX**.

The typical “happy” chain is:

```
RX888 source (135e6 real int16)  ──>  rx888_dsp  ──>  (optional mbuffer)  ──>  FIFO  ──>  GQRX
```

Why the optional buffer: GQRX can be sensitive to FIFO read jitter at ~257 MiB/s. A userspace buffer (mbuffer) smooths bursts and keeps GQRX stable.

---

## 1) Build

### Requirements
- Ubuntu Linux
- gcc with AVX2/FMA support
- pthreads, math

### Compile
```
gcc -O3 -march=native -mavx2 -mfma -std=c11 -o rx888_dsp rx888_dsp.c -lm -lpthread -Wall -Wextra
```

Tip: `-march=native` usually includes `-mavx2 -mfma` automatically on AVX2 CPUs, but keeping them explicit is fine.

---

## 2) What rx888_dsp does

**DSP pipeline (4 stages):**
1. **Stage 1:** Convert `int16` real samples to float and apply a `-Fs/4` complex shift to create an analytic/complex stream.
2. **Stage 2:** **Halfband decimate by 2** (HB1) with streaming history.
3. **Stage 3:** Apply a `+Fs/4` shift (SIMD-accelerated).
4. **Stage 4:** **Halfband decimate by 2** (HB2 folded polyphase, AVX2 kernel), then interleave IQ for output (SIMD-accelerated).

**Input:** 135,000,000 samples/sec, real int16  
**Output:** 33,750,000 samples/sec, complex float32 IQ interleaved (I,Q,I,Q,...)  
**Output byte rate:** `33.75e6 * 2 * 4 = 270,000,000 B/s` ≈ **257 MiB/s**

---

## 3) Basic CLI

```
rx888_dsp [-v] [--block-on-full] [-o /path/to/fifo] [-h]
```

- `-o PATH` : output FIFO path (recommended for GQRX). Must exist and must be a FIFO.
- `-v` : verbose logs to stderr
- `--block-on-full` : block instead of dropping when pipeline is congested (best for benchmarking / tests)
- `-h` : help/usage

Notes:
- When `-o` is used, the program validates that PATH exists and is a FIFO.
- FIFO write fd is switched to **blocking mode** after connect to preserve frame integrity.

---

## 4) FIFO + GQRX setup (single FIFO)

### Create a FIFO for GQRX
```
mkfifo /tmp/rx888_iq.fifo
```

### Configure GQRX
In GQRX, set:
- Input: **File / FIFO**: `/tmp/rx888_iq.fifo`
- Sample rate: **33.75e6**
- Format: **Complex float32 IQ (interleaved)**

### Run
From a real-time source:
```
rx888_stream_driver | ./rx888_dsp -o /tmp/rx888_iq.fifo -v
```

From a test file (one pass):
```
cat your_135Msps_real16.raw | ./rx888_dsp -o /tmp/rx888_iq.fifo -v
```

---

## 5) Recommended: two-FIFO + mbuffer relay

This smooths jitter for GQRX:

```
RX888 -> rx888_dsp -> FIFO_A -> mbuffer -> FIFO_B -> GQRX
```

### Create FIFOs
```
mkfifo /tmp/iq_a.fifo /tmp/iq_b.fifo
```

### Start GQRX reading FIFO_B
Configure GQRX to read:
- `/tmp/iq_b.fifo`
- Sample rate 33.75e6
- complex float32 interleaved

### Start mbuffer relay (your mbuffer supports -m and -P)
Recommended settings:
- `-m 512M` : 512 MiB buffer
- `-P 20` : start writing after buffer >20% full (prefill helps)
- `-q` : quiet status

Command:
```
mbuffer -m 512M -P 20 -q -i /tmp/iq_a.fifo -o /tmp/iq_b.fifo
```

(For debugging, omit `-q` and add `-v 2`.)

### Run rx888_dsp writing FIFO_A
```
rx888_stream_driver | ./rx888_dsp -o /tmp/iq_a.fifo -v
```

---

## 6) File-loop playback into GQRX (with the provided script)

### Why trimming happens
Block processing expects fixed-size blocks. For looped files, trimming the file to a whole number of blocks avoids repeated “EOF mid-block” conditions and makes file looping behave more like a live stream.

### Script usage
Assuming the script is `rx888_dsp_to_gqrx.sh` and your test file is `may17_blacksbeach_run3`:

```
./rx888_dsp_to_gqrx.sh may17_blacksbeach_run3
```

Typical behavior:
- Creates/uses the FIFO(s)
- Trims the input file to a whole number of blocks for clean looping
- Prompts you to open the FIFO in GQRX, then begins playback
- Ctrl-C stops cleanly

### With mbuffer relay in the chain
If you want to use the two-FIFO approach for stability:
- Change the script’s FIFO output to `/tmp/iq_a.fifo`
- Run mbuffer as shown above
- Point GQRX to `/tmp/iq_b.fifo`

---

## 7) Benchmarking (no GQRX)

### Pure compute benchmark
This blocks on backpressure and measures DSP time/block:
```
cat /dev/zero | ./rx888_dsp --block-on-full > /dev/null
```

### Rate-shaped input test (optional)
If you want to emulate 270,000,000 B/s input for stress tests:
```
pv -L 270000000 /dev/zero | ./rx888_dsp --block-on-full > /dev/null
```

---

## 8) Operational tips

- **CPU governor**: set performance governor when running for long periods to avoid frequency droop.
- **Pin threads** (optional): pin DSP and mbuffer to dedicated cores on busy systems.
- **mbuffer sizing**: start with 512M; increase if you still see GQRX jitter.
- **FIFO integrity**: this build uses blocking FIFO writes to avoid partial-frame corruption.

---

## 9) Troubleshooting quick hits

### GQRX stutters progressively
Use the mbuffer relay with `-P` prefill:
```
mbuffer -m 512M -P 20 -q -i /tmp/iq_a.fifo -o /tmp/iq_b.fifo
```

### “FIFO does not exist”
Create it first:
```
mkfifo /tmp/rx888_iq.fifo
```

### “Output path is not a FIFO”
You pointed `-o` to a regular file. Use `mkfifo` instead.

### DSP falls behind
- ensure AC power / performance governor
- ensure no CPU thermal throttling
- confirm you’re not running on battery power
