#!/usr/bin/env python3
"""Build a narrowly scoped, single-threaded Porffor/C++ experiment."""
import os
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parent
out = root / 'build'
out.mkdir(exist_ok=True)
porf = os.environ.get('PORFFOR', 'porf')
version = subprocess.check_output([porf, '--help'], text=True)
if 'alpha 19 (0c135ce 2026-10-21)' not in version:
    sys.exit('This experiment requires Porffor alpha-19 (0c135ce).')
cfile = out / 'quantize.c'
subprocess.run([porf, 'c', '-t', str(root / 'quantize.js'), '-o', str(cfile),
                '--module', '--fast-length'], check=True)
source = cfile.read_text()
pattern = r'jsval (p_\w+_cartaQuantizeSample_23m\w+)\(jsval, jsval, f64, f64, f64\);'
names = re.findall(pattern, source)
if len(names) != 1:
    sys.exit('Generated function signature changed; refusing to create a wrapper.')
main = '''int main(int argc, char** argv) {
  porf_init(argc, argv);
  porf_data_init();
  volatile int porf_stack_anchor = 0;
  porf_c_stack_top = (void*)&porf_stack_anchor;
  p__main__23main();
  return 0;
}'''
if source.count(main) != 1:
    sys.exit('Generated runtime entry point changed; refusing to create a wrapper.')
wrapper = '''/* Experiment only: singleton runtime, caller must serialize calls. */
int carta_experiment_quantize(double value, int scale, int bits) {
  volatile int porf_stack_anchor = 0;
  porf_c_stack_top = (void*)&porf_stack_anchor;
  static int initialized = 0;
  if (!initialized) {
    static char name[] = "carta-experiment";
    static char *argv[] = { name, NULL };
    porf_init(1, argv);
    porf_data_init();
    p__main__23main();
    initialized = 1;
  }
  jsval result = FUNCTION(porf_box((f64)0u, 0), porf_box((f64)0u, 0),
                          value, (f64)scale, (f64)bits);
  return (int)result.val;
}
'''.replace('FUNCTION', names[0])
cfile.write_text(source.replace(main, wrapper))
cc = os.environ.get('CC', 'clang')
cxx = os.environ.get('CXX', 'clang++')
subprocess.run([cc, '-O2', '-fno-fast-math', '-ffp-contract=off', '-fno-math-errno',
                '-c', str(cfile), '-o', str(out / 'quantize.o')], check=True)
subprocess.run(['ar', 'rcs', str(out / 'libcarta_quantize_experiment.a'), str(out / 'quantize.o')], check=True)
subprocess.run([cxx, '-std=c++17', '-O2', str(root / 'compare.cpp'),
                str(out / 'libcarta_quantize_experiment.a'), '-o', str(out / 'compare')], check=True)
subprocess.run(['node', str(root / 'compare.mjs'), str(out / 'compare')], check=True)

# Two private runtimes expose just their own bridge symbols. Linking two raw
# generated objects fails on duplicate Porffor symbols; shared libraries with
# hidden visibility avoid exporting or interposing those symbols.
if sys.platform not in ('darwin', 'linux'):
    sys.exit('The shared-runtime experiment supports macOS and Linux only.')
suffix = '.dylib' if sys.platform == 'darwin' else '.so'
shared_flag = '-dynamiclib' if sys.platform == 'darwin' else '-shared'
for label, name in [('a', 'carta_experiment_quantize'), ('b', 'carta_experiment_quantize_second')]:
    content = cfile.read_text().replace('carta_experiment_quantize(', name + '(')
    content = content.replace('int ' + name + '(', '__attribute__((visibility("default"))) int ' + name + '(')
    shared_source = out / (label + '.c')
    shared_source.write_text(content)
    subprocess.run([cc, '-O2', '-fno-fast-math', '-ffp-contract=off', '-fno-math-errno',
                    '-fPIC', '-fvisibility=hidden', shared_flag, str(shared_source),
                    '-o', str(out / ('lib' + label + suffix))], check=True)
rpath = '@loader_path' if sys.platform == 'darwin' else '$ORIGIN'
subprocess.run([cxx, '-std=c++17', '-O2', str(root / 'compare_shared.cpp'),
                str(out / ('liba' + suffix)), str(out / ('libb' + suffix)),
                '-Wl,-rpath,' + rpath, '-o', str(out / 'compare_shared')], check=True)
subprocess.run(['node', str(root / 'compare.mjs'), str(out / 'compare_shared')], check=True)
print('Two hidden-visibility Porffor runtimes linked and matched independently.')
