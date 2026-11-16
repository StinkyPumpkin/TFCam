"""Packages the TFCam release zip: the FOMOD installer with one TFCam.dll per runtime variant.

Run after `build_tfcam.bat ALL`, `build_tfcam.bat SE` and `build_tfcam.bat AE` (see BUILD.md, "Runtime variants"):

    python package_release.py            ->  release/TFCam-v<version>.zip

Zip layout (the folder names are the ones fomod/ModuleConfig.xml installs from):
    fomod/info.xml, fomod/ModuleConfig.xml
    LICENSE.txt   the original TFCam GPL-3.0 license, unchanged
    NOTICE.txt
    PrismaUI/views/TFCam/*              the Camera Path editor page (0.19.0; used when PrismaUI is installed)
    TFCam ALL/SKSE/Plugins/TFCam.dll    SE 1.5.97 + AE 1.6.x / 1.7.x + VR in one DLL
    TFCam SE/SKSE/Plugins/TFCam.dll     Skyrim SE 1.5.97 only
    TFCam AE/SKSE/Plugins/TFCam.dll     Skyrim AE 1.6.x and 1.7.x only
TFCam.ini is never part of the download: TFCam writes it from its code defaults on the first run.
"""
import re
import sys
import xml.dom.minidom
import zipfile
from pathlib import Path

root = Path(__file__).resolve().parent
version = re.search(r"project\(TFCam VERSION ([0-9.]+)", (root / "CMakeLists.txt").read_text(encoding="utf-8")).group(1)

dlls = {
    "TFCam ALL": root / "build/ng/release/TFCam.dll",
    "TFCam SE": root / "build/ng/release-SE/TFCam.dll",
    "TFCam AE": root / "build/ng/release-AE/TFCam.dll",
}
for name, path in dlls.items():
    if not path.is_file():
        sys.exit(f"missing {path} - build the {name.split()[-1]} variant first")
    if f"{version}".encode() not in path.read_bytes():
        sys.exit(f"{path} was not built from version {version}")

for xml_file in ("fomod/info.xml", "fomod/ModuleConfig.xml"):
    xml.dom.minidom.parse(str(root / xml_file))  # well-formed

# Every folder the FOMOD installs from must be in the zip.
config = (root / "fomod/ModuleConfig.xml").read_text(encoding="utf-8")
sources = set(re.findall(r'source="([^"]+)"', config))
expected = {"LICENSE.txt", "NOTICE.txt", "PrismaUI", *dlls}
views = sorted(p for p in (root / "PrismaUI/views/TFCam").iterdir() if p.is_file())
if {p.name for p in views} != {"index.html", "editor.css", "editor.js"}:
    sys.exit(f"PrismaUI/views/TFCam holds {[p.name for p in views]}, expected index.html, editor.css, editor.js")
if sources != expected:
    sys.exit(f"ModuleConfig.xml installs from {sorted(sources)} but the zip holds {sorted(expected)}")

out = root / "release" / f"TFCam-v{version}.zip"
out.parent.mkdir(exist_ok=True)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.write(root / "fomod/info.xml", "fomod/info.xml")
    z.write(root / "fomod/ModuleConfig.xml", "fomod/ModuleConfig.xml")
    z.write(root / "LICENSE", "LICENSE.txt")
    z.write(root / "NOTICE.txt", "NOTICE.txt")
    for p in views:
        z.write(p, f"PrismaUI/views/TFCam/{p.name}")
    for name, path in dlls.items():
        z.write(path, f"{name}/SKSE/Plugins/TFCam.dll")
with zipfile.ZipFile(out) as z:
    assert z.testzip() is None
    for i in z.infolist():
        print(f"{i.file_size:>9}  {i.filename}")
print("wrote", out)
