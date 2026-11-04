"""Protocol 2 emulator backed solely by a pseudo-terminal."""
import binascii
import errno
import math
import os
import pty
import select
import subprocess
import threading
import time


def frame(payload):
    encoded = payload.encode('ascii')
    return encoded + f'*{binascii.crc_hqx(encoded, 0xffff):04X}\n'.encode()


class Device:
    def __init__(self, binary, output, *, rate=1600, count=128, fault=None, records=None):
        self.master, self.slave = pty.openpty()
        self.port = os.ttyname(self.slave)
        self.binary, self.output = binary, output
        self.rate, self.count, self.fault = rate, count, fault
        self.records = records
        self.error = None
        self.token = None
        self.ready = threading.Event()
        self.sent = threading.Event()
        self.stopped = threading.Event()
        self.closed = threading.Event()
        self.buffer = b''
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.process = None

    def start(self):
        self.process = subprocess.Popen([str(self.binary), '-p', self.port, '-f', str(self.rate),
            '-s', str(self.output), '-t', '0.3'], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.thread.start()
        return self

    def command(self, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and not self.closed.is_set():
            if b'\n' in self.buffer:
                line, self.buffer = self.buffer.split(b'\n', 1)
                payload, checksum = line.rsplit(b'*', 1)
                assert int(checksum, 16) == binascii.crc_hqx(payload, 0xffff)
                return payload.decode().split(',')
            if select.select([self.master], [], [], 0.02)[0]:
                try:
                    chunk = os.read(self.master, 4096)
                except OSError as exc:
                    if exc.errno == errno.EIO:
                        time.sleep(.01)
                        continue
                    raise
                self.buffer += chunk
        return None

    def write(self, data):
        if self.fault == 'crlf':
            data = data.replace(b'\n', b'\r\n')
        for offset in range(0, len(data), 1024):
            os.write(self.master, data[offset:offset + 1024])

    def serve(self):
        try:
            hello = self.command()
            assert hello and hello[0] == 'HELLO'
            self.token = hello[1]
            if self.fault == 'legacy':
                self.write(b'time,x,y,z\n')
                return
            if self.fault == 'greeting_timeout':
                return
            version = 9 if self.fault == 'version' else 2
            self.write(frame(f'LIS2DWUSB,{version},{self.token}'))
            if version != 2:
                return
            start = self.command()
            if start is None:
                return
            assert start == ['START', str(self.rate), self.token]
            actual = 800 if self.fault == 'rate' else self.rate
            token = 'badbad00' if self.fault == 'nonce' else self.token
            self.write(frame(f'READY,2,{token},{actual},16384,POLL'))
            self.ready.set()
            if self.fault in ('rate', 'nonce'):
                self.command()
                return
            if self.fault == 'disconnect':
                os.close(self.master)
                self.master = -1
                return
            if self.fault in ('device_error', 'timing_detail'):
                reason = ('TIMING_UNCERTAIN:window_us=239:limit_us=156:work=USB'
                          if self.fault == 'timing_detail' else 'FIFO_OVERFLOW')
                self.write(frame(f'ERROR,{self.token},{reason}'))
                self.command()
                return
            rows = []
            for index in range(self.count):
                sequence = index + 1 if self.fault == 'gap' and index >= 64 else index
                timestamp = (index + 1) * (1000000 // self.rate)
                if self.fault == 'duplicate' and index == 64:
                    timestamp -= 1000000 // self.rate
                if self.fault == 'time_gap' and index >= 64:
                    timestamp += 1000000 // self.rate * 3
                window = 9999 if self.fault == 'uncertainty' else 20
                axis = 32760 if self.fault == 'clipping' else int(1024 * math.sin(2 * math.pi * 50 * index / self.rate)) * 4
                record = frame(f'S,{sequence},{timestamp},{axis},0,16384,{window}')
                if self.fault == 'crc' and index == 64:
                    record = record[:-3] + b'ZZ\n'
                rows.append(record)
            self.write(b''.join(rows) if self.records is None else b''.join(self.records))
            self.sent.set()
            stop = self.command()
            if stop is None:
                return
            assert stop == ['STOP', self.token]
            self.stopped.set()
            if self.fault == 'silent_stop':
                return
            count = self.count + 1 if self.fault == 'done_count' else self.count
            self.write(frame(f'DONE,{self.token},{count}'))
        except (OSError, ValueError) as exc:
            if not self.closed.is_set():
                self.error = exc
        except Exception as exc:
            self.error = exc
        finally:
            self.sent.set()

    def finish(self):
        assert self.sent.wait(3), 'Emulator did not finish sending samples'
        if self.process.poll() is None:
            try:
                self.process.stdin.write(b'Q\n')
                self.process.stdin.flush()
            except BrokenPipeError:
                pass
        return self.process.communicate(timeout=4)

    def close(self):
        self.closed.set()
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=4)
        self.thread.join(timeout=1)
        for fd in (self.master, self.slave):
            if fd >= 0:
                os.close(fd)
