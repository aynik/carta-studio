#!/usr/bin/env python3
"""Exercise marker replacement and title preservation through real IPC."""
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import wave

binary = os.path.abspath(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='carta-marker-title-') as directory:
    env = {**os.environ, 'CARTA_STUDIO_IPC_DIR': directory, 'CARTA_STUDIO_RECOVERY_DIR': directory}
    def command(*args):
        return subprocess.run([binary, 'ipc', *args], env=env, capture_output=True, text=True, timeout=10)
    def call(*args):
        result = command(*args)
        assert result.returncode == 0, (args, result.stdout, result.stderr)
        return json.loads(result.stdout)['result']
    def wait_for(*args):
        for _ in range(400):
            result = call(*args)
            if result['state'] != 'running':
                return result
            time.sleep(0.025)
        raise AssertionError(f'timed out waiting for {args}')

    source = os.path.join(directory, 'source.wav')
    with wave.open(source, 'wb') as output:
        output.setnchannels(1); output.setsampwidth(2); output.setframerate(44100)
        output.writeframes(struct.pack('<h', 8192) * 44100)
    editor = subprocess.Popen([binary], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if command('status').returncode == 0:
                break
            assert editor.poll() is None, 'editor exited during startup'
            time.sleep(0.1)
        else:
            raise AssertionError('editor IPC did not start')
        call('import', '--no-marker', source)
        for _ in range(200):
            state = call('status')
            if not state['pending_imports']:
                break
            time.sleep(0.05)
        assert state['track_count'] == 1, state
        call('marker', 'add', '100', 'Old')
        call('marker', 'add', '200', 'Removed')
        markers = call('markers'); old_id = markers[0]['id']
        marker_file = os.path.join(directory, 'markers.json')
        with open(marker_file, 'w', encoding='utf-8') as output:
            json.dump({'format': 'carta-markers', 'version': 1, 'sampleRate': 44100,
                       'markers': [{'sample': 100, 'name': 'Renamed'}]}, output)
        call('marker', 'import', 'json', marker_file)
        result = wait_for('marker', 'exchange', 'status')
        assert result['state'] == 'complete' and result['retained_marker_ids'] == 1 and result['removed_title_form_markers'] == 0, result
        markers = call('markers'); assert len(markers) == 1 and markers[0]['id'] == old_id and markers[0]['name'] == 'Renamed', markers
        call('disc', 'sync', 'markers')
        call('disc', 'track', 'title-forms', '0', 'Kept', '曲')
        disc = call('disc', 'status'); track = disc['tracks'][0]
        assert track['half_width_title'] == 'Kept' and track['full_width_title'] == '曲', track
        call('edit', 'undo')
        restored = call('disc', 'status'); assert restored['tracks'][0]['full_width_title'] == '', restored
        call('edit', 'redo')
        package = os.path.join(directory, 'project.carta'); call('project', 'save-as', package)
        assert wait_for('project', 'status')['state'] == 'complete'
        call('project', 'open', package); assert wait_for('project', 'status')['state'] == 'complete'
        reopened = call('disc', 'status'); track = reopened['tracks'][0]
        assert track['half_width_title'] == 'Kept' and track['full_width_title'] == '曲', track
    finally:
        editor.terminate()
        try:
            editor.wait(timeout=5)
        except subprocess.TimeoutExpired:
            editor.kill(); editor.wait(timeout=5)
print('Marker import retains title forms and reports removed metadata.')
