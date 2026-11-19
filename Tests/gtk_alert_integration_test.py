#!/usr/bin/env python3
"""Exercise native GTK dialogs and review export in an isolated X11 session."""
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import wave

binary = os.path.abspath(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='carta-gtk-alert-') as directory:
    env = {**os.environ, 'CARTA_STUDIO_IPC_DIR': directory, 'CARTA_STUDIO_RECOVERY_DIR': directory}
    def ipc(*args):
        return subprocess.run([binary, 'ipc', *args], env=env, capture_output=True, text=True, timeout=8)
    def call(*args):
        result = ipc(*args)
        assert result.returncode == 0, (args, result.stdout, result.stderr)
        return json.loads(result.stdout)['result']
    def wait_for(fn):
        for _ in range(200):
            value = fn()
            if value:
                return value
            assert process.poll() is None, 'Editor exited unexpectedly'
            time.sleep(0.05)
        raise AssertionError('GTK alert integration test timed out')
    def find(title):
        result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', '^' + title + '$'], capture_output=True, text=True)
        return result.stdout.strip().splitlines()[-1] if result.returncode == 0 and result.stdout.strip() else None
    def keys(*args):
        subprocess.run(['xdotool', *args], check=True, timeout=5)
    source = os.path.join(directory, 'source.wav')
    with wave.open(source, 'wb') as output:
        output.setnchannels(1); output.setsampwidth(2); output.setframerate(44100)
        output.writeframes(struct.pack('<h', 4096) * 44100)
    process = subprocess.Popen([binary, source], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(lambda: ipc('status').returncode == 0)
        wait_for(lambda: call('status')['track_count'] == 1 and not call('status')['pending_imports'])
        before = call('tracks')
        main = wait_for(lambda: find('.*Carta Studio'))
        keys('windowactivate', '--sync', main)
        time.sleep(0.3)
        keys('mousemove', '--window', main, '400', '100', 'click', '1')
        keys('key', 'ctrl+0')
        wait_for(lambda: 'Timeline focused' in call('status')['message'])
        keys('key', 'ctrl+i')
        picker = wait_for(lambda: find('Import audio'))
        assert ipc('edit', 'rename-clip', '0', '0', 'blocked').returncode != 0, 'File picker must block IPC edits'
        keys('windowactivate', '--sync', picker)
        keys('key', 'Escape')
        wait_for(lambda: not find('Import audio'))
        assert call('tracks') == before, 'Cancelling import must leave the project unchanged'
        keys('windowactivate', '--sync', main)
        keys('key', 'ctrl+i')
        picker = wait_for(lambda: find('Import audio'))
        keys('windowactivate', '--sync', picker)
        keys('key', 'ctrl+l')
        keys('type', '--clearmodifiers', source)
        keys('key', 'Return')
        time.sleep(0.5)
        if find('Import audio'):
            keys('key', 'Return')
        wait_for(lambda: not find('Import audio'))
        wait_for(lambda: len(call('tracks')[0]['clips']) == 2 and not call('status')['pending_imports'])
        original = call('tracks')[0]['clips']
        keys('windowactivate', '--sync', main)
        keys('mousemove', '--window', main, '190', '121')
        keys('keydown', 'ctrl')
        keys('mousedown', '1')
        time.sleep(0.15)
        keys('mousemove', '--sync', '--window', main, '235', '121')
        time.sleep(0.15)
        keys('mouseup', '1')
        keys('keyup', 'ctrl')
        copied = wait_for(lambda: (rows if len(rows := call('tracks')[0]['clips']) == 3 else None))
        assert copied[0] == original[0] and copied[1]['source'] == original[0]['source'] and copied[1]['start_sample'] > copied[0]['start_sample'], copied
        call('edit', 'undo')
        assert call('tracks')[0]['clips'] == original, 'one undo must remove the drag copy'
        call('marker', 'add', '22050', 'Middle')
        call('disc', 'sync', 'markers')
        linked = call('disc', 'status')['tracks']
        assert len(linked) == 2 and linked[1]['parts'][0]['start_sample'] == 22050, linked
        geometry = subprocess.run(['xdotool', 'getwindowgeometry', '--shell', main], check=True, capture_output=True, text=True).stdout
        width = int(dict(line.split('=', 1) for line in geometry.splitlines() if '=' in line)['WIDTH'])
        marker_x = 162 + round((width - 162) * 0.5 / 10)
        keys('windowactivate', '--sync', main)
        keys('mousemove', '--window', main, str(marker_x), '66')
        keys('keydown', 'shift')
        keys('mousedown', '1')
        time.sleep(0.15)
        keys('mousemove', '--sync', '--window', main, str(marker_x + 18), '66')
        time.sleep(0.15)
        keys('mouseup', '1')
        keys('keyup', 'shift')
        moved = call('markers')[0]['sample']
        call('disc', 'sync', 'markers')
        assert moved > 22050 and call('disc', 'status')['tracks'][1]['parts'][0]['start_sample'] == moved, 'syncing after dragging a marker must update the MD track boundary'
        call('edit', 'undo')
        call('edit', 'undo')
        assert call('markers')[0]['sample'] == 22050, 'undo must restore the dragged marker boundary'
        call('marker', 'delete', '0')
        call('disc', 'sync', 'layer1')
        call('disc', 'track', 'duplicate', '0')
        keys('windowactivate', '--sync', main)
        keys('key', 'ctrl+shift+p')
        search = wait_for(lambda: find('Search commands'))
        keys('windowactivate', '--sync', search)
        time.sleep(0.3)
        keys('mousemove', '--window', search, '100', '20', 'click', '1')
        keys('type', '--clearmodifiers', 'Export MiniDisc review WAV')
        time.sleep(0.3)
        keys('key', 'Return')
        picker = wait_for(lambda: find('Export disc program review WAV.*'))
        review = os.path.join(directory, 'review.wav')
        keys('windowactivate', '--sync', picker)
        keys('key', 'ctrl+l')
        keys('key', 'ctrl+a')
        keys('type', '--clearmodifiers', review)
        keys('key', 'Return')
        wait_for(lambda: not find('Export disc program review WAV.*'))
        try:
            wait_for(lambda: os.path.isfile(review) and 'Exported review.wav' in call('status')['message'])
        except AssertionError:
            raise AssertionError((call('status'), os.listdir(directory)))
        with open(review, 'rb') as stream:
            data = stream.read()
        assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
        chunks = {}; offset = 12
        while offset + 8 <= len(data):
            tag, size = struct.unpack_from('<4sI', data, offset); offset += 8
            chunks[tag] = data[offset:offset + size]; offset += size + (size & 1)
        assert len(chunks[b'data']) == 132300 * 2 * 4
        assert chunks[b'data'][:44100 * 2 * 4] == chunks[b'data'][44100 * 2 * 4:88200 * 2 * 4], 'review export must repeat the authored audio'
        call('select', 'clip', '0', '0')
        time.sleep(0.2)
        keys('key', 'ctrl+shift+p')
        search = wait_for(lambda: find('Search commands'))
        keys('windowactivate', '--sync', search)
        time.sleep(0.3)
        keys('mousemove', '--window', search, '100', '20', 'click', '1')
        keys('type', '--clearmodifiers', 'Rename selected clip')
        time.sleep(0.3)
        keys('key', 'Return')
        text_prompt = wait_for(lambda: find('Rename clip'))
        assert ipc('edit', 'rename-clip', '0', '0', 'blocked').returncode != 0, 'GTK text prompts must block IPC edits'
        keys('windowactivate', '--sync', text_prompt)
        keys('key', 'ctrl+a')
        keys('type', '--clearmodifiers', 'Native title')
        keys('key', 'Return')
        wait_for(lambda: not find('Rename clip'))
        assert call('tracks')[0]['clips'][0]['custom_name'] == 'Native title', 'Return must apply the text prompt'
        before = call('tracks')
        keys('windowactivate', '--sync', main)
        time.sleep(0.3)
        keys('key', 'alt+F4')
        prompt = wait_for(lambda: find('Unsaved project'))
        blocked = ipc('edit', 'rename-clip', '0', '0', 'must not apply')
        assert blocked.returncode != 0, blocked.stdout
        keys('windowactivate', '--sync', prompt)
        keys('key', '--clearmodifiers', 'Escape')
        wait_for(lambda: not find('Unsaved project'))
        assert process.poll() is None and call('tracks') == before and call('status')['dirty'], 'Escape must retain the unsaved project'
        call('edit', 'rename-clip', '0', '0', 'after cancel')
        assert call('tracks')[0]['clips'][0]['custom_name'] == 'after cancel', 'IPC edits should resume after cancellation'
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=5)
print('GTK dialog and review export integration passed; editor stopped')
