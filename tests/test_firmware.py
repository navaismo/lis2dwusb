import binascii
import json
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


def frames(firmware, scenario, rate=1600, *arguments):
    result = subprocess.run([firmware, scenario, str(rate), *map(str, arguments)],
                            capture_output=True, text=True, check=True)
    records = []
    for line in result.stdout.splitlines():
        if not line:
            continue
        assert len(line) + len('\r\n\0') <= 160
        payload, checksum = line.rsplit('*', 1)
        assert int(checksum, 16) == binascii.crc_hqx(payload.encode(), 0xffff)
        records.append(payload.split(','))
    return records


def test_firmware_formats_full_width_timestamps_without_wide_printf(firmware):
    records = frames(firmware, 'format')
    times = [0, 1, 2**32 - 1, 2**32, 2**64 - 1]
    assert records == [['S', str(index), str(timestamp), '-32756', '0', '16384', '20']
                       for index, timestamp in enumerate(times)]


@pytest.mark.parametrize('rate', [200, 400, 800, 1600])
@pytest.mark.parametrize('scenario', ['normal', 'wrap', 'workload'])
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
    errors = [record for record in records if record[:2] == ['ERROR', '1234abcd']]
    assert len(errors) == 1
    if scenario == 'uncertain':
        parts = errors[0][2].split(':')
        assert parts[0] == reason
        details = dict(part.split('=', 1) for part in parts[1:])
        assert int(details['window_us']) > int(details['limit_us']) == 156
        assert details['work'] in ('FORMAT', 'CRC', 'USB', 'PACKET', 'COMMAND', 'START')
    elif scenario == 'buffer':
        assert errors[0][2].startswith(reason + ':acq=')
        assert ':fmt=' in errors[0][2] and ':usb_busy=' in errors[0][2]
    elif scenario == 'clipping':
        details = dict(part.split('=', 1) for part in errors[0][2].split(':')[1:])
        assert errors[0][2].startswith('CLIPPING:why=raw_limit:')
        assert (details['x'], details['y'], details['z'], details['hit']) == ('32760', '0', '16384', '1')
        assert details['c6'] == details['c6v'] == details['c6f'] == '00'
    else:
        assert errors[0][2] == reason
    assert not any(record[0] == 'DONE' for record in records)


@pytest.mark.parametrize('candidate', [337, 339])
@pytest.mark.parametrize('axes', [(32760, -32760, 32764), (-32768, 32767, -32764),
    (32760, 0, 16384), (0, -32760, 0), (0, 0, 32760),
    (32760, -32760, 0), (32760, 0, -32760), (0, 32760, -32760),
    (32756, -32756, 16384)])
def test_clipping_preserves_original_candidate_before_fault_register_read(firmware, candidate, axes):
    records = frames(firmware, 'clipping_candidate', 800, *axes, candidate)
    hit = sum(1 << axis for axis, raw in enumerate(axes) if raw >= 32760 or raw <= -32760)
    if not hit:
        assert records == [['S', str(candidate), '96', *map(str, axes), '12']]
        return
    assert len(records) == 1 and records[0][:2] == ['ERROR', '1234abcd']
    details = dict(part.split('=', 1) for part in records[0][2].split(':')[1:])
    assert details == {'why': 'raw_limit', 'seq': str(candidate), 't_us': '96',
        **dict(zip(('x', 'y', 'z'), map(str, axes))), 'hit': str(hit), 'fifo': '81',
        'fs': '2', 'c6': '00', 'c6v': '00', 'c6f': '30'}


def test_clipping_diagnostic_full_width_fields_crc_bounds_and_usb_alignment(firmware):
    for padding in range(64):
        records = frames(firmware, 'clipping_format', 800, -32768, -32768, -32768, padding)
        assert records == [['ERROR', '1234abcd',
            'CLIPPING:why=raw_limit:seq=4294967294:t_us=18446744073709551615'
            ':x=-32768:y=-32768:z=-32768:hit=7:fifo=FF:fs=2:c6=00:c6v=00:c6f=FF']]


