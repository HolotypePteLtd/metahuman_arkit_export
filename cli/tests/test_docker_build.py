"""The native CMake image must never receive character or workspace assets."""
import argparse
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build_tools'))
import docker_build


class DockerContextTest(unittest.TestCase):
    def test_context_contains_only_tooling_and_platform_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = argparse.Namespace(platform='linux-x64', jobs=2, version='0.1.1',
                                      cache=root / 'cache', output=root / 'out', offline=False)
            calls = []

            def command(argv):
                calls.append(list(map(str, argv)))
                if argv[:2] == ['docker', 'build']:
                    context = Path(argv[-1])
                    names = {p.relative_to(context).as_posix() for p in context.rglob('*') if p.is_file()}
                    self.assertEqual(names, {'cli/platforms.json', 'cli/build_tools/toolchain.py',
                                             'cli/build_tools/toolchain.json', 'cli/build_tools/release_utils.py'})

            with (patch.object(docker_build.platform, 'system', return_value='Linux'),
                  patch.object(docker_build.os, 'getuid', return_value=1000, create=True),
                  patch.object(docker_build.os, 'getgid', return_value=1000, create=True),
                  patch.object(docker_build, 'command', side_effect=command)):
                docker_build.build(args)
            self.assertEqual(len(calls), 2)
            self.assertIn('--version', calls[1])
            self.assertNotIn('python', calls[1])
            self.assertFalse(any('run.py' in arg for arg in calls[1]))


if __name__ == '__main__':
    unittest.main()
