#!/usr/bin/env python3
"""Check a long-position linked boundary and WAV cues using the real IPC client."""
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import wave

binary = os.path.abspath(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='carta-long-marker-') as directory:
    env = {**os.environ, 'CARTA_STUDIO_IPC_DIR': directory, 'CARTA_STUDIO_RECOVERY_DIR': directory}
    def command(*args):
        return subprocess.run([binary, 'ipc', *map(str, args)], env=env, capture_output=True, text=True, timeout=10)
    def call(*args):
        result = command(*args)
        assert result.returncode == 0, (args, result.stdout, result.stderr)
        return json.loads(result.stdout)['result']
    def wait_for(read, ready):
        for _ in range(400):
            value = read()
            if ready(value): return value
            assert process.poll() is None, 'editor exited unexpectedly'
            time.sleep(0.05)
        raise AssertionError(('timed out', value))
    source = os.path.join(directory, 'short.wav')
    with wave.open(source, 'wb') as output:
        output.setnchannels(1); output.setsampwidth(2); output.setframerate(44100)
        output.writeframes(struct.pack('<h', 2048) * 64)
    process = subprocess.Popen([binary, source], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(lambda: command('status').returncode, lambda code: code == 0)
        wait_for(lambda: call('status'), lambda state: state['track_count'] == 1 and not state['pending_imports'])
        sample = 1587600008
        call('master', 'duration', sample + 20)
        call('marker', 'add', sample, 'Late start')
        call('marker', 'add', sample + 9, 'Exclusive end')
        markers = call('markers')
        assert [marker['sample'] for marker in markers] == [sample, sample + 9] and not any(marker['off_grid'] for marker in markers), markers
        call('disc', 'sync', 'markers')
        disc = call('disc', 'status')
        assert disc['plan_valid'] and [t['parts'][0]['start_sample'] for t in disc['tracks']] == [0, sample, sample + 9], disc
        call('select', 'range', '0', '0', sample, sample + 9)
        target = os.path.join(directory, 'late.wav')
        call('export', 'selection', 'full', target, 'float32', 'cues')
        exported = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
        assert exported['state'] == 'complete' and exported['cues'], exported
        with open(target, 'rb') as stream: raw = stream.read()
        assert raw[:4] == b'RIFF' and raw[8:12] == b'WAVE'
        chunks = {}; offset = 12
        while offset + 8 <= len(raw):
            name, size = struct.unpack_from('<4sI', raw, offset); offset += 8
            chunks[name] = raw[offset:offset + size]; offset += size + (size & 1)
        cue = chunks[b'cue ']
        assert struct.unpack_from('<I', cue)[0] == 1 and struct.unpack_from('<I', cue, 24)[0] == 0, 'only the inclusive-start marker should be exported'
        assert len(chunks[b'data']) == 9 * 2 * 4
        print('Long linked markers and half-open WAV cues passed.')
    finally:
        process.terminate()
        try: process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=5)
