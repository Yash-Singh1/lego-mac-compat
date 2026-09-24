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
    def test_saga_controller_revision_preserves_publisher_and_custom_profiles(self):
        with tempfile.TemporaryDirectory() as directory:
            resources = Path(directory) / "Resources"
            profiles = resources / "InputDevices/AnalogTriggers"
            profiles.mkdir(parents=True)
            original = {"VendorID": 1356, "ProductID": 1476,
                        "ButtonA": "9:2", "ButtonBack": "9:14"}
            source = profiles / "PS4Dualshock.plist"
            source.write_bytes(plistlib.dumps(original))
            module.install_saga_controller_profile(resources)
            target = profiles / "PS4DualshockV2.plist"
            result = plistlib.loads(target.read_bytes())
            self.assertEqual(result["ProductID"], 2508)
            self.assertEqual(result["ButtonA"], "9:2")
            self.assertEqual(result["ButtonBack"], "9:9")
            generic = profiles / "LP32StandardGamepad.plist"
            standard = plistlib.loads(generic.read_bytes())
            self.assertEqual((standard["VendorID"], standard["ProductID"]),
                             (0x7F32, 0x7F32))
            self.assertEqual(standard["ButtonA"], "9:2")
            self.assertEqual(standard["CGPDeviceType"], "Xbox")
            self.assertEqual(plistlib.loads(source.read_bytes()), original)
            target.write_bytes(b"custom profile")
            generic.write_bytes(b"custom generic profile")
            module.install_saga_controller_profile(resources)
            self.assertEqual(target.read_bytes(), b"custom profile")
            self.assertEqual(generic.read_bytes(), b"custom generic profile")

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
