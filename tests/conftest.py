"""Compile into temporary directories; tests never open physical devices."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope='session')
def binary(tmp_path_factory):
    output = tmp_path_factory.mktemp('wrapper') / 'lis2dwusb'
    subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                    str(ROOT / 'Linux_Wrapper/lis2dwusb.c'), '-o', str(output)], check=True)
    return output
