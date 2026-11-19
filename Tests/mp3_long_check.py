#!/usr/bin/env python3
"""Compare a longer disk-backed MP3 import with FFmpeg (optional)."""
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

binary = os.path.abspath(sys.argv[1])
ffmpeg = shutil.which('ffmpeg')
if not ffmpeg:
    sys.exit('ffmpeg is required for this optional check')
with tempfile.TemporaryDirectory(prefix='carta long mp3 ') as directory:
    source = os.path.join(directory, 'source.mp3')
    actual = os.path.join(directory, 'actual.f32')
    reference = os.path.join(directory, 'reference.f32')
    signal = 'aevalsrc=0.35*sin(2*PI*440*t)|0.25*sin(2*PI*773*t):s=44100:d=180'
    if '--protected' in sys.argv[2:]:
        lame = shutil.which('lame')
        if not lame:
            sys.exit('lame is required for the protected check')
        pcm = os.path.join(directory, 'source.wav')
        subprocess.run([ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', signal, '-y', pcm], check=True, timeout=90)
        subprocess.run([lame, '--silent', '-p', '-b', '128', pcm, source], check=True, timeout=90)
    else:
        subprocess.run([ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', signal,
                        '-c:a', 'libmp3lame', '-q:a', '2', '-y', source], check=True, timeout=90)
    subprocess.run([binary, '--mp3-gapless-decode-mapped', source, actual], check=True, timeout=120)
    subprocess.run([ffmpeg, '-v', 'error', '-f', 'mp3', '-i', source, '-f', 'f32le', '-y', reference], check=True, timeout=90)
    total = 0.0
    samples = 0
    peak = 0.0
    with open(actual, 'rb') as left, open(reference, 'rb') as right:
        while True:
            a, b = left.read(262144), right.read(262144)
            assert len(a) == len(b) and len(a) % 4 == 0
            if not a:
                break
            av = struct.unpack('<%df' % (len(a) // 4), a)
            bv = struct.unpack('<%df' % (len(b) // 4), b)
            for x, y in zip(av, bv):
                difference = abs(x-y)
                assert math.isfinite(difference)
                total += difference * difference
                peak = max(peak, difference)
            samples += len(av)
    assert samples == 44100 * 180 * 2, samples
    rms = math.sqrt(total / samples)
    assert rms < 0.0001, rms
    print('Three-minute stereo MP3 matches FFmpeg:', samples // 2, 'frames; RMS', rms, 'peak', peak)
