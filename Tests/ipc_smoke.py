#!/usr/bin/env python3
"""Exercise the real GUI and IPC client; requires an available desktop session."""
import json
import hashlib
import math
import os
import shutil
import socket
import subprocess
import struct
import sys
import tempfile
import time
import wave

binary = os.path.abspath(sys.argv[1])


def project_metadata(path):
    with open(path, 'rb') as stream:
        if stream.read(8) != b'CARTAP01':
            stream.seek(0)
            return json.load(stream)
        count, = struct.unpack('<I', stream.read(4))
        project = None
        for _ in range(count):
            name_length, payload_length = struct.unpack('<IQ', stream.read(12))
            name = stream.read(name_length).decode('utf-8')
            expected = stream.read(64).decode('ascii')
            payload = stream.read(payload_length)
            assert len(payload) == payload_length and hashlib.sha256(payload).hexdigest() == expected
            if name == 'Project.carta':
                assert project is None
                project = json.loads(payload)
        assert project is not None and not stream.read(1)
        return project


def command(*arguments):
    return subprocess.run([binary, *arguments], text=True, capture_output=True, timeout=8)


help_result = command('--help')
assert help_result.returncode == 0 and 'ipc status' in help_result.stdout
assert command('ipc', 'status').returncode != 0, 'Stop the existing Carta Studio IPC owner before this test'
with tempfile.TemporaryDirectory() as directory:
    source = os.path.join(directory, 'test audio.wav')
    with wave.open(source, 'wb') as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(44100)
        audio.writeframes(struct.pack('<h', 8192) * 4410)
    process = subprocess.Popen([binary], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               env={**os.environ, 'CARTA_STUDIO_RECOVERY_DIR': directory})
    try:
        for _ in range(100):
            status = command('ipc', 'status')
            if status.returncode == 0:
                break
            assert process.poll() is None, 'Editor exited during startup'
            time.sleep(0.1)
        assert status.returncode == 0, status.stderr
        assert json.loads(status.stdout)['result']['audition'] is None
        formats = json.loads(command('ipc', 'formats').stdout)['result']
        extensions = [ext.lower() for reader in formats['readers'] for ext in reader['extensions']]
        assert '.wav' in extensions and '.wav' in formats['import_wildcard'].lower() and '.ogg' in extensions
        assert command('ipc', 'project', 'save').returncode != 0, 'Untitled projects cannot be saved without a destination'
        intro_source = os.path.join(directory, '01_intro.wav')
        outro_source = os.path.join(directory, '02_outro.wav')
        shutil.copyfile(source, intro_source)
        shutil.copyfile(source, outro_source)
        seq_import = command('ipc', 'import', intro_source, outro_source)
        assert seq_import.returncode == 0 and json.loads(seq_import.stdout)['result']['queued'] == 2
        seq_state = {}
        for _ in range(100):
            seq_state = json.loads(command('ipc', 'status').stdout)['result']
            if seq_state['pending_imports'] == 0 and seq_state['layer_count'] == 1:
                break
            time.sleep(0.1)
        assert seq_state['pending_imports'] == 0 and seq_state['layer_count'] == 1 and seq_state['track_count'] == 1 and seq_state['marker_count'] == 0, seq_state
        layers_result = json.loads(command('ipc', 'layers').stdout)['result']
        tracks_result = json.loads(command('ipc', 'tracks').stdout)['result']
        assert layers_result == tracks_result and len(layers_result) == 1 and layers_result[0]['name'] == 'Layer 1', layers_result
        assert len(layers_result[0]['clips']) == 2 and layers_result[0]['clips'][0]['start_sample'] == 0 and layers_result[0]['clips'][1]['start_sample'] == layers_result[0]['clips'][0]['end_sample'], layers_result
        seq_markers = json.loads(command('ipc', 'markers').stdout)['result']
        assert seq_markers == [], seq_markers
        seq_disc = json.loads(command('ipc', 'disc', 'status').stdout)['result']
        assert seq_disc['track_count'] == 2 and [(t['half_width_title'], t['parts'][0]['start_sample'], t['parts'][0]['end_sample']) for t in seq_disc['tracks']] == [('01_intro', 0, layers_result[0]['clips'][0]['end_sample']), ('02_outro', layers_result[0]['clips'][1]['start_sample'], layers_result[0]['clips'][1]['end_sample'])], seq_disc
        add_layer_default = command('ipc', 'layer', 'add')
        add_layer_named = command('ipc', 'layer', 'add', 'Vocals')
        assert add_layer_default.returncode == 0 and add_layer_named.returncode == 0
        after_layers = json.loads(command('ipc', 'layers').stdout)['result']
        assert [layer['name'] for layer in after_layers] == ['Layer 1', 'Layer 2', 'Vocals'], after_layers
        for _ in range(4):
            assert command('ipc', 'edit', 'undo').returncode == 0
        cleaned_state = json.loads(command('ipc', 'status').stdout)['result']
        assert cleaned_state['layer_count'] == 0 and cleaned_state['marker_count'] == 0 and not cleaned_state['can_undo'], cleaned_state
        assert json.loads(command('ipc', 'disc', 'status').stdout)['result']['track_count'] == 0
        queued = command('ipc', 'import', '--no-marker', source)
        assert queued.returncode == 0 and json.loads(queued.stdout)['result']['queued'] == 1
        for _ in range(100):
            state = json.loads(command('ipc', 'status').stdout)['result']
            if state['pending_imports'] == 0 and state['track_count'] == 1:
                break
            time.sleep(0.1)
        assert state['pending_imports'] == 0 and state['track_count'] == 1, state
        tracks = json.loads(command('ipc', 'tracks').stdout)['result']
        assert tracks[0]['clips'][0]['source'] == source
        assert tracks[0]['clips'][0]['start_sample'] == 0 and tracks[0]['clips'][0]['end_sample'] == 4410
        assert tracks[0]['clips'][0]['custom_name'] == ''
        assert command('ipc', 'edit', 'move-clip', '0', '0', '1000').returncode == 0
        assert command('ipc', 'master', 'duration', '5411').returncode == 0
        assert command('ipc', 'select', 'range', '0', '0', '5410', '5411').returncode == 0
        assert command('ipc', 'select', 'clip-scope', 'range').returncode != 0
        assert json.loads(command('ipc', 'status').stdout)['result']['selected_clips'] == []
        assert command('ipc', 'edit', 'undo').returncode == 0
        assert command('ipc', 'edit', 'undo').returncode == 0
        assert json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips'][0]['start_sample'] == 0
        assert command('ipc', 'select', 'range', '0', '0', '100', '1000').returncode == 0
        assert command('ipc', 'edit', 'split-to-new-tracks').returncode == 0
        split_tracks = json.loads(command('ipc', 'tracks').stdout)['result']
        assert len(split_tracks) == 2 and len(split_tracks[1]['clips']) == 1
        assert abs(split_tracks[1]['clips'][0]['start_seconds'] * 44100 - 100) < 1e-6
        assert command('ipc', 'edit', 'undo').returncode == 0
        assert len(json.loads(command('ipc', 'tracks').stdout)['result']) == 1
        assert command('ipc', 'seek', '2205').returncode == 0
        assert command('ipc', 'edit', 'split-tracks', 'all').returncode == 0
        split_clips = json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips']
        assert len(split_clips) == 2 and abs(split_clips[0]['duration_seconds'] * 44100 - 2205) < 1e-6
        assert [(clip['start_sample'], clip['end_sample']) for clip in split_clips] == [(0, 2205), (2205, 4410)]
        assert command('ipc', 'select', 'clip', '0', '0').returncode == 0
        assert command('ipc', 'edit', 'join-next').returncode == 0
        assert len(json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips']) == 1
        assert command('ipc', 'edit', 'undo').returncode == 0
        assert len(json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips']) == 2
        assert command('ipc', 'edit', 'split-tracks', 'invalid').returncode != 0
        assert command('ipc', 'edit', 'undo').returncode == 0
        assert len(json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips']) == 1
        assert command('ipc', 'seek', '0').returncode == 0
        assert json.loads(command('ipc', 'master', 'duration').stdout)['result'] == {'minimum_samples': 0, 'end_sample': 4410}
        assert command('ipc', 'master', 'duration', '5000').returncode == 0
        assert json.loads(command('ipc', 'master', 'duration').stdout)['result'] == {'minimum_samples': 5000, 'end_sample': 5000}
        assert command('ipc', 'master', 'duration', '-1').returncode != 0
        assert command('ipc', 'edit', 'undo').returncode == 0
        assert json.loads(command('ipc', 'master', 'duration').stdout)['result'] == {'minimum_samples': 0, 'end_sample': 4410}
        assert command('ipc', 'edit', 'rename-clip', '0', '0', '  夜の曲  ').returncode == 0
        renamed = json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips'][0]
        assert renamed['name'] == '夜の曲' and renamed['custom_name'] == '夜の曲'
        assert command('ipc', 'edit', 'rename-clip', '0', '0', '夜の曲').returncode != 0
        assert command('ipc', 'edit', 'rename-clip', '0', '4', 'bad').returncode != 0
        assert command('ipc', 'edit', 'rename-clip', '0', '0', 'x' * 1025).returncode != 0
        assert command('ipc', 'edit', 'undo').returncode == 0
        assert json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips'][0]['name'] == 'test audio.wav'
        assert command('ipc', 'edit', 'redo').returncode == 0
        assert json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips'][0]['name'] == '夜の曲'
        assert command('ipc', 'edit', 'rename-clip', '0', '0', '').returncode == 0
        assert json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips'][0]['name'] == 'test audio.wav'
        assert command('ipc', 'edit', 'move-clip', '0', '0', '100').returncode == 0
        assert command('ipc', 'edit', 'trim-clip', '0', '0', 'keep', '4000').returncode == 0
        clip = json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips'][0]
        assert abs(clip['start_seconds'] * 44100 - 100) < 1e-6 and abs((clip['start_seconds'] + clip['duration_seconds']) * 44100 - 4000) < 1e-6
        assert command('ipc', 'select', 'clips', '0', '0').returncode == 0
        assert command('ipc', 'select', 'clip-span').returncode == 0
        span = json.loads(command('ipc', 'status').stdout)['result']['selection']
        assert (span['start_sample'], span['end_sample']) == (100, 4000)
        assert command('ipc', 'select', 'clips', '0', '0').returncode == 0
        assert command('ipc', 'edit', 'slip-clips', '100').returncode == 0
        slipped = json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips'][0]
        assert abs(slipped['offset_seconds'] * 44100 - 100) < 1e-6
        assert (slipped['start_sample'], slipped['end_sample']) == (clip['start_sample'], clip['end_sample'])
        assert command('ipc', 'edit', 'slip-clips', '10000').returncode != 0
        assert command('ipc', 'edit', 'undo').returncode == 0
        assert abs(json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips'][0]['offset_seconds']) < 1e-12
        assert command('ipc', 'select', 'clip', '0', '0').returncode == 0
        assert command('ipc', 'edit', 'slip-clip', '0', '0', '100').returncode == 0
        assert abs(json.loads(command('ipc', 'tracks').stdout)['result'][0]['clips'][0]['offset_seconds'] * 44100 - 100) < 1e-6
        assert command('ipc', 'edit', 'slip-clip', '0', '1', '100').returncode != 0
        assert command('ipc', 'edit', 'slip-clip', '0', '0', 'invalid').returncode != 0
        assert command('ipc', 'edit', 'undo').returncode == 0
        assert command('ipc', 'edit', 'trim-clip', '0', '0', 'keep', 'keep').returncode != 0
        assert command('ipc', 'edit', 'trim-clip', '0', '0', '5000', 'keep').returncode != 0
        assert command('ipc', 'edit', 'undo').returncode == 0
        assert command('ipc', 'edit', 'undo').returncode == 0
        assert json.loads(command('ipc', 'markers').stdout)['result'] == []
        assert json.loads(command('ipc', 'master-track', 'select', '0', 'all').stdout)['ok']
        selected = json.loads(command('ipc', 'status').stdout)['result']['selection']
        assert selected == {'first_track': 0, 'last_track': 0, 'start_sample': 0, 'end_sample': 4410}, selected
        assert command('ipc', 'master-track', 'select', '1', 'all').returncode != 0
        assert json.loads(command('ipc', 'select', 'clip', '0', '0').stdout)['ok']
        assert json.loads(command('ipc', 'seek', '2205').stdout)['ok']
        assert json.loads(command('ipc', 'select', 'clip-time', 'before').stdout)['ok']
        selected = json.loads(command('ipc', 'status').stdout)['result']['selection']
        assert (selected['start_sample'], selected['end_sample']) == (0, 2205), selected
        assert json.loads(command('ipc', 'seek', '2205').stdout)['ok']
        assert json.loads(command('ipc', 'select', 'clip-time', 'after').stdout)['ok']
        selected = json.loads(command('ipc', 'status').stdout)['result']['selection']
        assert (selected['start_sample'], selected['end_sample']) == (2205, 4410), selected
        assert command('ipc', 'transport', 'audition-crossfade', 'invalid').returncode != 0
        assert command('ipc', 'transport', 'audition-close-gap', 'invalid').returncode != 0
        invalid = command('ipc', 'nonsense')
        assert invalid.returncode != 0 and not json.loads(invalid.stdout)['ok']
        missing = command('ipc', 'import', os.path.join(directory, 'missing.wav'))
        assert missing.returncode != 0 and not json.loads(missing.stdout)['ok']
        assert command('ipc', 'transport', 'stop').returncode == 0
        def call(*args):
            result = command('ipc', *args)
            assert result.returncode == 0, (args, result.stdout, result.stderr)
            return json.loads(result.stdout)['result']

        invalid_audio = os.path.join(directory, 'invalid audio.wav')
        with open(invalid_audio, 'wb') as file:
            file.write(b'not an audio file')
        call('import', invalid_audio)
        for _ in range(100):
            imports = call('imports', 'status')
            if not imports['pending']:
                break
            time.sleep(0.05)
        assert len(imports['issues']) == 1 and imports['issues'][0]['path'] == invalid_audio and imports['issues'][0]['reason'] and imports['omitted'] == 0, imports
        assert call('status')['track_count'] == 1
        call('imports', 'clear-issues')
        assert call('imports', 'status')['issues'] == [] and call('status')['import_issues'] == 0

        memory_selection = call('status')['selection']
        assert memory_selection is not None
        call('select', 'memory', 'remember')
        memory = call('select', 'memory', 'status')
        assert memory['present'] and memory['available'] and memory['selection']['start_sample'] == memory_selection['start_sample']
        call('select', 'range', '0', '0', '100', '300')
        call('select', 'memory', 'restore')
        assert call('status')['selection'] == memory_selection
        call('select', 'memory', 'clear')
        assert call('select', 'memory', 'status') == {'present': False, 'available': False, 'selection': None}
        assert command('ipc', 'select', 'memory', 'restore').returncode != 0

        call('select', 'point', '0', '100')
        unchanged = call('tracks')
        for operation in ('duplicate-time', 'trim-time'):
            assert command('ipc', 'edit', operation).returncode != 0
            assert call('tracks') == unchanged
        call('select', 'range', '0', '0', '100', '300')
        for operation in ('duplicate-time', 'trim-time'):
            call('edit', operation)
            assert call('tracks') != unchanged
            call('edit', 'undo')
            assert call('tracks') == unchanged
        call('select', 'range', '0', '0', str(memory_selection['start_sample']), str(memory_selection['end_sample']))

        assert call('meter', 'status') == {'channels': 0, 'held': []}
        call('meter', 'reset')
        assert call('meter', 'status') == {'channels': 0, 'held': []}
        assert call('transport', 'context') == {'before_samples': 88200, 'after_samples': 88200}
        call('transport', 'context', '4410', '2205')
        assert call('transport', 'context') == {'before_samples': 4410, 'after_samples': 2205}
        assert command('ipc', 'transport', 'context', '0', '0').returncode != 0
        call('transport', 'context', '88200', '88200')
        assert not call('view', 'status')['waveform_rms']
        for zoom in (1, 2, 4, 8):
            call('view', 'waveform-zoom', str(zoom))
            assert call('view', 'status')['waveform_zoom'] == zoom
        for scale in ('decibels', 'linear'):
            call('view', 'waveform-scale', scale)
            assert call('view', 'status')['waveform_scale'] == scale
        assert command('ipc', 'view', 'waveform-zoom', '3').returncode != 0
        assert command('ipc', 'view', 'waveform-scale', 'invalid').returncode != 0
        call('view', 'waveform-shape', 'half')
        assert call('view', 'status')['waveform_shape'] == 'half'
        assert command('ipc', 'view', 'waveform-shape', 'invalid').returncode != 0
        call('view', 'waveform-shape', 'full')
        assert call('view', 'status')['waveform_shape'] == 'full'
        call('view', 'waveform-zoom', '1')
        capture_path = os.path.join(directory, 'editor capture.png')
        call('view', 'capture', capture_path)
        for _ in range(100):
            capture = call('view', 'capture', 'status')
            if capture['state'] != 'running':
                break
            time.sleep(0.1)
        assert capture['state'] == 'complete' and capture['width'] > 0 and capture['height'] > 0 and capture['pending_tiles'] >= 0, capture
        with open(capture_path, 'rb') as captured:
            capture_bytes = captured.read()
        assert capture_bytes[:8] == b'\x89PNG\r\n\x1a\n'
        assert struct.unpack('>II', capture_bytes[16:24]) == (capture['width'], capture['height'])
        assert command('ipc', 'view', 'capture', capture_path).returncode != 0
        with open(capture_path, 'rb') as captured:
            assert captured.read() == capture_bytes
        assert command('ipc', 'view', 'capture', 'relative.png').returncode != 0
        call('view', 'waveform-rms', 'on')
        assert call('view', 'status')['waveform_rms']
        assert command('ipc', 'view', 'waveform-rms', 'invalid').returncode != 0
        call('view', 'waveform-rms', 'off')
        assert not call('view', 'status')['waveform_rms']

        def export_with_options(filename, *options):
            path = os.path.join(directory, filename)
            call('export', 'wav', path, *options)
            for _ in range(100):
                result = call('status')['export']
                if result['state'] != 'running':
                    return path, result
                time.sleep(0.1)
            raise AssertionError('PCM export timed out')

        def read_wav_data(path):
            with open(path, 'rb') as file:
                raw = file.read()
            cursor = 12
            while cursor + 8 <= len(raw):
                tag, size = struct.unpack_from('<4sI', raw, cursor)
                cursor += 8
                if tag == b'data':
                    return raw[cursor:cursor+size]
                cursor += size + (size & 1)
            raise AssertionError('Missing WAV data')

        call('marker', 'add', '3000', 'After gap')
        baseline_tracks, baseline_markers = call('tracks'), call('markers')
        call('marker', 'add', '1000', 'Gap start')
        call('marker', 'add', '1500', 'Inside gap')
        call('select', 'range', '0', '0', '1000', '2000')
        before_focus = call('status')
        call('marker', 'focus', '0')
        assert call('status')['selected_marker'] == 0
        call('marker', 'focus', 'id:' + call('markers')[2]['id'])
        focused = call('status')
        assert focused['selected_marker'] == 2 and focused['selection'] == before_focus['selection'] and focused['playhead_sample'] == before_focus['playhead_sample']
        assert command('ipc', 'marker', 'focus', 'id:missing').returncode != 0 and call('status')['selected_marker'] == 2
        focused_id = call('markers')[2]['id']
        call('marker', 'rename-many', '0', 'Later renamed', 'id:' + focused_id, 'Middle renamed')
        assert call('markers')[0]['name'] == 'Later renamed' and call('markers')[2]['name'] == 'Middle renamed' and call('status')['selected_marker'] == 2
        call('edit', 'undo')
        call('marker', 'shift-many', '+100', '0', 'id:' + focused_id)
        shifted = call('markers')
        assert shifted[0]['sample'] == 3100 and shifted[2]['sample'] == 1600 and call('status')['selected_marker'] == 2
        call('edit', 'undo')
        assert command('ipc', 'marker', 'delete-many', '2', 'id:' + focused_id).returncode != 0 and call('status')['selected_marker'] == 2
        call('marker', 'delete', '0')
        assert call('status')['selected_marker'] == 1 and call('markers')[1]['id'] == focused_id
        call('edit', 'undo')
        assert call('status')['selected_marker'] == 2 and call('markers')[2]['id'] == focused_id
        call('marker', 'delete-many', '0', 'id:' + focused_id)
        assert call('status')['selected_marker'] == -1 and len(call('markers')) == 1
        call('edit', 'undo')
        assert call('status')['selected_marker'] == 2 and call('markers')[2]['id'] == focused_id
        call('select', 'range', '0', '0', '1000', '2000')
        conflicting_tracks, conflicting_markers, conflicting_selection = call('tracks'), call('markers'), call('status')['selection']
        assert call('marker', 'delete-selection')['removed'] == 2
        assert call('markers') == baseline_markers and call('tracks') == conflicting_tracks and call('status')['selection'] == conflicting_selection
        call('edit', 'undo')
        assert call('markers') == conflicting_markers and call('tracks') == conflicting_tracks
        rejected_preview = os.path.join(directory, 'gap-collision.wav')
        assert command('ipc', 'export', 'close-gap', 'all', rejected_preview).returncode != 0 and not os.path.exists(rejected_preview)
        assert command('ipc', 'edit', 'close-gap', 'all').returncode != 0
        assert call('tracks') == conflicting_tracks and call('markers') == conflicting_markers and call('status')['selection'] == conflicting_selection
        call('edit', 'undo')
        call('edit', 'undo')
        assert call('tracks') == baseline_tracks and call('markers') == baseline_markers
        for scope in ('focused', 'tracks', 'all'):
            call('select', 'range', '0', '0', '1000', '2000')
            call('edit', 'insert-gap', scope)
            shifted_marker = round(call('markers')[0]['position_seconds'] * 44100)
            assert shifted_marker == (4000 if scope == 'all' else 3000), (scope, shifted_marker)
            inserted, inserted_status = export_with_options('insert-gap-' + scope + '.wav')
            assert inserted_status['state'] == 'complete' and inserted_status['end_sample'] == 5410, inserted_status
            audio = read_wav_data(inserted)
            assert abs(struct.unpack_from('<f', audio, 999 * 8)[0] - 0.25) < 1e-6
            assert struct.unpack_from('<f', audio, 1500 * 8)[0] == 0.0
            assert abs(struct.unpack_from('<f', audio, 2500 * 8)[0] - 0.25) < 1e-6
            call('edit', 'undo')
            assert call('tracks') == baseline_tracks and call('markers') == baseline_markers
        baseline_file, _ = export_with_options('cut-gap-baseline.wav')
        for scope in ('focused', 'tracks', 'all'):
            call('select', 'range', '0', '0', '1000', '2000')
            call('edit', 'cut-close-gap', scope)
            assert round(call('markers')[0]['position_seconds'] * 44100) == (2000 if scope == 'all' else 3000)
            call('seek', '1000')
            call('edit', 'paste', 'insert-all' if scope == 'all' else 'insert')
            restored_file, restored_status = export_with_options('cut-gap-restored-' + scope + '.wav')
            assert restored_status['state'] == 'complete' and read_wav_data(restored_file) == read_wav_data(baseline_file)
            assert round(call('markers')[0]['position_seconds'] * 44100) == 3000
            call('edit', 'undo')
            call('edit', 'undo')
            assert call('tracks') == baseline_tracks and call('markers') == baseline_markers
        for scope in ('focused', 'tracks', 'all'):
            call('select', 'range', '0', '0', '1000', '2000')
            selection_before = call('status')['selection']
            target = os.path.join(directory, 'gap-preview-' + scope + '.wav')
            response = subprocess.run([binary, 'ipc', 'export', 'close-gap', scope, os.path.basename(target)], cwd=directory, text=True, capture_output=True, timeout=8)
            assert response.returncode == 0, (response.stdout, response.stderr)
            for _ in range(100):
                gap_preview = call('status')['export']
                if gap_preview['state'] != 'running':
                    break
                time.sleep(0.1)
            assert gap_preview['state'] == 'complete' and gap_preview['kind'] == 'close-gap-preview' and gap_preview['scope'] == scope and (gap_preview['start_sample'], gap_preview['end_sample']) == (0, 3410), gap_preview
            assert call('tracks') == baseline_tracks and call('markers') == baseline_markers and call('status')['selection'] == selection_before
            call('edit', 'close-gap', scope)
            marker_sample = round(call('markers')[0]['position_seconds'] * 44100)
            assert marker_sample == (2000 if scope == 'all' else 3000), (scope, marker_sample)
            applied, applied_status = export_with_options('gap-applied-' + scope + '.wav')
            assert applied_status['state'] == 'complete' and read_wav_data(target) == read_wav_data(applied), applied_status
            call('edit', 'undo')
            assert call('tracks') == baseline_tracks and call('markers') == baseline_markers
        call('track', 'duplicate', '0')
        call('track', 'duplicate', '0')
        call('select', 'tracks', '1000', '2000', '0', '2')
        before_track_focus = call('status')
        call('select', 'focus-track', '1')
        focused_track = call('status')
        assert focused_track['selected_track'] == 1 and focused_track['selected_tracks'] == [0, 2] and focused_track['selection'] == before_track_focus['selection'] and focused_track['playhead_sample'] == before_track_focus['playhead_sample'], focused_track
        assert command('ipc', 'select', 'focus-track', '99').returncode != 0
        call('select', 'focus-track', '0')
        original_independent = call('tracks')
        original_markers = call('markers')
        call('track', 'set', '2', 'gain', '0.5')
        call('track', 'scale-gain-db', '-6.020599913279624')
        assert [track['gain'] for track in call('tracks')] == [0.5, 1.0, 0.25]
        call('edit', 'undo')
        call('edit', 'undo')
        assert call('tracks') == original_independent and call('status')['selected_tracks'] == [0, 2]
        assert command('ipc', 'track', 'set-selected', 'gain-db', '25').returncode != 0
        call('track', 'set-selected', 'gain-db', '-6.020599913279624')
        assert [track['gain'] for track in call('tracks')] == [0.5, 1.0, 0.5]
        call('edit', 'undo')
        assert call('tracks') == original_independent and call('status')['selected_tracks'] == [0, 2]
        assert command('ipc', 'track', 'set-selected', 'mute', 'invalid').returncode != 0
        assert call('tracks') == original_independent
        assert command('ipc', 'track', 'set-selected', 'gain', '16.1').returncode != 0
        call('track', 'set-selected', 'gain', '0.5')
        gain_tracks = call('tracks')
        assert [track['gain'] for track in gain_tracks] == [0.5, 1.0, 0.5]
        gained, gained_status = export_with_options('selected-gain.wav')
        assert gained_status['state'] == 'complete' and abs(struct.unpack_from('<f', read_wav_data(gained), 1500 * 8)[0] - 0.5) < 1e-6
        assert call('track', 'set-selected', 'gain', '0.5') == 'Unchanged'
        call('edit', 'undo')
        assert call('tracks') == original_independent and call('status')['selected_tracks'] == [0, 2]
        assert command('ipc', 'track', 'set-selected', 'balance', '1.1').returncode != 0
        call('track', 'set-selected', 'balance', '1')
        balanced = call('tracks')
        assert [track['pan'] for track in balanced] == [1.0, 0.0, 1.0]
        balance_file, balance_status = export_with_options('selected-balance.wav')
        left, right = struct.unpack_from('<ff', read_wav_data(balance_file), 1500 * 8)
        assert balance_status['state'] == 'complete' and abs(left - 0.25) < 1e-6 and abs(right - 0.75) < 1e-6
        assert call('track', 'set-selected', 'balance', '1') == 'Unchanged'
        call('edit', 'undo')
        assert call('tracks') == original_independent and call('status')['selected_tracks'] == [0, 2]
        for flag, expected in (('mute', 0.25), ('solo', 0.5)):
            call('track', 'set-selected', flag, 'on')
            changed = call('tracks')
            key = 'muted' if flag == 'mute' else 'solo'
            assert changed[0][key] and not changed[1][key] and changed[2][key]
            mixed, mixed_status = export_with_options('selected-' + flag + '.wav')
            assert mixed_status['state'] == 'complete'
            assert abs(struct.unpack_from('<f', read_wav_data(mixed), 1500 * 8)[0] - expected) < 1e-6
            assert call('track', 'set-selected', flag, 'on') == 'Unchanged'
            call('edit', 'undo')
            assert call('tracks') == original_independent and call('status')['selected_tracks'] == [0, 2]
        call('select', 'range', '0', '1', '1000', '2000')
        call('track', 'set-selected', 'mute', 'on')
        assert [track['muted'] for track in call('tracks')] == [True, True, False]
        call('edit', 'undo')
        call('select', 'point', '1', '1000')
        call('track', 'set-selected', 'solo', 'on')
        assert [track['solo'] for track in call('tracks')] == [False, True, False]
        call('edit', 'undo')
        call('select', 'tracks', '1000', '2000', '0', '2')
        assert call('tracks') == original_independent
        for operation in ('duplicate-time', 'trim-time'):
            call('edit', operation)
            edited = call('tracks')
            assert edited[1] == original_independent[1] and edited[0] != original_independent[0] and edited[2] != original_independent[2], (operation, edited)
            assert call('markers') == original_markers and call('status')['selected_tracks'] == [0, 2]
            call('edit', 'undo')
            assert call('tracks') == original_independent and call('markers') == original_markers and call('status')['selected_tracks'] == [0, 2]
        call('edit', 'cut-close-gap', 'tracks')
        assert call('tracks')[1] == original_independent[1] and call('markers') == original_markers
        independent_cut, cut_status = export_with_options('cut-independent.wav')
        assert cut_status['state'] == 'complete'
        assert abs(struct.unpack_from('<f', read_wav_data(independent_cut), 3500 * 8)[0] - 0.25) < 1e-6
        call('edit', 'undo')
        assert call('tracks') == original_independent and call('status')['selected_tracks'] == [0, 2]
        call('edit', 'insert-gap', 'tracks')
        assert call('tracks')[1] == original_independent[1] and call('markers') == original_markers
        independent_gap, gap_status = export_with_options('insert-independent.wav')
        assert gap_status['state'] == 'complete'
        independent_audio = read_wav_data(independent_gap)
        assert abs(struct.unpack_from('<f', independent_audio, 1500 * 8)[0] - 0.25) < 1e-6
        assert abs(struct.unpack_from('<f', independent_audio, 2500 * 8)[0] - 0.75) < 1e-6
        call('edit', 'undo')
        assert call('tracks') == original_independent and call('status')['selected_tracks'] == [0, 2]
        assert command('ipc', 'edit', 'move-range', '1', '2000').returncode != 0
        assert call('tracks') == original_independent and call('status')['selected_tracks'] == [0, 2]
        call('edit', 'move-range', '0', '2000')
        moved_state = call('status')
        assert moved_state['selected_tracks'] == [0, 2] and moved_state['selection'] == {'first_track': 0, 'last_track': 2, 'start_sample': 2000, 'end_sample': 3000} and call('markers') == original_markers
        moved_file, moved_export = export_with_options('move-independent.wav')
        assert moved_export['state'] == 'complete'
        moved_audio = read_wav_data(moved_file)
        assert abs(struct.unpack_from('<f', moved_audio, 1500 * 8)[0] - 0.25) < 1e-6
        assert abs(struct.unpack_from('<f', moved_audio, 2500 * 8)[0] - 1.25) < 1e-6
        call('edit', 'undo')
        assert call('tracks') == original_independent and call('markers') == original_markers and call('status')['selected_tracks'] == [0, 2]
        independent_tracks = call('tracks')
        independent_markers = call('markers')
        target = os.path.join(directory, 'gap-preview-independent.wav')
        call('export', 'close-gap', 'tracks', target)
        for _ in range(100):
            gap_preview = call('status')['export']
            if gap_preview['state'] != 'running':
                break
            time.sleep(0.1)
        assert gap_preview['state'] == 'complete' and (gap_preview['start_sample'], gap_preview['end_sample']) == (0, 4410), gap_preview
        assert call('tracks') == independent_tracks and call('status')['selected_tracks'] == [0, 2]
        call('edit', 'close-gap', 'tracks')
        assert call('tracks')[1] == independent_tracks[1] and call('markers') == independent_markers
        applied, applied_status = export_with_options('gap-applied-independent.wav')
        assert applied_status['state'] == 'complete' and read_wav_data(target) == read_wav_data(applied), applied_status
        call('edit', 'undo')
        assert call('tracks') == independent_tracks and call('status')['selected_tracks'] == [0, 2]
        call('edit', 'undo')
        call('edit', 'undo')
        call('edit', 'undo')
        assert len(call('tracks')) == 1 and call('markers') == []
        pcm16, pcm16_status = export_with_options('master pcm16.wav', 'pcm16')
        assert pcm16_status['state'] == 'complete' and pcm16_status['encoding'] == 'pcm16' and pcm16_status['file_written'] and all(row['clipped_samples'] == 0 for row in pcm16_status['preflight']), pcm16_status
        assert read_wav_data(pcm16) == struct.pack('<hh', 8192, 8192) * 4410
        pcm24, pcm24_status = export_with_options('master pcm24.wav', 'pcm24')
        assert pcm24_status['state'] == 'complete' and pcm24_status['encoding'] == 'pcm24', pcm24_status
        assert read_wav_data(pcm24) == b'\x00\x00\x20\x00\x00\x20' * 4410
        call('select', 'range', '0', '0', '0', '4410')
        call('analysis', 'zero-crossings', 'all')
        for _ in range(100):
            crossings = call('analysis', 'status')
            if crossings['state'] != 'running':
                break
            time.sleep(0.1)
        assert crossings['state'] == 'complete' and not crossings['stale'] and (crossings['start_sample'], crossings['end_sample']) == (0, 4410), crossings
        call('select', 'range', '0', '0', '100', '300')
        assert call('analysis', 'status')['stale']
        call('analysis', 'zero-crossings', 'all')
        for _ in range(100):
            crossings = call('analysis', 'status')
            if crossings['state'] != 'running':
                break
            time.sleep(0.1)
        assert crossings['state'] == 'failed' and 'common' in crossings['error'], crossings
        assert call('status')['selection'] == {'first_track': 0, 'last_track': 0, 'start_sample': 100, 'end_sample': 300}
        selected_pcm = os.path.join(directory, 'selection pcm24.wav')
        call('export', 'selection', 'tracks', selected_pcm, 'pcm24', 'dither')
        for _ in range(100):
            selected_pcm_status = call('status')['export']
            if selected_pcm_status['state'] != 'running':
                break
            time.sleep(0.1)
        assert selected_pcm_status['state'] == 'complete' and selected_pcm_status['encoding'] == 'pcm24' and selected_pcm_status['dither'] and (selected_pcm_status['start_sample'], selected_pcm_status['end_sample']) == (100, 300) and len(read_wav_data(selected_pcm)) == 200 * 6, selected_pcm_status
        call('select', 'clip', '0', '0')
        dither_a, dither_status = export_with_options('dither a.wav', 'pcm16', 'dither')
        dither_b, _ = export_with_options('dither b.wav', 'pcm16', 'dither')
        assert dither_status['state'] == 'complete' and dither_status['dither'] and read_wav_data(dither_a) == read_wav_data(dither_b) and read_wav_data(dither_a) != read_wav_data(pcm16), dither_status
        assert command('ipc', 'export', 'wav', os.path.join(directory, 'invalid.wav'), 'float32', 'dither').returncode != 0
        assert command('ipc', 'export', 'wav', os.path.join(directory, 'invalid.wav'), 'pcm16', 'pcm24').returncode != 0
        call('marker', 'add', '100', '章')
        cued, cue_status = export_with_options('cued pcm24.wav', 'pcm24', 'cues')
        assert cue_status['state'] == 'complete' and cue_status['cues'], cue_status
        if shutil.which('ffprobe'):
            probe = subprocess.run(['ffprobe', '-v', 'error', '-show_streams', '-show_chapters', '-of', 'json', cued], capture_output=True, text=True, check=True)
            metadata = json.loads(probe.stdout)
            assert metadata['streams'][0]['codec_name'] == 'pcm_s24le' and metadata['streams'][0]['sample_rate'] == '44100' and metadata['streams'][0]['channels'] == 2, metadata
            assert any(chapter.get('tags', {}).get('title') == '章' for chapter in metadata.get('chapters', [])), metadata
        call('edit', 'undo')
        call('clip', 'set', '0', '0', 'gain', '2')
        call('track', 'set', '0', 'gain', '4')
        clipped_file, rejected_pcm = export_with_options('clipping.wav', 'pcm16')
        assert rejected_pcm['state'] == 'failed' and not rejected_pcm['file_written'] and not os.path.exists(clipped_file) and all(row['clipped_samples'] == 4410 and row['first_clipped_sample'] == 0 for row in rejected_pcm['preflight']), rejected_pcm
        clipping_parent = os.path.join(directory, 'clipping batch')
        os.mkdir(clipping_parent)
        call('export', 'tracks', clipping_parent, 'pcm16')
        for _ in range(100):
            rejected_batch = call('status')['export']
            if rejected_batch['state'] != 'running':
                break
            time.sleep(0.1)
        assert rejected_batch['state'] == 'failed' and not rejected_batch['file_written'] and not os.listdir(clipping_parent) and all(row['clipped_samples'] == 4410 for row in rejected_batch['preflight']), rejected_batch
        call('export', 'tracks', clipping_parent, 'pcm16', 'allow-clip')
        for _ in range(100):
            allowed_batch = call('status')['export']
            if allowed_batch['state'] != 'running':
                break
            time.sleep(0.1)
        assert allowed_batch['state'] == 'complete' and allowed_batch['completed_tracks'] == 1 and allowed_batch['expected_tracks'] == 1 and allowed_batch['manifest_written'] and allowed_batch['clipped'], allowed_batch
        assert read_wav_data(os.path.join(allowed_batch['output_folder'], 'Track 01.wav')) == struct.pack('<hh', 32767, 32767) * 4410
        _, allowed_pcm = export_with_options('clipping.wav', 'pcm16', 'allow-clip')
        assert allowed_pcm['state'] == 'complete' and allowed_pcm['clipped'] and allowed_pcm['file_written'] and read_wav_data(clipped_file) == struct.pack('<hh', 32767, 32767) * 4410, allowed_pcm
        call('edit', 'undo')
        call('edit', 'undo')

        call('analysis', 'source-silence', '0', '0', '100')
        for _ in range(100):
            source_silence = call('analysis', 'status')
            if source_silence['state'] != 'running':
                break
            time.sleep(0.1)
        assert source_silence['kind'] == 'source-silence' and source_silence['state'] == 'complete' and source_silence['gaps'] == [] and not source_silence['stale'], source_silence
        assert command('ipc', 'analysis', 'select', '0').returncode != 0

        call('track', 'add', 'History one')
        assert call('analysis', 'status')['stale']
        call('track', 'add', 'History two')
        history = call('history', 'list')
        target = history['undo'][1]
        assert target['steps'] == 2 and target['action'], history
        jump_args = ('history', 'undo', str(target['steps']), str(history['generation']), str(target['revision']))
        jump = call(*jump_args)
        assert jump['complete'] and jump['steps_moved'] == 2 and call('status')['track_count'] == 1, jump
        stale = command('ipc', *jump_args)
        assert stale.returncode != 0 and not json.loads(stale.stdout)['ok'] and call('status')['track_count'] == 1
        redo = call('history', 'list')
        target = redo['redo'][1]
        jump = call('history', 'redo', str(target['steps']), str(redo['generation']), str(target['revision']))
        assert jump['complete'] and jump['steps_moved'] == 2 and call('status')['track_count'] == 3, jump
        undo = call('history', 'list')
        target = undo['undo'][1]
        call('history', 'undo', str(target['steps']), str(undo['generation']), str(target['revision']))
        assert call('status')['track_count'] == 1

        call('snap', 'grid', '0.25 s')
        call('snap', 'mode', 'grid')
        snap = call('snap', 'status')
        assert snap['grid_samples'] == 11025 and snap['mode'] == 'grid', snap
        assert snap['targets'] == {'clip_edges': True, 'markers': True, 'selection': True, 'playhead': True, 'timeline_start': True}, snap
        for target, key in [('clip-edges', 'clip_edges'), ('markers', 'markers'), ('selection', 'selection'), ('playhead', 'playhead'), ('timeline-start', 'timeline_start')]:
            call('snap', 'target', target, 'off')
            assert not call('snap', 'status')['targets'][key]
        assert command('ipc', 'snap', 'target', 'unknown', 'on').returncode != 0
        for target in ('clip-edges', 'markers', 'selection', 'playhead', 'timeline-start'):
            call('snap', 'target', target, 'on')
        call('snap', 'grid', '1')
        call('snap', 'mode', 'boundaries')

        call('seek', '123')
        call('transport', 'stop-here')
        assert call('status')['playhead_sample'] == 123
        call('transport', 'stop')
        assert call('status')['playhead_sample'] == 0

        def source_check(*scope):
            call('sources', 'verify', *scope)
            for _ in range(100):
                checked = call('sources', 'status')
                if checked['state'] != 'running':
                    return checked
                time.sleep(0.1)
            raise AssertionError('source check timed out')

        checked = source_check('all')
        assert checked['state'] == 'complete' and not checked['stale'] and checked['sources'][0]['status'] == 'matches' and checked['sources'][0]['path'] == source, checked
        with open(source, 'rb') as fixture:
            original_bytes = fixture.read()
        with open(source, 'ab') as fixture:
            fixture.write(b'changed')
        checked = source_check('track', '0')
        assert checked['state'] == 'complete' and checked['sources'][0]['status'] == 'changed', checked
        os.unlink(source)
        checked = source_check('all')
        assert checked['sources'][0]['status'] == 'missing', checked
        with open(source, 'wb') as fixture:
            fixture.write(original_bytes)
        checked = source_check('all')
        assert checked['sources'][0]['status'] == 'matches', checked
        assert command('ipc', 'sources', 'verify', 'track', '999').returncode != 0
        replacement = os.path.join(directory, 'replacement audio.wav')
        shutil.copyfile(source, replacement)
        bad_replacement = os.path.join(directory, 'wrong audio.wav')
        with open(bad_replacement, 'wb') as bad:
            bad.write(original_bytes + b'changed')

        def relink(path, cwd=None):
            if cwd is None:
                call('sources', 'relink', '0', '0', path)
            else:
                result = subprocess.run([binary, 'ipc', 'sources', 'relink', '0', '0', path], cwd=cwd, text=True, capture_output=True, timeout=8)
                assert result.returncode == 0, (result.stdout, result.stderr)
            for _ in range(100):
                operation = call('sources', 'status')
                if operation['state'] != 'running':
                    return operation
                time.sleep(0.1)
            raise AssertionError('source relink timed out')

        failed = relink(bad_replacement)
        assert failed['state'] == 'failed' and call('tracks')[0]['clips'][0]['source'] == source, failed
        linked = relink(os.path.basename(replacement), cwd=directory)
        assert linked['state'] == 'complete' and linked['changed'] and not linked['stale'] and os.path.samefile(call('tracks')[0]['clips'][0]['source'], replacement), linked
        call('edit', 'undo')
        assert call('tracks')[0]['clips'][0]['source'] == source and call('sources', 'status')['stale']

        call('select', 'range', '0', '0', '1000', '2000')
        call('edit', 'delete')
        call('track', 'add', 'Empty one')
        call('track', 'add', 'Empty two')
        call('select', 'point', '0', '1500')
        call('select', 'point-gap', 'tracks', '2', '0')
        assert call('status')['selection'] == {'first_track': 0, 'last_track': 2, 'start_sample': 1000, 'end_sample': 2000} and call('status')['selected_tracks'] == [0, 2]
        call('edit', 'undo')
        call('edit', 'undo')
        assert len(call('tracks')) == 1
        call('select', 'point', '0', '1500')
        call('select', 'point-gap')
        assert call('status')['selection'] == {'first_track': 0, 'last_track': 0, 'start_sample': 1000, 'end_sample': 2000}
        call('analysis', 'quiet', '-80', '100')
        for _ in range(100):
            analysis = call('analysis', 'status')
            if analysis['state'] != 'running':
                break
            time.sleep(0.1)
        assert analysis['state'] == 'complete' and not analysis['stale'], analysis
        assert len(analysis['gaps']) == 1 and analysis['suggestion_count'] == 1, analysis
        gap = analysis['gaps'][0]
        assert (gap['start_sample'], gap['end_sample'], gap['suggested_sample'], gap['existing_marker']) == (1000, 2000, 1500, False) and gap['suggested_name'].startswith('Track at '), analysis
        assert command('ipc', 'analysis', 'audition', '99').returncode != 0
        call('select', 'range', '0', '0', '0', '100')
        assert command('ipc', 'analysis', 'select', '99').returncode != 0
        call('analysis', 'select', '0')
        assert call('status')['selection'] == {'first_track': 0, 'last_track': 0, 'start_sample': 1000, 'end_sample': 2000} and call('markers') == []
        assert command('ipc', 'analysis', 'apply-markers', '0', '0').returncode != 0 and call('markers') == []
        assert call('analysis', 'apply-markers', '0')['added'] == 1
        assert abs(call('markers')[0]['position_seconds'] * 44100 - 1500) < 1e-6 and call('markers')[0]['name'] == gap['suggested_name']
        assert call('analysis', 'status')['stale']
        assert command('ipc', 'analysis', 'audition', '0').returncode != 0
        call('edit', 'undo')
        assert call('markers') == [] and command('ipc', 'analysis', 'apply-markers').returncode != 0
        call('edit', 'undo')
        assert call('analysis', 'status')['stale'], 'Editing after a scan must mark its snapshot stale'
        call('analysis', 'levels', 'master')
        for _ in range(100):
            levels = call('analysis', 'status')
            if levels['state'] != 'running':
                break
            time.sleep(0.1)
        assert levels['state'] == 'complete' and levels['kind'] == 'levels' and (levels['start_sample'], levels['end_sample']) == (0, 4410), levels
        assert all(abs(output['peak'] - 0.25) < 1e-7 and output['peak_sample'] == 0 and abs(output['rms'] - 0.25) < 1e-7 and output['above_full_scale'] == 0 and output['first_above_full_scale'] is None for output in levels['outputs']), levels
        call('seek', '2205')
        assert call('analysis', 'locate-peak', 'left') == 0 and call('status')['playhead_sample'] == 0
        call('seek', '2205')
        assert command('ipc', 'analysis', 'locate-over', 'left').returncode != 0 and call('status')['playhead_sample'] == 2205
        call('select', 'range', '0', '0', '0', '1102')
        call('analysis', 'levels', 'selection')
        for _ in range(100):
            levels = call('analysis', 'status')
            if levels['state'] != 'running':
                break
            time.sleep(0.1)
        assert levels['state'] == 'complete' and levels['end_sample'] == 1102 and not levels['stale'], levels
        reference = os.path.join(directory, 'selected-reference.wav')
        rendered = os.path.join(directory, 'selected-render.wav')
        call('export', 'selection', 'tracks', reference)
        for _ in range(100):
            operation = call('status')['export']
            if operation['state'] != 'running':
                break
            time.sleep(0.1)
        assert operation['state'] == 'complete', operation
        relative_render = subprocess.run([binary, 'ipc', 'render', 'selected', os.path.basename(rendered)], cwd=directory, text=True, capture_output=True, timeout=8)
        assert relative_render.returncode == 0, (relative_render.stdout, relative_render.stderr)
        for _ in range(100):
            operation = call('status')['export']
            if operation['state'] != 'running':
                break
            time.sleep(0.1)
        assert operation['state'] == 'complete' and operation['file_written'] and operation['track_index'] == 1, operation
        assert open(reference, 'rb').read() == open(rendered, 'rb').read(), 'Rendered track must contain the exact selected mix'
        rendered_track = call('tracks')[1]
        assert rendered_track['muted'] and os.path.samefile(rendered_track['clips'][0]['source'], rendered) and abs(rendered_track['clips'][0]['duration_seconds'] * 44100 - 1102) < 1e-6
        call('edit', 'undo')
        assert len(call('tracks')) == 1 and os.path.exists(rendered)
        assert command('ipc', 'render', 'selected', rendered).returncode != 0, 'Render must not replace a user-owned WAV'
        call('select', 'clip', '0', '0')
        full_reference = os.path.join(directory, 'full-reference.wav')
        full_rendered = os.path.join(directory, 'full-render.wav')
        call('export', 'wav', full_reference)
        for _ in range(100):
            operation = call('status')['export']
            if operation['state'] != 'running':
                break
            time.sleep(0.1)
        assert operation['state'] == 'complete', operation
        call('render', 'tracks', full_rendered)
        for _ in range(100):
            operation = call('status')['export']
            if operation['state'] != 'running':
                break
            time.sleep(0.1)
        assert operation['state'] == 'complete' and operation['kind'] == 'render-tracks' and (operation['start_sample'], operation['end_sample']) == (0, 4410), operation
        assert open(full_reference, 'rb').read() == open(full_rendered, 'rb').read() and call('tracks')[1]['muted']
        call('edit', 'undo')
        assert len(call('tracks')) == 1 and os.path.exists(full_rendered)
        clip_reference = os.path.join(directory, 'clip-reference.wav')
        clip_rendered = os.path.join(directory, 'clip-render.wav')
        call('export', 'clip', clip_reference)
        for _ in range(100):
            operation = call('status')['export']
            if operation['state'] != 'running':
                break
            time.sleep(0.1)
        assert operation['state'] == 'complete', operation
        call('render', 'clips', clip_rendered)
        for _ in range(100):
            operation = call('status')['export']
            if operation['state'] != 'running':
                break
            time.sleep(0.1)
        assert operation['state'] == 'complete' and operation['kind'] == 'render-clips' and (operation['start_sample'], operation['end_sample']) == (0, 4410), operation
        assert open(clip_reference, 'rb').read() == open(clip_rendered, 'rb').read() and call('tracks')[1]['muted']
        call('edit', 'undo')
        assert len(call('tracks')) == 1 and os.path.exists(clip_rendered)
        call('select', 'point', '0', '1102')
        assert call('analysis', 'status')['stale'], 'Changing a scoped selection must mark its levels stale'
        assert call('status')['edit_point'] == {'layer': 0, 'track': 0, 'sample': 1102}
        call('marker', 'add-point', 'Chapter')
        assert abs(call('markers')[0]['position_seconds'] * 44100 - 1102) < 1e-6
        assert command('ipc', 'marker', 'add-point', 'Duplicate').returncode != 0
        call('select', 'point-range', 'start')
        assert call('status')['selection'] == {'first_track': 0, 'last_track': 0, 'start_sample': 0, 'end_sample': 1102}
        call('select', 'point', '0', '1102')
        call('select', 'point-boundary', 'start')
        assert call('select', 'point-boundary', 'next-marker') == 1102
        assert call('select', 'point-boundary', 'next-clip') == 4410
        assert call('select', 'point-boundary', 'previous-marker') == 1102
        call('edit', 'undo')
        assert call('markers') == [] and call('status')['edit_point']['sample'] == 1102
        call('edit', 'split')
        pieces = call('tracks')[0]['clips']
        assert len(pieces) == 2 and round(pieces[1]['start_seconds'] * 44100) == 1102
        call('select', 'clip', '0', '0')
        call('select', 'source-clips')
        assert call('status')['selected_clips'] == [{'layer': 0, 'track': 0, 'clip': 0}, {'layer': 0, 'track': 0, 'clip': 1}]
        call('select', 'source-clips', 'tracks')
        assert len(call('status')['selected_clips']) == 2
        call('select', 'range', '0', '0', '100', '500')
        call('select', 'focus-clip', '0', '1')
        focus = call('status')
        assert focus['selected_clip'] == 1 and focus['selection']['start_sample'] == 100 and focus['selected_clips'] == [], focus
        assert command('ipc', 'select', 'focus-clip', '0', '999').returncode != 0
        call('select', 'source-clips', 'range')
        assert call('status')['selected_clips'] == [{'layer': 0, 'track': 0, 'clip': 0}]
        call('select', 'clip-scope', 'range')
        scoped = call('status')
        assert scoped['selected_clips'] == [{'layer': 0, 'track': 0, 'clip': 0}] and scoped['selection']['start_sample'] == 100 and scoped['selection']['end_sample'] == 500, scoped
        assert command('ipc', 'select', 'clip-scope', 'invalid').returncode != 0
        call('select', 'clip-scope', 'tracks')
        assert call('status')['selected_clips'] == [{'layer': 0, 'track': 0, 'clip': 0}, {'layer': 0, 'track': 0, 'clip': 1}]
        call('select', 'clip-scope', 'all')
        assert len(call('status')['selected_clips']) == 2
        consolidated = os.path.join(directory, 'consolidated.wav')
        call('render', 'consolidate', consolidated)
        for _ in range(100):
            operation = call('status')['export']
            if operation['state'] != 'running':
                break
            time.sleep(0.1)
        assert operation['state'] == 'complete' and operation['kind'] == 'consolidate-clips' and operation.get('track_index') == 0 and operation.get('clip_index') == 0, operation
        assert len(call('tracks')) == 1 and len(call('tracks')[0]['clips']) == 1 and call('tracks')[0]['clips'][0]['source'] == consolidated
        consolidated_master = os.path.join(directory, 'consolidated-master.wav')
        call('export', 'wav', consolidated_master)
        for _ in range(100):
            operation = call('status')['export']
            if operation['state'] != 'running':
                break
            time.sleep(0.1)
        assert operation['state'] == 'complete' and open(full_reference, 'rb').read() == open(consolidated_master, 'rb').read(), operation
        call('edit', 'undo')
        assert len(call('tracks')[0]['clips']) == 2 and os.path.exists(consolidated)
        call('edit', 'undo')
        assert len(call('tracks')[0]['clips']) == 1 and call('status')['edit_point']['sample'] == 1102
        call('select', 'range', '0', '0', '0', '100')
        call('edit', 'copy')
        call('select', 'point', '0', '2000')
        call('seek', '3000')
        for policy in ('mix', 'replace', 'insert', 'insert-all'):
            call('edit', 'paste-point', policy)
            assert any(round(clip['start_seconds'] * 44100) == 2000 for clip in call('tracks')[0]['clips'])
            call('edit', 'undo')
            assert len(call('tracks')[0]['clips']) == 1 and call('status')['edit_point']['sample'] == 2000
        call('seek', '0')
        call('track', 'duplicate', '0')
        call('select', 'clips', '1', '0')
        call('edit', 'align-clips', '1', '0', 'start', '500')
        call('select', 'clips', '0', '0', '1', '0')
        call('edit', 'group')
        grouped_tracks = call('tracks')
        group_id = grouped_tracks[0]['clips'][0]['group_id']
        assert len(group_id) == 32 and grouped_tracks[1]['clips'][0]['group_id'] == group_id
        call('select', 'range', '0', '0', '100', '300')
        assert call('edit', 'move-range-impact')['linked_groups'] == 1
        assert call('tracks') == grouped_tracks
        call('select', 'clips', '0', '0', '1', '0')
        call('track', 'duplicate', '0')
        call('select', 'clips', '0', '0', '1', '0')
        call('edit', 'sequence')
        sequenced_tracks = call('tracks')
        assert abs(sequenced_tracks[0]['clips'][0]['start_seconds']) < 1e-9
        assert abs(sequenced_tracks[2]['clips'][0]['start_seconds'] - 500 / 44100) < 1e-9
        assert abs(sequenced_tracks[1]['clips'][0]['start_seconds'] - 4910 / 44100) < 1e-9
        call('edit', 'undo')
        call('edit', 'undo')
        grouped_project = os.path.join(directory, 'grouped.carta')
        call('project', 'save-copy', grouped_project)
        for _ in range(100):
            operation = call('status')['project_operation']
            if operation['state'] != 'running':
                break
            time.sleep(0.1)
        assert operation['state'] == 'complete', operation
        project = project_metadata(grouped_project)
        assert project['version'] == 10 and project['tracks'][0]['clips'][0]['groupId'] == project['tracks'][1]['clips'][0]['groupId'] == group_id
        call('edit', 'align-clips', '0', '0', 'start', '1000')
        assert [round(track['clips'][0]['start_seconds'] * 44100) for track in call('tracks')] == [1000, 1500]
        call('edit', 'undo')
        call('seek', '1000')
        call('edit', 'align-group', 'start')
        assert [round(track['clips'][0]['start_seconds'] * 44100) for track in call('tracks')] == [1000, 1500]
        call('edit', 'undo')
        call('edit', 'trim-group-range', '0', '0', '100', '4000')
        trimmed_tracks = call('tracks')
        assert [round(track['clips'][0]['start_seconds'] * 44100) for track in trimmed_tracks] == [100, 600]
        assert all(abs(track['clips'][0]['duration_seconds'] - 3900 / 44100) < 1e-9 for track in trimmed_tracks)
        call('edit', 'undo')
        call('select', 'clip', '0', '0')
        call('seek', '1102')
        call('edit', 'split')
        assert len(call('tracks')[0]['clips']) == len(call('tracks')[1]['clips']) == 2
        call('edit', 'undo')
        call('select', 'clip', '0', '0')
        call('edit', 'ungroup')
        assert all(not track['clips'][0]['group_id'] for track in call('tracks'))
        call('edit', 'undo')
        assert all(track['clips'][0]['group_id'] == group_id for track in call('tracks'))
        call('edit', 'undo')
        call('edit', 'undo')
        call('edit', 'undo')
        assert len(call('tracks')) == 1

        call('view', 'range', '441', '2205')
        viewport = call('view', 'status')
        assert abs(viewport['start_seconds'] - 441 / 44100) < 1e-9
        assert abs(viewport['end_seconds'] - 2205 / 44100) < 1e-9
        assert command('ipc', 'view', 'range', '0', '15').returncode != 0
        assert command('ipc', 'view', 'range', '20', '20').returncode != 0
        call('view', 'default', 'spectrogram')
        assert call('view', 'status')['default'] == 'spectrogram'
        call('view', 'spectrum-range', '300', '12000')
        spectrum = call('view', 'status')
        assert (spectrum['spectrum_band'], spectrum['spectrum_custom_low_hz'], spectrum['spectrum_custom_high_hz']) == ('custom', 300, 12000)
        assert command('ipc', 'view', 'spectrum-range', '300', '300').returncode != 0
        assert call('view', 'status')['spectrum_band'] == 'custom'
        call('view', 'spectrum-preset', 'full')
        assert call('view', 'status')['spectrum_window'] == 'hann'
        call('view', 'spectrum-floor', '-120')
        call('view', 'spectrum-fft', '4096')
        assert (call('view', 'status')['spectrum_floor_db'], call('view', 'status')['spectrum_fft_size']) == (-120, 4096)
        assert command('ipc', 'view', 'spectrum-floor', '-110').returncode != 0
        assert command('ipc', 'view', 'spectrum-fft', '512').returncode != 0
        call('view', 'spectrum-floor', '-80')
        call('view', 'spectrum-fft', '2048')
        for window in ('hamming', 'blackman', 'hann'):
            call('view', 'spectrum-window', window)
            assert call('view', 'status')['spectrum_window'] == window
        assert command('ipc', 'view', 'spectrum-window', 'invalid').returncode != 0
        assert call('view', 'status')['spectrum_window'] == 'hann'
        for follow_mode in ('page', 'pinned', 'off'):
            call('view', 'follow', follow_mode)
            assert call('view', 'status')['follow'] == follow_mode
        assert command('ipc', 'view', 'follow', 'invalid').returncode != 0
        assert call('view', 'status')['follow'] == 'off'
        assert command('ipc', 'view', 'follow', 'resume').returncode != 0
        call('view', 'default', 'waveform')
        call('view', 'fit')
        assert call('view', 'status')['start_seconds'] == 0
        initial_height = call('tracks')[0]['height']
        assert call('tracks')[0]['waveform_zoom'] == 'default'
        call('track', 'set', '0', 'waveform-zoom', '4')
        assert call('tracks')[0]['waveform_zoom'] == 4
        assert command('ipc', 'track', 'set', '0', 'waveform-zoom', '3').returncode != 0
        call('edit', 'undo')
        assert call('tracks')[0]['waveform_zoom'] == 'default'
        call('track', 'set', '0', 'view', 'combined')
        call('track', 'set', '0', 'height', '240')
        assert call('tracks')[0]['view'] == 'combined' and call('tracks')[0]['height'] == 240
        assert command('ipc', 'track', 'set', '0', 'height', '801').returncode != 0
        assert command('ipc', 'track', 'set', '0', 'view', 'invalid').returncode != 0
        call('edit', 'undo')
        assert call('tracks')[0]['height'] == initial_height
        call('edit', 'undo')
        assert call('tracks')[0]['view'] == 'default'

        assert call('track', 'add', 'Assembly') == 1
        call('select', 'range', '0', '0', '0', '1102')
        call('edit', 'copy')
        call('select', 'range', '1', '1', '0', '1102')
        call('seek', '0')
        call('edit', 'paste', 'mix')
        assert len(call('tracks')[1]['clips']) == 1
        call('edit', 'undo')
        assert call('tracks')[1]['clips'] == []
        call('track', 'rename', '1', 'Second row')
        call('track', 'duplicate', '1')
        assert [track['name'] for track in call('tracks')][1:] == ['Second row', 'Second row copy']
        assert call('status')['selection']['first_track'] == call('status')['selection']['last_track'] == 1
        call('select', 'tracks', '0', '1102', '0', '2')
        call('track', 'move', '2', '0')
        assert [track['name'] for track in call('tracks')][0] == 'Second row copy' and call('status')['selected_tracks'] == [0, 1]
        call('edit', 'undo')
        assert [track['name'] for track in call('tracks')][1:] == ['Second row', 'Second row copy'] and call('status')['selected_tracks'] == [0, 2]
        call('edit', 'redo')
        assert call('tracks')[0]['name'] == 'Second row copy' and call('status')['selected_tracks'] == [0, 1]
        call('track', 'remove', '0')
        assert call('status')['selected_tracks'] == [0]
        call('track', 'remove', '1')
        assert len(call('tracks')) == 1 and call('tracks')[0]['clips'][0]['source'] == source
        call('select', 'clip', '0', '0')
        call('marker', 'add', '100', 'Keep marker')
        assert command('ipc', 'track', 'remove', '0').returncode != 0
        assert len(call('tracks')) == 1 and len(call('markers')) == 1 and call('status')['selected_clip'] == 0
        call('marker', 'delete', '0')
        assert command('ipc', 'track', 'move', '0', '1').returncode != 0
        assert command('ipc', 'track', 'add', '  ').returncode != 0

        missing_range = os.path.join(directory, 'no selection.wav')
        assert command('ipc', 'export', 'selection', 'full', missing_range).returncode != 0
        assert not os.path.exists(missing_range)
        assert command('ipc', 'export', 'clips', os.path.join(directory, 'no clips.wav')).returncode != 0
        assert command('ipc', 'edit', 'copy').returncode != 0
        call('select', 'range', '0', '0', '0', '3300')
        call('edit', 'copy')
        call('seek', '2205')
        call('edit', 'paste', 'mix')
        call('select', 'clips', '0', '0', '0', '1')
        untouched = call('tracks')
        before_preview = call('status')
        preview_paths = []
        for scope in ('selected', 'same'):
            preview = os.path.join(directory, 'crossfade-' + scope + '.wav')
            response = subprocess.run([binary, 'ipc', 'export', 'crossfade', scope, os.path.basename(preview)], cwd=directory, text=True, capture_output=True, timeout=8)
            assert response.returncode == 0, (response.stdout, response.stderr)
            for _ in range(100):
                preview_status = call('status')['export']
                if preview_status['state'] != 'running':
                    break
                time.sleep(0.1)
            assert preview_status['state'] == 'complete' and preview_status['kind'] == 'crossfade-preview' and preview_status['scope'] == scope and (preview_status['start_sample'], preview_status['end_sample']) == (0, 5505), preview_status
            assert call('tracks') == untouched and call('status')['selected_clips'] == before_preview['selected_clips']
            preview_paths.append(preview)
        assert command('ipc', 'export', 'crossfade', 'across', os.path.join(directory, 'bad-crossfade.wav')).returncode != 0
        assert not os.path.exists(os.path.join(directory, 'bad-crossfade.wav'))
        call('edit', 'crossfade-selected')
        applied_path, applied_status = export_with_options('applied-crossfade.wav')
        assert applied_status['state'] == 'complete', applied_status
        assert all(read_wav_data(path) == read_wav_data(applied_path) for path in preview_paths)
        call('edit', 'undo')
        assert call('tracks') == untouched
        call('edit', 'undo')
        assert len(call('tracks')[0]['clips']) == 1
        call('track', 'add', 'Crossfade incoming')
        call('select', 'point', '1', '2205')
        call('edit', 'paste-point', 'mix')
        call('select', 'clips', '0', '0', '1', '0')
        cross_track_before = call('tracks')
        previews = []
        for scope in ('selected', 'across'):
            target = os.path.join(directory, 'cross-track-' + scope + '.wav')
            call('export', 'crossfade', scope, target)
            for _ in range(100):
                preview_status = call('status')['export']
                if preview_status['state'] != 'running':
                    break
                time.sleep(0.1)
            assert preview_status['state'] == 'complete' and preview_status['scope'] == scope, preview_status
            assert call('tracks') == cross_track_before
            previews.append(target)
        call('edit', 'crossfade-selected')
        cross_applied, cross_status = export_with_options('cross-track-applied.wav')
        assert cross_status['state'] == 'complete' and all(read_wav_data(path) == read_wav_data(cross_applied) for path in previews), cross_status
        call('edit', 'undo')
        assert call('tracks') == cross_track_before
        call('edit', 'undo')
        call('edit', 'undo')
        assert len(call('tracks')) == 1
        call('select', 'range', '0', '0', '0', '1102')
        call('edit', 'copy')
        call('seek', '1102')
        call('edit', 'paste', 'mix')
        mixed = os.path.join(directory, 'overlap.wav')
        call('export', 'wav', mixed)
        for _ in range(100):
            result = call('status')['export']
            if result['state'] != 'running':
                break
            time.sleep(0.1)
        assert result['state'] == 'complete', result
        with open(mixed, 'rb') as file:
            raw = file.read()
        cursor = 12
        while cursor + 8 <= len(raw):
            chunk, length = struct.unpack_from('<4sI', raw, cursor)
            cursor += 8
            if chunk == b'data':
                mixed_samples = struct.unpack_from('<%df' % (length // 4), raw, cursor)
                break
            cursor += length + (length & 1)
        else:
            raise AssertionError('Missing WAV data chunk')
        assert all(abs(value - 0.5) < 1e-6 for value in mixed_samples[1102 * 2:2204 * 2])
        assert all(abs(value - 0.25) < 1e-6 for value in mixed_samples[:1102 * 2] + mixed_samples[2204 * 2:])
        call('edit', 'undo')
        assert len(call('tracks')[0]['clips']) == 1
        assert command('ipc', 'edit', 'paste', 'invalid').returncode != 0
        call('clip', 'set', '0', '0', 'gain', '0.5')
        call('clip', 'set', '0', '0', 'fade-in', '1102')
        assert abs(call('tracks')[0]['clips'][0]['fade_in_seconds'] - 1102 / 44100) < 1e-7
        assert command('ipc', 'clip', 'set', '0', '0', 'fade-out', '5000').returncode != 0
        fade_file = os.path.join(directory, 'fade.wav')
        call('export', 'wav', fade_file)
        for _ in range(100):
            result = call('status')['export']
            if result['state'] != 'running':
                break
            time.sleep(0.1)
        assert result['state'] == 'complete', result
        with open(fade_file, 'rb') as file:
            raw = file.read()
        cursor = 12
        while cursor + 8 <= len(raw):
            chunk, length = struct.unpack_from('<4sI', raw, cursor)
            cursor += 8
            if chunk == b'data':
                faded = struct.unpack_from('<%df' % (length // 4), raw, cursor)
                break
            cursor += length + (length & 1)
        else:
            raise AssertionError('Missing WAV data chunk')
        assert all(abs(faded[2 * i + channel] - 0.125 * min(1.0, i / 1102)) < 1e-6
                   for i in range(4410) for channel in range(2))
        call('edit', 'undo')
        call('edit', 'undo')
        assert call('tracks')[0]['clips'][0]['gain'] == 1
        call('select', 'range', '0', '0', '0', '1102')
        call('edit', 'cut')
        assert all(clip['start_seconds'] >= 1102 / 44100 - 1e-8 for clip in call('tracks')[0]['clips'])
        call('seek', '1102')
        call('edit', 'paste', 'replace')
        assert len(call('tracks')[0]['clips']) >= 2
        call('edit', 'undo')
        call('edit', 'undo')
        assert len(call('tracks')[0]['clips']) == 1

        call('select', 'clip', '0', '0')
        call('seek', '2205')
        call('edit', 'split')
        assert len(call('tracks')[0]['clips']) == 2
        call('edit', 'undo')
        assert len(call('tracks')[0]['clips']) == 1
        call('edit', 'redo')
        assert len(call('tracks')[0]['clips']) == 2
        call('marker', 'add', '1102', 'Side A')
        marker_info = call('markers')[0]
        assert marker_info['name'] == 'Side A' and marker_info['sample'] == 1102
        assert marker_info['valid_position'] and not any(marker_info[key] for key in ('off_grid', 'outside_master', 'duplicate_sample', 'empty_name'))
        marker_id = marker_info['id']
        assert len(marker_id) == 32 and call('marker', 'find', marker_id) == 0
        duplicate = command('ipc', 'marker', 'add', '1102', 'Duplicate')
        assert duplicate.returncode != 0 and len(call('markers')) == 1
        call('marker', 'set', '0', 'sample', '1200')
        call('marker', 'set', 'id:' + marker_id, 'name', '終曲')
        assert call('markers')[0]['name'] == '終曲'
        assert call('markers')[0]['id'] == marker_id and call('marker', 'find', marker_id) == 0
        assert abs(call('markers')[0]['position_seconds'] * 44100 - 1200) < 1e-6
        assert command('ipc', 'marker', 'set', '0', 'sample', '4410').returncode != 0
        assert command('ipc', 'marker', 'set', '0', 'name', 'bad\nname').returncode != 0
        call('marker', 'add', '1300', 'Another')
        call('marker', 'set-positions', 'id:' + marker_id, '1300', '1', '1200')
        swapped = call('markers')
        assert swapped[0]['name'] == '終曲' and abs(swapped[0]['position_seconds'] * 44100 - 1300) < 1e-6
        assert swapped[1]['name'] == 'Another' and abs(swapped[1]['position_seconds'] * 44100 - 1200) < 1e-6
        assert [marker['sample'] for marker in swapped] == [1300, 1200]
        collision = command('ipc', 'marker', 'set-positions', '0', '1200', '1', '1200')
        assert collision.returncode != 0 and call('markers') == swapped
        call('edit', 'undo')
        call('marker', 'nudge', 'id:' + marker_id, '1')
        assert abs(call('markers')[0]['position_seconds'] * 44100 - 1201) < 1e-6
        assert command('ipc', 'marker', 'nudge', '0', '2').returncode != 0
        call('edit', 'undo')
        assert abs(call('markers')[0]['position_seconds'] * 44100 - 1200) < 1e-6
        assert command('ipc', 'marker', 'set', '0', 'sample', '1300').returncode != 0
        assert abs(call('markers')[0]['position_seconds'] * 44100 - 1200) < 1e-6
        call('edit', 'undo')
        call('edit', 'undo')
        assert call('markers')[0]['name'] == 'Side A'
        call('edit', 'undo')
        assert abs(call('markers')[0]['position_seconds'] * 44100 - 1102) < 1e-6
        call('edit', 'redo')
        call('edit', 'redo')
        assert call('markers')[0]['name'] == '終曲'
        call('edit', 'undo')
        call('edit', 'undo')
        call('marker', 'delete', 'id:' + marker_id)
        assert call('markers') == []
        call('edit', 'undo')
        assert len(call('markers')) == 1
        call('select', 'range', '0', '0', '0', '1102')
        selection = call('status')['selection']
        assert selection['start_sample'] == 0 and selection['end_sample'] == 1102
        call('loop', 'from-selection')
        assert call('status')['loop_region'] == {'start_sample': 0, 'end_sample': 1102, 'valid': True, 'playing': False}
        assert command('ipc', 'loop', 'set', '3000', '5000').returncode != 0
        call('select', 'range', '0', '0', '1102', '2205')
        assert call('status')['loop_region']['end_sample'] == 1102
        call('loop', 'set', '100', '2000')
        assert call('status')['loop_region']['start_sample'] == 100
        call('loop', 'clear')
        assert call('status')['loop_region'] is None
        call('select', 'range', '0', '0', '0', '1102')
        call('edit', 'delete')
        assert all(clip['start_seconds'] >= 1102 / 44100 - 1e-8 for clip in call('tracks')[0]['clips'])
        output = os.path.join(directory, 'rendered audio.wav')
        call('export', 'wav', output)
        for _ in range(100):
            exported = call('status')['export']
            if exported['state'] != 'running':
                break
            time.sleep(0.1)
        assert exported['state'] == 'complete' and exported['path'] == output, exported
        with open(output, 'rb') as file:
            data = file.read()
        assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
        cursor = 12
        samples = None
        while cursor + 8 <= len(data):
            chunk, length = struct.unpack_from('<4sI', data, cursor)
            cursor += 8
            if chunk == b'data':
                samples = struct.unpack_from('<%df' % (length // 4), data, cursor)
                break
            cursor += length + (length & 1)
        assert samples is not None and len(samples) == 4410 * 2
        assert all(value == 0 for value in samples[:1102 * 2])
        assert all(abs(value - 0.25) < 1e-6 for value in samples[1102 * 2:])
        prior = data
        refused = command('ipc', 'export', 'wav', output)
        assert refused.returncode != 0 and not json.loads(refused.stdout)['ok']
        with open(output, 'rb') as file:
            assert file.read() == prior
        refused_source = command('ipc', 'export', 'wav', source)
        assert refused_source.returncode != 0 and not json.loads(refused_source.stdout)['ok']
        call('edit', 'undo')
        assert call('tracks')[0]['clips'][0]['start_seconds'] == 0
        stereo_source = os.path.join(directory, 'stereo.wav')
        with wave.open(stereo_source, 'wb') as audio:
            audio.setnchannels(2)
            audio.setsampwidth(2)
            audio.setframerate(48000)
            audio.writeframes(struct.pack('<hh', 4096, -4096) * 4800)
        call('import', '--new-layer', stereo_source)
        for _ in range(100):
            state = call('status')
            if state['pending_imports'] == 0 and state['track_count'] == 2:
                break
            time.sleep(0.1)
        assert state['pending_imports'] == 0 and state['track_count'] == 2, state
        assert call('tracks')[1]['clips'][0]['source_sample_rate'] == 48000
        assert call('tracks')[1]['clips'][0]['source_channels'] == 2

        def render(filename, channels, expected):
            target = os.path.join(directory, filename)
            call('export', 'wav', target)
            for _ in range(100):
                result = call('status')['export']
                if result['state'] != 'running':
                    break
                time.sleep(0.1)
            assert result['state'] == 'complete' and result['directory_sync'] in ('confirmed', 'unconfirmed', 'unsupported'), result
            with open(target, 'rb') as file:
                contents = file.read()
            offset = 12
            while offset + 8 <= len(contents):
                tag, length = struct.unpack_from('<4sI', contents, offset)
                offset += 8
                if tag == b'data':
                    values = struct.unpack_from('<%df' % (length // 4), contents, offset)
                    assert len(values) == 4410 * channels
                    assert all(abs(value - expected[i % channels]) < 1e-6 for i, value in enumerate(values))
                    return
                offset += length + (length & 1)
            raise AssertionError('Missing WAV data')

        render('mix.wav', 2, (0.375, 0.125))
        assert command('ipc', 'track', 'stereo', '0', 'swap').returncode != 0
        call('track', 'stereo', '1', 'swap')
        render('stereo-swapped.wav', 2, (0.125, 0.375))
        call('track', 'stereo', '1', 'fold')
        render('stereo-folded.wav', 2, (0.25, 0.25))
        assert command('ipc', 'track', 'stereo', '1', 'fold').returncode != 0
        call('edit', 'undo')
        call('edit', 'undo')
        render('stereo-routing-undo.wav', 2, (0.375, 0.125))
        assert command('ipc', 'track', 'channels', '0', 'split').returncode != 0
        call('track', 'channels', '1', 'split')
        assert len(call('tracks')) == 3
        render('stereo-channels-split.wav', 2, (0.375, 0.125))
        call('track', 'channels', '1', 'combine')
        assert len(call('tracks')) == 2
        render('stereo-channels-combined.wav', 2, (0.375, 0.125))
        call('edit', 'undo')
        call('edit', 'undo')
        render('stereo-channels-undo.wav', 2, (0.375, 0.125))
        call('track', 'set', '0', 'gain', '0.5')
        call('track', 'set', '0', 'balance', '0.5')
        render('adjusted-mix.wav', 2, (0.1875, 0.0))
        assert command('ipc', 'track', 'set', '0', 'gain', 'nan').returncode != 0
        assert command('ipc', 'track', 'set', '0', 'balance', '1.1').returncode != 0
        call('edit', 'undo')
        call('edit', 'undo')
        render('mix-after-undo.wav', 2, (0.375, 0.125))
        call('track', 'set', '1', 'solo', 'on')
        render('solo.wav', 2, (0.125, -0.125))
        call('master', 'mono')
        render('mono.wav', 1, (0.0,))
        call('edit', 'undo')
        assert call('status')['master_channels'] == 2
        call('track', 'set', '1', 'solo', 'off')
        call('track', 'set', '1', 'mute', 'on')
        render('muted.wav', 2, (0.25, 0.25))
        call('edit', 'undo')
        render('restored.wav', 2, (0.375, 0.125))
        moving_source = os.path.join(directory, 'varying-48k.wav')
        with wave.open(moving_source, 'wb') as audio:
            audio.setnchannels(2)
            audio.setsampwidth(2)
            audio.setframerate(48000)
            audio.writeframes(b''.join(struct.pack('<hh', round(6000 * math.sin(i * 0.08)),
                                                   round(4000 * math.cos(i * 0.13))) for i in range(4800)))
        call('import', '--new-layer', moving_source)
        for _ in range(100):
            state = call('status')
            if state['pending_imports'] == 0 and state['track_count'] == 3:
                break
            time.sleep(0.1)
        assert state['pending_imports'] == 0 and state['track_count'] == 3, state
        call('track', 'set', '2', 'solo', 'on')
        def export_bytes(filename):
            target = os.path.join(directory, filename)
            call('export', 'wav', target)
            for _ in range(100):
                result = call('status')['export']
                if result['state'] != 'running':
                    break
                time.sleep(0.1)
            assert result['state'] == 'complete', result
            with open(target, 'rb') as file:
                return file.read()
        before_split = export_bytes('varying-before.wav')
        offset = 12
        varying_samples = None
        while offset + 8 <= len(before_split):
            tag, length = struct.unpack_from('<4sI', before_split, offset)
            offset += 8
            if tag == b'data':
                varying_samples = struct.unpack_from('<%df' % (length // 4), before_split, offset)
                break
            offset += length + (length & 1)
        assert varying_samples is not None and len(varying_samples) == 4410 * 2
        assert max(varying_samples[::2]) > 0.15 and min(varying_samples[::2]) < -0.15
        assert max(varying_samples[1::2]) > 0.1 and min(varying_samples[1::2]) < -0.1
        call('select', 'clip', '2', '0')
        call('seek', '2205')
        call('edit', 'split')
        after_split = export_bytes('varying-after.wav')
        assert before_split == after_split, '48 kHz split changed rendered output'
        call('edit', 'undo')
        assert export_bytes('varying-undo.wav') == before_split
        call('edit', 'redo')
        assert export_bytes('varying-redo.wav') == before_split
        call('marker', 'add', '3000', 'Finale')
        master_tracks = call('master-tracks')
        assert [(row['start_sample'], row['end_sample']) for row in master_tracks] == [(0, 1102), (1102, 3000), (3000, 4410)]
        assert [row['name'] for row in master_tracks] == ['Track 1', 'Side A', 'Finale']
        call('master-track', 'rename', '0', 'Opening')
        assert call('master-tracks')[0]['name'] == 'Opening'
        call('edit', 'undo')
        assert call('master-tracks')[0]['name'] == 'Track 1'
        call('master-track', 'rename', '1', 'Side B')
        assert call('master-tracks')[1]['name'] == 'Side B'
        call('edit', 'undo')
        assert call('master-tracks')[1]['name'] == 'Side A'
        call('view', 'master-track', '1')
        fitted = call('view', 'status')
        assert fitted['start_seconds'] <= 1102 / 44100 and fitted['end_seconds'] >= 3000 / 44100, fitted
        assert command('ipc', 'view', 'master-track', '3').returncode != 0
        assert call('view', 'status') == fitted
        call('view', 'fit')
        def wav_payload(blob):
            offset = 12
            while offset + 8 <= len(blob):
                tag, length = struct.unpack_from('<4sI', blob, offset)
                offset += 8
                if tag == b'data':
                    return blob[offset:offset + length]
                offset += length + (length & 1)
            raise AssertionError('Missing WAV data')
        regions = []
        for index, row in enumerate(master_tracks):
            target = os.path.join(directory, 'region %d.wav' % index)
            call('export', 'track', str(index), target)
            for _ in range(100):
                region = call('status')['export']
                if region['state'] != 'running':
                    break
                time.sleep(0.1)
            assert region['state'] == 'complete' and (region['start_sample'], region['end_sample']) == (row['start_sample'], row['end_sample']), region
            with open(target, 'rb') as file:
                payload = wav_payload(file.read())
            assert len(payload) == (row['end_sample'] - row['start_sample']) * 2 * 4
            regions.append(payload)
        assert b''.join(regions) == wav_payload(before_split)
        batch_parent = os.path.join(directory, 'batch parent')
        os.mkdir(batch_parent)
        relative_batch = subprocess.run([binary, 'ipc', 'export', 'tracks', os.path.basename(batch_parent), 'pcm16', 'dither'], cwd=directory, text=True, capture_output=True, timeout=8)
        assert relative_batch.returncode == 0, (relative_batch.stdout, relative_batch.stderr)
        for _ in range(100):
            batch = call('status')['export']
            if batch['state'] != 'running':
                break
            time.sleep(0.1)
        assert batch['state'] == 'complete' and batch['expected_tracks'] == batch['completed_tracks'] == 3 and batch['manifest_written'] and batch['file_written'] and batch['dither'], batch
        folder = batch['output_folder']
        with open(os.path.join(folder, 'Carta tracks.json'), encoding='utf-8') as file:
            manifest = json.load(file)
        assert [(track['startSample'], track['endSample'], track['name']) for track in manifest['tracks']] == [(row['start_sample'], row['end_sample'], row['name']) for row in master_tracks], manifest
        dithered_master, dithered_status = export_with_options('batch reference pcm16.wav', 'pcm16', 'dither')
        assert dithered_status['state'] == 'complete'
        batch_payload = b''.join(read_wav_data(os.path.join(folder, 'Track %02d.wav' % (index + 1))) for index in range(3))
        assert batch_payload == read_wav_data(dithered_master), 'Batch PCM dither must match the full master sample-for-sample'
        call('export', 'tracks', batch_parent, 'pcm16', 'dither')
        for _ in range(100):
            second_batch = call('status')['export']
            if second_batch['state'] != 'running':
                break
            time.sleep(0.1)
        assert second_batch['state'] == 'complete' and second_batch['output_folder'] != folder and os.path.exists(os.path.join(folder, 'Carta tracks.json')), second_batch
        rejected = command('ipc', 'export', 'track', '999', os.path.join(directory, 'invalid.wav'))
        assert rejected.returncode != 0 and not os.path.exists(os.path.join(directory, 'invalid.wav'))
        project_path = os.path.join(directory, 'edited project.carta')
        call('project', 'save-copy', project_path)
        for _ in range(100):
            saved = call('status')['project_operation']
            if saved['state'] != 'running':
                break
            time.sleep(0.1)
        assert saved['state'] == 'complete' and saved['path'] == project_path and saved['directory_sync'] in ('confirmed', 'unconfirmed', 'unsupported'), saved
        saved_state = call('status')
        assert saved_state['dirty'] and saved_state['project'] == ''
        original_project = open(project_path, 'rb').read()
        refused = command('ipc', 'project', 'save-copy', project_path)
        assert refused.returncode != 0 and open(project_path, 'rb').read() == original_project
        refused = command('ipc', 'project', 'open', project_path)
        assert refused.returncode != 0 and call('status')['track_count'] == 3
        process.terminate()
        process.wait(timeout=5)
        process = subprocess.Popen([binary], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   env={**os.environ, 'CARTA_STUDIO_RECOVERY_DIR': directory})
        for _ in range(100):
            initial = command('ipc', 'status')
            if initial.returncode == 0:
                break
            assert process.poll() is None, 'Editor exited during restart'
            time.sleep(0.1)
        assert initial.returncode == 0
        call('project', 'open', project_path)
        for _ in range(100):
            loaded = call('status')['project_operation']
            if loaded['state'] != 'running':
                break
            time.sleep(0.1)
        assert loaded['state'] == 'complete', loaded
        loaded_state = call('status')
        assert loaded_state['project'] == project_path and not loaded_state['dirty'] and loaded_state['track_count'] == 3
        assert call('markers')[0]['name'] == 'Side A'
        assert len(call('tracks')[2]['clips']) == 2 and call('tracks')[2]['solo']
        assert export_bytes('reopened.wav') == before_split
        saved_marker_id = call('markers')[0]['id']
        call('marker', 'set', 'id:' + saved_marker_id, 'name', 'Saved via IPC')
        call('project', 'save')
        for _ in range(100):
            save_result = call('status')['project_operation']
            if save_result['state'] != 'running':
                break
            time.sleep(0.1)
        assert save_result['state'] == 'complete' and save_result['clean'] and save_result['file_written'], save_result
        saved_project = project_metadata(project_path)
        assert saved_project['markers'][0]['id'] == saved_marker_id and saved_project['markers'][0]['name'] == 'Saved via IPC'
        call('edit', 'undo')
        call('project', 'save')
        for _ in range(100):
            save_result = call('status')['project_operation']
            if save_result['state'] != 'running':
                break
            time.sleep(0.1)
        assert save_result['state'] == 'complete' and save_result['clean'], save_result
        renamed_source = moving_source + '.missing'
        os.rename(moving_source, renamed_source)
        try:
            call('project', 'open', project_path)
            for _ in range(100):
                rejected = call('status')['project_operation']
                if rejected['state'] != 'running':
                    break
                time.sleep(0.1)
            assert rejected['state'] == 'complete', rejected
            assert call('status')['track_count'] == 3 and call('markers')[0]['name'] == 'Side A'
        finally:
            os.rename(renamed_source, moving_source)
        invalid_track = command('ipc', 'track', 'set', '7', 'mute', 'on')
        assert invalid_track.returncode != 0 and not json.loads(invalid_track.stdout)['ok']
        invalid = command('ipc', 'seek', '1garbage')
        assert invalid.returncode != 0 and not json.loads(invalid.stdout)['ok']
        before_tracks = call('tracks')
        call('select', 'tracks', '0', '1102', '2', '0')
        assert call('status')['selected_tracks'] == [0, 2]
        def export_selection(mode, filename):
            target = os.path.join(directory, filename)
            call('export', 'selection', mode, target)
            for _ in range(100):
                result = call('status')['export']
                if result['state'] != 'running':
                    break
                time.sleep(0.1)
            assert result['state'] == 'complete' and result['start_sample'] == 0 and result['end_sample'] == 1102, result
            with open(target, 'rb') as file:
                return wav_payload(file.read())
        assert export_selection('full', 'selection-full.wav') == wav_payload(before_split)[:1102 * 2 * 4]
        assert command('ipc', 'export', 'selection', 'full', os.path.join(directory, 'selection-full.wav')).returncode != 0
        assert export_selection('tracks', 'selection-independent.wav') == wav_payload(before_split)[:1102 * 2 * 4]
        call('select', 'tracks', '0', '1102', '0')
        selected_only = export_selection('tracks', 'selection-track-zero.wav')
        call('track', 'set', '2', 'solo', 'off')
        call('track', 'set', '1', 'mute', 'on')
        call('track', 'set', '2', 'mute', 'on')
        assert export_selection('full', 'selection-reference.wav') == selected_only
        call('edit', 'undo')
        call('edit', 'undo')
        call('edit', 'undo')
        call('select', 'tracks', '0', '1102', '2', '0')
        invalid = command('ipc', 'select', 'tracks', '0', '1102', '0', '0')
        assert invalid.returncode != 0 and call('status')['selected_tracks'] == [0, 2]
        call('edit', 'copy')
        call('edit', 'cut')
        after_cut = call('tracks')
        assert after_cut[1] == before_tracks[1]
        assert all(clip['start_seconds'] >= 1102 / 44100 - 1e-8 for clip in after_cut[0]['clips'])
        assert all(clip['start_seconds'] >= 1102 / 44100 - 1e-8 for clip in after_cut[2]['clips'])
        cut_audio = wav_payload(export_bytes('independent-cut.wav'))
        assert cut_audio[:1102 * 2 * 4] == b'\0' * (1102 * 2 * 4)
        call('edit', 'undo')
        assert call('tracks') == before_tracks
        assert call('status')['selected_tracks'] == [0, 2]
        assert export_bytes('independent-undo.wav') == before_split
        call('select', 'clips', '0', '0', '2', '0')
        selected_clips = [{'layer': 0, 'track': 0, 'clip': 0}, {'layer': 2, 'track': 2, 'clip': 0}]
        assert call('status')['selected_clips'] == selected_clips
        assert command('ipc', 'clip', 'envelope-selected', '0.5', '999999', 'keep').returncode != 0
        assert call('tracks') == before_tracks
        call('clip', 'envelope-selected', '0.5', '100', 'keep')
        envelopes = call('tracks')
        assert envelopes[0]['clips'][0]['gain'] == 0.5 and envelopes[2]['clips'][0]['gain'] == 0.5
        assert abs(envelopes[0]['clips'][0]['fade_in_seconds'] - 100 / 44100) < 1e-12
        assert envelopes[1] == before_tracks[1] and call('status')['selected_clips'] == selected_clips
        assert call('clip', 'envelope-selected', '0.5', '100', 'keep') == 'Unchanged'
        call('edit', 'undo')
        assert call('tracks') == before_tracks and call('status')['selected_clips'] == selected_clips
        def export_clips(scope, filename):
            target = os.path.join(directory, filename)
            call('export', scope, target)
            for _ in range(100):
                state = call('status')['export']
                if state['state'] != 'running':
                    break
                time.sleep(0.1)
            assert state['state'] == 'complete', state
            with open(target, 'rb') as file:
                return state, wav_payload(file.read())
        state, group_audio = export_clips('clips', 'isolated group.wav')
        assert (state['start_sample'], state['end_sample']) == (0, 2205)
        assert group_audio == wav_payload(before_split)[:2205 * 2 * 4]
        call('track', 'set', '2', 'solo', 'off')
        call('track', 'set', '1', 'mute', 'on')
        _, overlapping_audio = export_clips('clips', 'isolated overlap.wav')
        assert overlapping_audio == wav_payload(export_bytes('isolated overlap reference.wav'))[:2205 * 2 * 4]
        call('edit', 'undo')
        call('edit', 'undo')
        call('select', 'clip', '2', '0')
        state, focused_audio = export_clips('clip', 'isolated clip.wav')
        assert (state['start_sample'], state['end_sample']) == (0, 2205)
        assert focused_audio == wav_payload(before_split)[:2205 * 2 * 4]
        call('select', 'clips', '0', '0', '2', '0')
        before_marker_edit = call('markers')
        call('marker', 'add', '1750', 'Temporary')
        assert call('status')['selected_clips'] == selected_clips
        call('marker', 'delete', str(len(before_marker_edit)))
        assert call('status')['selected_clips'] == selected_clips and call('markers') == before_marker_edit
        call('track', 'set', '1', 'mute', 'on')
        assert call('status')['selected_clips'] == selected_clips
        call('edit', 'undo')
        assert call('status')['selected_clips'] == selected_clips and call('tracks') == before_tracks
        call('clip', 'set', '0', '0', 'gain', '0.5')
        assert call('status')['selected_clips'] == selected_clips
        call('edit', 'undo')
        assert call('status')['selected_clips'] == selected_clips and call('tracks') == before_tracks
        assert command('ipc', 'select', 'clips', '0', '0', '0', '0').returncode != 0
        assert call('status')['selected_clips'] == selected_clips
        assert command('ipc', 'edit', 'trim-group', 'start', '-1').returncode != 0
        assert call('tracks') == before_tracks
        call('edit', 'trim-group', 'start', '500')
        assert call('status')['selected_clips'] == selected_clips
        assert call('tracks')[1] == before_tracks[1]
        trimmed = wav_payload(export_bytes('group-trim.wav'))
        baseline = wav_payload(before_split)
        assert trimmed[:500 * 2 * 4] == b'\0' * (500 * 2 * 4)
        assert trimmed[500 * 2 * 4:] == baseline[500 * 2 * 4:]
        call('edit', 'undo')
        assert call('tracks') == before_tracks
        assert call('status')['selected_clips'] == selected_clips
        assert export_bytes('group-trim-undo.wav') == before_split
        call('seek', '1000')
        call('edit', 'align-group', 'start')
        aligned_start = wav_payload(export_bytes('group-align-start.wav'))
        assert aligned_start[:1000 * 2 * 4] == b'\0' * (1000 * 2 * 4)
        assert aligned_start[1000 * 2 * 4:] == baseline[1000 * 2 * 4:]
        assert call('status')['selected_clips'] == selected_clips
        call('edit', 'undo')
        call('seek', '1000')
        call('edit', 'align-group', 'end')
        aligned_end = wav_payload(export_bytes('group-align-end.wav'))
        assert aligned_end[:1000 * 2 * 4] == baseline[:1000 * 2 * 4]
        assert aligned_end[1000 * 2 * 4:2205 * 2 * 4] == b'\0' * ((2205 - 1000) * 2 * 4)
        assert aligned_end[2205 * 2 * 4:] == baseline[2205 * 2 * 4:]
        call('edit', 'undo')
        assert call('tracks') == before_tracks and call('status')['selected_clips'] == selected_clips
        call('select', 'clips', '0', '0', '2', '0')
        call('edit', 'clips', 'copy')
        call('seek', '2205')
        call('edit', 'paste', 'mix')
        assert len(call('status')['selected_clips']) == 2
        mixed_payload = wav_payload(export_bytes('group-paste.wav'))
        base_values = struct.unpack('<%df' % (len(baseline) // 4), baseline)
        mixed_values = struct.unpack('<%df' % (len(mixed_payload) // 4), mixed_payload)
        assert len(mixed_values) == len(base_values)
        assert all(abs(mixed_values[i] - (base_values[i] + (base_values[i - 4410] if i >= 4410 else 0))) < 1e-6 for i in range(len(base_values)))
        call('edit', 'undo')
        assert export_bytes('group-paste-undo.wav') == before_split
        call('select', 'clips', '0', '0', '2', '0')
        call('edit', 'clips', 'cut')
        assert call('tracks')[1] == before_tracks[1]
        cut_group = wav_payload(export_bytes('group-cut.wav'))
        assert cut_group[:2205 * 2 * 4] == b'\0' * (2205 * 2 * 4)
        assert cut_group[2205 * 2 * 4:] == baseline[2205 * 2 * 4:]
        call('edit', 'undo')
        assert call('tracks') == before_tracks
        call('select', 'clips', '0', '0', '2', '0')
        call('edit', 'clips', 'duplicate')
        assert len(call('status')['selected_clips']) == 2
        call('edit', 'undo')
        call('select', 'clips', '0', '0', '2', '0')
        call('seek', '1000')
        call('edit', 'clips', 'split')
        assert len(call('status')['selected_clips']) == 4
        assert export_bytes('group-split.wav') == before_split
        call('edit', 'clips', 'join')
        assert len(call('status')['selected_clips']) == 2
        assert export_bytes('group-join.wav') == before_split
        call('edit', 'undo')
        assert len(call('status')['selected_clips']) == 4
        call('edit', 'redo')
        assert len(call('status')['selected_clips']) == 2
        call('edit', 'undo')
        call('edit', 'undo')
        assert call('tracks') == before_tracks
        assert len(call('status')['selected_clips']) == 2
        call('edit', 'redo')
        assert len(call('status')['selected_clips']) == 4
        call('edit', 'undo')
        assert len(call('status')['selected_clips']) == 2
        call('select', 'clip', '0', '0')
        assert command('ipc', 'edit', 'clips', 'delete').returncode != 0
        call('select', 'clips', '0', '0', '2', '0')
        call('edit', 'clips', 'delete')
        assert wav_payload(export_bytes('group-delete.wav')) == cut_group
        call('edit', 'undo')
        assert call('tracks') == before_tracks
        original_markers = call('markers')
        call('select', 'clips', '2', '0', '0', '0')
        assert call('marker', 'from-clips', 'start')['added'] == 1
        assert call('marker', 'from-clips', 'end')['added'] == 1
        boundaries = call('master-tracks')
        assert [(row['start_sample'], row['end_sample']) for row in boundaries] == [(0, 1102), (1102, 2205), (2205, 3000), (3000, 4410)]
        assert boundaries[0]['name'] == before_tracks[0]['clips'][0]['name']
        assert boundaries[2]['name'] == 'After ' + before_tracks[0]['clips'][0]['name']
        unchanged_markers = call('markers')
        assert command('ipc', 'marker', 'from-clips', 'end').returncode != 0
        assert call('markers') == unchanged_markers
        pieces = []
        for index in range(len(boundaries)):
            target = os.path.join(directory, 'clip-boundary-track-%d.wav' % index)
            call('export', 'track', str(index), target)
            for _ in range(100):
                result = call('status')['export']
                if result['state'] != 'running':
                    break
                time.sleep(0.1)
            assert result['state'] == 'complete', result
            with open(target, 'rb') as file:
                pieces.append(wav_payload(file.read()))
        assert b''.join(pieces) == baseline
        call('edit', 'undo')
        call('edit', 'undo')
        assert call('markers') == original_markers and call('tracks') == before_tracks
        assert command('ipc', 'export', 'cancel').returncode != 0
        surround_source = os.path.join(directory, 'four channel.wav')
        with wave.open(surround_source, 'wb') as audio:
            audio.setnchannels(4)
            audio.setsampwidth(2)
            audio.setframerate(44100)
            audio.writeframes(struct.pack('<4h', 4096, 8192, 12288, 16384) * 64)
        call('import', '--new-layer', surround_source)
        for _ in range(200):
            state = call('status')
            if state['pending_imports'] == 0:
                break
            time.sleep(0.05)
        assert state['track_count'] == 4 and state['pending_imports'] == 0, state
        call('track', 'set', '3', 'channel-heights', '4,3,2,1')
        assert call('tracks')[3]['channel_height_weights'] == [4, 3, 2, 1]
        assert command('ipc', 'track', 'set', '3', 'channel-heights', '1,2').returncode != 0
        call('edit', 'undo')
        assert call('tracks')[3]['channel_height_weights'] == []
        call('track', 'set', '2', 'solo', 'off')
        call('track', 'set', '3', 'solo', 'on')
        assert not call('tracks')[3]['routing_confirmed']
        assert command('ipc', 'export', 'wav', os.path.join(directory, 'unconfirmed.wav')).returncode != 0
        assert command('ipc', 'track', 'routing', '3', '1', '0', '0', '1').returncode != 0
        assert command('ipc', 'track', 'routing', '3', '1', '0', '0', '1', '0.5', '0', '0', 'nan').returncode != 0
        assert not call('tracks')[3]['routing_confirmed']
        call('track', 'routing', '3', '1', '0', '0', '1', '0.5', '0', '0', '0.25')
        routed = call('tracks')[3]
        assert routed['routing_confirmed'] and routed['routing'] == [{'left': 1, 'right': 0}, {'left': 0, 'right': 1}, {'left': 0.5, 'right': 0}, {'left': 0, 'right': 0.25}]
        left, right = struct.unpack_from('<2f', wav_payload(export_bytes('ipc routed stereo.wav')))
        assert abs(left - 0.3125) < 1e-7 and abs(right - 0.375) < 1e-7
        call('master', 'mono')
        mono, = struct.unpack_from('<f', wav_payload(export_bytes('ipc routed mono.wav')))
        assert abs(mono - 0.34375) < 1e-7
        call('edit', 'undo')
        call('track', 'routing', '3', 'clear')
        assert not call('tracks')[3]['routing_confirmed']
        assert command('ipc', 'export', 'wav', os.path.join(directory, 'cleared.wav')).returncode != 0
        call('edit', 'undo')
        assert call('tracks')[3]['routing'] == routed['routing']
        call('edit', 'undo')
        assert not call('tracks')[3]['routing_confirmed']
        call('edit', 'redo')
        assert call('tracks')[3]['routing'] == routed['routing']
        long_source = os.path.join(directory, 'long silence.wav')
        with wave.open(long_source, 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(44100)
            for _ in range(120):
                audio.writeframesraw(b'\0' * (44100 * 2))
        call('import', '--new-layer', long_source)
        for _ in range(200):
            state = call('status')
            if state['pending_imports'] == 0:
                break
            time.sleep(0.1)
        assert state['pending_imports'] == 0 and state['track_count'] == 5, state
        cancelled_path = os.path.join(directory, 'cancelled.wav')
        call('export', 'wav', cancelled_path)
        running = call('status')['export']
        assert running['state'] == 'running' and running['phase'] in ('preflight', 'writing'), running
        assert 0 <= running['progress'] <= 1 and not running['cancellation_requested'], running
        # The worker can finish after the status reply but before this request.
        cancellation = command('ipc', 'export', 'cancel')
        for _ in range(200):
            result = call('status')['export']
            if result['state'] != 'running':
                break
            time.sleep(0.05)
        assert result['phase'] == 'finished' and result['state'] in ('cancelled', 'complete'), result
        if result['state'] == 'cancelled':
            assert cancellation.returncode == 0 and result['cancellation_requested'] and not os.path.exists(cancelled_path), result
        else:
            assert os.path.exists(cancelled_path) and result['progress'] == 1.0, result
        assert command('ipc', 'export', 'cancel').returncode != 0
        loop_end = min(100, call('master', 'duration')['end_sample'])
        assert loop_end > 0
        call('loop', 'set', '0', str(loop_end))
        assert call('status')['dirty']
        save_as_path = os.path.join(directory, 'named through ipc.carta')
        call('project', 'save-as', save_as_path)
        project_progress = call('project', 'status')
        assert project_progress['state'] in ('running', 'complete') and 0.0 <= project_progress['progress'] <= 1.0, project_progress
        for _ in range(200):
            save_as_result = call('status')['project_operation']
            if save_as_result['state'] != 'running':
                break
            time.sleep(0.05)
        assert save_as_result['state'] == 'complete' and save_as_result['clean'] and save_as_result['file_written'] and not save_as_result['stale'], save_as_result
        assert save_as_result['progress'] == 1.0 and not save_as_result['cancellation_requested'], save_as_result
        assert call('project', 'status') == save_as_result
        assert command('ipc', 'project', 'cancel').returncode != 0
        assert call('status')['project'] == save_as_path and not call('status')['dirty']
        with open(save_as_path, 'rb') as project_file:
            saved_bytes = project_file.read()
        saved_json = project_metadata(save_as_path)
        assert saved_json['version'] == 14 and (saved_json['loopStartSample'], saved_json['loopEndSample']) == (0, loop_end), saved_json
        call('loop', 'clear')
        assert call('project', 'status')['stale'] and call('status')['dirty']
        call('loop', 'set', '0', str(loop_end))
        assert not call('project', 'status')['stale'] and not call('status')['dirty']
        call('project', 'open', save_as_path)
        for _ in range(200):
            opened = call('project', 'status')
            if opened['state'] != 'running':
                break
            time.sleep(0.05)
        restored = call('status')
        assert opened['state'] == 'complete' and not restored['dirty'] and not restored['playing'] and restored['loop_region'] == {'start_sample': 0, 'end_sample': loop_end, 'valid': True, 'playing': False}, (opened, restored)
        call('loop', 'clear')
        assert call('status')['dirty']
        call('loop', 'set', '0', str(loop_end))
        assert not call('status')['dirty']
        assert command('ipc', 'project', 'save-as', save_as_path).returncode != 0
        with open(save_as_path, 'rb') as project_file:
            assert project_file.read() == saved_bytes
        marker_input = os.path.join(directory, 'marker input.json')
        with open(marker_input, 'w', encoding='utf-8') as marker_file:
            json.dump({'format': 'carta-markers', 'version': 1, 'sampleRate': 44100,
                       'markers': [{'sample': 44100, 'name': '始まり'}, {'sample': 88200, 'name': 'Second'}]}, marker_file, ensure_ascii=False)

        def marker_exchange(action, kind, path):
            result = subprocess.run([binary, 'ipc', 'marker', action, kind, os.path.basename(path)], cwd=directory, text=True, capture_output=True, timeout=8)
            assert result.returncode == 0, (result.stdout, result.stderr)
            for _ in range(200):
                operation = call('marker', 'exchange', 'status')
                if operation['state'] != 'running':
                    return operation
                time.sleep(0.05)
            raise AssertionError('marker exchange timed out')

        imported = marker_exchange('import', 'json', marker_input)
        markers = call('markers')
        assert imported['state'] == 'complete' and imported['changed'] and imported['marker_count'] == 2, imported
        assert [m['name'] for m in markers] == ['始まり', 'Second'] and [round(m['position_seconds'] * 44100) for m in markers] == [44100, 88200], markers
        marker_ids = [m['id'] for m in markers]
        json_output = os.path.join(directory, 'exported markers.json')
        label_output = os.path.join(directory, 'exported labels.txt')
        assert marker_exchange('export', 'json', json_output)['file_written']
        assert marker_exchange('export', 'labels', label_output)['file_written']
        with open(json_output, encoding='utf-8') as marker_file:
            assert json.load(marker_file)['markers'] == [{'sample': 44100, 'name': '始まり'}, {'sample': 88200, 'name': 'Second'}]
        unchanged = marker_exchange('import', 'labels', label_output)
        assert unchanged['state'] == 'complete' and not unchanged['changed'] and [m['id'] for m in call('markers')] == marker_ids, unchanged
        with open(label_output, 'w', encoding='utf-8') as marker_file:
            marker_file.write('3.0\t3.0\tChanged\n')
        assert marker_exchange('import', 'labels', label_output)['changed'] and call('markers')[0]['name'] == 'Changed'
        call('edit', 'undo')
        assert [m['id'] for m in call('markers')] == marker_ids
        with open(label_output, 'w', encoding='utf-8') as marker_file:
            marker_file.write('1.0\t2.0\tUnsupported region\n')
        rejected = marker_exchange('import', 'labels', label_output)
        assert rejected['state'] == 'failed' and [m['id'] for m in call('markers')] == marker_ids, rejected
        assert command('ipc', 'marker', 'export', 'json', json_output).returncode != 0
        # A large arrangement listing must survive the old 1 MiB response limit
        # and UTF-8 characters split across socket reads.
        long_name = 'é' * 30000
        for _ in range(18):
            call('track', 'add', long_name)
        large_listing = command('ipc', 'tracks')
        assert large_listing.returncode == 0 and len(large_listing.stdout.encode('utf-8')) > 1024 * 1024, large_listing.stderr
        listed = json.loads(large_listing.stdout)['result']
        assert len(listed) >= 18 and all(track['name'] == long_name for track in listed[-18:])
        # Oversized requests are rejected without dispatching a partial command;
        # the endpoint must remain available for subsequent clients.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(8)
            connection.connect(f'/tmp/carta-studio-{os.getuid()}/control.sock')
            try:
                connection.sendall(b'["track","add","' + b'x' * (1024 * 1024) + b'"]\n')
                assert connection.recv(1) == b''
            except (BrokenPipeError, ConnectionResetError):
                pass
        assert call('status')['track_count'] == len(listed)
        silence_source = os.path.join(directory, 'source silence.wav')
        with wave.open(silence_source, 'wb') as audio:
            audio.setnchannels(2)
            audio.setsampwidth(2)
            audio.setframerate(44100)
            silent = lambda i: 3000 <= i < 10000 or 13000 <= i < 22000 or 24500 <= i < 24600
            audio.writeframes(b''.join(struct.pack('<hh', 0 if 100 <= i < 200 or silent(i) else 8192,
                                                   0 if silent(i) else 8192) for i in range(27000)))
        call('import', '--new-layer', silence_source)
        for _ in range(100):
            state = call('status')
            if state['pending_imports'] == 0 and state['track_count'] == len(listed) + 1:
                break
            time.sleep(0.1)
        assert state['track_count'] == len(listed) + 1, state
        source_track = str(len(listed))
        call('analysis', 'source-silence', source_track, '0', '50')
        for _ in range(100):
            source_silence = call('analysis', 'status')
            if source_silence['state'] != 'running':
                break
            time.sleep(0.1)
        gap = source_silence['gaps'][0]
        assert source_silence['state'] == 'complete' and len(source_silence['gaps']) == 3 and (gap['start_sample'], gap['end_sample']) == (3000, 10000) and 3000 < gap['detach_start_sample'] < gap['detach_end_sample'] < 10000, source_silence
        call('analysis', 'select', '0')
        assert call('status')['selection'] == {'first_track': len(listed), 'last_track': len(listed), 'start_sample': 3000, 'end_sample': 10000}
        assert call('analysis', 'detach', '0') == {'start_sample': gap['detach_start_sample'], 'end_sample': gap['detach_end_sample']}
        assert len(call('tracks')[int(source_track)]['clips']) == 2 and command('ipc', 'analysis', 'detach', '0').returncode != 0
        call('edit', 'undo')
        assert len(call('tracks')[int(source_track)]['clips']) == 1
        extreme_source = os.path.join(directory, 'extreme-rate.wav')
        with wave.open(extreme_source, 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(3072000)
            audio.writeframes(struct.pack('<h', 8192) * 3072)
        old_count = call('status')['track_count']
        call('import', '--new-layer', extreme_source)
        for _ in range(100):
            state = call('status')
            if state['pending_imports'] == 0:
                break
            time.sleep(0.1)
        assert state['track_count'] == old_count + 1, state
        call('track', 'set', str(old_count), 'solo', 'on')
        extreme_output = os.path.join(directory, 'extreme-rate-export.wav')
        rejected = command('ipc', 'export', 'wav', extreme_output)
        assert rejected.returncode != 0 and 'sample rate' in (rejected.stdout + rejected.stderr).lower() and not os.path.exists(extreme_output), (rejected.stdout, rejected.stderr)
        call('edit', 'undo')
        call('edit', 'undo')
        assert call('status')['track_count'] == old_count
        pair_indices = []
        for name, rate, value, frames in [('pair-left.wav', 44100, 8192, 1000), ('pair-right.wav', 48000, 4096, 1089)]:
            pair_file = os.path.join(directory, name)
            with wave.open(pair_file, 'wb') as audio:
                audio.setnchannels(1)
                audio.setsampwidth(2)
                audio.setframerate(rate)
                audio.writeframes(struct.pack('<h', value) * frames)
            old_count = call('status')['track_count']
            call('import', '--new-layer', pair_file)
            for _ in range(100):
                state = call('status')
                if state['pending_imports'] == 0 and state['track_count'] == old_count + 1:
                    break
                time.sleep(0.1)
            assert state['track_count'] == old_count + 1, state
            pair_indices.append(old_count)
        pair_output = os.path.join(directory, 'paired stereo.wav')
        relative_pair = subprocess.run([binary, 'ipc', 'render', 'stereo-pair', str(pair_indices[1]), str(pair_indices[0]), os.path.basename(pair_output)], cwd=directory, text=True, capture_output=True, timeout=8)
        assert relative_pair.returncode == 0, (relative_pair.stdout, relative_pair.stderr)
        for _ in range(200):
            operation = call('status')['export']
            if operation['state'] != 'running':
                break
            time.sleep(0.1)
        assert operation['state'] == 'complete' and operation['kind'] == 'stereo-pair' and (operation['start_sample'], operation['end_sample']) == (0, 1001), operation
        paired_track = call('tracks')[operation['track_index']]
        assert paired_track['muted'] and os.path.realpath(paired_track['clips'][0]['source']) == os.path.realpath(pair_output), paired_track
        with open(pair_output, 'rb') as file:
            payload = wav_payload(file.read())
        assert len(payload) == 1001 * 2 * 4, len(payload)
        samples = struct.unpack_from('<1000f', payload)
        assert all(abs(samples[2*i] - 0.125) < 1e-6 and abs(samples[2*i+1] - 0.25) < 1e-6 for i in range(500)), samples[:10]
        call('edit', 'undo')
        assert len(call('tracks')) == pair_indices[1] + 1 and os.path.exists(pair_output)
        call('analysis', 'source-silence', source_track, '0', '50')
        for _ in range(100):
            source_silence = call('analysis', 'status')
            if source_silence['state'] != 'running':
                break
            time.sleep(0.1)
        assert source_silence['state'] == 'complete', source_silence
        assert call('analysis', 'detach-all') == {'detached': 2, 'skipped': 1}
        assert len(call('tracks')[int(source_track)]['clips']) == 3 and command('ipc', 'analysis', 'detach-all').returncode != 0
        call('edit', 'undo')
        assert len(call('tracks')[int(source_track)]['clips']) == 1
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
print('IPC smoke test passed; editor stopped')
