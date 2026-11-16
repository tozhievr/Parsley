"""Install the official CMake release used by CI, verifying its published SHA256."""
import hashlib
import os
from pathlib import Path
import platform
import tarfile
import urllib.request
import zipfile

version = '4.5.0-rc1'
system = platform.system()
suffix = {'Linux': 'linux-x86_64.tar.gz', 'Darwin': 'macos-universal.tar.gz',
          'Windows': 'windows-x86_64.zip'}[system]
name = f'cmake-{version}-{suffix}'
base = f'https://github.com/Kitware/CMake/releases/download/v{version}/'
root = Path(os.environ['RUNNER_TEMP']) / 'parsley-cmake'
root.mkdir(parents=True, exist_ok=True)
archive = root / name
urllib.request.urlretrieve(base + name, archive)
checksums = urllib.request.urlopen(base + f'cmake-{version}-SHA-256.txt').read().decode()
expected = next(line.split()[0] for line in checksums.splitlines()
                if line.split()[-1].lstrip('*') == name)
with archive.open('rb') as source:
    actual = hashlib.file_digest(source, 'sha256').hexdigest()
if actual != expected:
    raise RuntimeError(f'CMake checksum mismatch: {actual} != {expected}')
if system == 'Windows':
    with zipfile.ZipFile(archive) as package:
        package.extractall(root)
else:
    with tarfile.open(archive) as package:
        package.extractall(root, filter='data')
folder = root / name.removesuffix('.tar.gz').removesuffix('.zip')
bin_dir = folder / ('CMake.app/Contents/bin' if system == 'Darwin' else 'bin')
if not (bin_dir / ('cmake.exe' if system == 'Windows' else 'cmake')).is_file():
    raise RuntimeError(f'CMake executable not found in {bin_dir}')
with open(os.environ['GITHUB_PATH'], 'a', encoding='utf-8') as output:
    output.write(str(bin_dir) + '\n')
print(f'Installed CMake {version}: {bin_dir}')
