# LIS2DWUSB

LIS2DWUSB records acceleration from the BIGTREETECH S2DW (LIS2DW12) for
input-shaping measurements. We use an RP2040 to acquire the samples and a Linux
wrapper to save them as CSV, with timestamps from the device.

We support 200, 400, 800 and 1600 Hz, with selectable ranges of +/-2, +/-4,
+/-8 and +/-16 g. Protocol 2 includes sequence numbers, CRC checks and explicit
errors for clipping, lost samples and uncertain timing.

## Build

You'll need PlatformIO for the RP2040 firmware and a C compiler with Make for
the Linux wrapper. Run these commands from the directory containing
`platformio.ini`:

```bash
pio run -e btt_lis2dw
make -C Linux_Wrapper
```

The firmware is generated at:

```text
.pio/build/btt_lis2dw/firmware.uf2
```

Our SPI wiring is CS=GPIO9, SCK=GPIO10, MOSI=GPIO11 and MISO=GPIO8. The platform,
mbed framework and ARM toolchain versions are set in `platformio.ini`.

Build the wrapper on the machine where you'll run it. It needs no pigpio
library, and normal captures run without sudo.

### Clean builds and VS Code

Save your source files before building. In VS Code, select `btt_lis2dw` and use
PlatformIO's Clean and Build commands. From the terminal, the same steps are:

```bash
pio run -e btt_lis2dw -t clean
pio run -e btt_lis2dw
```

A clean build shows `Generating UF2 image` and finishes with `SUCCESS`. Check
the output file with:

```bash
stat .pio/build/btt_lis2dw/firmware.uf2
sha256sum .pio/build/btt_lis2dw/firmware.uf2
```

An incremental build can keep the same UF2 timestamp when the image hasn't
changed. Use the checksum to compare builds. The build output stays in `.pio`;
copies in `FW_Binaries/` or downloaded archives aren't updated by these commands.

Building doesn't flash the board. Use the generated UF2 when updating your
RP2040. One firmware image supports all four ranges; we select the range at the
start of each capture.

## Install the wrapper

Copy the complete source tree to your Raspberry Pi, including `include/`, then
build and install:

```bash
make -C Linux_Wrapper
Linux_Wrapper/lis2dwusb -h
sudo make -C Linux_Wrapper install
/usr/local/bin/lis2dwusb -h
```

The installed executable is `/usr/local/bin/lis2dwusb`. Check that its help
includes `-r 2|4|8|16`; the version string alone isn't enough to identify range
support.

For a first-time setup on Raspberry Pi OS or Debian, we also provide an
installer. Replace `pi` with the account that runs OctoPrint:

```bash
sudo OCTOPRINT_USER=pi bash Linux_Wrapper/install.sh
```

This installs the compiler and Make packages, builds the wrapper and adds the
account to `dialout`. Restart OctoPrint to pick up the new group membership.
Update the RP2040 firmware separately.

### Exec format error

If you get `cannot execute binary file: Exec format error`, check that the
executable matches your machine:

```bash
uname -m
file /usr/local/bin/lis2dwusb
```

An x86-64 binary won't run on an ARM Raspberry Pi. Rebuild and install from
source on the Pi using the commands above. Our Makefile always rebuilds the
wrapper, even if a copied executable has a newer timestamp.

## Capture acceleration

Use the accelerometer's serial port. If you also have a printer connected,
check which device belongs to each port.

For a 50-second capture at 800 Hz and +/-16 g:

```bash
lis2dwusb -p /dev/ttyACM0 -f 800 -r 16 -d 50 -s capture.csv
```

You can use a stable `/dev/serial/by-id/` path too:

```bash
lis2dwusb -p /dev/serial/by-id/YOUR_SENSOR -f 800 -r 16 -s capture.csv
```

Press **Q** or **Ctrl+C** to stop. SIGTERM also requests a normal stop, including
confirmation of the final sample count.

| Option | Purpose | Default |
| --- | --- | --- |
| `-p` | Sensor serial port | `/dev/ttyACM0` |
| `-f` | Sampling rate: 200, 400, 800 or 1600 Hz | 1600 Hz |
| `-r` | Full-scale range: 2, 4, 8 or 16 g | 2 g |
| `-d` | Capture duration in seconds | Until stopped |
| `-s` | CSV output path | Standard output |
| `-t` | Communication timeout in seconds | 3 seconds |
| `-h` | Show help | |
| `-V` | Show wrapper and protocol version | |

Pass `-f 800` explicitly for the 800 Hz captures shown here. We don't change the
rate or range automatically when a capture fails.

### Measurement range

| Range | CTRL6 | Raw counts/g |
| --- | --- | ---: |
| +/-2 g | 0x00 | 16384 |
| +/-4 g | 0x10 | 8192 |
| +/-8 g | 0x20 | 4096 |
| +/-16 g | 0x30 | 2048 |

We change only the full-scale bits in CTRL6. Low-pass filtering and the noise
setting stay the same. At 16 g we use `0x30`; Klipper's `0x34` also enables
LOW_NOISE. The sensor provides 14-bit measurements, left-aligned in the 16-bit
registers we read.

