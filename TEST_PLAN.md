# rx888_dsp Test Plan

This plan validates **performance**, **startup/shutdown robustness**, and **edge failure behavior** of `rx888_dsp` in three environments:
1) `/dev/zero` synthetic source  
2) a real recorded 135 MS/s real-int16 test file  
3) the live RX888 driver stream

The plan assumes:
- Input format: **int16 real** at **135,000,000 samples/sec**
- Output format: **float32 complex interleaved IQ** at **33,750,000 samples/sec**
- Output sink: FIFO consumed by GQRX (optionally via mbuffer relay)

---

## 0) Definitions / constants

- DSP input block size: `INPUT_SAMPLES * sizeof(int16)`  
  `262144 * 2 = 524,288 bytes`
- Output per block: `OUTPUT_SAMPLES * 2 * sizeof(float)`  
  `65536 * 2 * 4 = 524,288 bytes`
- Budget per block at 135 MS/s:  
  `262144 / 135e6 ≈ 1.94 ms`

Target:
- Sustained average processing time < 1.94 ms/block (ideally with headroom)
- No crashes, clean signal handling, clean FIFO disconnect/reconnect behavior

---

## A) Performance validation

### A1) Zero-source compute benchmark
Goal: confirm basic throughput headroom independent of real I/O variability.

Command:
```
cat /dev/zero | ./rx888_dsp --block-on-full > /dev/null
```

Record:
- avg/min/max time per block
- headroom %
- CPU frequency behavior (optional): `watch -n1 "grep -m1 MHz /proc/cpuinfo"`

Pass criteria:
- avg time/block < 1.94 ms with non-trivial headroom
- clean Ctrl-C exit (no segfault)
- no runaway logs

---

### A1b) Single-block framing sanity check
Goal: ensure exactly one input block yields exactly one output block with correct byte size.

Command:
```
head -c 524288 /dev/zero | ./rx888_dsp --block-on-full > /tmp/rx888_iq.out
ls -l /tmp/rx888_iq.out
```

Pass criteria:
- output file size is exactly 524,288 bytes (one block)
- process exits cleanly after EOF

---

### A2) Rate-shaped input stress test
Goal: simulate “real rate” pacing using pv shaping.

Command:
```
pv -L 270000000 /dev/zero | ./rx888_dsp --block-on-full > /dev/null
```

Pass criteria:
- avg time/block stays under budget
- no increasing latency trend over multi-minute run

---

### A3) Real test file (one-shot)
Goal: confirm correctness and stability on real recorded samples.

Prepare:
- Ensure file is readable and in expected format.
- Optionally verify block-alignment and trim (for loop tests).

Command (one pass):
```
cat your_real16_135Msps.raw | ./rx888_dsp --block-on-full > /dev/null
```

Pass criteria:
- no errors
- time/block under budget
- clean EOF handling

---

### A4) Real test file loop into FIFO (GQRX)
Goal: sustained streaming with the full chain.

Setup:
- Create FIFO: `mkfifo /tmp/rx888_iq.fifo`
- Configure GQRX:
  - input file/FIFO: `/tmp/rx888_iq.fifo`
  - sample rate: 33.75e6
  - complex float32 interleaved

Command (loop):
- Use the script:
```
./rx888_dsp_to_gqrx.sh your_real16_file
```

Observe:
- Does GQRX progressively stutter?
- CPU load (GQRX %, DSP %)
- DSP stats (drops, avg time)

Pass criteria:
- stable GQRX playback for 10–30 minutes
- no DSP drops in normal mode
- clean shutdown

---

### A5) Live RX888 stream (end-to-end)
Goal: validate sustained throughput with real USB/driver variability.

Command:
```
rx888_stream_driver | ./rx888_dsp -o /tmp/rx888_iq.fifo -v
```

Pass criteria:
- stable display/audio for 30+ minutes
- no persistent drop alerts
- clean recovery when GQRX is restarted (see robustness tests)

---

## B) Clean startup/shutdown behavior

### B1) Ctrl-C on each component
Goal: clean termination without crashes and with bounded logs.

Cases:
1) Ctrl-C DSP while source continues
2) Ctrl-C source while DSP continues
3) Ctrl-C relay (mbuffer) while DSP + GQRX continue
4) Ctrl-C whole pipeline (script)

Expected:
- No segfaults
- DSP prints final stats once
- No “stopping…” storms from the script (trap guard)

---

### B2) Kill signals
Goal: handle typical operator/process-manager actions.

Commands:
```
# run pipeline in terminal A:
rx888_stream_driver | ./rx888_dsp -o /tmp/iq_a.fifo -v

# terminal B:
pkill -TERM rx888_dsp
pkill -INT  rx888_dsp
pkill -HUP  rx888_dsp
```

Expected:
- DSP exits cleanly on TERM/INT/HUP
- No deadlocks on pthread_join
- FIFO fd closes cleanly

