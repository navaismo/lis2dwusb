import binascii
from pathlib import Path
import subprocess

import pytest

from device import Device, frame

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='module')
def firmware(tmp_path_factory):
    output = tmp_path_factory.mktemp('firmware') / 'harness'
    subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
        '-I', str(ROOT / 'include'), '-I', str(ROOT / 'tests/stubs'),
        str(ROOT / 'tests/firmware_harness.cpp'), '-o', str(output)], check=True)
    return output


def frames(firmware, scenario, rate=1600):
    result = subprocess.run([firmware, scenario, str(rate)], capture_output=True, text=True, check=True)
    records = []
    for line in result.stdout.splitlines():
        payload, checksum = line.rsplit('*', 1)
        assert int(checksum, 16) == binascii.crc_hqx(payload.encode(), 0xffff)
        records.append(payload.split(','))
    return records


@pytest.mark.parametrize('rate', [200, 400, 800, 1600])
@pytest.mark.parametrize('scenario', ['normal', 'wrap'])
def test_actual_firmware_preserves_samples_clock_and_stop_through_partial_usb_writes(firmware, scenario, rate):
    records = frames(firmware, scenario, rate)
    assert records[0] == ['LIS2DWUSB', '2', '1234abcd']
    assert records[1] == ['READY', '2', '1234abcd', str(rate), '16384', 'POLL']
    samples = [record for record in records if record[0] == 'S']
    assert len(samples) >= rate * 2
    assert records[-1] == ['DONE', '1234abcd', str(len(samples))]
    times = [int(record[2]) for record in samples]
    assert all(b > a for a, b in zip(times, times[1:]))
    assert max(times) < 3000000
    assert [int(record[1]) for record in samples] == list(range(len(samples)))
    assert all(record[3:6] == ['4096', '0', '16384'] for record in samples)
    assert all(int(record[6]) <= 1000000 / rate / 4 for record in samples)
    assert not any(record[0] == 'ERROR' for record in records)


@pytest.mark.parametrize('scenario,reason', [('backlog', 'TIMING_BACKLOG'), ('overflow', 'FIFO_OVERFLOW'),
    ('uncertain', 'TIMING_UNCERTAIN'), ('clipping', 'CLIPPING'), ('sensor_timeout', 'SENSOR_TIMEOUT'),
    ('buffer', 'BUFFER_OVERFLOW'), ('identity', 'SENSOR_CONFIG'), ('reset', 'SENSOR_CONFIG')])
def test_actual_firmware_stops_and_reports_faults_without_success(firmware, scenario, reason):
    records = frames(firmware, scenario)
    assert ['ERROR', '1234abcd', reason] in records
    assert not any(record[0] == 'DONE' for record in records)


def test_actual_firmware_powers_down_on_usb_disconnect(firmware):
    records = frames(firmware, 'disconnect')
    assert not any(record[0] == 'DONE' for record in records)


def test_real_wrapper_consumes_actual_firmware_frames(firmware, binary, tmp_path):
    records = frames(firmware, 'normal')
    samples = [frame(','.join(record)) for record in records if record[0] == 'S']
    device = Device(binary, tmp_path / 'firmware.csv', count=len(samples), records=samples).start()
    try:
        _, stderr = device.finish()
        assert device.process.returncode == 0, stderr.decode()
        assert b'verified complete' in stderr
    finally:
        device.close()
