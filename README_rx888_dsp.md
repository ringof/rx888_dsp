# rx888_dsp — High-Throughput DSP Front-End for RX888-Class SDRs

## 1. Project Overview

`rx888_dsp` is a real-time digital signal processing (DSP) front-end designed to sit between
a high-rate SDR sample source (RX888 family, USB 3.0) and a downstream SDR receiver
application (e.g. GQRX), using standard Unix pipes and FIFOs.

Its purpose is to transform:

- 135 MS/s, 16-bit real-valued samples

into:

- 33.75 MS/s, complex float32 IQ samples (interleaved)

while never falling behind real time, even under moderate system load.

The program is explicitly designed to:
- run continuously,
- process fixed-size blocks deterministically,
- preserve frame integrity for FIFO consumers,
- and exploit modern x86 SIMD (AVX2) efficiently.

---

## 2. Motivation

Modern wideband SDR front ends (RX888, RX888 MK2, etc.) can deliver extremely high raw
sample rates, but many SDR applications:
- expect complex IQ rather than real samples,
- cannot ingest 135 MS/s directly,
- are sensitive to jitter at very high FIFO data rates.

`rx888_dsp` solves this by acting as a specialized DSP reducer:
it performs only the minimum required front-end DSP (analytic signal creation and
decimation) so that downstream receivers can operate at a manageable rate without
sacrificing signal integrity.

---

## 3. High-Level Data Flow

RX888 source (135e6 real int16)
    -> rx888_dsp
        -> FIFO (optional mbuffer smoothing)
            -> SDR receiver (e.g. GQRX)

---

## 4. DSP Pipeline Summary

### Stage 1 — Real to Complex + -Fs/4 Shift
- Converts int16 real samples to float32
- Applies a -Fs/4 frequency shift to generate an analytic signal
- Avoids a Hilbert transform by exploiting the Fs/4 identity

### Stage 2 — Halfband Decimation by 2 (HB1)
- Reduces sample rate from 135 MS/s to 67.5 MS/s
- Uses a halfband FIR filter with streaming history
- Halfband structure minimizes arithmetic cost

### Stage 3 — +Fs/4 Shift
- Complements the earlier -Fs/4 shift
- Re-centers the spectrum correctly after decimation
- Implemented using a deterministic 4-sample periodic pattern

### Stage 4 — Folded Halfband Decimation by 2 (HB2)
- Reduces sample rate from 67.5 MS/s to 33.75 MS/s
- Uses a folded polyphase halfband FIR
- Inner loop is AVX2-accelerated
- Produces final fixed-size output blocks

---

## 5. Block-Based Design

Fixed block sizes are fundamental:

- Input block: 262,144 samples
- Output block: 65,536 complex samples

At 135 MS/s this gives a processing budget of approximately 1.94 ms per block.
Fixed-size blocks simplify timing analysis, prevent partial-frame FIFO writes,
and make backpressure behavior deterministic.

---

## 6. Performance Philosophy

Key assumptions:
- Memory movement is more expensive than arithmetic
- Cache misses are more damaging than extra math
- Predictability is more important than peak throughput

Design consequences:
- AVX2 is used where it clearly improves throughput
- All buffers are preallocated
- No dynamic allocation occurs in hot paths
- Partial FIFO writes are strictly avoided

---

## 7. Threading and Buffering Model

Two main threads are used:
- A processing thread that runs the DSP pipeline
- An output thread that writes complete blocks to the FIFO

A fixed pool of reusable buffers provides elasticity when downstream consumers stall.
The program can either block or drop blocks under backpressure, depending on configuration.

---

## 8. FIFO and Stream Integrity

The output path must be a FIFO and is validated at startup.
FIFO writes are performed in blocking mode to guarantee that only whole frames are written.
Consumer disconnects are handled gracefully without crashing the DSP pipeline.

---

## 9. SIMD Strategy

AVX2 is used selectively:
- FIR inner loops
- Large, contiguous data operations

Scalar code is retained where control flow or memory movement dominates.
AVX-512 is intentionally not used by default due to frequency throttling and
memory-bandwidth limits on consumer CPUs.

---

## 10. What This Is Not

`rx888_dsp` is not:
- a general-purpose DSP framework
- dynamically reconfigurable for arbitrary rates
- tolerant of partial-frame output
- a replacement for a full SDR receiver

It is a purpose-built, high-throughput DSP front-end reducer.

---

## 11. Mental Model Summary

rx888_dsp is a deterministic, block-based, AVX2-accelerated DSP pipeline whose primary
goal is to never fall behind real time while delivering clean, FIFO-safe complex IQ
to downstream SDR software.
