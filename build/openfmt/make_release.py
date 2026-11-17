"""Собирает пакет выпуска OpenFMT: папку и zip рядом с ней.

    python build/openfmt/make_release.py [папка для выпуска]

Берёт уже собранный OpenFMT.exe (Release x64) и всё, что нужно рядом с ним:
библиотеки, переходники плагинов, раскладки клавиатуры, README, COPYING,
NOTICE и лицензии компонентов. Библиотеку инструментов и демо Furnace не
кладёт — они не наши.
"""
import hashlib
import re
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / 'bin/release/vs2022-win10-static/amd64'

# Лицензии компонентов: тот же список, что у OpenMPT
# (build/auto/update_package_template.cmd), без UnRAR и с ymfm.
LICENSES = {
    'LICENSE.OpenMPT': 'License.OpenMPT.txt',
    'include/ymfm/LICENSE': 'License.ymfm.txt',
    'src/mpt/LICENSE.BSD-3-Clause.txt': 'License.mpt.BSD-3-Clause.txt',
    'src/mpt/LICENSE.BSL-1.0.txt': 'License.mpt.BSL-1.0.txt',
    'include/ancient/LICENSE': 'License.ancient.txt',
    'include/ancient/LICENSE.bzip2': 'License.ancient.bzip2.txt',
    'include/flac/COPYING.Xiph': 'License.FLAC.txt',
    'include/lame/COPYING': 'License.lame.txt',
    'include/lhasa/COPYING.md': 'License.lhasa.txt',
    'include/mpg123/COPYING': 'License.mpg123.txt',
    'include/mpg123/AUTHORS': 'License.mpg123.Authors.txt',
    'include/nlohmann-json/LICENSE.MIT': 'License.nlohmann-json.txt',
    'include/ogg/COPYING': 'License.ogg.txt',
    'include/opus/COPYING': 'License.Opus.txt',
    'include/opusenc/COPYING': 'License.Opusenc.txt',
    'include/opusenc/AUTHORS': 'License.Opusenc.Authors.txt',
    'include/opusfile/COPYING': 'License.Opusfile.txt',
    'include/portaudio/LICENSE.txt': 'License.PortAudio.txt',
    'include/r8brain/LICENSE': 'License.R8Brain.txt',
    'include/rtaudio/LICENSE': 'License.RtAudio.txt',
    'include/rtkit/rtkit.h': 'License.RealtimeKit.txt',
    'include/rtmidi/LICENSE': 'License.RtMidi.txt',
    'include/SignalsmithStretch/SignalsmithStretch/LICENSE.txt': 'License.SignalsmithStretch.txt',
    'include/vorbis/COPYING': 'License.Vorbis.txt',
    'include/zlib/LICENSE': 'License.zlib.txt',
    'include/zlib/contrib/minizip/LICENSE.Info-Zip': 'License.minizip.txt',
}

PROGRAM = ['OpenFMT.exe', 'openmpt-lame.dll', 'openmpt-mpg123.dll',
           'PluginBridge-amd64.exe', 'PluginBridgeLegacy-amd64.exe']


def version():
    text = (ROOT / 'mptrack/OpenFMTVersion.h').read_text(encoding='utf-8')
    return re.search(r'#define OPENFMT_VERSION_A "([^"]+)"', text).group(1)


def main():
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT.parent.parent / 'release'
    name = f'OpenFMT-{version()}-win64'
    out = target / name
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    for file in PROGRAM:
        shutil.copy2(BIN / file, out / file)
    shutil.copytree(ROOT / 'packageTemplate/extraKeymaps', out / 'extraKeymaps')
    (out / 'OpenFMT.portable').write_bytes(b'')
    shutil.copy2(ROOT / 'README.md', out / 'README.md')
    shutil.copy2(ROOT / 'COPYING', out / 'COPYING.txt')
    shutil.copy2(ROOT / 'NOTICE.md', out / 'NOTICE.md')
    licenses = out / 'Licenses'
    licenses.mkdir()
    for source, dest in LICENSES.items():
        shutil.copy2(ROOT / source, licenses / dest)

    archive = target / f'{name}.zip'
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(out.rglob('*')):
            if path.is_file():
                z.write(path, Path(name) / path.relative_to(out))
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    # Перевод строки только LF: иначе `sha256sum -c` считает \r частью имени файла.
    (target / f'{name}.zip.sha256').write_bytes(f'{digest}  {archive.name}\n'.encode('ascii'))
    print(archive, archive.stat().st_size, digest)


if __name__ == '__main__':
    main()
