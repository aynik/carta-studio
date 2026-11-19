#!/usr/bin/env python3
"""Drive the GTK prompt with real X11 keyboard events inside Xvfb."""
import os
import subprocess
import sys
import time

process = subprocess.Popen([sys.argv[1], '--keyboard'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env={**os.environ, 'LC_ALL': 'C.UTF-8'})
try:
    def find(title):
        for _ in range(200):
            result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', '^' + title + '$'], capture_output=True, text=True)
            if result.returncode == 0 and result.stdout.strip():
                return result.stdout.strip().splitlines()[-1]
            assert process.poll() is None, 'GTK test exited before showing the prompt'
            time.sleep(0.05)
        raise AssertionError('GTK prompt did not appear: ' + title)
    def keys(*args):
        subprocess.run(['xdotool', *args], check=True, timeout=5)
    window = find('Carta GTK keyboard test')
    keys('windowactivate', '--sync', window)
    parent = find('GTK prompt test')
    keys('windowactivate', parent)
    focused = subprocess.run(['xdotool', 'getwindowfocus'], check=True, capture_output=True, text=True).stdout.strip()
    assert focused == window, 'The modal prompt must retain focus when its parent is activated'
    keys('key', 'ctrl+a')
    keys('type', '--clearmodifiers', 'Typed title')
    keys('key', 'Return')
    window = find('Carta GTK escape test')
    keys('windowactivate', '--sync', window)
    keys('key', 'Escape')
    window = find('Carta GTK save test')
    keys('windowactivate', '--sync', window)
    keys('key', 'Escape')
    find('Carta GTK overwrite cancel')
    keys('key', 'Escape')
    find('Carta GTK overwrite replace')
    keys('key', 'alt+r')
    stdout, stderr = process.communicate(timeout=10)
    assert process.returncode == 0, (stdout, stderr)
    print(stdout.strip())
finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=5)
