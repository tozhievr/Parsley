"""Write a deterministic SHA256SUMS.txt for the files in one package directory."""
import hashlib
from pathlib import Path
import sys
root = Path(sys.argv[1])
files = sorted(p for p in root.iterdir() if p.is_file() and p.name != 'SHA256SUMS.txt' and not p.name.endswith('.sha256'))
with (root / 'SHA256SUMS.txt').open('w', encoding='ascii', newline='\n') as output:
    for path in files:
        with path.open('rb') as source:
            output.write(f'{hashlib.file_digest(source, "sha256").hexdigest()}  {path.name}\n')