#!/usr/bin/env python3
"""Exercise LAME timing-tag checksums across MPEG versions (optional)."""
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

binary = os.path.abspath(sys.argv[1])
lame = shutil.which('lame')
if not lame:
    sys.exit('lame is required for this optional check')
ffmpeg = shutil.which('ffmpeg')
if '--decode' in sys.argv[2:] and not ffmpeg:
    sys.exit('ffmpeg is required for decoded-sample checks')
with tempfile.TemporaryDirectory(prefix='carta lame tag ') as directory:
    checked = 0
    for rate in (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000):
        for channels in (1, 2):
            for protected in (False, True):
                for vbr in (False, True):
                    source = os.path.join(directory, 'source.wav')
                    encoded = os.path.join(directory, 'source.mp3')
                    with wave.open(source, 'wb') as audio:
                        audio.setparams((channels, 2, rate, 0, 'NONE', ''))
                        audio.writeframes(struct.pack('<%dh' % (channels*1000), *[(i % 16000 - 8000) for i in range(channels*1000)]))
                    options = ['-V', '3'] if vbr else ['-b', '64' if rate < 32000 else '128']
                    subprocess.run([lame, '--silent', *(['-p'] if protected else []), *options, source, encoded], check=True, timeout=20)
                    valid = subprocess.run([binary, '--mp3-gapless-info', encoded], capture_output=True, text=True, timeout=20)
                    assert valid.returncode == 0, (rate, channels, protected, vbr, valid.stderr)
                    assert list(map(int, valid.stdout.split()))[-1] == 1000, valid.stdout
                    if '--decode' in sys.argv[2:]:
                        reference = subprocess.run([ffmpeg, '-v', 'error', '-f', 'mp3', '-i', encoded, '-f', 'f32le', '-'], capture_output=True, check=True, timeout=20).stdout
                        assert len(reference) == channels*1000*4, (rate, channels, protected, vbr, len(reference))
                        expected = struct.unpack('<%df' % (len(reference)//4), reference)
                        for mode in ('--mp3-gapless-decode', '--mp3-gapless-decode-mapped'):
                            output = os.path.join(directory, 'decoded.f32')
                            if os.path.exists(output):
                                os.unlink(output)
                            decoded = subprocess.run([binary, mode, encoded, output], capture_output=True, text=True, timeout=20)
                            assert decoded.returncode == 0, (rate, channels, protected, vbr, mode, decoded.stderr)
                            with open(output, 'rb') as stream:
                                raw = stream.read()
                            assert len(raw) == len(reference), (rate, channels, protected, vbr, mode, len(raw))
                            actual = struct.unpack('<%df' % (len(raw)//4), raw)
                            rms = math.sqrt(sum((a-b)**2 for a, b in zip(actual, expected)) / len(actual))
                            assert rms < 0.0001, (rate, channels, protected, vbr, mode, rms)
                    with open(encoded, 'rb') as stream:
                        data = bytearray(stream.read())
                    side = (17 if channels == 1 else 32) if rate >= 32000 else (9 if channels == 1 else 17)
                    xing = 4 + side
                    assert data[xing:xing+4] in (b'Info', b'Xing')
                    flags = int.from_bytes(data[xing+4:xing+8], 'big')
                    tag = xing + 8 + sum(size for mask, size in ((1,4),(2,4),(4,100),(8,4)) if flags & mask)
                    assert data[tag:tag+4] == b'LAME' and data[tag+34:tag+36] != b'\0\0'
                    data[tag+22] ^= 1  # changes delay/padding but not the MPEG frame CRC
                    with open(encoded, 'wb') as stream:
                        stream.write(data)
                    broken = subprocess.run([binary, '--mp3-gapless-info', encoded], capture_output=True, text=True, timeout=20)
                    assert broken.returncode != 0 and 'timing tag checksum' in broken.stderr.lower(), (rate, channels, protected, vbr, broken.stderr)
                    checked += 1
    print('LAME timing checksums accepted valid and rejected damaged tags for', checked, 'fixtures')
