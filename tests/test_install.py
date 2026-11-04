"""Stage installations without root or access to physical devices."""
import os
from pathlib import Path
import shutil
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]


def test_install_rebuilds_a_newer_incompatible_binary(tmp_path):
    wrapper = tmp_path / 'Linux_Wrapper'
    include = tmp_path / 'include'
    wrapper.mkdir()
    include.mkdir()
    for name in ('Makefile', 'lis2dwusb.c'):
        shutil.copy2(ROOT / 'Linux_Wrapper' / name, wrapper / name)
    shutil.copy2(ROOT / 'include/lis2dw_protocol.h', include)
    source_time = time.time() - 3600
    for source in (wrapper / 'lis2dwusb.c', include / 'lis2dw_protocol.h'):
        os.utime(source, (source_time, source_time))

    binary = wrapper / 'lis2dwusb'
    binary.write_bytes(b'copied incompatible executable\n')
    binary.chmod(0o755)

    staging = tmp_path / 'staging'
    subprocess.run(['make', '-C', str(wrapper), 'install',
                    f'DESTDIR={staging}', 'PREFIX=/usr/local'], check=True)
    installed = staging / 'usr/local/bin/lis2dwusb'
    result = subprocess.run([str(installed), '-V'], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert 'protocol 2' in result.stdout
