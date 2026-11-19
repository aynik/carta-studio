#!/usr/bin/env python3
"""Exercise parity-harness failure handling without a compiled codec."""
from pathlib import Path
import subprocess
import tempfile
import unittest

COMPARE = Path(__file__).resolve().with_name('compare.mjs')


class ComparatorFailures(unittest.TestCase):
    def run_probe(self, body, expected):
        with tempfile.TemporaryDirectory(prefix='carta-parity-test-') as directory:
            probe = Path(directory) / 'probe'
            probe.write_text('#!/usr/bin/env python3\nimport sys\n' + body + '\n')
            probe.chmod(0o755)
            result = subprocess.run(['node', str(COMPARE), str(probe), '1'],
                                    capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(expected, result.stderr)

    def test_mismatch(self):
        self.run_probe('sys.stdout.buffer.write(bytes(2260))', 'Mismatch at frame 0')

    def test_truncated_output(self):
        self.run_probe('sys.stdout.buffer.write(bytes(3))', 'Incomplete native output')

    def test_native_failure(self):
        self.run_probe('sys.exit(3)', 'Native probe failed')


if __name__ == '__main__':
    unittest.main()
