"""Run the pinned WebRTC notice generator with its H.264 license mappings."""
import importlib.util
import sys
from pathlib import Path

source, artifact, output = map(Path, sys.argv[1:])
spec = importlib.util.spec_from_file_location('webrtc_licenses', source / 'tools_webrtc/libs/generate_licenses.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
# These paths are declared by the pinned dependencies' README.chromium files.
# The upstream helper lacks mappings when rtc_use_h264/proprietary_codecs is on.
mapping = dict(module.LIB_TO_LICENSES_DICT,
               ffmpeg=['third_party/ffmpeg/CREDITS.chromium', 'third_party/ffmpeg/COPYING.LGPLv2.1'],
               openh264=['third_party/openh264/src/LICENSE'])
module.LicenseBuilder([str(artifact)], ['//:webrtc'], lib_to_licenses_dict=mapping).generate_license_text(str(output))
