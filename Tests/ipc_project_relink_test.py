#!/usr/bin/env python3
"""Exercise verified project relinking through the real IPC client and editor."""
import hashlib
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

def command(*args, cwd=None):
    return subprocess.run([binary, 'ipc', *args], cwd=cwd, capture_output=True, text=True, timeout=8)

def call(*args, cwd=None):
    result = command(*args, cwd=cwd)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return json.loads(result.stdout)['result']

def wait_project():
    for _ in range(200):
        state = call('project', 'status')
        if state['state'] != 'running':
            return state
        time.sleep(0.05)
    raise AssertionError('project operation timed out')

def wav_payload(path):
    with open(path, 'rb') as stream:
        data = stream.read()
    offset = 12
    while offset + 8 <= len(data):
        tag, size = struct.unpack_from('<4sI', data, offset)
        offset += 8
        if tag == b'data':
            return data[offset:offset + size]
        offset += size + (size & 1)
    raise AssertionError('WAV data missing')

assert command('status').returncode != 0, 'Stop the existing Carta Studio IPC owner before this test'
with tempfile.TemporaryDirectory() as directory:
    originals, replacements, hashes = [], [], []
    for index, value in enumerate((4096, 8192)):
        source = os.path.join(directory, 'source ' + str(index) + '.wav')
        replacement = os.path.join(directory, 'moved ' + str(index) + '.wav')
        with wave.open(source, 'wb') as output:
            output.setnchannels(1); output.setsampwidth(2); output.setframerate(44100)
            output.writeframes(struct.pack('<h', value) * 1024)
        shutil.copyfile(source, replacement)
        with open(source, 'rb') as input_file:
            hashes.append(hashlib.sha256(input_file.read()).hexdigest())
        originals.append(source); replacements.append(replacement)
    project = os.path.join(directory, 'project.carta')
    process = subprocess.Popen([binary], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               env={**os.environ, 'CARTA_STUDIO_RECOVERY_DIR': directory})
    try:
        for _ in range(100):
            if command('status').returncode == 0:
                break
            assert process.poll() is None, 'Editor exited during startup'
            time.sleep(0.1)
        call('import', '--new-layer', *originals)
        for _ in range(200):
            state = call('status')
            if state['pending_imports'] == 0:
                break
            time.sleep(0.05)
        assert state['track_count'] == 2, state
        call('project', 'save-as', project)
        assert wait_project()['clean'] and not call('status')['dirty']
        original_tracks = call('tracks')
        # New saves are self-contained packages. Construct an old external-
        # source JSON project from its verified metadata to exercise relinking.
        with open(project, 'rb') as stream:
            assert stream.read(8) == b'CARTAP01'
            count, = struct.unpack('<I', stream.read(4))
            metadata = None
            for _ in range(count):
                name_length, payload_length = struct.unpack('<IQ', stream.read(12))
                name = stream.read(name_length).decode('utf-8')
                digest = stream.read(64).decode('ascii')
                payload = stream.read(payload_length)
                assert len(payload) == payload_length and hashlib.sha256(payload).hexdigest() == digest
                if name == 'Project.carta':
                    assert metadata is None
                    metadata = json.loads(payload)
            assert metadata is not None and not stream.read(1)
        for index, track in enumerate(metadata['tracks']):
            assert len(track['clips']) == 1 and track['clips'][0]['sourceSha256'] == hashes[index]
            track['clips'][0]['source'] = os.path.basename(originals[index])
        with open(project, 'w', encoding='utf-8') as stream:
            json.dump(metadata, stream)
        for path in originals:
            os.remove(path)

        call('project', 'open', project)
        missing = wait_project()
        assert missing['state'] == 'failed' and missing['relink_request'] == {'original': originals[0], 'sha256': hashes[0]}, missing
        assert call('tracks') == original_tracks and not call('status')['dirty']
        call('project', 'open', project, originals[0], replacements[1])
        wrong = wait_project()
        assert wrong['state'] == 'failed' and wrong['relink_request']['original'] == originals[0] and call('tracks') == original_tracks, wrong
        call('project', 'open', project, originals[0], replacements[0])
        missing = wait_project()
        assert missing['state'] == 'failed' and missing['relink_request'] == {'original': originals[1], 'sha256': hashes[1]} and call('tracks') == original_tracks, missing
        assert command('project', 'open', project, originals[0]).returncode != 0
        assert command('project', 'open', project, originals[0], replacements[0], originals[0], replacements[1]).returncode != 0
        unrelated = os.path.join(directory, 'unrelated.wav')
        call('project', 'open', project, originals[0], replacements[0], originals[1], replacements[1], unrelated, replacements[0])
        invalid = wait_project()
        assert invalid['state'] == 'failed' and 'does not belong' in invalid['error'] and call('tracks') == original_tracks, invalid

        # The binary resolves every project and mapping path relative to the client.
        call('project', 'open', os.path.basename(project), os.path.basename(originals[0]), os.path.basename(replacements[0]),
             os.path.basename(originals[1]), os.path.basename(replacements[1]), cwd=directory)
        opened = wait_project(); state = call('status'); tracks = call('tracks')
        assert opened['state'] == 'complete' and opened['relinked'] and state['dirty'], (opened, state)
        assert [os.path.realpath(track['clips'][0]['source']) for track in tracks] == [os.path.realpath(path) for path in replacements], tracks
        rendered = os.path.join(directory, 'relinked mix.wav'); call('export', 'wav', rendered)
        for _ in range(200):
            operation = call('status')['export']
            if operation['state'] != 'running':
                break
            time.sleep(0.05)
        assert operation['state'] == 'complete', operation
        payload = wav_payload(rendered); left, right = struct.unpack_from('<ff', payload)
        assert abs(left - 0.375) < 1e-6 and abs(right - 0.375) < 1e-6, (left, right)
        call('project', 'save'); assert wait_project()['clean']
        call('project', 'open', project); reopened = wait_project()
        assert reopened['state'] == 'complete' and not reopened['relinked'] and not call('status')['dirty'], reopened
        assert [os.path.realpath(track['clips'][0]['source']) for track in call('tracks')] == [os.path.realpath(path) for path in replacements]
        print('IPC project relinking passed; editor stopped')
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
