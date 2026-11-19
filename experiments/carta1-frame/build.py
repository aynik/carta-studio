#!/usr/bin/env python3
"""Build an experimental Carta1 frame bridge using its existing native transforms."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

here = Path(__file__).resolve().parent
studio = here.parents[1]
codec = studio / 'third_party/carta1'
build = here / 'build'
build.mkdir(exist_ok=True)
porf = os.environ.get('PORFFOR', 'porf')
if 'alpha 19 (0c135ce 2026-10-21)' not in subprocess.check_output([porf, '--help'], text=True):
    sys.exit('Requires Porffor alpha-19 (0c135ce).')
# Resolve Babel in a disposable tools directory, never in the codec checkout.
tools = build / 'tools'
tools.mkdir(exist_ok=True)
modules = tools / 'node_modules'
if not modules.exists():
    existing = os.environ.get('CARTA1_NODE_MODULES')
    if existing:
        modules.symlink_to(Path(existing).resolve(), target_is_directory=True)
    else:
        shutil.copy(codec / 'package.json', tools)
        shutil.copy(codec / 'package-lock.json', tools)
        subprocess.run(['npm', 'ci', '--ignore-scripts', '--prefix', str(tools)], check=True)
shutil.copy(codec / 'package.json', tools)
script = (codec / 'build/native.js').read_text()
needle = "for (const directory of codec.sources) stage(directory)\n"
if script.count(needle) != 1:
    sys.exit('Codec staging logic changed; review the experiment adapter.')
script = script.replace(needle, needle + "fs.copyFileSync(process.env.FRAME_BRIDGE, path.join(work, 'bridge.js'))\n")
start = script.index('const nativeCompiler = process.env.CC')
script = script[:start] + '''
try {
  runBuildCommand(porffor, ['c', '-t', path.join(work, 'entry.js'), '-o', output, '--fast-length'])
} finally {
  fs.rmSync(work, { recursive: true, force: true })
}
'''
(tools / 'stage.mjs').write_text(script)
env = dict(os.environ, PORFFOR=porf, FRAME_BRIDGE=str(here / 'bridge.js'),
           NATIVE_BUILD=json.dumps({'name': 'carta1', 'root': str(codec), 'entry': 'bridge.js',
               'argv': ['carta1', 'cli.js'], 'sources': ['codec'], 'constants': 'codec/core/constants.js'}))
cfile = build / 'frame.c'
subprocess.run(['node', str(tools / 'stage.mjs'), str(cfile)], env=env, check=True)
source = cfile.read_text()
def find(name):
    matches = re.findall(r'jsval (p_\w+_' + name + r'_23m\w+)\(jsval, jsval, f64, f64, f64\);', source)
    if len(matches) != 1:
        sys.exit('Generated ' + name + ' signature changed; review the bridge.')
    return matches[0]
encode, decode = find('bridgeEncode'), find('bridgeDecode')
main = '''int main(int argc, char** argv) {
  porf_init(argc, argv);
  porf_data_init();
  volatile int porf_stack_anchor = 0;
  porf_c_stack_top = (void*)&porf_stack_anchor;
  p__main__23main();
  return 0;
}'''
if source.count(main) != 1:
    sys.exit('Generated runtime entry point changed; review the bridge.')
wrapper = '''
static int carta_frame_initialized = 0;
static void carta_frame_init(void) {
  if (!carta_frame_initialized) {
    static char name[] = "carta-frame-probe";
    static char *argv[] = { name, NULL };
    porf_init(1, argv);
    porf_data_init();
    p__main__23main();
    carta_frame_initialized = 1;
  }
}
int carta_frame_encode(int stream, const float *input, unsigned char *output) {
  volatile int anchor = 0; porf_c_stack_top = (void*)&anchor;
  if (stream < 0 || stream > 1 || !input || !output) return -1;
  carta_frame_init();
  return (int)ENCODE(porf_box(0, 0), porf_box(0, 0), (f64)stream,
                     (f64)(uintptr_t)input, (f64)(uintptr_t)output).val;
}
int carta_frame_decode(int stream, const unsigned char *input, float *output) {
  volatile int anchor = 0; porf_c_stack_top = (void*)&anchor;
  if (stream < 0 || stream > 1 || !input || !output) return -1;
  carta_frame_init();
  return (int)DECODE(porf_box(0, 0), porf_box(0, 0), (f64)stream,
                     (f64)(uintptr_t)input, (f64)(uintptr_t)output).val;
}
'''.replace('ENCODE', encode).replace('DECODE', decode)
cfile.write_text(source.replace(main, wrapper))
cc = os.environ.get('CC', 'clang')
cxx = os.environ.get('CXX', 'clang++')
subprocess.run([cc, '-O2', '-fno-fast-math', '-ffp-contract=off', '-fno-math-errno', '-c', str(cfile), '-o', str(build / 'frame.o')], check=True)
subprocess.run(['ar', 'rcs', str(build / 'libcarta1-frame.a'), str(build / 'frame.o')], check=True)
subprocess.run([cxx, '-std=c++17', '-O2', str(here / 'probe.cpp'), str(build / 'libcarta1-frame.a'), '-o', str(build / 'probe')], check=True)
subprocess.run(['node', str(here / 'compare.mjs'), str(build / 'probe'), *sys.argv[1:]], check=True)
