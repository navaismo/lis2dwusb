# LIS2DWUSB

RP2040 firmware and a Linux USB wrapper for the BIGTREETECH S2DW (LIS2DW12)
accelerometer, intended for OctoPrint input-shaping measurements.

The `plugin_optimization` branch introduces **protocol 2** and remains under
review. Firmware and wrapper must be upgraded together. Existing binaries in
`FW_Binaries/` are legacy releases and have not been replaced. Software tests
and a successful build do not establish physical sensor performance.

## Changes

- Device-side microsecond timestamps, sample sequence numbers and CRC-16 frames.
- Explicit protocol, session, sampling-rate and scale acknowledgement.
- FIFO monitoring, BDU and bounded polling uncertainty; ambiguous timing,
  clipping or overflow invalidates the capture instead of concealing loss.
- A bounded sample queue and nonblocking USB transmission.
- Interruptible Linux reads, startup/data/stop deadlines and verified stop counts.
- CSV output in g with preserved sensor precision and a validity metadata sidecar.
- Exact supported rates: **200, 400, 800 and 1600 Hz**. Other rates are rejected.

See [the acquisition protocol](docs/protocol.md) for the timing model and limits.

## Build

Requirements: PlatformIO, a C compiler, Make, Python and pytest for offline tests.
The firmware targets the existing SPI wiring: CS=GPIO9, SCK=GPIO10,
MOSI=GPIO11 and MISO=GPIO8. It does not require an additional interrupt wire.

```bash
pio run
make -C Linux_Wrapper
python3 -m pytest -q
```

`platformio.ini` pins the platform, mbed framework and ARM toolchain. The build
creates `.pio/build/btt_lis2dw/firmware.uf2`; `pio run` does not upload it.
Review and physically validate the new firmware before installation on a printer.
The wrapper build needs no pigpio library and runs without sudo.
Make always rebuilds the wrapper with the selected compiler, so a copied binary
from another CPU architecture cannot bypass compilation based on timestamps.

## Installation

Copy the **whole source tree**, including `include/`; the wrapper uses the shared
protocol header. On Raspberry Pi OS / Debian, select the actual service account:

```bash
sudo OCTOPRINT_USER=pi bash Linux_Wrapper/install.sh
```

The installer builds and installs `/usr/local/bin/lis2dwusb` and adds that account
to `dialout`. Restart the OctoPrint service to refresh group membership. The
installer does not flash firmware, connect to a sensor or create sudoers rules.
Review existing sudoers entries separately if an old installer created them.

If running the wrapper reports `cannot execute binary file: Exec format error`,
check the host architecture and executable format on the affected machine:

```bash
uname -m
file /usr/local/bin/lis2dwusb
```

Rebuild from the complete source tree **on that machine** with its native C
compiler, then replace only the installed wrapper:

```bash
make -B -C Linux_Wrapper
Linux_Wrapper/lis2dwusb -V
sudo make -B -C Linux_Wrapper install
/usr/local/bin/lis2dwusb -V
```

The version checks do not open a serial device. An x86-64 executable cannot run
natively on an ARM Raspberry Pi; changing permissions does not fix its format.

Firmware flashing is a separate operator step after review. Use the newly built
protocol 2 UF2, not the existing 0.2.2 release files. The wrapper rejects legacy
firmware; it does not silently timestamp old XYZ streams on the host.

## Capture

Use the sensor's stable serial path, distinct from the printer serial port:

```bash
lis2dwusb -p /dev/serial/by-id/YOUR_SENSOR -f 1600 -s capture.csv
lis2dwusb -p /dev/ttyACM0 -f 800 -d 5 -s short-test.csv
```

Press **Q**, Ctrl+C or send SIGTERM to request a verified stop. `-d` sets a host
capture duration; `-t` sets the timeout in seconds (default 3). `-h` displays help;
`-V` prints the wrapper/protocol version. Without `-s`, CSV is written to stdout.

CSV columns remain `time,x,y,z`. Times are device-relative seconds with six
decimals; XYZ values are in g, derived from raw counts with ten decimals.
`capture.csv.meta.json` records validity, session, sample count, effective rate
and maximum observation window. Successful exit requires a matching final count
and durable CSV output. Failed or interrupted captures exit nonzero and retain
available partial data with `valid: false`; never use them for calibration.
Existing CSV and metadata files are not overwritten. A successful short capture
still may be too short or too quiet for input-shaping analysis.

## OctoPrint compatibility

The matching `OctoPrint-Pinput_Shaping` optimization branch already uses
`-p PORT -f RATE -s FILE`, waits for `Press Q to stop`, checks CSV progress and
requires successful process exit after Q. Those interfaces are preserved.
The wrapper exports acquisition timing instead of host receipt timing, so the
plugin can validate and analyze it without relaxing its quality gates.

The plugin's pinned upstream installer still references legacy commit
`02d01ba`. Until a reviewed protocol 2 revision is committed and published,
install this local wrapper explicitly. Do not invent a new installer commit
pin or assume that a plugin-only upgrade replaces the sensor firmware.

## Validation

Tests compile the real C wrapper and run it through pseudo-terminals. A host
C++ harness also executes the actual firmware source with simulated SPI and USB.
It covers rates, raw units, partial USB writes, timestamp rollover, FIFO/software
overflow, uncertain timing, missing data, clipping, initialization failure,
checksums, sequence gaps, rate/session mismatch, disconnection and bounded stop.
The harness also covers ten-second synthetic captures with SPI-call costs and
delayed short USB packets, plus aborts across USB packet boundaries. These costs
are injected test conditions, not measured target timings or USB guarantees.

An optional integration check exercises the real plugin capturer and analyzer:

```bash
PINPUT_PLUGIN_ROOT=/path/to/OctoPrint-Pinput_Shaping python3 -m pytest -q
```

Use the plugin's Python environment for this optional check. Hardware validation
still needs repeated captures at the supported rates, timing/repeatability
measurements and sensor checks while the OctoPrint host is under load.

## License

GPLv3, preserving the project's existing license declaration.
