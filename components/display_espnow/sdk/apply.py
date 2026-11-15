"""Fail closed on upstream drift; only replace the two reviewed pinned SDK files."""
import hashlib
import json
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
for name, change in json.loads(pathlib.Path(__file__).with_name('overlay.json').read_text()).items():
    target = root / name
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    if digest == change['patched_sha256']:
        continue
    if digest != change['sha256']:
        raise SystemExit(f'Unrecognized pinned ESP-NOW source: {name}')
    target.write_text(change['content'])
