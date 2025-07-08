import os
import subprocess
import pytest

ROOT_DIR = os.path.dirname(os.path.dirname(__file__))
BINARY = os.path.join(ROOT_DIR, 'lis2dwusb')

@pytest.fixture(scope='session', autouse=True)
def build_binary():
    if not os.path.exists(BINARY):
        subprocess.check_call(['gcc', '-Wall', '-pthread',
                               os.path.join('Linux_Wrapper', 'lis2dwusb.c'),
                               '-o', BINARY], cwd=ROOT_DIR)
    yield
    if os.path.exists(BINARY):
        os.remove(BINARY)

def test_help_message(build_binary):
    result = subprocess.run([BINARY, '-h'], capture_output=True, text=True)
    assert result.returncode != 0
    assert 'Usage:' in result.stderr

def test_invalid_frequency(build_binary):
    result = subprocess.run([BINARY, '-f', '50'], capture_output=True, text=True)
    assert result.returncode != 0
    assert 'Frequency must be between' in result.stderr
