#!/usr/bin/env python3
"""Run desktop tests inside Xvfb with a window manager for native modals."""
import os
import subprocess
import re
import sys
import time


build = sys.argv[1] if len(sys.argv) > 1 else 'build'
manager = subprocess.Popen(['openbox'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if manager.poll() is not None:
            raise RuntimeError('Window manager exited before the tests started')
        result = subprocess.run(['xprop', '-root', '_NET_SUPPORTING_WM_CHECK'],
                                capture_output=True, text=True, timeout=2)
        match = re.search(r'window id #\s*(0x[0-9a-fA-F]+)', result.stdout)
        if result.returncode == 0 and match and int(match.group(1), 16) != 0:
            break
        time.sleep(0.05)
    else:
        raise RuntimeError('Window manager did not become ready')
    result = subprocess.call(['ctest', '--test-dir', build, '--output-on-failure', '--no-tests=error'])
    if result == 0 and '--ipc' in sys.argv[2:]:
        binary = os.path.join(build, 'CartaStudio_artefacts', 'Carta Studio')
        if not os.path.isfile(binary):
            binary = os.path.join(build, 'CartaStudio_artefacts', 'Release', 'Carta Studio')
        for script in ('ipc_smoke.py', 'ipc_paste_test.py', 'ipc_project_relink_test.py', 'ipc_source_scope_test.py', 'ipc_multichannel_consolidate_test.py', 'ipc_long_marker_test.py', 'ipc_marker_title_import_test.py', 'ipc_md_authoring_test.py', 'path_smoke.py'):
            result = subprocess.call([sys.executable, os.path.join(os.path.dirname(__file__), script), binary])
            if result != 0:
                break
        if result == 0 and '--ffmpeg' in sys.argv[2:]:
            result = subprocess.call([sys.executable, os.path.join(os.path.dirname(__file__), 'ffmpeg_wav_check.py'), binary])
    sys.exit(result)
finally:
    manager.terminate()
    try:
        manager.wait(timeout=5)
    except subprocess.TimeoutExpired:
        manager.kill()
        manager.wait()
