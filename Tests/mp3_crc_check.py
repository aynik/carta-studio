#!/usr/bin/env python3
"""Compare CRC-protected LAME imports with FFmpeg (optional)."""
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

binary = os.path.abspath(sys.argv[1])
lame, ffmpeg = shutil.which('lame'), shutil.which('ffmpeg')
if not lame or not ffmpeg:
    sys.exit('lame and ffmpeg are required for this optional check')
with tempfile.TemporaryDirectory(prefix='carta protected mp3 ') as directory:
    checked = 0
    for rate in (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000):
        for channels in (1, 2):
            for frames in (100, 5000, 44100):
                for encoding in ('cbr', 'vbr'):
                    source = os.path.join(directory, 'source.wav')
                    encoded = os.path.join(directory, 'source.mp3')
                    samples = [((frame * (ch + 3) + ch * 71) % 20001 - 10000) for frame in range(frames) for ch in range(channels)]
                    with wave.open(source, 'wb') as audio:
                        audio.setnchannels(channels); audio.setsampwidth(2); audio.setframerate(rate)
                        audio.writeframes(struct.pack('<%dh' % len(samples), *samples))
                    options = ['-b', '64' if rate < 32000 else '128'] if encoding == 'cbr' else ['-V', '3']
                    subprocess.run([lame, '--silent', '-p', *options, source, encoded], check=True, timeout=20)
                    with open(encoded, 'rb') as stream:
                        data = stream.read()
                    assert data[0] == 0xff and data[1] & 1 == 0, (rate, channels, frames, encoding)
                    # Exercise nonzero first-frame offsets as well as bare streams.
                    if frames == 5000:
                        with open(encoded, 'wb') as stream:
                            stream.write(b'ID3\x03\x00\x00\x00\x00\x00\x00' + data)
                    info = subprocess.run([binary, '--mp3-gapless-info', encoded], capture_output=True, text=True, timeout=20)
                    assert info.returncode == 0, (rate, channels, frames, encoding, info.stderr)
                    detected_rate, detected_channels, _, _, count = map(int, info.stdout.split())
                    assert (detected_rate, detected_channels, count) == (rate, channels, frames), (rate, channels, frames, encoding, info.stdout)
                    reference = subprocess.run([ffmpeg, '-v', 'error', '-f', 'mp3', '-i', encoded, '-f', 'f32le', '-'], capture_output=True, check=True, timeout=20).stdout
                    assert len(reference) == frames * channels * 4, (rate, channels, frames, encoding, len(reference))
                    expected = struct.unpack('<%df' % (len(reference)//4), reference)
                    for mode in ('--mp3-gapless-decode', '--mp3-gapless-decode-mapped'):
                        output = os.path.join(directory, 'decoded.f32')
                        if os.path.exists(output):
                            os.unlink(output)
                        result = subprocess.run([binary, mode, encoded, output], capture_output=True, text=True, timeout=30)
                        assert result.returncode == 0, (rate, channels, frames, encoding, mode, result.stderr)
                        with open(output, 'rb') as stream:
                            raw = stream.read()
                        assert len(raw) == len(reference), (rate, channels, frames, encoding, mode, len(raw), len(reference))
                        actual = struct.unpack('<%df' % (len(raw)//4), raw)
                        rms = math.sqrt(sum((a-b)**2 for a, b in zip(actual, expected)) / len(actual))
                        assert rms < 0.0001, (rate, channels, frames, encoding, mode, rms)
                    checked += 1
    # Damage the stored CRC and the first audio frame's protected side data.
    header = int.from_bytes(data[:4], 'big')
    bitrate_index, rate_index = (header >> 12) & 15, (header >> 10) & 3
    bitrates = (0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0)
    first_frame = 144000 * bitrates[bitrate_index] // (44100,48000,32000)[rate_index] + ((header >> 9) & 1)
    for name, offset in (('info-crc', 4), ('audio-crc', first_frame + 4), ('side-info', first_frame + 6)):
        broken = bytearray(data); broken[offset] ^= 1
        path = os.path.join(directory, name + '.mp3')
        with open(path, 'wb') as stream:
            stream.write(broken)
        result = subprocess.run([binary, '--mp3-gapless-info', path], capture_output=True, text=True, timeout=20)
        assert result.returncode != 0 and 'checksum' in result.stderr.lower(), (name, result.stdout, result.stderr)
        result = subprocess.run([binary, '--mp3-gapless-decode', path, os.path.join(directory, name + '.f32')], capture_output=True, text=True, timeout=20)
        assert result.returncode != 0, (name, result.stdout)
    xing = 4 + 32  # final fixture is MPEG-1 stereo
    flags = int.from_bytes(data[xing+4:xing+8], 'big')
    tag = xing + 8 + sum(size for mask, size in ((1,4),(2,4),(4,100),(8,4)) if flags & mask)
    assert data[tag:tag+4] == b'LAME' and data[tag+34:tag+36] != b'\0\0'
    for name, offset in (('timing-tag', tag+22), ('timing-tag-crc', tag+35)):
        broken = bytearray(data); broken[offset] ^= 1
        path = os.path.join(directory, name + '.mp3')
        with open(path, 'wb') as stream:
            stream.write(broken)
        result = subprocess.run([binary, '--mp3-gapless-info', path], capture_output=True, text=True, timeout=20)
        assert result.returncode != 0 and 'timing tag checksum' in result.stderr.lower(), (name, result.stdout, result.stderr)
        result = subprocess.run([binary, '--mp3-gapless-decode', path, os.path.join(directory, name + '.f32')], capture_output=True, text=True, timeout=20)
        assert result.returncode != 0, (name, result.stdout)
    # A valid CRC does not make a nonempty protected metadata frame safe to
    # normalize: its side information may refer to audio.
    nonempty = bytearray(data); nonempty[6] = 1; crc = 0xffff
    for value in nonempty[2:4] + nonempty[6:38]:
        for bit in range(7, -1, -1):
            high = ((crc >> 15) ^ (value >> bit)) & 1
            crc = ((crc << 1) & 0xffff) ^ (0x8005 if high else 0)
    nonempty[4:6] = crc.to_bytes(2, 'big')
    tag_crc = 0
    for value in nonempty[:tag+34]:
        tag_crc ^= value
        for _ in range(8):
            tag_crc = (tag_crc >> 1) ^ (0xa001 if tag_crc & 1 else 0)
    nonempty[tag+34:tag+36] = tag_crc.to_bytes(2, 'big')
    path = os.path.join(directory, 'nonempty-info.mp3')
    with open(path, 'wb') as stream:
        stream.write(nonempty)
    result = subprocess.run([binary, '--mp3-gapless-info', path], capture_output=True, text=True, timeout=20)
    assert result.returncode != 0 and 'information frame' in result.stderr, (result.stdout, result.stderr)
    print('CRC-protected LAME imports matched FFmpeg for', checked, 'fixtures; damaged protected fields rejected')
