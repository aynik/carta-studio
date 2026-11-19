#!/usr/bin/env python3
"""Optional WAV interoperability check; requires ffmpeg, ffprobe and a desktop."""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave

binary = os.path.abspath(sys.argv[1])
ffmpeg, ffprobe = shutil.which('ffmpeg'), shutil.which('ffprobe')
if not ffmpeg or not ffprobe:
    sys.exit('ffmpeg and ffprobe are required for this optional check')


def command(*args):
    return subprocess.run([binary, 'ipc', *args], capture_output=True, text=True, timeout=8)


def call(*args):
    result = command(*args)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return json.loads(result.stdout)['result']


def wait_for(read, ready):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        value = read()
        if ready(value):
            return value
        time.sleep(0.05)
    raise AssertionError(('Timed out', value))


assert command('status').returncode != 0, 'Stop the existing Carta Studio IPC owner before this test'
with tempfile.TemporaryDirectory(prefix='carta wav interoperability ') as directory:
    source = os.path.join(directory, 'source.wav')
    # Asymmetric, even-valued channels include positive, negative and zero samples.
    frames = [(((i * 137) % 28001 - 14000) * 2, ((i * 313) % 24001 - 12000) * 2) for i in range(8273)]
    frames[0] = (16384, -16384)
    with wave.open(source, 'wb') as stream:
        stream.setnchannels(2); stream.setsampwidth(2); stream.setframerate(44100)
        stream.writeframes(b''.join(struct.pack('<hh', *frame) for frame in frames))
    process = subprocess.Popen([binary, source], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               env={**os.environ, 'CARTA_STUDIO_RECOVERY_DIR': directory})
    try:
        def startup():
            result = command('status')
            if result.returncode:
                assert process.poll() is None, 'Editor exited during startup'
                return None
            return json.loads(result.stdout)['result']

        wait_for(startup, lambda state: state is not None and state['pending_imports'] == 0 and state['track_count'] == 1)
        for channels in (2, 1):
            call('master', 'stereo' if channels == 2 else 'mono')
            expected = [sample / 32768 for frame in frames for sample in (frame if channels == 2 else ((frame[0] + frame[1]) / 2,))]
            for encoding, codec, tolerance in [('float32', 'pcm_f32le', 0), ('pcm16', 'pcm_s16le', 2 / 32768), ('pcm24', 'pcm_s24le', 2 / 8388608)]:
                for dither in ((False,) if encoding == 'float32' else (False, True)):
                    path = os.path.join(directory, f'{channels}-{encoding}-{dither}.wav')
                    call('export', 'wav', path, encoding, *(['dither'] if dither else []))
                    result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
                    assert result['state'] == 'complete', result
                    probe = subprocess.run([ffprobe, '-v', 'error', '-select_streams', 'a:0', '-show_entries',
                                            'stream=codec_name,sample_rate,channels', '-of', 'json', path],
                                           check=True, capture_output=True, text=True)
                    info = json.loads(probe.stdout)['streams'][0]
                    assert info['codec_name'] == codec and int(info['sample_rate']) == 44100 and info['channels'] == channels, info
                    raw = subprocess.run([ffmpeg, '-v', 'error', '-i', path, '-f', 'f32le', '-acodec', 'pcm_f32le', '-'],
                                         check=True, capture_output=True).stdout
                    assert len(raw) == len(expected) * 4, (path, len(raw), len(expected))
                    decoded = struct.unpack('<' + str(len(expected)) + 'f', raw)
                    maximum = max(abs(actual - reference) for actual, reference in zip(decoded, expected))
                    assert maximum <= (tolerance if dither else 0), (path, maximum, tolerance)
                    print(channels, encoding, 'dither' if dither else 'no dither', 'max error', maximum)
        call('master', 'stereo')
        for sample in (4095, 4096, 8193):
            call('marker', 'add', str(sample), 'Boundary ' + str(sample))
        for encoding in ('pcm16', 'pcm24'):
            parent = os.path.join(directory, 'batch-' + encoding)
            os.mkdir(parent)
            call('export', 'tracks', parent, encoding, 'dither')
            batch = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
            assert batch['state'] == 'complete' and batch['completed_tracks'] == 4 and batch['manifest_written'], batch
            with open(os.path.join(batch['output_folder'], 'Carta tracks.json'), encoding='utf-8') as stream:
                manifest = json.load(stream)
            assert [(row['startSample'], row['endSample']) for row in manifest['tracks']] == [(0, 4095), (4095, 4096), (4096, 8193), (8193, len(frames))], manifest
            def decode(path):
                return subprocess.run([ffmpeg, '-v', 'error', '-i', path, '-f', 'f32le', '-acodec', 'pcm_f32le', '-'],
                                      check=True, capture_output=True).stdout
            tracks = b''.join(decode(os.path.join(batch['output_folder'], 'Track %02d.wav' % (index + 1))) for index in range(4))
            assert tracks == decode(os.path.join(directory, '2-' + encoding + '-True.wav')), encoding
        print('Dithered marker tracks match the full master across block boundaries')
        call('disc', 'sync', 'markers')
        call('disc', 'track', 'rename', '0', '夜の音')
        call('disc', 'track', 'duplicate', '0')
        review = os.path.join(directory, 'disc review.wav')
        call('export', 'disc-review', review)
        result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
        assert result['state'] == 'complete' and result['cues'], result
        probe = subprocess.run([ffprobe, '-v', 'error', '-show_chapters', '-of', 'json', review],
                               check=True, capture_output=True, text=True)
        chapters = json.loads(probe.stdout)['chapters']
        assert [int(chapter['start']) for chapter in chapters] == [0, 4095, 8190, 8191, 12288], chapters
        assert [chapter.get('tags', {}).get('title') for chapter in chapters[:2]] == ['夜の音', '夜の音'], chapters
        decoded = decode(review)
        master = decode(os.path.join(directory, '2-float32-False.wav'))
        assert decoded == master[:4095 * 8] * 2 + master[4095 * 8:], 'review WAV must follow program order'
        print('FFmpeg reads repeated program audio and Unicode track cues')
        call('track', 'set', '0', 'gain', '2')
        call('select', 'range', '0', '0', '0', '1')
        for encoding in ('pcm16', 'pcm24'):
            path = os.path.join(directory, 'positive-full-scale-' + encoding + '.wav')
            call('export', 'selection', 'full', path, encoding)
            result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
            assert result['state'] == 'failed' and not os.path.exists(path), result
            assert [row['clipped_samples'] for row in result['preflight']] == [1, 0], result
            assert result['preflight'][0]['first_clipped_sample'] == 0, result
            call('export', 'selection', 'full', path, encoding, 'allow-clip')
            result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
            assert result['state'] == 'complete', result
            raw = subprocess.run([ffmpeg, '-v', 'error', '-i', path, '-f', 'f32le', '-acodec', 'pcm_f32le', '-'],
                                 check=True, capture_output=True).stdout
            left, right = struct.unpack('<ff', raw)
            scale = 32768 if encoding == 'pcm16' else 8388608
            assert left == (scale - 1) / scale and right == -1.0, (encoding, left, right)
        print('Positive full-scale saturation reported and confirmed for both PCM formats')
        print('FFmpeg WAV interoperability check passed')
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
