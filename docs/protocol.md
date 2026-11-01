# LIS2DWUSB Acquisition Protocol 2

## Reference and scope

Register behavior follows ST's [LIS2DW12 datasheet, DS11811 Rev 9](https://www.st.com/resource/en/datasheet/lis2dw12.pdf),
particularly CTRL1/CTRL2, FIFO operation and ODR stabilization. The target is the
RP2040 BTT S2DW SPI wiring already used by this repository. Marlin firmware is
not changed by this work.

## Framing and session ownership

Each command and response is ASCII: `PAYLOAD*CCCC\n`, where `CCCC` is four hex
digits of CRC-16/CCITT-FALSE over PAYLOAD (polynomial 0x1021, initial 0xffff,
no reflection or final XOR; `123456789` gives 0x29b1). The maximum line buffer
is 160 bytes including terminator. CSV output is a separate host-facing format.

The Linux wrapper generates an eight-digit hex session nonce. HELLO stops any
old acquisition and empties its queued samples before the new handshake.
A partially transmitted old frame finishes before the greeting; the wrapper
drains old frames only while awaiting its nonce-matched greeting. Subsequent
malformed or corrupt frames invalidate the session.

| Direction | Payload | Meaning |
| --- | --- | --- |
| Host | `HELLO,0123abcd` | Negotiate a fresh session |
| Device | `LIS2DWUSB,2,0123abcd` | Version and nonce acknowledgement |
| Host | `START,1600,0123abcd` | Request an exact supported ODR |
| Device | `READY,2,0123abcd,1600,16384,POLL` | Confirm rate, raw scale and clock model |
| Device | `S,0,1875,4096,0,16384,20` | Sequence, time_us, raw XYZ, observation window_us |
| Host | `STOP,0123abcd` | Stop acquisition and drain accepted samples |
| Device | `DONE,0123abcd,4096` | Confirm total accepted/sent samples |
| Device | `ERROR,0123abcd,TIMING_BACKLOG` | Abort: the capture is invalid |

Samples begin at sequence zero. DONE is valid only after STOP, with a nonzero
count exactly matching all received samples. A disconnect, missing confirmation,
sequence discontinuity, CRC failure or firmware ERROR cannot produce exit zero.
No fallback to legacy host-timestamped data is supported.

## Acquisition and timestamp meaning

The device reads WHO_AM_I=0x44, resets with a 100 ms deadline, enables BDU and
auto-increment (CTRL2=0x0c), selects high-performance mode and +/-2 g, verifies
registers, allows settling and enables continuous FIFO (FIFO_CTRL=0xc0).
The first two FIFO samples are discarded. The firmware retains raw signed,
left-aligned 14-bit output; the wrapper divides by 16384 counts/g rather than
quantizing to integer milligravity.

The current board configuration has no assigned data-ready interrupt pin.
Instead, the main loop polls the FIFO and brackets a transition from empty to
one sample. The timestamp is the midpoint of that observed interval, measured
with the RP2040 microsecond clock and exported relative to the session epoch.
The complete interval width accompanies each sample. This is a **bounded device
observation**, not a hardware-latched conversion timestamp. Its physical timing
accuracy still needs measurement on the board.

A window wider than one quarter of a nominal period is rejected. A FIFO depth
above one is also rejected: these buffered values no longer have individually
observed edges. The firmware never reconstructs their times from an assumed
uniform rate. FIFO overflow is separately detected. This conservative policy
can reject captures under scheduling pressure; do not relax it without timing
evidence. The plugin retains its independent jitter/gap validation.

The 32-bit microsecond ticker is extended to 64 bits by unsigned delta
accumulation on every main-loop iteration, including idle operation. Output
times therefore continue across the approximately 71-minute ticker rollover.
The sample counter aborts before wrapping. Sensor ODR tolerance is reflected
in measured timestamps, not removed by scaling them to the requested rate.

## Buffering and stop

A fixed queue holds 256 accepted samples. Formatting and the pinned mbed USB
CDC `send_nb` API run separately from acquisition; no USB send waits for the
host. Partial writes retain the unsent suffix. Queue exhaustion invalidates the
capture; no sample is silently dropped. Commands are processed with a bounded
byte budget to prevent command traffic from starving acquisition.

A sensor producing no samples for 500 ms fails. Configuration, uncertain
observation, clipping near full scale, FIFO or queue overflow and USB stalls
produce ERROR and power down sampling. STOP also powers down sampling, but
retains already accepted samples and emits DONE only after their queue drains.
USB disconnect powers down sampling and clears pending state. A later HELLO
starts a fresh session.

The Linux wrapper uses nonblocking descriptors and poll for both serial and
keyboard input. Startup, data and stop phases have monotonic deadlines. Q,
SIGINT and SIGTERM request STOP; failure to acknowledge it within the deadline
is an invalid capture, not a successful forced save.

## CSV and integration

The wrapper preserves `time,x,y,z`, `-p`, `-f`, `-s` and `Press Q to stop` for
OctoPrint. CSV times use six decimals and XYZ use g with ten decimals. It
validates session, ODR, raw range, sequence, timestamps, observation windows,
checksum and final count before reporting success. The metadata sidecar is
written atomically and records validity and the device timing model. CSV flush,
sync and close errors also prevent successful exit.

Physical acquisition behavior, operating-system latency and repeatability
remain unvalidated. The source tests and pseudo-terminal tests verify software
contracts and deliberate failure responses. Existing binaries and the plugin's
legacy installer pin remain unchanged until a reviewed release is authorized.
