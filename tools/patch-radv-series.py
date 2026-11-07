#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The driver patches in tools/patch-radv-series, on the pinned Mesa.

tools/deps.json pins the Mesa revision and tools/build-radv.sh exports that revision. Patches that
are too big for tools/patch-radv-wsi.py's anchors go here, as plain git patches with their authors
in their headers (exploitizm's mesh shaders as compute and Oliv3rmoon's stall dump are not ours).

They apply in name order to a copy of the files they name, in .deps/work/radv-series, and the
result goes into the build export. tools/patch-radv-wsi.py and tools/patch-radv-memory-report.py
then start from that copy instead of the pinned checkout, so they run after this and keep their
anchors. Nothing here touches the Mesa checkout, and the pin, the driver's build id and with it the
cache UUID stay what they were.
"""
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
series = sorted((root/'tools/patch-radv-series').glob('*.patch'))
pristine = root.parent/'mihawk-mesa-review'
overlay = root.parent/'mihawk-vulkan-review/.deps/work/radv-series'
export = root.parent/'mihawk-vulkan-review/.deps/work/radv-src'
if not series:
    sys.exit('tools/patch-radv-series holds no patches: tools/patch-radv-wsi.py anchors need them')

names = set()
for patch in series:
    for line in patch.read_text(errors='replace').splitlines():
        if line.startswith('diff --git a/'):
            names.add(line.split(' b/', 1)[1])
shutil.rmtree(overlay, ignore_errors=True)
overlay.mkdir(parents=True)
for name in names:
    source = pristine/name
    if source.exists():
        (overlay/name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, overlay/name)
# .deps sits inside the PS5_Vulkan checkout, and git apply run in a checkout's subfolder skips a git
# patch without a word: so from the checkout's top, and a patch that changes nothing stops the build.
def files():
    return {name: (overlay/name).read_bytes() for name in names if (overlay/name).exists()}

for patch in series:
    before = files()
    subprocess.run(['git', '-C', str(root.parent/'mihawk-vulkan-review'), 'apply', '--whitespace=nowarn',
                    '--directory=.deps/work/radv-series', str(patch)], check=True)
    assert files() != before, f'{patch.name} changed nothing'
    print(f'RADV series: {patch.name}')
missing = [name for name in names if not (overlay/name).exists()]
assert not missing, f'The series did not make {missing}'

changed = False
for name in sorted(names):
    text = (overlay/name).read_bytes()
    target = export/name
    if not target.exists() or target.read_bytes() != text:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(text)
        changed = True
if changed:
    # As tools/patch-radv-wsi.py: Ninja keeps every unchanged object and rebuilds these files.
    receipt = root.parent/'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if receipt.exists():
        receipt.replace(receipt.with_suffix('.base.txt'))
print(f'RADV series: {len(series)} patches, {len(names)} files, changed={changed}')
