import subprocess

import pytest


def test_help_and_version_do_not_open_a_device(binary):
    result = subprocess.run([binary, '-h'], capture_output=True, text=True)
    assert result.returncode == 0
    assert 'Usage:' in result.stderr
    result = subprocess.run([binary, '-V'], capture_output=True, text=True)
    assert result.returncode == 0
    assert 'protocol 2' in result.stdout


@pytest.mark.parametrize('frequency', ['50', '500', '1000', '0', '-200', '1600x', 'nan', '99999999999999999999999'])
def test_invalid_frequencies_fail_before_serial_access(binary, frequency):
    result = subprocess.run([binary, '-f', frequency], capture_output=True, text=True)
    assert result.returncode != 0
    assert 'Frequency must be one of' in result.stderr


@pytest.mark.parametrize('flag,value', [('-t', '0'), ('-t', 'nan'), ('-t', 'inf'), ('-d', '-1'), ('-d', '2seconds')])
def test_invalid_durations_fail_before_serial_access(binary, flag, value):
    result = subprocess.run([binary, flag, value], capture_output=True, text=True)
    assert result.returncode != 0
    assert 'Usage:' in result.stderr


def test_existing_capture_is_never_overwritten(binary, tmp_path):
    output = tmp_path / 'capture.csv'
    output.write_text('keep this measurement')
    result = subprocess.run([binary, '-s', output], capture_output=True, text=True)
    assert result.returncode != 0
    assert output.read_text() == 'keep this measurement'
