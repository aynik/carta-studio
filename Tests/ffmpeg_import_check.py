#!/usr/bin/env python3
"""Optional real-process compressed-import check; needs ffmpeg and a desktop."""
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave

binary = os.path.abspath(sys.argv[1])
ffmpeg = shutil.which('ffmpeg')
if not ffmpeg:
    sys.exit('ffmpeg is required for this optional check')

def command(*args):
    return subprocess.run([binary, 'ipc', *args], capture_output=True, text=True, timeout=8)

def call(*args):
    result = command(*args)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return json.loads(result.stdout)['result']

def wav_floats(path):
    with open(path, 'rb') as stream:
        data = stream.read()
    offset = 12
    while offset + 8 <= len(data):
        tag, size = struct.unpack_from('<4sI', data, offset)
        offset += 8
        if tag == b'data':
            payload = data[offset:offset + size]
            assert len(payload) % 4 == 0
            return struct.unpack('<' + str(len(payload) // 4) + 'f', payload)
        offset += size + (size & 1)
    raise AssertionError('Missing WAV data')

def wait_import(index):
    for _ in range(200):
        state = call('status')
        if state['pending_imports'] == 0:
            break
        time.sleep(0.05)
    assert state['track_count'] == index + 1, state

def export_clip(index, path):
    call('select', 'clip', str(index), '0')
    call('export', 'clip', path)
    for _ in range(200):
        operation = call('status')['export']
        if operation['state'] != 'running':
            break
        time.sleep(0.05)
    assert operation['state'] == 'complete', operation
    return wav_floats(path)

assert command('status').returncode != 0, 'Stop the existing Carta Studio IPC owner before this test'
with tempfile.TemporaryDirectory() as directory:
    process = subprocess.Popen([binary], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               env={**os.environ, 'CARTA_STUDIO_RECOVERY_DIR': directory})
    try:
        for _ in range(100):
            if command('status').returncode == 0:
                break
            assert process.poll() is None, 'Editor exited during startup'
            time.sleep(0.1)
        extensions = {ext.lower() for reader in call('formats')['readers'] for ext in reader['extensions']}
        tested = []
        for extension, codec in [('.ogg', 'libvorbis'), ('.mp3', 'libmp3lame')]:
            if extension not in extensions:
                continue
            source = os.path.join(directory, 'tone' + extension)
            subprocess.run([ffmpeg, '-v', 'error', '-f', 'lavfi', '-i',
                            'aevalsrc=0.3*sin(2*PI*440*t)|-0.3*sin(2*PI*440*t):s=44100:d=0.5',
                            '-c:a', codec, '-y', source], check=True)
            index = call('status')['track_count']
            call('import', source)
            wait_import(index)
            clip = call('tracks')[index]['clips'][0]
            assert clip['source_channels'] == 2 and clip['source_sample_rate'] == 44100, clip
            exported = os.path.join(directory, 'render' + extension + '.wav')
            samples = export_clip(index, exported)
            assert len(samples) % 2 == 0 and len(samples) > 4000, len(samples)
            frames = len(samples) // 2
            left_rms = math.sqrt(sum(samples[2*i] ** 2 for i in range(frames)) / frames)
            opposite_rms = math.sqrt(sum((samples[2*i] + samples[2*i+1]) ** 2 for i in range(frames)) / frames)
            assert 0.1 < left_rms < 0.3 and opposite_rms < 0.03, (extension, left_rms, opposite_rms)
            raw = subprocess.run([ffmpeg, '-v', 'error', '-i', source, '-f', 'f32le', '-acodec', 'pcm_f32le', '-'], check=True, capture_output=True).stdout
            reference = struct.unpack('<' + str(len(raw) // 4) + 'f', raw)
            compare = min(len(reference), len(samples))
            assert compare > 0 and abs(frames - len(reference) // 2) <= 2048, (extension, frames, len(reference) // 2)
            error_rms = math.sqrt(sum((samples[i] - reference[i]) ** 2 for i in range(compare)) / compare)
            assert error_rms < 0.002, (extension, error_rms)
            if extension == '.mp3' and '--require-mp3' in sys.argv[2:]:
                assert len(samples) == len(reference) and error_rms < 0.0001, (frames, len(reference) // 2, error_rms)
            print(extension, 'frames:', frames, 'ffmpeg frames:', len(reference) // 2, 'sample RMS difference:', error_rms)
            if len(samples) > compare:
                print(extension, 'unmatched tail RMS:', math.sqrt(sum(value * value for value in samples[compare:]) / (len(samples) - compare)))
                start = compare // 2
                tail_error = math.sqrt(sum((samples[2*i] - 0.3 * math.sin(2 * math.pi * 440 * i / 44100)) ** 2 for i in range(start, frames)) / (frames - start))
                print(extension, 'unmatched tail vs generated tone RMS difference:', tail_error)
            tested.append(extension)
        assert '.ogg' in tested, tested
        if sys.platform == 'darwin' and '.mp3' in tested:
            for count in (100, 5000):
                uncompressed = os.path.join(directory, 'mono-' + str(count) + '.wav')
                source = os.path.join(directory, 'mono-' + str(count) + '.mp3')
                with wave.open(uncompressed, 'wb') as audio:
                    audio.setnchannels(1); audio.setsampwidth(2); audio.setframerate(44100)
                    audio.writeframes(b''.join(struct.pack('<h', round(12000 * math.sin(0.001*i + 0.000013*i*i))) for i in range(count)))
                subprocess.run([ffmpeg, '-v', 'error', '-i', uncompressed, '-c:a', 'libmp3lame', '-y', source], check=True)
                raw = subprocess.run([ffmpeg, '-v', 'error', '-f', 'mp3', '-i', source, '-f', 'f32le', '-'], check=True, capture_output=True).stdout
                reference = struct.unpack('<' + str(len(raw) // 4) + 'f', raw)
                assert len(reference) == count, (count, len(reference))
                index = call('status')['track_count']
                call('import', source); wait_import(index)
                clip = call('tracks')[index]['clips'][0]
                assert clip['source_channels'] == 1 and abs(clip['duration_seconds'] * 44100 - count) < 0.01, clip
                exported = export_clip(index, os.path.join(directory, 'render-mono-' + str(count) + '.wav'))
                assert len(exported) == count * 2, (count, len(exported))
                rms = math.sqrt(sum((exported[2*i+ch] - reference[i]) ** 2 for i in range(count) for ch in range(2)) / (count * 2))
                assert rms < 0.0001, (count, rms)
            print('Mono MP3 duration and samples matched FFmpeg through IPC')
            low_source = os.path.join(directory, 'low-rate-mac.mp3')
            subprocess.run([ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=8000:duration=0.5',
                            '-c:a', 'libmp3lame', '-y', low_source], check=True)
            index = call('status')['track_count']
            call('import', low_source); wait_import(index)
            clip = call('tracks')[index]['clips'][0]
            assert clip['source_channels'] == 1 and clip['source_sample_rate'] == 8000 and abs(clip['duration_seconds']-0.5) < 0.0001, clip
            print('Low-rate mono MP3 duration matched through IPC')
        if '--require-mp3' in sys.argv[2:]:
            assert '.mp3' in tested, ('MP3 reader was not registered', tested)
        if '--require-software-mp3' in sys.argv[2:]:
            assert '.mp3' in tested, tested
            with open(os.path.join(directory, 'tone.mp3'), 'rb') as audio:
                encoded = audio.read()
            concatenated = os.path.join(directory, 'concatenated.mp3')
            with open(concatenated, 'wb') as audio:
                audio.write(encoded + encoded)
            no_xing = os.path.join(directory, 'no-xing.mp3')
            subprocess.run([ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=0.5',
                            '-c:a', 'libmp3lame', '-write_xing', '0', '-y', no_xing], check=True)
            low_rate = os.path.join(directory, 'mpeg2.mp3')
            subprocess.run([ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=22050:duration=0.5',
                            '-c:a', 'libmp3lame', '-y', low_rate], check=True)
            for source, reason in ((concatenated, 'trailing'), (no_xing, 'gapless')):
                before = call('status'); call('import', source)
                for _ in range(200):
                    state = call('status')
                    if state['pending_imports'] == 0:
                        break
                    time.sleep(0.05)
                assert state['track_count'] == before['track_count'] and state['import_issues'] == before['import_issues'] + 1, state
                issue = call('imports', 'status')['issues'][-1]
                assert issue['path'] == source and reason in issue['reason'], issue
            index = call('status')['track_count']
            call('import', low_rate); wait_import(index)
            clip = call('tracks')[index]['clips'][0]
            assert clip['source_channels'] == 1 and clip['source_sample_rate'] == 22050 and abs(clip['duration_seconds']-0.5) < 0.0001, clip
            print('MPEG-2 MP3 imported; untagged and concatenated files were rejected without modifying the session')
        lame = shutil.which('lame')
        if '.mp3' in tested and lame:
            pcm = os.path.join(directory, 'protected.wav')
            protected = os.path.join(directory, 'protected.mp3')
            with wave.open(pcm, 'wb') as audio:
                audio.setnchannels(1); audio.setsampwidth(2); audio.setframerate(44100)
                audio.writeframes(b''.join(struct.pack('<h', round(10000 * math.sin(i * 0.03))) for i in range(22050)))
            subprocess.run([lame, '--silent', '-p', pcm, protected], check=True)
            index = call('status')['track_count']
            call('import', protected); wait_import(index)
            clip = call('tracks')[index]['clips'][0]
            assert clip['source_channels'] == 1 and abs(clip['duration_seconds']-0.5) < 0.0001, clip
            with open(protected, 'rb') as stream:
                content = bytearray(stream.read())
            xing = 4 + 17  # generated file is MPEG-1 mono
            flags = int.from_bytes(content[xing+4:xing+8], 'big')
            tag = xing + 8 + sum(size for mask, size in ((1,4),(2,4),(4,100),(8,4)) if flags & mask)
            assert content[tag:tag+4] == b'LAME' and content[tag+34:tag+36] != b'\0\0'
            for name, offset in (('protected-damaged', 4), ('timing-damaged', tag+22)):
                damaged = os.path.join(directory, name + '.mp3')
                broken = bytearray(content); broken[offset] ^= 1
                with open(damaged, 'wb') as stream:
                    stream.write(broken)
                before = call('status'); call('import', damaged)
                for _ in range(200):
                    state = call('status')
                    if state['pending_imports'] == 0:
                        break
                    time.sleep(0.05)
                assert state['track_count'] == before['track_count'] and state['import_issues'] == before['import_issues'] + 1, state
            print('CRC-protected MP3 imported; damaged frame and timing checksums rejected without modifying the session')
        print('Compressed import/export checked:', ', '.join(tested))
        sox = shutil.which('sox')
        if sox:
            ffmpeg_lengths = []
            for count in (100, 127, 128, 129, 576, 1000, 4410, 22050, 44100, 96001):
                uncompressed = os.path.join(directory, 'source-' + str(count) + '.wav')
                source = os.path.join(directory, 'source-' + str(count) + '.ogg')
                with wave.open(uncompressed, 'wb') as audio:
                    audio.setnchannels(1); audio.setsampwidth(2); audio.setframerate(44100)
                    audio.writeframes(b''.join(struct.pack('<h', round(12000 * math.sin(0.001*i + 0.000013*i*i))) for i in range(count)))
                subprocess.run([ffmpeg, '-v', 'error', '-i', uncompressed, '-c:a', 'libvorbis', '-y', source], check=True)
                reference_bytes = subprocess.run([sox, source, '-t', 'f32', '-L', '-'], check=True, capture_output=True).stdout
                reference = struct.unpack('<' + str(len(reference_bytes) // 4) + 'f', reference_bytes)
                assert len(reference) == count, (count, len(reference))
                ffmpeg_bytes = subprocess.run([ffmpeg, '-v', 'error', '-i', source, '-f', 'f32le', '-acodec', 'pcm_f32le', '-'], check=True, capture_output=True).stdout
                ffmpeg_lengths.append((count, len(ffmpeg_bytes) // 4))
                index = call('status')['track_count']; call('import', source); wait_import(index)
                exported = export_clip(index, os.path.join(directory, 'render-' + str(count) + '.wav'))
                assert len(exported) == count * 2, (count, len(exported) // 2)
                rms = math.sqrt(sum((exported[2*i+ch] - reference[i]) ** 2 for i in range(count) for ch in range(2)) / (count * 2))
                assert rms < 0.0001, (count, rms)
            print('Vorbis full-length samples agree with SoX for 10 short and long fixtures')
            print('FFmpeg frame counts (source, decoded):', ffmpeg_lengths)
        else:
            print('SoX unavailable; full-length Vorbis comparison skipped')
        mono_link = os.path.join(directory, 'mono-link.ogg')
        subprocess.run([ffmpeg, '-v', 'error', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=0.2',
                        '-c:a', 'libvorbis', '-y', mono_link], check=True)
        chain = os.path.join(directory, 'chained.ogg')
        with open(chain, 'wb') as output:
            for part in (os.path.join(directory, 'tone.ogg'), mono_link):
                with open(part, 'rb') as stream:
                    output.write(stream.read())
        before = call('status'); call('import', chain)
        for _ in range(200):
            state = call('status')
            if state['pending_imports'] == 0:
                break
            time.sleep(0.05)
        assert state['track_count'] == before['track_count'] and state['import_issues'] == before['import_issues'] + 1, state
        issues = call('imports', 'status')['issues']
        assert issues[-1]['path'] == chain and 'multiple logical streams' in issues[-1]['reason'], issues
        print('Chained stereo/mono Vorbis was rejected without terminating the editor')
        if '.mp3' in tested:
            source = os.path.join(directory, 'tone.mp3')
            index = next(i for i, track in enumerate(call('tracks')) if any(clip['source'] == source for clip in track['clips']))
            before = export_clip(index, os.path.join(directory, 'mp3-before-reopen.wav'))
            project = os.path.join(directory, 'mp3-project.carta')
            for action in ('save-as', 'open'):
                call('project', action, project)
                for _ in range(200):
                    operation = call('project', 'status')
                    if operation['state'] != 'running':
                        break
                    time.sleep(0.05)
                assert operation['state'] == 'complete', operation
            after = export_clip(index, os.path.join(directory, 'mp3-after-reopen.wav'))
            assert after == before, 'MP3 samples changed after saving and reopening the project'
            print('MP3 project save and reopen preserved the rendered samples')
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
