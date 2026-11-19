#!/usr/bin/env python3
"""Verify multichannel consolidation and its undo through the real IPC client."""
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


def float_samples(path):
    with open(path, 'rb') as stream:
        data = stream.read()
    offset = 12
    while offset + 8 <= len(data):
        tag, size = struct.unpack_from('<4sI', data, offset)
        offset += 8
        if tag == b'data':
            return struct.unpack_from('<%df' % (size // 4), data, offset)
        offset += size + (size & 1)
    raise AssertionError('WAV data chunk missing')


assert command('status').returncode != 0, 'Stop the existing Carta Studio IPC owner before this test'
with tempfile.TemporaryDirectory(prefix='carta multichannel ') as directory:
    source = os.path.join(directory, 'four channels.wav')
    samples = [((i * (ch * 2 + 3) + ch * 71) % 2001 - 1000) for i in range(1000) for ch in range(4)]
    with wave.open(source, 'wb') as audio:
        audio.setnchannels(4)
        audio.setsampwidth(2)
        audio.setframerate(44100)
        audio.writeframes(struct.pack('<%dh' % len(samples), *samples))
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
        call('import', source)
        wait_for(lambda: call('status'), lambda state: state['pending_imports'] == 0 and state['track_count'] == 1)
        assert call('tracks')[0]['clips'][0]['source_channels'] == 4
        call('track', 'routing', 0, '0.8', '0.1', '0.2', '0.7', '0.3', '0.05', '0.1', '0.4')
        call('select', 'clips', 0, 0)
        call('edit', 'clips', 'duplicate')
        call('edit', 'move-clip', 0, 1, 500)
        call('select', 'clips', 0, 0, 0, 1)

        def export(name):
            path = os.path.join(directory, name)
            call('export', 'wav', path, 'float32')
            state = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
            assert state['state'] == 'complete', state
            return float_samples(path)

        before = export('before.wav')
        output = os.path.join(directory, 'consolidated.wav')
        call('render', 'consolidate', output)
        result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
        assert result['state'] == 'complete', result
        tracks = call('tracks')
        assert len(tracks[0]['clips']) == 1 and tracks[0]['clips'][0]['source_channels'] == 4, tracks
        raw = float_samples(output)
        assert len(raw) == 1500 * 4, len(raw)
        for frame in (0, 100, 499):
            for ch in range(4):
                assert raw[frame * 4 + ch] == samples[frame * 4 + ch] / 32768, (frame, ch)
        for frame in (500, 599, 999):
            for ch in range(4):
                assert raw[frame * 4 + ch] == (samples[frame * 4 + ch] + samples[(frame - 500) * 4 + ch]) / 32768, (frame, ch)
        if shutil.which('ffmpeg'):
            decoded = subprocess.run(['ffmpeg', '-v', 'error', '-i', output, '-f', 'f32le', '-acodec', 'pcm_f32le', '-'], capture_output=True, check=True, timeout=15).stdout
            assert struct.unpack('<%df' % (len(decoded) // 4), decoded) == raw, 'FFmpeg must decode the same channel samples and order'
        after = export('after.wav')
        assert len(before) == len(after) == 3000
        assert max(abs(a - b) for a, b in zip(before, after)) < 1e-6
        call('edit', 'undo')
        assert len(call('tracks')[0]['clips']) == 2 and os.path.isfile(output)
        undone = export('undo.wav')
        assert before == undone
        call('edit', 'redo')
        assert len(call('tracks')[0]['clips']) == 1
        project = os.path.join(directory, 'multichannel.carta')
        call('project', 'save-as', project)
        saved = wait_for(lambda: call('project', 'status'), lambda state: state['state'] != 'running')
        assert saved['state'] == 'complete' and saved['clean'], saved
        call('project', 'open', project)
        opened = wait_for(lambda: call('project', 'status'), lambda state: state['state'] != 'running')
        assert opened['state'] == 'complete', opened
        assert call('tracks')[0]['clips'][0]['source_channels'] == 4
        assert export('reopened.wav') == after

        # Exercise both conventional surround sizes and discrete layouts above
        # the 32-bit WAV speaker-mask range. These are source lanes, not a
        # claim that they represent particular speaker positions.
        for channels in (3, 6, 8, 16, 33):
            call('track', 'remove', 0)
            path = os.path.join(directory, 'source-%d.wav' % channels)
            data = [((frame * (ch + 3) + ch * 73) % 2001 - 1000) for frame in range(80) for ch in range(channels)]
            with wave.open(path, 'wb') as audio:
                audio.setnchannels(channels)
                audio.setsampwidth(2)
                audio.setframerate(44100)
                audio.writeframes(struct.pack('<%dh' % len(data), *data))
            call('import', '--new-layer', path)
            wait_for(lambda: call('status'), lambda state: state['pending_imports'] == 0 and state['track_count'] == 1)
            assert call('tracks')[0]['clips'][0]['source_channels'] == channels
            routes = ['0.01', '0.02'] * channels
            call('track', 'routing', 0, *routes)
            call('select', 'clips', 0, 0)
            call('edit', 'clips', 'duplicate')
            call('edit', 'move-clip', 0, 1, 40)
            call('select', 'clips', 0, 0, 0, 1)
            baseline = export('before-%d.wav' % channels)
            output = os.path.join(directory, 'consolidated-%d.wav' % channels)
            call('render', 'consolidate', output)
            result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
            assert result['state'] == 'complete', (channels, result)
            assert call('tracks')[0]['clips'][0]['source_channels'] == channels
            raw = float_samples(output)
            assert len(raw) == 120 * channels, (channels, len(raw))
            for frame in (0, 10, 39):
                for ch in range(channels):
                    assert raw[frame * channels + ch] == data[frame * channels + ch] / 32768, (channels, frame, ch)
            for frame in (40, 50, 79):
                for ch in range(channels):
                    assert raw[frame * channels + ch] == (data[frame * channels + ch] + data[(frame - 40) * channels + ch]) / 32768, (channels, frame, ch)
            if shutil.which('ffmpeg'):
                decoded = subprocess.run(['ffmpeg', '-v', 'error', '-i', output, '-f', 'f32le', '-acodec', 'pcm_f32le', '-'], capture_output=True, check=True, timeout=15).stdout
                assert struct.unpack('<%df' % (len(decoded) // 4), decoded) == raw, channels
            rendered = export('after-%d.wav' % channels)
            assert len(baseline) == len(rendered)
            assert max(abs(a - b) for a, b in zip(baseline, rendered)) < 1e-6, channels
            original_bytes = open(output, 'rb').read()
            rejected = command('render', 'consolidate', output)
            assert rejected.returncode != 0 and open(output, 'rb').read() == original_bytes
            call('edit', 'undo')
            assert len(call('tracks')[0]['clips']) == 2 and os.path.isfile(output)
        # Selected clips 0 and 2 surround an unselected clip in summation
        # order. Combining them changes a full-scale sample despite each clip
        # being representable; the edit must be rejected without losing audio.
        call('track', 'remove', 0)
        source = os.path.join(directory, 'cancellation.wav')
        pcm = struct.pack('<3f', 1e20, 1.0, -1e20)
        with open(source, 'wb') as audio:
            audio.write(struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', 36 + len(pcm), b'WAVE', b'fmt ', 16, 3, 1, 44100, 176400, 4, 32, b'data', len(pcm)) + pcm)
        call('import', source)
        wait_for(lambda: call('status'), lambda state: state['pending_imports'] == 0 and state['track_count'] == 1)
        call('seek', 1); call('edit', 'split-tracks', 'all')
        call('seek', 2); call('edit', 'split-tracks', 'all')
        call('edit', 'move-clip', 0, 1, 0); call('edit', 'move-clip', 0, 2, 0)
        call('select', 'clips', 0, 0, 0, 2)
        baseline = export('cancellation-before.wav')
        clips_before = call('tracks')[0]['clips']
        output = os.path.join(directory, 'rejected-consolidation.wav')
        call('render', 'consolidate', output)
        result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
        assert result['state'] == 'failed' and 'verification tolerance' in result['error'], result
        assert result['file_written'] and os.path.isfile(output) and call('tracks')[0]['clips'] == clips_before
        assert export('cancellation-after.wav') == baseline
        # A preceding track can also change summation order even when the
        # selected clips are adjacent and their isolated track is unchanged.
        call('track', 'remove', 0)
        for name, values in (('preceding.wav', [1e20]), ('selected.wav', [-1e20, 1.0])):
            source = os.path.join(directory, name)
            pcm = struct.pack('<%df' % len(values), *values)
            with open(source, 'wb') as audio:
                audio.write(struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', 36 + len(pcm), b'WAVE', b'fmt ', 16, 3, 1, 44100, 176400, 4, 32, b'data', len(pcm)) + pcm)
            call('import', '--new-layer', source)
            expected_tracks = 1 if name == 'preceding.wav' else 2
            wait_for(lambda: call('status'), lambda state: state['pending_imports'] == 0 and state['track_count'] == expected_tracks)
        call('seek', 1); call('edit', 'split-tracks', 'all')
        call('edit', 'move-clip', 1, 1, 0); call('select', 'clips', 1, 0, 1, 1)
        baseline = export('cross-track-before.wav')
        assert baseline[:2] == (1.0, 1.0), baseline
        clips_before = call('tracks')[1]['clips']
        output = os.path.join(directory, 'cross-track-rejected.wav')
        call('render', 'consolidate', output)
        result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
        assert result['state'] == 'failed' and 'verification tolerance' in result['error'], result
        assert result['file_written'] and os.path.isfile(output) and call('tracks')[1]['clips'] == clips_before
        assert export('cross-track-after.wav') == baseline
        forced = os.path.join(directory, 'cross-track-forced.wav')
        call('render', 'consolidate', forced, 'allow-routing-change')
        result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
        assert result['state'] == 'failed' and 'verification tolerance' in result['error'] and result['routing_change_allowed'], result
        assert call('tracks')[1]['clips'] == clips_before and export('cross-track-forced-after.wav') == baseline
        # A mixed mono/stereo source can match the current mix yet change
        # after switching between automatic and explicit channel routing.
        call('track', 'remove', 1); call('track', 'remove', 0)
        for channels in (1, 2):
            path = os.path.join(directory, 'mixed-source-%d.wav' % channels)
            data = [((frame * (ch + 3) + ch * 71) % 2001 - 1000) for frame in range(200) for ch in range(channels)]
            with wave.open(path, 'wb') as audio:
                audio.setnchannels(channels); audio.setsampwidth(2); audio.setframerate(44100)
                audio.writeframes(struct.pack('<%dh' % len(data), *data))
            call('import', '--new-layer', path)
            wait_for(lambda: call('status'), lambda state: state['pending_imports'] == 0 and state['track_count'] == channels)
        call('select', 'clips', 1, 0); call('edit', 'clips', 'copy')
        call('select', 'point', 0, 50); call('edit', 'paste-point', 'mix')
        call('select', 'clips', 0, 0, 0, 1)
        for routing in ('automatic', 'explicit'):
            if routing == 'automatic':
                call('track', 'routing', 0, 'clear')
            else:
                call('track', 'routing', 0, '0.7', '0.2', '0.1', '0.8')
            baseline = export('mixed-%s-before.wav' % routing)
            clips_before = call('tracks')[0]['clips']
            output = os.path.join(directory, 'mixed-%s-rejected.wav' % routing)
            call('render', 'consolidate', output)
            result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
            expected_reason = 'source-channel' if routing == 'automatic' else 'automatic routing'
            assert result['state'] == 'failed' and expected_reason in result['error'], result
            assert result['file_written'] and os.path.isfile(output) and call('tracks')[0]['clips'] == clips_before
            assert export('mixed-%s-after.wav' % routing) == baseline
            opted_in = os.path.join(directory, 'mixed-%s-opted-in.wav' % routing)
            call('render', 'consolidate', opted_in, 'allow-routing-change')
            result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
            assert result['state'] == 'complete' and result['routing_change_allowed'], result
            consolidated = export('mixed-%s-opted-in-mix.wav' % routing)
            assert len(consolidated) == len(baseline) and max(abs(a - b) for a, b in zip(consolidated, baseline)) < 1e-6
            if routing == 'automatic':
                call('track', 'routing', 0, '1', '0', '0', '1')
            else:
                call('track', 'routing', 0, 'clear')
            changed = export('mixed-%s-changed-routing.wav' % routing)
            call('edit', 'undo')  # restore original routing
            call('edit', 'undo')  # restore original clips
            if routing == 'automatic':
                call('track', 'routing', 0, '1', '0', '0', '1')
            else:
                call('track', 'routing', 0, 'clear')
            original_changed = export('mixed-%s-original-routing.wav' % routing)
            assert len(changed) == len(original_changed) and max(abs(a - b) for a, b in zip(changed, original_changed)) > 1e-4
            call('edit', 'undo')  # restore original routing for the next case
        alternative = os.path.join(directory, 'mixed-rendered-track.wav')
        call('render', 'clips', alternative)
        result = wait_for(lambda: call('status')['export'], lambda state: state['state'] != 'running')
        assert result['state'] == 'complete', result
        tracks = call('tracks')
        assert len(tracks) == 3 and tracks[2]['muted'] and tracks[0]['clips'] == clips_before, tracks
        assert export('mixed-with-muted-render.wav') == baseline
        print('Multichannel consolidation, channel order, output protection, undo and project reopen passed; editor stopped')
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
