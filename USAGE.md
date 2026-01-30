# rx888_dsp Usage

## Synopsis

```sh
rx888_dsp [OPTIONS]
```

`rx888_dsp` is designed to be used as part of a Unix pipeline. It reads raw
high-rate SDR samples from standard input and produces decimated complex IQ
samples on standard output or a FIFO.

---

## Options

- `-o, --output PATH`  
  Write output IQ stream to `PATH`.  
  `PATH` **must be a FIFO**. If omitted, output is written to stdout.

- `-v, --verbose`  
  Enable verbose logging and performance statistics.

- `--block-on-full`  
  Apply backpressure when downstream is slow instead of dropping blocks.

- `-h, --help`  
  Show detailed help and exit.

- `-V, --version`  
  Show program version and exit.

---

## Input / Output Format

### Input
- Source: `stdin`
- Format: signed 16-bit **real** samples
- Sample rate: **135 MS/s**
- Byte order: native little-endian

### Output
- Destination: `stdout` or FIFO
- Format: **complex float32 IQ**, interleaved (`I0,Q0,I1,Q1,...`)
- Sample rate: **33.75 MS/s**

Binary I/O to a terminal is refused. Input and output must be connected
via pipes, redirection, or FIFOs.

---

## Signals

- `SIGINT`, `SIGTERM`, `SIGHUP`  
  Trigger a clean shutdown.

- `SIGUSR1`  
  Print performance statistics to stderr.  
  Requires `-v` to be enabled.

- `SIGPIPE`  
  Ignored; handled internally to allow downstream reconnects.

---

## Exit Status

- `0` – Success  
- `1` – Runtime error (I/O, memory allocation, thread failure)  
- `2` – Usage error (invalid options, TTY detected)

---

## Examples

### Pipe from a file into a FIFO for GQRX

```sh
mkfifo /tmp/iq.fifo
cat capture.i16 | rx888_dsp --block-on-full -v -o /tmp/iq.fifo
```

Configure GQRX to read from `/tmp/iq.fifo` using:
- Sample format: complex float32 IQ (interleaved)
- Sample rate: 33.75e6

---

## Notes

- This program is a **Unix filter**, not an interactive application.
- Output via `-o` is restricted to FIFOs to prevent accidental disk writes.
  For file output, use shell redirection instead:
  ```sh
  rx888_dsp > output.iq
  ```
- The program assumes an AVX2 + FMA capable CPU and will not run otherwise.
