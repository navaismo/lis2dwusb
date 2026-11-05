# Repository Guidelines

## Scope and Working Agreements

Optimize LIS2DW acquisition for reliable input-shaping measurements with OctoPrint. Work on `plugin_optimization`. Do not commit, push, flash firmware, install system tools or interact with physical devices without explicit approval. Preserve unrelated changes. Write all comments, README content and documents in English; communicate with the user in Spanish.

## Project Structure

`src_RP2040/main.cpp` contains the RP2040 Arduino/mbed firmware and SPI acquisition. `Linux_Wrapper/lis2dwusb.c` contains the Linux USB serial reader and CSV exporter. Build configuration is in `platformio.ini`; wrapper build/install scripts are under `Linux_Wrapper/`. `FW_Binaries/` contains existing release artifacts; do not replace them during development. Tests are in `tests/`; shared framing is in `include/` and the timing contract in `docs/protocol.md`.

## Baseline and Confirmed Findings

The reviewed baseline is `02d01ba0d2b7161cbe82fe76e1d5b4f62c9b779b`, matching the published repository when checked. The wrapper timestamps Linux receipt with `gettimeofday()` and exports only three decimal places. At 1600 Hz the nominal interval is 0.625 ms, so timestamps repeat. RP2040 output contains XYZ without acquisition timestamps or sequence numbers.

A temporary pseudo-terminal simulation reproduced duplicate timestamps and a blocked Q cancellation while serial input was silent. These are software reproductions, not hardware measurements. Firmware currently disables BDU, does not use FIFO, and maps intermediate requested frequencies to discrete ODRs. Consult the official LIS2DW12 datasheet before changing registers.

## Implementation Priorities

Introduce a versioned acquisition protocol with precise device-side timing, sample sequencing, actual sampling-rate acknowledgement and explicit loss/overflow reporting. Decouple acquisition from USB transmission using bounded buffering; review FIFO, BDU and data-ready handling. Preserve sensor precision and document units. Add bounded initialization and stop behavior.

Update the wrapper to consume that protocol, preserve acquisition timing, validate supported rates and use interruptible reads with timeout/disconnect handling. Do not fabricate uniform timestamps to conceal dropped samples. Keep malformed or incomplete captures from reporting success.

## Plugin Integration and Validation

The matching plugin is `/home/shaka/Projects/Python/OctoPrint-Pinput_Shaping`, also on `plugin_optimization`, with uncommitted changes. Read its `docs/firmware-contract.md` and `capture.py` before protocol changes. Its capture contract uses `-p PORT -f 1600 -s FILE`, a `Press Q to stop` readiness prompt, CSV columns `time,x,y,z`, and a successful exit after Q.

Compile firmware with `pio run` without uploading. Build the C wrapper with compiler warnings enabled into a temporary directory. Test with synthetic samples and pseudo-terminals for timing, missing samples, startup, cancellation, disconnection and parsing errors. Protocol 2 now has wrapper, actual-source firmware-harness and optional plugin integration tests; none establish physical acquisition reliability. Coordinate plugin compatibility changes and installer pinning before claiming end-to-end support.
