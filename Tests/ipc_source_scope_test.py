#!/usr/bin/env python3
"""Exercise scoped source selection, grouped editing and saved output via IPC."""
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
    return subprocess.run([binary, 'ipc', *map(str, args)], capture_output=True, text=True, timeout=8)


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


def audio_data(path):
    with open(path, 'rb') as stream:
        raw = stream.read()
    offset = 12
    while offset + 8 <= len(raw):
        tag, size = struct.unpack_from('<4sI', raw, offset)
        offset += 8
        if tag == b'data':
            return struct.unpack_from('<%df' % (size // 4), raw, offset)
        offset += size + (size & 1)
    raise AssertionError('WAV data chunk missing')


assert command('status').returncode != 0, 'Stop the existing Carta Studio IPC owner before this test'
with tempfile.TemporaryDirectory(prefix='carta source scope ') as directory:
    samples_a = [((i * 19) % 6001 - 3000) for i in range(1000)]
    samples_b = [((i * 37) % 4001 - 2000) for i in range(1000)]
    paths = []
    for name, samples in [('repeated.wav', samples_a), ('linked.wav', samples_b)]:
        path = os.path.join(directory, name)
        with wave.open(path, 'wb') as output:
            output.setnchannels(1); output.setsampwidth(2); output.setframerate(44100)
            output.writeframes(struct.pack('<1000h', *samples))
        paths.append(path)
    process = subprocess.Popen([binary], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               env={**os.environ, 'CARTA_STUDIO_RECOVERY_DIR': directory})
    try:
        def startup():
            result = command('status')
            if result.returncode:
                assert process.poll() is None, 'Editor exited during startup'
                return None
            return json.loads(result.stdout)['result']

        wait_for(startup, lambda state: state is not None)
        for index, path in enumerate([paths[0], paths[1], paths[0]]):
            call('import', path)
            wait_for(lambda: call('status'), lambda state: state['pending_imports'] == 0 and state['track_count'] == index + 1)
        call('marker', 'add', 500, 'Track two')
        call('select', 'clips', 1, 0, 2, 0)
        call('edit', 'group')
        group = call('tracks')[1]['clips'][0]['group_id']
        assert group and call('tracks')[2]['clips'][0]['group_id'] == group
        call('select', 'tracks', 100, 200, 0, 2)
        before = call('status')
        call('select', 'focus-clip', 2, 0)
        call('select', 'source-clips', 'range')
        selected = call('status')
        assert selected['selected_tracks'] == [0, 2] and selected['selection'] == before['selection'], selected
        assert selected['selected_clips'] == [{'track': 0, 'clip': 0}, {'track': 1, 'clip': 0}, {'track': 2, 'clip': 0}], selected
        assert selected['selected_track'] == 2 and selected['selected_clip'] == 0, selected
        call('clip', 'envelope-selected', '0.5', 'keep', 'keep')
        assert [track['clips'][0]['gain'] for track in call('tracks')] == [0.5, 0.5, 0.5]

        def export(name, gain):
            output = os.path.join(directory, name)
            call('export', 'wav', output, 'float32')
            result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
            assert result['state'] == 'complete', result
            actual = audio_data(output)
            assert len(actual) == 2000, len(actual)
            for index, (a, b) in enumerate(zip(samples_a, samples_b)):
                expected = ((a * 2 + b) / 32768) * gain
                assert actual[index * 2] == expected and actual[index * 2 + 1] == expected, (index, actual[index * 2:index * 2 + 2], expected)

        export('edited.wav', 0.5)
        call('edit', 'undo')
        assert [track['clips'][0]['gain'] for track in call('tracks')] == [1.0, 1.0, 1.0]
        export('restored.wav', 1.0)
        project = os.path.join(directory, 'source-scope.carta')
        call('project', 'save-as', project)
        saved = wait_for(lambda: call('project', 'status'), lambda state: state['state'] != 'running')
        assert saved['state'] == 'complete' and saved['clean'], saved
        call('project', 'open', project)
        opened = wait_for(lambda: call('project', 'status'), lambda state: state['state'] != 'running')
        assert opened['state'] == 'complete', opened
        reopened = call('tracks')
        assert reopened[1]['clips'][0]['group_id'] == group and reopened[2]['clips'][0]['group_id'] == group
        assert call('markers')[0]['sample'] == 500
        export('reopened.wav', 1.0)
        print('Scoped source selection, grouped gain, undo and reopen passed; editor stopped')
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