---

### B3) Stop / restart GQRX while streaming
Goal: validate output thread behavior on consumer disconnect.

Procedure:
1) Start DSP (to FIFO) + GQRX reading it
2) Close GQRX (or switch input)
3) Restart GQRX and reopen FIFO

Expected:
- DSP does not crash
- Output thread detects EPIPE/close and reconnects when reader returns
- Depending on your configured behavior, some blocks may be dropped while no reader exists (acceptable)

---

## C) Edge failure conditions

### C1) Upstream source failure: sudden EOF
Goal: graceful handling when producer exits.

Command:
```
head -c 1000000 /dev/urandom | ./rx888_dsp -v > /dev/null
```

Expected:
- DSP reports “input ended mid-block” once
- exits cleanly with non-zero status (if implemented)
- no deadlock

---

### C2) Upstream stall (producer stops writing)
Goal: observe DSP behavior when stdin blocks.

Procedure:
- Run rx888 driver, then SIGSTOP it:
```
rx888_stream_driver | ./rx888_dsp -v > /dev/null &
PID=$!
# Identify driver PID and stop it:
kill -STOP <driver_pid>
sleep 10
kill -CONT <driver_pid>
```

Expected:
- DSP blocks waiting for input (normal)
- resumes when input resumes
- no runaway CPU spin

---

### C3) FIFO path misconfiguration
Goal: fail fast and safely.

Cases:
1) Output path does not exist:
```
./rx888_dsp -o /tmp/does_not_exist.fifo
```
Expected: error + hint to run `mkfifo`, exit.

2) Output path exists but is a regular file:
```
echo test > /tmp/not_fifo
./rx888_dsp -o /tmp/not_fifo
```
Expected: error, exit (avoid writing huge IQ to disk).

---

### C4) Downstream stall (GQRX slow / stops reading)
Goal: ensure stream integrity (no partial-frame corruption) and stable behavior.

**Recommended test uses mbuffer relay:**
- DSP -> FIFO_A -> mbuffer -> FIFO_B -> dummy slow reader OR GQRX

Create FIFOs:
```
mkfifo /tmp/iq_a.fifo /tmp/iq_b.fifo
```

Run relay (with prefill):
```
mbuffer -m 512M -P 20 -i /tmp/iq_a.fifo -o /tmp/iq_b.fifo
```

Simulate slow consumer:
```
pv -L 10M /tmp/iq_b.fifo > /dev/null
```

Feed DSP:
```
pv -L 270000000 /dev/zero | ./rx888_dsp -o /tmp/iq_a.fifo -v
```

Expected:
- mbuffer buffer fill grows (since consumer is slow)
- DSP either blocks (if your pipeline/backpressure blocks) or drops blocks in a controlled way depending on options
- **no partial-frame writes** to the consumer FIFO

---

### C5) Output FIFO disappears / is recreated
Goal: robustness when FIFO file is removed or replaced.

Procedure:
1) Start DSP writing `-o /tmp/rx888_iq.fifo`
2) While running:
```
rm /tmp/rx888_iq.fifo
mkfifo /tmp/rx888_iq.fifo
```

Expected:
- DSP reports an error and/or reconnect behavior depending on implementation
- no crash

(If you don’t want to support this case, document it as “unsupported”; most pipelines assume FIFO path stays stable.)

---

## D) Instrumentation / diagnostics

### D1) CPU + frequency + throttling
- CPU usage: `top` / `htop`
- frequency: `watch -n1 "grep -m1 MHz /proc/cpuinfo"`
- thermal throttling indicators (Intel): `dmesg | grep -i therm`

### D2) Syscall-level sanity (optional)
Trace only open/close/write/read to reduce output:
```
strace -ff -tt -e trace=read,write,open,openat,close,fcntl,poll,ppoll -p <pid>
```

### D3) FIFO throughput check
Expected output rate ≈ 270,000,000 B/s (257 MiB/s). Use:
```
pv /tmp/iq_b.fifo > /dev/null
```

---

## E) Acceptance criteria summary

**Performance:**
- avg time/block < 1.94 ms with headroom on target hardware

**Stability:**
- no segfaults under Ctrl-C/TERM/HUP
- predictable behavior when GQRX disconnects/reconnects

**Correctness/stream integrity:**
- no partial-frame corruption to FIFO consumers
- clean handling of EOF mid-block and misconfigured output paths

---

## F) Recommended long-duration test
Run for 6–24 hours with:
- live RX888 stream (preferred), or
- rate-shaped /dev/zero if hardware is unavailable

Command (example):
```
timeout 86400 rx888_stream_driver | ./rx888_dsp -o /tmp/iq_a.fifo -v > /dev/null 2>&1
```

Monitor:
- CPU frequency stability
- memory usage (RSS stable)
- GQRX responsiveness (if in the loop)
