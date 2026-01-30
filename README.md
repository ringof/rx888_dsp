# rx888_dsp

## Overview

`rx888_dsp` is a high-performance, real-time DSP decimation pipeline designed for
the RX888 family of SDRs.

It converts a **135 MS/s real int16 input stream** into a
**33.75 MS/s complex float32 IQ stream**, suitable for downstream receivers
such as GQRX.

The program is designed as a **Unix filter**:

stdin → DSP pipeline → stdout or FIFO

It is optimized for modern x86-64 CPUs with **AVX2 + FMA** support and is intended
to run continuously in real time with processing headroom.

---

## Project Status

**Status:** Stable  
**Scope:** Single-pipeline, real-time DSP filter  
**Target platforms:** Linux x86-64, AVX2-capable CPUs  

The current implementation represents a mature, production-ready design.
Further optimization work is deferred until a concrete performance or
functional requirement emerges.

---

## Design Goals

- Sustain real-time processing at 135 MS/s with margin
- Predictable latency and bounded buffering
- Robust operation in Unix pipelines and FIFO chains
- Minimal memory traffic via stage fusion and SIMD
- Clear separation between DSP logic and I/O

---

## Non-Goals

- Multi-device or multi-pipeline operation
- GPU or accelerator offload
- General-purpose DSP framework
- Automatic recording or storage management

---

## Build

```sh
gcc -O3 -mavx2 -mfma -std=c11 \
    -o rx888_dsp rx888_dsp.c \
    -lm -lpthread

