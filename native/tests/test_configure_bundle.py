import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "configure_bundle", Path(__file__).parents[1] / "tools/configure_bundle.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class BundleSettingTest(unittest.TestCase):
    def test_default_enable_disable_preserves_other_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Info.plist"
            original = {"CFBundleIdentifier": "test.bundle", "Unrelated": [1, "two"]}
            path.write_bytes(plistlib.dumps(original))
            for enabled in (False, True, False):
                if enabled:
                    module.configure(path, True)
                else:
                    module.configure(path)
                result = plistlib.loads(path.read_bytes())
                self.assertIs(result.pop("LP32ContinueWhenInactive"), enabled)
                self.assertEqual(result, original)


if __name__ == "__main__":
    unittest.main()
