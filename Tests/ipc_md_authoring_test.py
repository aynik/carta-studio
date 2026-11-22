#!/usr/bin/env python3
"""Exercise UTOC-native MiniDisc authoring, groups, coalescing, UTOC I/O, and Mock NetMD through CLI IPC."""
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import time
import wave

binary = os.path.abspath(sys.argv[1])


def write_tone_wav(path, frames=44032, freq=440.0, amp=0.25):
    with wave.open(path, 'wb') as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(44100)
        buf = bytearray()
        for i in range(frames):
            v = int(round(amp * math.sin(2.0 * math.pi * freq * i / 44100.0) * 32000.0))
            buf.extend(struct.pack('<hh', v, v))
        out.writeframes(bytes(buf))


with tempfile.TemporaryDirectory(prefix='carta-md-authoring-ipc-') as directory:
    env = {
        **os.environ,
        'HOME': directory,
        'CARTA_STUDIO_IPC_DIR': directory,
        'CARTA_STUDIO_RECOVERY_DIR': directory,
    }

    def command(*args):
        return subprocess.run(
            [binary, 'ipc', *map(str, args)],
            env=env,
            capture_output=True,
            text=True,
            timeout=15,
        )

    def call(*args):
        res = command(*args)
        assert res.returncode == 0, (args, res.stdout, res.stderr)
        payload = json.loads(res.stdout)
        assert payload.get('ok') is True, (args, payload)
        return payload['result']

    wav1 = os.path.join(directory, 'track_one.wav')
    wav2 = os.path.join(directory, 'track_two.wav')
    wav3 = os.path.join(directory, 'track_three.wav')
    write_tone_wav(wav1, 44032, 220.0, 0.25)
    write_tone_wav(wav2, 44032, 330.0, 0.20)
    write_tone_wav(wav3, 44032, 440.0, 0.15)

    editor = subprocess.Popen([binary], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(150):
            if command('status').returncode == 0:
                break
            assert editor.poll() is None, 'editor exited during startup'
            time.sleep(0.1)
        else:
            raise AssertionError('editor IPC server did not start')

        # 1. Sequential Layer 1 imports auto-create MD tracks + atomic undo/redo
        call('import', wav1, wav2)
        for _ in range(300):
            st = call('status')
            if st['pending_imports'] == 0 and st['track_count'] == 1:
                break
            time.sleep(0.05)
        assert st['pending_imports'] == 0, st

        disc = call('disc', 'status')
        assert disc['auto_track_on_import'] is True and disc['track_count'] == 2, disc
        assert disc['tracks'][0]['parts'] == [{'index': 0, 'start_sample': 0, 'end_sample': 44032}], disc
        assert disc['tracks'][1]['parts'] == [{'index': 0, 'start_sample': 44032, 'end_sample': 88064}], disc

        call('edit', 'undo')
        assert call('disc', 'status')['track_count'] == 1
        call('edit', 'redo')
        assert call('disc', 'status')['track_count'] == 2

        # Disabling auto-track prevents automatic MD track creation on third sequential import
        call('disc', 'auto-track', 'off')
        call('import', wav3)
        for _ in range(300):
            st = call('status')
            if st['pending_imports'] == 0 and len(call('tracks')[0]['clips']) == 3:
                break
            time.sleep(0.05)
        assert len(call('tracks')[0]['clips']) == 3 and call('disc', 'status')['track_count'] == 2

        # 2. Delete all MD tracks and rebuild cleanly from Clips / Layer 1 / Markers
        call('disc', 'track', 'remove', '1')
        call('disc', 'track', 'remove', '0')
        assert call('disc', 'status')['track_count'] == 0

        synced_clips = call('disc', 'sync', 'clips')
        assert synced_clips['track_count'] == 3, synced_clips
        assert synced_clips['tracks'][2]['parts'][0] == {'index': 0, 'start_sample': 88064, 'end_sample': 132096}, synced_clips

        call('disc', 'track', 'remove', '2')
        synced_l1 = call('disc', 'sync', 'layer1')
        assert synced_l1['track_count'] == 3, synced_l1
        assert synced_l1['tracks'][2]['parts'][0] == {'index': 0, 'start_sample': 88064, 'end_sample': 132096}, synced_l1

        assert call('markers') == []
        call('marker', 'add', '22016', 'Bridge')
        call('marker', 'add', '44032', 'Two')
        call('marker', 'add', '88064', 'Three')
        synced_m = call('disc', 'sync', 'markers')
        assert synced_m['track_count'] == 4, synced_m
        call('edit', 'undo')
        assert call('disc', 'status')['track_count'] == 3

        # 3. Grouping + mid-group insert / split / merge consistency
        grouped = call('disc', 'group', 'set', '1', '2', 'Side A')
        assert grouped['group_count'] == 1 and grouped['groups'][0]['start_track'] == 1 and grouped['groups'][0]['end_track'] == 2, grouped

        inserted = call('disc', 'track', 'add', '0', '22016', 'Interlude', '1')
        assert inserted['track_count'] == 4 and inserted['tracks'][1]['half_width_title'] == 'Interlude', inserted
        assert inserted['groups'][0]['start_track'] == 1 and inserted['groups'][0]['end_track'] == 3, inserted

        split_res = call('disc', 'track', 'split', '1', '11008')
        assert split_res['track_count'] == 5 and split_res['groups'][0]['end_track'] == 4, split_res
        assert split_res['plan_valid'] and split_res['utoc_valid'], split_res

        merged_res = call('disc', 'track', 'merge', '1')
        assert merged_res['track_count'] == 4 and merged_res['groups'][0]['end_track'] == 3, merged_res
        call('edit', 'undo')
        assert call('disc', 'status')['track_count'] == 5

        # 4. Play Order aliasing coalescing + UTOC export / import + Hi-MD alias stats
        duped = call('disc', 'track', 'duplicate', '0', '1')
        assert duped['track_count'] == 6 and duped['aliased_track_count'] >= 1, duped
        assert duped['physical_plan']['saved_clusters'] > 0, duped
        assert duped['budget']['shared_fragment_count'] > 0, duped

        # Canonical rename propagates across aliases; detach isolates an alias for range edits
        aid = duped['tracks'][0]['audio_id']
        aliased_res = call('disc', 'track', 'rename', '0', 'Shared Theme')
        assert aliased_res['tracks'][1]['half_width_title'] == 'Shared Theme', aliased_res
        detached = call('disc', 'track', 'detach', '1')
        assert detached['tracks'][1]['audio_id'] != aid and detached['tracks'][0]['alias_count'] == 1, detached
        ranged = call('disc', 'track', 'range', '1', '0', '22016')
        assert ranged['tracks'][1]['parts'][0]['end_sample'] == 22016, ranged
        call('edit', 'undo')
        call('edit', 'undo')
        call('edit', 'undo')

        utoc_path = os.path.join(directory, 'album.utoc')
        exp = call('utoc', 'export', utoc_path)
        assert exp['bytes'] == 9408 and os.path.getsize(utoc_path) == 9408, exp

        call('disc', 'title', 'TEMP')
        imp = call('utoc', 'import', utoc_path)
        assert imp['tracks'] == 6 and call('disc', 'status')['half_width_title'] == '', imp

        # Forced gapped break toggle roundtrip via IPC
        gap_on = call('disc', 'track', 'gap', '1', 'on')
        assert gap_on['tracks'][1]['force_gap_before'] is True, gap_on
        gap_off = call('disc', 'track', 'gap', '1', 'off')
        assert gap_off['tracks'][1]['force_gap_before'] is False, gap_off

        # Hi-MD Play Order aliasing saves estimated bytes without touching hardware
        himd_st = call('disc', 'medium', 'himd300')
        assert himd_st['is_himd'] is True and himd_st['max_tracks'] == 2047, himd_st
        assert himd_st['unique_audio_count'] < himd_st['track_count'] and himd_st['saved_bytes'] > 0, himd_st
        call('disc', 'medium', 'md80')

        # 5. Record & Device tab routing, NetMD status/devices, and hardware guard checks
        assert call('disc', 'view', 'map') == 'record'
        assert call('disc', 'view', 'record') == 'record'
        assert call('disc', 'view', 'tracks') == 'tracks'
        devs = call('netmd', 'devices')
        assert isinstance(devs['devices'], list), devs
        st = call('netmd', 'status')
        assert st['active'] is False, st
        def expect_fail(*args):
            res = command(*args)
            payload = json.loads(res.stdout)
            assert res.returncode != 0 and payload.get('ok') is False, (args, res.returncode, payload)

        expect_fail('disc', 'track', 'part', 'add', '0', '0', '1024')
        expect_fail('netmd', 'use-mock', 'mz-n920')
        expect_fail('netmd', 'erase', 'usb:054c:0188')
        expect_fail('netmd', 'write-utoc', 'usb:054c:0188')
        expect_fail('netmd', 'record', 'usb:054c:0188')
    finally:
        command('app', 'quit', 'force')
        try:
            editor.wait(timeout=5)
        except subprocess.TimeoutExpired:
            editor.kill()
            editor.wait(timeout=5)

print('IPC MiniDisc authoring test passed.')
