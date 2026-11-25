# OpenFMT

OpenFMT is a music tracker for Windows built on [OpenMPT](https://openmpt.org/)
with native sound chip support: Yamaha **YM2612** (OPN2, Mega Drive / Genesis),
**YM2151** (OPM, arcade), TI **SN76489** (PSG) and the OPL chips OpenMPT already
had. It can import **Furnace** (`.fur`), **DefleMask** (`.dmf`), **VGM** and
Nintendo DS **2SF** (`.mini2sf`) files for editing.

**Status: 0.1.3 — early preview.** Expect rough edges; see *Known issues*.

*Русская версия ниже.*

## Features

- YM2612, YM2151 and SN76489 instruments that live in the sample slots of a
  normal OpenMPT module and play through emulated chips (ymfm).
- Instrument editors for the FM chips (operators, algorithm, feedback, SSG-EG,
  LFO, randomize) and a macro editor for the PSG.
- Furnace `.fur` and DefleMask `.dmf` import: patterns, orders, instruments
  (including ADSR volume macros), samples and the chip effects that have an
  equivalent (FM operator levels, PSG noise mode, arpeggio speed,
  Furnace-style slides and arpeggios). YMZ280B sample channels play as
  samples with the chip's panning.
- Instrument presets: Furnace `.fui`, DefleMask `.dmp` (YM2612, YM2151, SMS
  PSG), TFM Music Maker `.tfi`, `.opni`. They can be previewed and dragged
  from the instrument library in the tree view.
- VGM import (YM2612, YM2151, SN76489, YM2612 DAC, YMZ280B, OKIM6295, 32X PWM
  and Mega CD RF5C164; Master System / Game Gear), with slides,
  vibrato and detune, one frame (or driver timer tick) per row. *Setup → General → Compact VGM
  import* packs several frames per row for readability, at some cost in
  sound.
- Polyphony: up to four YM2612, YM2151 and SN76489 chips per song (Chips tab).
- One to four effect columns per channel (channel header menu, or drag the
  header edge).
- Per-channel oscilloscopes and a Chips tab with per-chip volume.
- Optional dark interface.
- Songs with chip parts saved by OpenFMT tell plain OpenMPT, in the song
  comments, where to get OpenFMT.

## Getting started

Unzip and run `OpenFMT.exe`. The package is portable: settings are stored next
to the program (because of the `OpenFMT.portable` file). Delete that file to
keep settings in `%APPDATA%\OpenFMT` instead.

Open a `.fur` file with **File → Open**, or create a new module and add chip
instruments on the **Samples** tab.

The dark interface is on by default. It is experimental: switch it off with
**View → Setup → Display → Dark interface** and restart.

## File compatibility

OpenFMT saves regular `.mptm` files with extra data for the chip instruments
and chip effects. Other trackers, **including OpenMPT itself, will not play
the chip parts of such files correctly.** Keep using OpenFMT for them.
Older OpenFMT test builds saved `.fmt` files; these still open.

## Known issues

- Furnace import aims at the Furnace versions used by the files it was tested
  with; some macros, effects and chip quirks still sound different.
- YM2612 extended channel 3 (separate operator pitches) is not supported yet.
- In compact VGM import, vibrato and arpeggios stay as per-row finetune and
  legato notes instead of 4xy / 0xy.
- Opening modules from `.rar` archives is not available (see *License*).
- Tested on Windows 11 only so far.

## Building

Visual Studio 2022 (or Build Tools) with the C++ desktop and MFC components:

```
MSBuild build\vs2022win10\OpenMPT.vcxproj -p:Configuration=Release -p:Platform=x64
```

The program is written to `bin\release\vs2022-win10-static\amd64\OpenFMT.exe`.
`build\openfmt\make_release.py` assembles the release package.

## License

OpenFMT as a whole is distributed under the **GNU General Public License,
version 3 or (at your option) any later version** — see `COPYING`.

It is a modified version of OpenMPT, whose code is under the BSD 3-Clause
license (`LICENSE.OpenMPT`, its original readme is `README.OpenMPT.md`), and parts of its chip playback behaviour are adapted from
Furnace (GPL-2.0-or-later). Third-party components and their licenses are
listed in `NOTICE.md` and the `Licenses` folder.

OpenFMT is not affiliated with or endorsed by the OpenMPT or Furnace
developers.

---

# OpenFMT (по-русски)

OpenFMT — трекер для Windows на основе [OpenMPT](https://openmpt.org/) со
встроенными звуковыми чипами: **YM2612** (OPN2, Mega Drive), **YM2151** (OPM),
**SN76489** (PSG) и OPL, который был в OpenMPT и раньше. Открывает для
редактирования файлы **Furnace** (`.fur`), **DefleMask** (`.dmf`), **VGM** и
музыку Nintendo DS **2SF** (`.mini2sf`).

**Версия 0.1.3 — ранняя.** Шероховатости есть, см. «Известные проблемы».

## Что умеет

- Инструменты YM2612, YM2151 и SN76489 живут в слотах сэмплов обычного модуля
  OpenMPT и звучат через эмуляцию чипов (ymfm).
- Редакторы FM-инструментов (операторы, алгоритм, обратная связь, SSG-EG, LFO,
  случайный патч) и редактор макросов PSG.
- Импорт `.fur` и `.dmf`: паттерны, порядок, инструменты (в том числе
  макросы громкости ADSR), сэмплы и эффекты чипов, у которых есть аналог.
  Каналы YMZ280B играют сэмплами с панорамой как у чипа.
- Пресеты инструментов: `.fui` Furnace, `.dmp` DefleMask (YM2612, YM2151,
  PSG SMS), `.tfi`, `.opni`. Их можно прослушивать и перетаскивать из
  библиотеки инструментов в дереве слева.
- Импорт VGM (YM2612, YM2151, SN76489, ЦАП YM2612, YMZ280B, OKIM6295, PWM 32X
  и RF5C164 Mega CD; Master System / Game Gear) со слайдами,
  вибрато и подстройкой, кадр (или тик таймера игры) на строку. *Setup → General → Compact VGM
  import* упаковывает несколько кадров в строку ради читаемости, ценой
  точности звука.
- Полифония: до четырёх чипов YM2612, YM2151 и SN76489 в песне (вкладка Chips).
- От одной до четырёх колонок эффектов на канал (меню шапки канала или
  перетаскивание края шапки).
- Осциллографы по каналам и вкладка Chips с громкостью чипов.
- Тёмный интерфейс.
- Песни с партиями чипов, сохранённые в OpenFMT, в комментарии подсказывают
  обычному OpenMPT, где взять OpenFMT.

## Как начать

Распакуйте архив и запустите `OpenFMT.exe`. Сборка портативная: настройки
хранятся рядом с программой (из-за файла `OpenFMT.portable`). Удалите его,
чтобы настройки хранились в `%APPDATA%\OpenFMT`.

Тёмный интерфейс включён по умолчанию и пока экспериментальный: выключается в
**View → Setup → Display → Dark interface**, затем перезапуск.

## Совместимость файлов

OpenFMT сохраняет обычные `.mptm` с дополнительными данными для инструментов
и эффектов чипов. Другие трекеры, **в том числе сам OpenMPT, партии чипов в
таких файлах правильно не сыграют.** Старые тестовые `.fmt` открываются.

## Известные проблемы

- Импорт Furnace рассчитан на версии Furnace из проверенных файлов; часть
  макросов, эффектов и особенностей чипов звучит иначе.
- Расширенный третий канал YM2612 (свои частоты операторов) пока не
  поддерживается.
- В компактном импорте VGM вибрато и арпеджио остаются построчной подстройкой
  и легато, а не 4xy / 0xy.
- Модули из архивов `.rar` не открываются (см. «Лицензия»).
- Проверено пока только на Windows 11.

## Лицензия

OpenFMT целиком распространяется по **GNU GPL версии 3 или (на ваш выбор)
любой более поздней** — см. `COPYING`. Это изменённая версия OpenMPT (код
OpenMPT под лицензией BSD 3-Clause, `LICENSE.OpenMPT`); часть логики воспроизведения
чипов перенесена из Furnace (GPL-2.0-or-later). Остальные компоненты и их
лицензии — в `NOTICE.md` и папке `Licenses`.

OpenFMT не связан с разработчиками OpenMPT и Furnace.
