#!/usr/bin/env python3
"""Compare public JS and native Carta1 CLIs on one deterministic stereo file."""
import hashlib
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave

if len(sys.argv) != 2:
    raise SystemExit('Usage: compare_cli.py /absolute/path/to/native-carta1')
native = Path(sys.argv[1]).resolve(strict=True)
studio = Path(__file__).resolve().parents[2]
js = studio / 'third_party/carta1/bin/cli.js'
with tempfile.TemporaryDirectory(prefix='carta1-public-cli-') as directory:
    root = Path(directory)
    source = root / 'input.wav'
    with wave.open(str(source), 'wb') as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(44100)
        for frame in range(44100 * 3):
            left = round(12000 * math.sin(2 * math.pi * 440 * frame / 44100))
            right = round(9000 * math.sin(2 * math.pi * 997 * frame / 44100))
            output.writeframesraw(struct.pack('<hh', left, right))
    for name, cli in [('js', ['node', str(js)]), ('native', [str(native)])]:
        target = root / name
        target.mkdir()
        for action, input_file, output_file in [
            ('encode', source, target / 'audio.aea'),
            ('decode', target / 'audio.aea', target / 'audio.wav'),
        ]:
            subprocess.run(cli + ['--' + action, str(input_file), str(output_file)],
                           check=True, capture_output=True, timeout=120)
    for extension in ('aea', 'wav'):
        reference = (root / 'js' / ('audio.' + extension)).read_bytes()
        actual = (root / 'native' / ('audio.' + extension)).read_bytes()
        if actual != reference:
            raise SystemExit(f'{extension}: public CLI output differs')
        print(f'{extension}: {len(actual)} matching bytes, SHA-256 {hashlib.sha256(actual).hexdigest()}')
