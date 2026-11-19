#!/usr/bin/env python3
"""Exercise command-line import and project/export paths in a real editor."""
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import wave

binary = os.path.abspath(sys.argv[1])


def command(*args):
    return subprocess.run([binary, 'ipc', *args], text=True, capture_output=True, timeout=8)


def call(*args):
    result = command(*args)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return json.loads(result.stdout)['result']


def wait_for(read, ready, message):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        state = read()
        if ready(state):
            return state
        time.sleep(0.05)
    raise AssertionError((message, state))


def audio_data(path):
    with open(path, 'rb') as file:
        data = file.read()
    offset = 12
    while offset + 8 <= len(data):
        tag, length = struct.unpack_from('<4sI', data, offset)
        offset += 8
        if tag == b'data':
            return data[offset:offset + length]
        offset += length + (length & 1)
    raise AssertionError('WAV data chunk missing')


assert command('status').returncode != 0, 'Stop the existing Carta Studio IPC owner before this test'
with tempfile.TemporaryDirectory(prefix='carta paths ') as directory:
    folder = os.path.join(directory, '音楽 & mixes')
    os.mkdir(folder)
    paths = [os.path.join(folder, '夜 の "曲".wav'), os.path.join(folder, 'café 音.wav')]
    for path, value in zip(paths, (4096, 8192)):
        with wave.open(path, 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(44100)
            audio.writeframes(struct.pack('<h', value) * 1000)
    project = os.path.join(folder, 'マスター project.carta')
    output = os.path.join(folder, '完成 master.wav')
    process = subprocess.Popen([binary, os.path.relpath(paths[0], directory), paths[1]], cwd=directory,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               env={**os.environ, 'CARTA_STUDIO_RECOVERY_DIR': directory})
    try:
        def startup():
            result = command('status')
            if result.returncode != 0:
                assert process.poll() is None, 'Editor exited during startup'
                return None
            return json.loads(result.stdout)['result']

        wait_for(startup, lambda state: state is not None and state['pending_imports'] == 0 and state['track_count'] == 1 and state['md_disc']['track_count'] == 2,
                 'command-line imports did not complete')
        tracks = call('tracks')
        assert len(tracks) == 1 and len(tracks[0]['clips']) == 2, tracks
        assert all(os.path.samefile(clip['source'], path) for clip, path in zip(tracks[0]['clips'], paths)), tracks
        call('edit', 'rename-clip', '0', '0', '夜の音 🎵')
        call('marker', 'add', '500', '次の曲')
        call('project', 'save-as', project)
        saved = wait_for(lambda: call('project', 'status'), lambda state: state['state'] != 'running', 'save timed out')
        assert saved['state'] == 'complete' and saved['clean'] and os.path.samefile(saved['path'], project), saved
        call('project', 'open', project)
        opened = wait_for(lambda: call('project', 'status'), lambda state: state['state'] != 'running', 'open timed out')
        assert opened['state'] == 'complete', opened
        tracks = call('tracks')
        assert len(tracks) == 1 and len(tracks[0]['clips']) == 2 and all(audio_data(clip['source']) == audio_data(path) for clip, path in zip(tracks[0]['clips'], paths)), tracks
        assert tracks[0]['clips'][0]['custom_name'] == '夜の音 🎵' and call('markers')[0]['name'] == '次の曲'
        assert call('marker', 'focus-adjacent', 'next') == 0 and call('status')['selected_marker'] == 0
        assert 'Marker sample 500' in call('status')['message']
        call('export', 'wav', output, 'float32')
        exported = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running', 'export timed out')
        assert exported['state'] == 'complete', exported
        data = audio_data(output)
        assert len(data) == 2000 * 2 * 4
        for sample in range(2000):
            left, right = struct.unpack_from('<ff', data, sample * 8)
            expected = 0.125 if sample < 1000 else 0.25
            assert abs(left - expected) < 1e-6 and abs(right - expected) < 1e-6, (sample, left, right, expected)
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
print('Unicode and spaced-path smoke test passed; editor stopped')
