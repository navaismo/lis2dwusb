import csv
import json
import os
from pathlib import Path
import sys
import time
import signal

import pytest

from device import Device, frame


@pytest.mark.parametrize('rate', [200, 400, 800, 1600])
def test_batched_usb_preserves_device_times_units_and_verified_stop(binary, tmp_path, rate):
    path = tmp_path / 'capture with spaces.csv'
    device = Device(binary, path, rate=rate).start()
    try:
        _, stderr = device.finish()
        assert device.process.returncode == 0, stderr.decode()
        assert device.error is None
        rows = list(csv.DictReader(path.open()))
        assert len(rows) == device.count
        stamps = [float(row['time']) for row in rows]
        assert all(b > a for a, b in zip(stamps, stamps[1:]))
        assert stamps[-1] - stamps[0] == pytest.approx((device.count - 1) / rate)
        assert float(rows[0]['z']) == 1
        metadata = json.loads(Path(str(path) + '.meta.json').read_text())
        assert metadata['valid'] is True
        assert metadata['effective_rate'] == pytest.approx(rate)
        assert metadata['samples'] == device.count
        assert metadata['max_window_us'] == 20
        assert device.stopped.is_set()
    finally:
        device.close()


@pytest.mark.parametrize('fault', ['legacy', 'greeting_timeout', 'version', 'nonce', 'rate', 'crc',
    'gap', 'duplicate', 'time_gap', 'uncertainty', 'clipping', 'device_error', 'done_count', 'disconnect'])
def test_invalid_or_incomplete_captures_never_report_success(binary, tmp_path, fault):
    path = tmp_path / 'capture.csv'
    device = Device(binary, path, fault=fault).start()
    try:
        _, stderr = device.finish()
        assert device.process.returncode != 0, stderr.decode()
        assert b'verified complete' not in stderr
        metadata = json.loads(Path(str(path) + '.meta.json').read_text())
        assert metadata['valid'] is False
    finally:
        device.close()


def test_q_cancellation_is_bounded_when_serial_becomes_silent(binary, tmp_path):
    device = Device(binary, tmp_path / 'capture.csv', fault='silent_stop').start()
    try:
        started = time.monotonic()
        _, stderr = device.finish()
        assert device.stopped.is_set()
        assert device.process.returncode != 0
        assert b'STOP confirmation timeout' in stderr
        assert time.monotonic() - started < 2
    finally:
        device.close()


def test_malformed_sample_reports_payload_without_accepting_data(binary, tmp_path):
    payload = 'S,0,lu,4096,0,16384,20'
    path = tmp_path / 'malformed.csv'
    device = Device(binary, path, count=1, records=[frame(payload)]).start()
    try:
        _, stderr = device.finish()
        assert device.process.returncode != 0
        assert f'Rejected sample payload (7 fields): {payload}'.encode() in stderr
        assert b'Captured 0 samples; Malformed sample frame' in stderr
        assert json.loads(Path(str(path) + '.meta.json').read_text())['valid'] is False
    finally:
        device.close()


def test_firmware_timing_diagnostics_are_reported_and_capture_stays_invalid(binary, tmp_path):
    path = tmp_path / 'timing.csv'
    device = Device(binary, path, fault='timing_detail').start()
    try:
        _, stderr = device.finish()
        assert device.process.returncode != 0
        assert b'Device error: TIMING_UNCERTAIN:window_us=239:limit_us=156:work=USB' in stderr
        assert b'verified complete' not in stderr
        assert json.loads(Path(str(path) + '.meta.json').read_text())['valid'] is False
    finally:
        device.close()


def test_crlf_frames_preserve_capture_and_stop(binary, tmp_path):
    path = tmp_path / 'crlf.csv'
    device = Device(binary, path, fault='crlf').start()
    try:
        _, stderr = device.finish()
        assert device.process.returncode == 0, stderr.decode()
        assert device.error is None
        assert device.stopped.is_set()
        metadata = json.loads(Path(str(path) + '.meta.json').read_text())
        assert metadata['valid'] is True
        assert metadata['samples'] == device.count
    finally:
        device.close()


@pytest.mark.parametrize('stop_signal', [signal.SIGINT, signal.SIGTERM])
def test_signals_request_and_verify_a_graceful_stop(binary, tmp_path, stop_signal):
    device = Device(binary, tmp_path / 'capture.csv').start()
    try:
        assert device.sent.wait(3)
        device.process.send_signal(stop_signal)
        _, stderr = device.process.communicate(timeout=3)
        assert device.process.returncode == 0, stderr.decode()
        assert device.stopped.is_set()
    finally:
        device.close()


def test_stalled_sensor_times_out_without_a_stop_key(binary, tmp_path):
    device = Device(binary, tmp_path / 'capture.csv', count=0).start()
    try:
        _, stderr = device.process.communicate(timeout=3)
        assert device.process.returncode != 0
        assert b'sample timeout' in stderr
    finally:
        device.close()


@pytest.mark.parametrize('rate', [800, 1600])
def test_real_plugin_capture_and_analyzer_accept_precise_wrapper_output(binary, tmp_path, rate):
    root = os.environ.get('PINPUT_PLUGIN_ROOT')
    if not root:
        pytest.skip('Set PINPUT_PLUGIN_ROOT to exercise the matching OctoPrint plugin')
    sys.path.insert(0, root)
    from octoprint_Pinput_Shaping.capture import AccelerometerCapture
    from octoprint_Pinput_Shaping.inputshaping_analyzer import InputShapingAnalyzer
    path = tmp_path / 'capture.csv'
    device = Device(binary, path, rate=rate, count=4096)
    capture = AccelerometerCapture('lis2dw', path, lis2dw_rate=rate)
    capture.driver, capture.port = str(binary), device.port
    device.thread.start()
    try:
        capture.start()
        assert device.sent.wait(3)
        capture.ensure_alive()
        capture.stop()
        assert device.error is None
        analyzer = InputShapingAnalyzer(tmp_path, path)
        analyzer.analyze()
        assert abs(analyzer.base_freq - 50) < 1
        assert analyzer.quality['timestamp_jitter'] < 0.001
        assert analyzer.sampling_rate == pytest.approx(rate)
    finally:
        capture.abort()
        device.close()
