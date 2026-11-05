# LIS2DWUSB Acquisition Protocol 2

## Reference and scope

Register behavior follows ST's [LIS2DW12 datasheet, DS11811 Rev 9](https://www.st.com/resource/en/datasheet/lis2dw12.pdf),
particularly CTRL1/CTRL2, FIFO operation and ODR stabilization. The target is the
RP2040 BTT S2DW SPI wiring already used by this repository. Marlin firmware is
not changed by this work.

## Framing and session ownership

Each command and response is ASCII: `PAYLOAD*CCCC\n`, where `CCCC` is four hex
digits of CRC-16/CCITT-FALSE over PAYLOAD (polynomial 0x1021, initial 0xffff,
no reflection or final XOR; `123456789` gives 0x29b1). CRLF is also accepted;
control responses use it when needed to end in a short USB packet. The maximum line buffer
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
The firmware converts the full 64-bit timestamp to decimal explicitly because
the pinned target's Newlib Nano `printf` does not support 64-bit integer formats.
The host firmware harness rejects wide integer formatting arguments and checks
serialization through `UINT64_MAX`; Linux libc alone would hide this limitation.
The sample counter aborts before wrapping. Sensor ODR tolerance is reflected
in measured timestamps, not removed by scaling them to the requested rate.

## Buffering and stop

A fixed queue holds 256 accepted samples, with one additional sample held by the
serializer. Each bounded serializer step formats at most one numeric field
directly into its frame, computes CRC over at most 32 payload bytes, or copies a
frame suffix into a separate 512-byte transmit ring. USB packet preparation and
nonblocking writes are serviced independently after every two serializer steps.
The main loop processes up to eight serializer steps, polling the FIFO before
every serializer or USB operation. Active captures consume
at most four command bytes per loop. FIFO status and XYZ are read with one
buffered SPI transaction each, avoiding a separate driver call per byte. The
timing acceptance limit is unchanged and still needs physical validation.

A 64-byte staging buffer draws from the transmit ring to submit full CDC packets.
Serialization can continue while that packet waits for the endpoint; the old
single staging buffer stopped both tasks whenever USB was busy. A busy endpoint
is retried after at least 50 us while acquisition and serialization continue.
The sample queue remains 256 entries; neither buffer can compensate for a host
whose sustained throughput is below the stream's required bandwidth. Frame
boundaries and CRCs are independent of USB packet boundaries. Partly filled
packets flush after 2 ms, or promptly at a control response, including DONE.
Control responses always terminate in a short packet, including when adding a
CR before LF is necessary to avoid an exact 64-byte boundary.
The sender preserves partial writes and finishes a partly transmitted frame
before emitting ERROR. This reduces short-packet overhead without adding sample
queue capacity or assuming a host throughput guarantee. Queue exhaustion invalidates the
capture; no sample is silently dropped. Commands are processed with a bounded
byte budget to prevent command traffic from starving acquisition.

`TIMING_UNCERTAIN` includes colon-separated `window_us`, `limit_us` and `work`
details in its reason field. `work` identifies the most recent format, CRC, USB
or command step; it helps locate delays but does not measure their individual
execution times. The wrapper reports these details and rejects the capture.
`BUFFER_OVERFLOW` includes `acq` (accepted samples), `fmt` (sample frames copied
to the transmit ring), `usb_ok` (successful nonblocking write calls) and
`usb_busy` (write calls that accepted zero bytes). These counters help separate
serialization pressure from USB backpressure; they do not establish capture
validity or the number of samples received by the host.

`CLIPPING` retains the same per-axis raw limits (`>=32760` or `<=-32760`) and
adds failure-only details inside the existing third ERROR field, for example:

```text
ERROR,0123abcd,CLIPPING:why=raw_limit:seq=339:t_us=427500:x=32764:y=-16:z=16380:hit=1:fifo=01:fs=2:c6=00:c6v=00:c6f=00
```

The example is illustrative; the normal CRC suffix and newline are omitted.
`seq` is the rejected candidate's zero-based index, before incrementing the
accepted-sample counter. `t_us` uses the same device epoch and observation-window
midpoint as accepted sample frames. `x/y/z` are the original signed register
words. `hit` is a decimal bitmask: X=1, Y=2, Z=4; every crossing axis is included.
`fifo` is the hexadecimal FIFO_SAMPLES byte observed before reading that sample.
`fs` is the configured full-scale magnitude in g, still 2. `c6` is configured
CTRL6, `c6v` is its verified startup readback, and `c6f` is a separate fault-time
readback, all hexadecimal. The latter is read only after preserving the original
candidate, timestamp, mask and FIFO status; XYZ is not read again. A fault-time
readback is diagnostic evidence, not an independently verified sensor state.

No diagnostic formatting or additional register read runs for an accepted
sample. The compile-time maximum ERROR size is 153 bytes including the nonce,
CRC, optional CR, newline and NUL, within the unchanged 160-byte line buffer.
The reason uses colons rather than extra commas, so existing protocol-2 wrappers
display it unchanged. An unexpected formatting failure still aborts with
`CLIPPING:why=diagnostic_format`. No triggering sample is written to CSV, no
DONE is emitted, and already accepted but unsent samples can be discarded.
Consequently, the wrapper's received sample count can be lower than `seq`.

The wrapper's independent guard keeps its existing failure summary and also
prints `Clipping diagnostic:source=wrapper:why=raw_limit` followed by `seq`,
`t_us`, raw `x/y/z`, the complete `hit` mask, `fs` and `counts_per_g`. It cannot
recover XYZ from a bare firmware ERROR or obtain sensor register/FIFO state.
Both paths retain exit code 2 and invalid metadata. A near-rail decoded word
does not by itself distinguish physical saturation from an incorrect SPI read;
the USB CRC protects the subsequent framed transmission.

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

The combined synthetic SPI/formatting workload and 1 ms bulk-endpoint delay
reproduced inadequate 1600 Hz throughput with the previous transmitter. The
revised path completes 15 seconds at all four rates in that simulation, with
contiguous sample sequences, CRC checks, accepted timing windows and matching
DONE counts. These workload costs are a test model, not measured target costs;
physical 1600 Hz stability still requires testing with the actual USB host.