@pytest.mark.parametrize('corrupt', [False, True])
def test_actual_firmware_clipping_error_reaches_wrapper_without_protocol_change(firmware, binary, tmp_path, corrupt):
    reason = frames(firmware, 'clipping_format', 800, -32768, -32768, -32768)[0][2]
    path = tmp_path / 'clipping.csv'
    device = Device(binary, path, rate=800, error_reason=reason,
                    fault='error_crc' if corrupt else None).start()
    try:
        _, stderr = device.finish()
        assert device.process.returncode == 2
        assert b'verified complete' not in stderr
        assert json.loads(Path(str(path) + '.meta.json').read_text())['valid'] is False
        if corrupt:
            assert b'Invalid frame checksum or format' in stderr
            assert b'Device error:' not in stderr
        else:
            assert f'Device error: {reason}'.encode() in stderr
            assert b'Firmware rejected or invalidated this capture' in stderr
        assert path.read_text() == 'time,x,y,z\n'
    finally:
        device.close()


@pytest.mark.parametrize('scenario', ['abort_fields', 'abort_crc', 'abort_send', 'abort_usb', 'abort_packet'])
def test_abort_preserves_line_boundaries_during_staged_transmission(firmware, scenario):
    records = frames(firmware, scenario)
    assert records[-1] == ['ERROR', '00000000', 'TEST_ABORT']
    if scenario in ('abort_send', 'abort_usb'):
        assert records[0] == ['S', '0', '625', '4096', '0', '16384', '20']
        assert len(records) == 2
    elif scenario == 'abort_packet':
        assert records[0] == ['S', str(2**32 - 1), str(2**64 - 1),
                              '-32768', '-32768', '-32768', str(2**32 - 1)]
        assert len(records) == 2
    else:
        assert len(records) == 1


def test_actual_firmware_powers_down_on_usb_disconnect(firmware):
    # A physical disconnect may truncate the packet currently in flight.
    # The harness verifies power-down; no completion may be advertised.
    result = subprocess.run([firmware, 'disconnect'], capture_output=True, check=True)
    assert b'DONE,' not in result.stdout


@pytest.mark.parametrize('rate', [200, 400, 800, 1600])
@pytest.mark.parametrize('scenario', ['throughput', 'bulk_latency'])
def test_full_packet_streaming_survives_spi_cost_and_short_packet_delays(firmware, rate, scenario):
    records = frames(firmware, scenario, rate)
    samples = [record for record in records if record[0] == 'S']
    assert len(samples) >= rate * 15
    assert [int(record[1]) for record in samples] == list(range(len(samples)))
    assert records[-1] == ['DONE', '1234abcd', str(len(samples))]
    assert not any(record[0] == 'ERROR' for record in records)
    times = [int(record[2]) for record in samples]
    assert all(b > a for a, b in zip(times, times[1:]))
    assert all(int(record[6]) <= 1000000 // rate // 4 for record in samples)


def test_control_responses_end_in_a_short_packet_at_every_alignment(firmware):
    assert frames(firmware, 'control_boundary') == [['LIS2DWUSB', '2', '1234abcd']] * 64


@pytest.mark.parametrize('rate', [200, 400, 800, 1600])
@pytest.mark.parametrize('scenario', ['normal', 'workload'])
def test_real_wrapper_consumes_actual_firmware_frames(firmware, binary, tmp_path, rate, scenario):
    records = frames(firmware, scenario, rate)
    samples = [frame(','.join(record)) for record in records if record[0] == 'S']
    device = Device(binary, tmp_path / 'firmware.csv', rate=rate, count=len(samples), records=samples).start()
    try:
        _, stderr = device.finish()
        assert device.process.returncode == 0, stderr.decode()
        assert b'verified complete' in stderr
    finally:
        device.close()
