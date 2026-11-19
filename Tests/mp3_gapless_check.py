#!/usr/bin/env python3
"""Compare the experimental MP3 timing inspector with FFmpeg-generated files."""
import os
import math
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

binary = os.path.abspath(sys.argv[1])
ffmpeg = shutil.which('ffmpeg')
if not ffmpeg:
    sys.exit('ffmpeg is required for this optional check')

with tempfile.TemporaryDirectory(prefix='carta mp3 timing ') as directory:
    checked = 0
    for rate in (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000):
        for channels in (1, 2):
            for frames in (100, 1152, 5000, 22050, 96001):
                for encoding in ('cbr', 'vbr'):
                    source = os.path.join(directory, 'source.wav')
                    encoded = os.path.join(directory, 'encoded.mp3')
                    samples = [((frame * (ch + 3) + ch * 71) % 20001 - 10000) for frame in range(frames) for ch in range(channels)]
                    with wave.open(source, 'wb') as audio:
                        audio.setnchannels(channels); audio.setsampwidth(2); audio.setframerate(rate)
                        audio.writeframes(struct.pack('<%dh' % len(samples), *samples))
                    settings = ['-b:a', '64k' if rate < 32000 else '128k'] if encoding == 'cbr' else ['-q:a', '3']
                    subprocess.run([ffmpeg, '-v', 'error', '-i', source, '-c:a', 'libmp3lame', *settings, '-y', encoded], check=True, timeout=15)
                    result = subprocess.run([binary, '--mp3-gapless-info', encoded], capture_output=True, text=True, timeout=15)
                    assert result.returncode == 0, (rate, channels, frames, encoding, result.stderr)
                    detected_rate, detected_channels, raw, leading, count = map(int, result.stdout.split())
                    decoded_result = subprocess.run([ffmpeg, '-v', 'error', '-f', 'mp3', '-i', encoded, '-f', 'f32le', '-acodec', 'pcm_f32le', '-'], capture_output=True, timeout=15)
                    assert decoded_result.returncode == 0, (rate, channels, frames, encoding, decoded_result.stderr.decode(errors='replace'))
                    decoded = decoded_result.stdout
                    assert detected_rate == rate and detected_channels == channels and count == len(decoded) // (channels * 4) == frames, (rate, channels, frames, encoding, result.stdout, len(decoded))
                    assert 0 <= leading and leading + count <= raw
                    if '--decode' in sys.argv[2:]:
                        raw_path = os.path.join(directory, 'decoded.f32')
                        expected = struct.unpack('<%df' % (len(decoded) // 4), decoded)
                        for mode in ('--mp3-gapless-decode', '--mp3-gapless-decode-mapped'):
                            if os.path.exists(raw_path):
                                os.unlink(raw_path)
                            imported = subprocess.run([binary, mode, encoded, raw_path], capture_output=True, text=True, timeout=30)
                            assert imported.returncode == 0, (rate, channels, frames, encoding, mode, imported.stderr)
                            with open(raw_path, 'rb') as audio:
                                raw_samples = audio.read()
                            assert len(raw_samples) == len(decoded), (rate, channels, frames, encoding, mode, len(raw_samples), len(decoded))
                            actual = struct.unpack('<%df' % (len(raw_samples) // 4), raw_samples)
                            rms = math.sqrt(sum((a - b) ** 2 for a, b in zip(actual, expected)) / len(actual))
                            assert rms < 0.0001, (rate, channels, frames, encoding, mode, rms)
                    checked += 1
    with open(encoded, 'rb') as audio:
        valid = audio.read()
    for name, damaged in (('truncated', valid[:-1]), ('concatenated', valid + valid)):
        path = os.path.join(directory, name + '.mp3')
        with open(path, 'wb') as audio:
            audio.write(damaged)
        result = subprocess.run([binary, '--mp3-gapless-info', path], capture_output=True, text=True, timeout=15)
        assert result.returncode != 0, name
        if '--decode' in sys.argv[2:]:
            result = subprocess.run([binary, '--mp3-gapless-decode', path, os.path.join(directory, name + '.f32')], capture_output=True, text=True, timeout=30)
            assert result.returncode != 0, (name, result.stdout)
    print('MP3 gapless timing matched FFmpeg for', checked, 'MPEG Layer III fixtures')
