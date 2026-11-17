# OpenFMT — notices

OpenFMT is distributed under the GNU General Public License, version 3 or
(at your option) any later version. The full text is in `COPYING`.

## Modified program

OpenFMT is a modified version of **OpenMPT** (Open ModPlug Tracker),
based on OpenMPT 1.33 development sources. Since November 2026 it has been
modified by the OpenFMT contributors: native YM2612, YM2151 and SN76489 chips,
Furnace and VGM import, chip instrument editors, a second effect column,
oscilloscopes, a dark interface and the OpenFMT name and artwork. The full
change history is in the Git repository.

OpenMPT code is used under its BSD 3-Clause license:

    Copyright (c) 2004-2026, OpenMPT Project Developers and Contributors
    Copyright (c) 1997-2003, Olivier Lapicque

The license text is in `LICENSE` (repository) / `Licenses/License.OpenMPT.txt`
(package). OpenMPT's name is used only to state where OpenFMT comes from;
OpenFMT is not endorsed by the OpenMPT project.

## Furnace

Parts of the chip playback behaviour (Furnace pitch slides in chip units,
arpeggio timing, macro handling, the `.fur` and `.fui` formats) follow and
are adapted from **Furnace**:

    Furnace Tracker — Copyright (C) 2021-2026 tildearrow and contributors
    Licensed under the GNU GPL, version 2 or any later version.

Furnace's instrument library and demo songs are not part of OpenFMT.

## Bundled components

| Component | License | File in `Licenses` |
|---|---|---|
| ymfm (Aaron Giles) — YM2612 / YM2151 / OPL emulation | BSD 3-Clause | License.ymfm.txt |
| mpt library (OpenMPT) | BSD 3-Clause / BSL-1.0 | License.mpt.*.txt |
| ancient | BSD 2-Clause | License.ancient*.txt |
| FLAC | BSD-style (Xiph) | License.FLAC.txt |
| LAME (openmpt-lame.dll) | LGPL 2 | License.lame.txt |
| lhasa | ISC | License.lhasa.txt |
| mpg123 (openmpt-mpg123.dll) | LGPL 2.1 | License.mpg123*.txt |
| nlohmann/json | MIT | License.nlohmann-json.txt |
| Ogg, Vorbis | BSD-style (Xiph) | License.ogg.txt, License.Vorbis.txt |
| Opus, libopusenc, opusfile | BSD-style | License.Opus*.txt |
| PortAudio | MIT | License.PortAudio.txt |
| r8brain-free-src | MIT | License.R8Brain.txt |
| RtAudio, RtMidi | MIT-style | License.RtAudio.txt, License.RtMidi.txt |
| RealtimeKit header | MIT | License.RealtimeKit.txt |
| Signalsmith Stretch | MIT | License.SignalsmithStretch.txt |
| zlib, minizip | zlib / Info-ZIP | License.zlib.txt, License.minizip.txt |

UnRAR is not included: its license is not compatible with the GPL, so OpenFMT
cannot open modules packed in `.rar` archives.
