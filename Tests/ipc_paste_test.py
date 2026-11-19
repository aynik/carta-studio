#!/usr/bin/env python3
"""Check independent-row clipboard policies through a real IPC editor."""
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
    return subprocess.run([binary, 'ipc', *map(str, args)], capture_output=True,
                          text=True, timeout=8)


def call(*args):
    result = command(*args)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return json.loads(result.stdout)['result']


def wait_for(predicate):
    for _ in range(200):
        state = call('status')
        if predicate(state):
            return state
        time.sleep(0.05)
    raise AssertionError(('Timed out', state))


def payload(path):
    with open(path, 'rb') as audio:
        data = audio.read()
    offset = 12
    while offset + 8 <= len(data):
        tag, length = struct.unpack_from('<4sI', data, offset)
        offset += 8
        if tag == b'data':
            return struct.unpack_from('<%df' % (length // 4), data, offset)
        offset += length + (length & 1)
    raise AssertionError('WAV data chunk missing')


assert command('status').returncode != 0, 'Stop the existing Carta Studio IPC owner before this test'
with tempfile.TemporaryDirectory() as directory:
    paths, rows = [], []
    for track in range(4):
        # Distinct, nonconstant samples make incorrect row mapping and shifts visible.
        samples = [((i * (track + 3)) % 127 - 63) * (track + 1) for i in range(1000)]
        rows.append([value / 32768 for value in samples])
        path = os.path.join(directory, 'row %d.wav' % track)
        with wave.open(path, 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(44100)
            audio.writeframes(struct.pack('<1000h', *samples))
        paths.append(path)
    process = subprocess.Popen([binary], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               env={**os.environ, 'CARTA_STUDIO_RECOVERY_DIR': directory})
    try:
        for _ in range(100):
            if command('status').returncode == 0:
                break
            assert process.poll() is None, 'Editor exited during startup'
            time.sleep(0.1)
        else:
            raise AssertionError('Editor did not start')
        # Queue separately so row order does not depend on parallel import completion.
        for index, path in enumerate(paths):
            call('import', path)
            wait_for(lambda state: state['pending_imports'] == 0 and state['track_count'] == index + 1)
        call('marker', 'add', 600, 'Boundary')
        baseline_tracks, baseline_markers = call('tracks'), call('markers')
        call('select', 'tracks', 100, 200, 0, 2)
        call('edit', 'copy')
        call('seek', 400)

        def export(name):
            path = os.path.join(directory, name + '.wav')
            call('export', 'wav', path)
            state = wait_for(lambda state: state['export']['state'] != 'running')
            assert state['export']['state'] == 'complete', state['export']
            return payload(path)

        baseline = export('baseline')
        for policy in ('mix', 'replace', 'insert', 'insert-all'):
            call('edit', 'paste', policy)
            markers = call('markers')
            expected_marker = 700 if policy == 'insert-all' else 600
            assert round(markers[0]['position_seconds'] * 44100) == expected_marker, (policy, markers)
            actual = export(policy)
            length = 1100 if policy.startswith('insert') else 1000
            assert len(actual) == length * 2, (policy, len(actual))
            for sample in range(length):
                total = 0.0
                for track, row in enumerate(rows):
                    source = sample
                    if policy.startswith('insert') and (track < 3 or policy == 'insert-all'):
                        source = sample if sample < 400 else sample - 100 if sample >= 500 else -1
                    elif policy == 'replace' and track < 3 and 400 <= sample < 500:
                        source = -1
                    if 0 <= source < len(row):
                        total += row[source]
                    if track in (0, 2) and 400 <= sample < 500:
                        total += row[sample - 300]
                assert actual[2 * sample] == total and actual[2 * sample + 1] == total, (policy, sample, actual[2 * sample:2 * sample + 2], total)
            call('edit', 'undo')
            assert call('tracks') == baseline_tracks and call('markers') == baseline_markers, policy
            assert export(policy + '-undo') == baseline, policy
            call('edit', 'redo')
            assert export(policy + '-redo') == actual, policy
            assert round(call('markers')[0]['position_seconds'] * 44100) == expected_marker
            call('edit', 'undo')
            assert call('tracks') == baseline_tracks and call('markers') == baseline_markers, policy
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
print('Independent-row IPC paste policies passed; editor stopped')