A wider range gives more acceleration headroom at the cost of resolution.
For example, raw 2048 at 16 g and raw 16384 at 2 g both export as 1 g. These
are nominal conversion factors; they don't calibrate sensor gain or offset.

The wrapper checks the requested range and scale against the firmware's READY
response before accepting samples. A failed CTRL6 readback, incompatible scale
or unsupported range stops the capture.

With `-r 2`, or without `-r`, we use the original protocol-2 exchange. Older
protocol-2 firmware works at 2 g; selecting 4, 8 or 16 g requires firmware with
range selection. Each new capture without a range starts at 2 g, regardless of
the previous capture.

### CSV and metadata

The CSV columns are `time,x,y,z`. Time is in device-relative seconds with six
decimal places, and acceleration is in g with ten decimal places.

We also write `capture.csv.meta.json`, which records:

- Session, requested rate, effective rate and sample count.
- Requested and confirmed range: `requested_range_g` and `range_g`.
- Confirmed scale: `counts_per_g`.
- Timing information and capture validity.

If startup fails before the range is confirmed, `range_g` and `counts_per_g`
are null. A later failure keeps the confirmed values but sets `valid` to false.
We keep any partial CSV for diagnosis. Use only captures with a successful exit
and `valid: true` for analysis. Existing CSV and metadata files aren't overwritten.

## Clipping and other capture errors

We reject a sample if any raw axis is `>=32760` or `<=-32760`, at every range.
The firmware reports `Device error: CLIPPING:why=raw_limit:...` with the following
details:

| Field | Meaning |
| --- | --- |
| `seq`, `t_us` | Rejected sample's candidate index and device timestamp |
| `x`, `y`, `z` | Original signed raw XYZ |
| `hit` | Affected axes: X=1, Y=2, Z=4; combinations are added |
| `fifo` | FIFO status before reading the sample, in hexadecimal |
| `fs` | Configured range in g |
| `c6` | Configured CTRL6 |
| `c6v` | CTRL6 verified at startup |
| `c6f` | CTRL6 read when clipping occurred |

The CTRL6 values are hexadecimal. We save the original sample and timing
before the extra register read. These diagnostics run only when clipping occurs.
The wrapper also reports sample details if its own clipping check detects a
near-rail value.

The rejected firmware sample never reaches the CSV, and queued samples can be
discarded on failure. The final `Captured N samples` count can therefore be
lower than `seq`. A near-rail reading can come from physical saturation or an
incorrect SPI read; the diagnostic values help us investigate which occurred.

FIFO overflow, timing uncertainty and a full software buffer have separate
error codes. Failed captures exit with code 2 and invalid metadata. To save the
wrapper's messages alongside the CSV:

```bash
lis2dwusb -p /dev/ttyACM0 -f 800 -r 16 -d 50 -s capture.csv 2>capture.csv.log
```

See [the protocol documentation](docs/protocol.md) for frame layouts, CRC,
timestamp behavior and error details.

## OctoPrint

We use LIS2DWUSB with OctoPrint-Pinput_Shaping. Its range selection uses the
`lis2dwRange` setting and passes the range through `-r`. It checks wrapper
support, the confirmed range and scale, and the capture metadata before analysis.

New plugin configurations use 16 g and 800 Hz. When migrating existing saved
settings, the plugin keeps the previous 2 g range. Check your saved setting
before starting a measurement.

If your installed plugin doesn't offer range selection, it will continue to
capture at 2 g. Older analyzers may also reject wider-range metadata. Update the
plugin, wrapper and RP2040 firmware as needed; installing the plugin alone
doesn't update the firmware on the sensor. Use `lis2dwusb -h` to check the
wrapper installed on your Pi.

## Tests

Our tests compile the C wrapper and run it through pseudo-terminals. We also
run the firmware source in a C++ harness with simulated SPI and USB. You'll
need Python, pytest and a C++ compiler:

```bash
python3 -m pytest -q
```

The tests cover all rates and ranges, physical-unit conversion, partial USB
writes, clock rollover, FIFO and buffer overflow, clipping, failed register
reads, CRC errors, missing samples, disconnects and stopping. We also check
15-second streams at 800 Hz for every range and full-width clipping messages
at every USB alignment.

To include the plugin capturer and analyzer, run from a Python environment
with OctoPrint and the plugin's dependencies installed:

```bash
PINPUT_PLUGIN_ROOT=/path/to/OctoPrint-Pinput_Shaping python3 -m pytest -q
```

The pinned `btt_lis2dw` build uses 83,040 bytes of flash and 50,380 bytes of
static RAM. We measure flash from the ELF load image; PlatformIO's 4,142-byte
summary misses some sections in this setup. Static RAM doesn't include peak
stack usage.

Software tests use simulated devices. On the printer, check stationary gravity
and axis orientation first, then repeat captures during excitation. Compare
clipping, effective rate, timestamp gaps and jitter under normal USB and CPU
load before using the measurements for calibration.

## License

GPLv3.
