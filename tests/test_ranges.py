"""Range negotiation and physical-unit conversion without physical devices."""
import csv
import json
from pathlib import Path
import subprocess

import pytest

from device import Device, frame
from test_firmware import firmware, frames

RANGES = [(2, 0x00, 16384), (4, 0x10, 8192), (8, 0x20, 4096), (16, 0x30, 2048)]


@pytest.mark.parametrize('full_scale,ctrl6,scale', RANGES)
def test_actual_firmware_and_wrapper_keep_800_hz_timing_and_scale(firmware, binary, tmp_path, full_scale, ctrl6, scale):
    records = frames(firmware, 'range_session', 800, full_scale)
    assert records[1] == ['READY', '2', '1234abcd', '800', str(scale), 'POLL', str(full_scale)]
    samples = [r for r in records if r[0] == 'S']
    assert len(samples) >= 12000
    assert [int(r[1]) for r in samples] == list(range(len(samples)))
    assert all(r[3:6] == [str(scale // 4), '0', str(scale)] for r in samples)
    assert all(0 <= int(r[6]) <= 312 for r in samples)
    assert records[-1] == ['DONE', '1234abcd', str(len(samples))]
    path = tmp_path / 'range.csv'
    device = Device(binary, path, rate=800, full_scale=full_scale, count=len(samples),
                    records=[frame(','.join(r)) for r in samples],
                    ready_payload=','.join(records[1]).replace('1234abcd', '{token}')).start()
    try:
        _, stderr = device.finish()
        assert device.error is None and device.process.returncode == 0, stderr.decode()
        rows = list(csv.DictReader(path.open()))
        assert len(rows) == len(samples)
        assert all((float(r['x']), float(r['y']), float(r['z'])) == (0.25, 0, 1) for r in rows)
        assert [round(float(r['time']) * 1000000) for r in rows] == [int(r[2]) for r in samples]
        metadata = json.loads(Path(str(path) + '.meta.json').read_text())
        assert metadata['valid'] is True and metadata['requested_rate'] == 800
        assert metadata['requested_range_g'] == metadata['range_g'] == full_scale
        assert metadata['counts_per_g'] == scale and metadata['units'] == 'g'
        assert metadata['effective_rate'] == pytest.approx(800)
    finally:
        device.close()


@pytest.mark.parametrize('full_scale,ctrl6,scale', RANGES)
@pytest.mark.parametrize('fault', ['readback', 'short_read', 'failed_read'])
def test_bad_startup_readback_never_acknowledges_a_range(firmware, full_scale, ctrl6, scale, fault):
    records = frames(firmware, 'range_session', 800, full_scale, fault)
    assert records == [['LIS2DWUSB', '2', '1234abcd'], ['ERROR', '1234abcd', 'SENSOR_CONFIG']]


def test_startup_verifies_noise_and_filter_bits_too(firmware):
    assert frames(firmware, 'range_session', 800, 16, 'filter_readback')[-1] == ['ERROR', '1234abcd', 'SENSOR_CONFIG']


def test_16g_diagnostic_fits_with_full_width_fields_and_every_usb_alignment(firmware):
    for padding in range(64):
        records = frames(firmware, 'clipping_format', 800, -32768, -32768, -32768, padding, 16)
        assert records == [['ERROR', '1234abcd',
            'CLIPPING:why=raw_limit:seq=4294967294:t_us=18446744073709551615'
            ':x=-32768:y=-32768:z=-32768:hit=7:fifo=FF:fs=16:c6=30:c6v=30:c6f=FF']]


@pytest.mark.parametrize('value', ['0', '1', '3', '32', '-2', '+16', '16x', ' 16', '99999999999999999999'])
def test_firmware_rejects_unsupported_range_without_starting(firmware, value):
    records = frames(firmware, 'range_session', 800, value)
    assert records == [['LIS2DWUSB', '2', '1234abcd'], ['ERROR', '1234abcd', 'UNSUPPORTED_RANGE']]


@pytest.mark.parametrize('value', ['', '16,extra', '16,', ',16'])
def test_firmware_rejects_malformed_range_requests(firmware, value):
    records = frames(firmware, 'range_session', 800, value)
    assert records[-1] == ['ERROR', '1234abcd', 'BAD_COMMAND']
    assert not any(r[0] in ('S', 'READY', 'DONE') for r in records)


@pytest.mark.parametrize('full_scale,ctrl6,scale', RANGES)
def test_clipping_keeps_raw_limits_detailed_fields_and_invalid_metadata(firmware, binary, tmp_path, full_scale, ctrl6, scale):
    records = frames(firmware, 'range_session', 800, full_scale, 'clipping')
    reason = records[-1][2]
    assert records[-1][:2] == ['ERROR', '1234abcd']
    details = dict(part.split('=', 1) for part in reason.split(':')[1:])
    assert details['why'] == 'raw_limit'
    assert (details['x'], details['y'], details['z'], details['hit']) == ('32764', '-32768', '32760', '7')
    assert details['fs'] == str(full_scale)
    assert details['c6'] == details['c6v'] == details['c6f'] == f'{ctrl6:02X}'
    assert int(details['seq']) > 0 and int(details['t_us']) > 0 and details['fifo'] == '01'
    assert not any(r[0] == 'DONE' for r in records)
    path = tmp_path / 'clipping.csv'
    device = Device(binary, path, rate=800, full_scale=full_scale, error_reason=reason).start()
    try:
        _, stderr = device.finish()
        assert device.process.returncode == 2 and f'Device error: {reason}'.encode() in stderr
        assert json.loads(Path(str(path) + '.meta.json').read_text())['valid'] is False
    finally:
        device.close()


@pytest.mark.parametrize('full_scale', [4, 8, 16])
def test_legacy_start_resets_range_after_a_nondefault_session(firmware, full_scale):
    records = frames(firmware, 'range_session', 800, full_scale, 'legacy_reset')
    ready = [r for r in records if r[0] == 'READY']
    assert ready[-1] == ['READY', '2', '1234abcd', '800', '16384', 'POLL']
    assert records[-1][0] == 'DONE'


@pytest.mark.parametrize('fault', ['range_unsupported', 'range_timeout'])
def test_old_firmware_range_negotiation_fails_clearly(binary, tmp_path, fault):
    path = tmp_path / 'old-firmware.csv'
    device = Device(binary, path, rate=800, full_scale=16, fault=fault).start()
    try:
        _, stderr = device.finish()
        assert device.process.returncode == 2 and b'range negotiation' in stderr.lower()
        assert b'Press Q to stop' not in stderr and path.read_text() == 'time,x,y,z\n'
        metadata = json.loads(Path(str(path) + '.meta.json').read_text())
        assert metadata['valid'] is False and metadata['requested_range_g'] == 16
        assert metadata['range_g'] is None and metadata['counts_per_g'] is None
    finally:
        device.close()


@pytest.mark.parametrize('ready', [
    'READY,2,{token},{rate},16384,POLL',  # An old device ignores the new argument.
    'READY,2,{token},{rate},2048,POLL',   # Scale alone is not explicit range negotiation.
    'READY,2,{token},{rate},16384,POLL,16',
    'READY,2,{token},{rate},2048,POLL,2',
    'READY,2,{token},{rate},2048,POLL,32',
    'READY,2,{token},{rate},2048,POLL,nan',
    'READY,2,{token},{rate},2048,POLL,',
    'READY,2,{token},{rate},2048,POLL,16,extra',
    'READY,2,{token},1600,2048,POLL,16',
    'READY,2,badbad00,{rate},2048,POLL,16',
    'READY,2,{token},{rate},2048,HOST,16',
])
def test_ready_must_confirm_selected_range_scale_rate_and_clock(binary, tmp_path, ready):
    path = tmp_path / 'mismatch.csv'
    device = Device(binary, path, rate=800, full_scale=16, ready_payload=ready).start()
    try:
        _, stderr = device.finish()
        assert device.process.returncode == 2 and b'Press Q to stop' not in stderr
        assert b'Invalid READY' in stderr or b'did not confirm range negotiation' in stderr
        assert path.read_text() == 'time,x,y,z\n'
        metadata = json.loads(Path(str(path) + '.meta.json').read_text())
        assert metadata['valid'] is False and metadata['counts_per_g'] is None
    finally:
        device.close()


@pytest.mark.parametrize('full_scale', [4, 8, 16])
@pytest.mark.parametrize('fault', ['crc', 'gap', 'duplicate', 'time_gap', 'uncertainty', 'clipping', 'done_count', 'disconnect'])
def test_nondefault_ranges_preserve_all_capture_rejections(binary, tmp_path, full_scale, fault):
    path = tmp_path / 'invalid.csv'
    device = Device(binary, path, rate=800, full_scale=full_scale, fault=fault).start()
    try:
        _, stderr = device.finish()
        assert device.process.returncode == 2 and b'verified complete' not in stderr
        assert json.loads(Path(str(path) + '.meta.json').read_text())['valid'] is False
        if fault == 'clipping':
            assert f':fs={full_scale}:counts_per_g={32768 // full_scale}'.encode() in stderr
    finally:
        device.close()


def test_explicit_default_range_preserves_legacy_start(binary, tmp_path):
    device = Device(binary, tmp_path / 'legacy.csv', rate=800, extra_args=('-r', '2')).start()
    try:
        _, stderr = device.finish()
        assert device.error is None and device.process.returncode == 0, stderr.decode()
    finally:
        device.close()


@pytest.mark.parametrize('value', ['0', '1', '3', '32', '-2', '+16', '16x', '0x10', 'nan', '999999999999999999999999999'])
def test_cli_range_is_validated_before_serial_access(binary, tmp_path, value):
    path = tmp_path / 'untouched.csv'
    result = subprocess.run([binary, '-r', value, '-s', str(path)], capture_output=True, text=True)
    assert result.returncode == 1 and 'Range must be one of' in result.stderr
    assert not path.exists()


def test_cli_keeps_legacy_frequency_default_and_documents_range(binary):
    result = subprocess.run([binary, '-h'], capture_output=True, text=True, check=True)
    assert '1600 Hz' in result.stderr and '+/-2 g' in result.stderr
    assert '[-r 2|4|8|16]' in result.stderr
