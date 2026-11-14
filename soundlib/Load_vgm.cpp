/*
 * Load_vgm.cpp
 * ------------
 * Назначение: чтение логов VGM с Mega Drive в редактируемый модуль.
 *
 * VGM хранит не музыку, а стенограмму: записи в регистры микросхемы вперемешку
 * с паузами. Нот в файле нет, поэтому их приходится восстанавливать. У YM2612
 * нота складывается из трёх записей — старшие биты частоты с октавой (A4–A6),
 * младшие (A0–A2) и включение голоса (0x28); у SN76489 из защёлки и досылки.
 * Тембр берётся снимком регистров канала в момент включения: это ровно те
 * двадцать девять байт, из которых состоит наш патч OPN2.
 *
 * Что теряется. Плавное изменение регистра между нотами в стенограмме
 * неотличимо от нарисованного вручную, поэтому эффекты не восстанавливаются:
 * выходит модуль с верными нотами и тембрами, но без огибающих и скольжений.
 * Обратно в VGM он не выгружается — исходной стенограммы мы не храним.
 */

#include "stdafx.h"
#include "Loaders.h"
#ifdef MPT_WITH_ZLIB
#include "../common/zlib_helper.h"
#include <zlib.h>
#endif
#include <cmath>
#include <map>

OPENMPT_NAMESPACE_BEGIN

namespace
{

constexpr uint32 VgmRate = 44100;             // Время в VGM считается отсчётами 44100
constexpr size_t MaxVgmBytes = 64u * 1024u * 1024u;
constexpr uint8 FMChannels = 6, PSGChannels = 4;
constexpr uint8 VgmChannels = FMChannels + PSGChannels;
constexpr double MiddleC = 261.6255;

// Смещения регистров операторов внутри патча: патч перечисляет операторы в
// логическом порядке 1, 2, 3, 4, а чип раскладывает их иначе.
constexpr uint8 OperatorOffset[] = {0, 8, 4, 12};
constexpr uint8 OperatorRegister[] = {0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90};
constexpr uint8 RegisterMask[] = {0x7F, 0x7F, 0xDF, 0x9F, 0x1F, 0xFF, 0x0F};
// Какие операторы доходят до выхода при каждом из восьми алгоритмов: только
// их уровень и слышен как громкость канала.
constexpr uint8 CarrierMask[] = {0x08, 0x08, 0x08, 0x08, 0x0A, 0x0E, 0x0E, 0x0F};

struct VgmHeader
{
	uint32 version = 0;
	uint32 dataOffset = 0x40;
	uint32 totalSamples = 0;
	uint32 loopOffset = 0;
	uint32 loopSamples = 0;
	uint32 psgClock = 0;
	uint32 opn2Clock = 0;
	uint32 opmClock = 0;
	bool foreignChip = false;
};

// Частоты прочих микросхем лежат дальше в заголовке. Если какая-то из них
// задана, файл не наш: играть его было бы обманом.
constexpr uint16 ForeignClockOffset[] = {0x38, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x80, 0x84, 0x88, 0x8C, 0x90, 0x98, 0x9C, 0xA0, 0xA4};

uint32 ReadLE32(const std::vector<uint8> &data, size_t offset)
{
	if(offset + 4 > data.size())
		return 0;
	return data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (static_cast<uint32>(data[offset + 3]) << 24);
}

bool ParseHeader(const std::vector<uint8> &data, VgmHeader &header)
{
	if(data.size() < 0x40 || data[0] != 'V' || data[1] != 'g' || data[2] != 'm' || data[3] != ' ')
		return false;
	header.version = ReadLE32(data, 8);
	header.totalSamples = ReadLE32(data, 0x18);
	header.loopOffset = ReadLE32(data, 0x1C);
	header.loopSamples = ReadLE32(data, 0x20);
	header.dataOffset = (header.version >= 0x150 && ReadLE32(data, 0x34)) ? ReadLE32(data, 0x34) + 0x34 : 0x40;
	if(header.dataOffset >= data.size())
		return false;
	const size_t headerEnd = std::min<size_t>(header.dataOffset, data.size());
	header.psgClock = ReadLE32(data, 0x0C) & 0x3FFFFFFF;
	header.opn2Clock = (0x2C + 4 <= headerEnd) ? (ReadLE32(data, 0x2C) & 0x3FFFFFFF) : 0;
	header.opmClock = (0x30 + 4 <= headerEnd) ? (ReadLE32(data, 0x30) & 0x3FFFFFFF) : 0;
	for(uint16 offset : ForeignClockOffset)
	{
		if(offset == 0x30)
			continue;  // YM2151 у нас есть
		if(offset + 4u <= headerEnd && (ReadLE32(data, offset) & 0x3FFFFFFF))
			header.foreignChip = true;
	}
	return header.psgClock || header.opn2Clock || header.opmClock;
}

// Одно событие потока: включение или выключение голоса, смена громкости.
struct VgmEvent
{
	uint32 sample = 0;
	uint8 channel = 0;
	ModCommand::NOTE note = NOTE_NONE;
	uint8 volume = 0;       // 0–64, 255 — не менять
	int patch = -1;         // индекс тембра, -1 — не менять
};

ModCommand::NOTE NoteFromFrequency(double hertz)
{
	if(!(hertz > 8.0) || hertz > 30000.0)
		return NOTE_NONE;
	// Голоса микросхем в этой сборке живут по нотной шкале Furnace, где до
	// первой октавы это C-4, а не C-5: импорт из Furnace переносит ноты один
	// в один, и шкала досталась от него. Проверено сравнением регистров: без
	// этой поправки чип получал частоту ровно вдвое больше нужной.
	const int note = static_cast<int>(std::lround(12.0 * std::log2(hertz / MiddleC))) + NOTE_MIDDLEC - 12;
	if(note < NOTE_MIN || note > NOTE_MAX)
		return NOTE_NONE;
	return static_cast<ModCommand::NOTE>(note);
}

}  // namespace


#ifdef MPT_WITH_ZLIB
static bool InflateGzip(FileReader &file, std::vector<uint8> &output)
{
	file.Rewind();
	if(file.GetLength() > MaxVgmBytes)
		return false;
	zlib::z_inflate_stream stream{};
	// 16 + MAX_WBITS просит zlib разобрать именно обёртку gzip, в которую
	// упакованы .vgz.
	if(!zlib::is_Z_OK(inflateInit2(&*stream, 16 + MAX_WBITS)))
		return false;
	std::array<uint8, 16384> input{}, block{};
	int result = Z_OK;
	while(result == Z_OK)
	{
		if(stream->avail_in == 0)
		{
			const auto chunk = file.ReadRaw(mpt::as_span(input));
			if(chunk.empty())
				break;
			stream->next_in = input.data();
			stream->avail_in = static_cast<uInt>(chunk.size());
		}
		stream->next_out = block.data();
		stream->avail_out = static_cast<uInt>(block.size());
		result = inflate(&*stream, Z_NO_FLUSH);
		if(result != Z_OK && result != Z_STREAM_END)
			return false;
		output.insert(output.end(), block.begin(), block.begin() + (block.size() - stream->avail_out));
		if(output.size() > MaxVgmBytes)
			return false;
	}
	return result == Z_STREAM_END && output.size() > 0x40;
}
#endif


CSoundFile::ProbeResult CSoundFile::ProbeFileHeaderVGM(MemoryFileReader file, const uint64 *pfilesize)
{
	MPT_UNREFERENCED_PARAMETER(pfilesize);
	uint8 magic[4]{};
	if(!file.ReadArray(magic))
		return ProbeWantMoreData;
	if(magic[0] == 0x1F && magic[1] == 0x8B)
		return ProbeSuccess;  // .vgz, распаковку пробуем уже при загрузке
	if(magic[0] != 'V' || magic[1] != 'g' || magic[2] != 'm' || magic[3] != ' ')
		return ProbeFailure;
	return ProbeSuccess;
}


bool CSoundFile::ReadVGM(FileReader &file, ModLoadingFlags loadFlags)
{
	file.Rewind();
	std::vector<uint8> data;
	const auto head = file.ReadArray<uint8, 2>();
	file.Rewind();
	if(head[0] == 0x1F && head[1] == 0x8B)
	{
#ifdef MPT_WITH_ZLIB
		if(!InflateGzip(file, data))
			return false;
#else
		return false;
#endif
	} else
	{
		if(file.GetLength() < 0x40 || file.GetLength() > MaxVgmBytes)
			return false;
		data.resize(static_cast<size_t>(file.GetLength()));
		if(!file.ReadRaw(mpt::as_span(data)).size())
			return false;
	}

	VgmHeader header;
	if(!ParseHeader(data, header))
		return false;
	if(header.foreignChip || !header.opn2Clock)
		return false;  // Не Mega Drive: чужие микросхемы мы не изображаем
	if(loadFlags == onlyVerifyHeader)
		return true;

	// Шаг строки. Драйверы Mega Drive тикают кадром, поэтому строка — кадр.
	// Какой именно, видно по тому, какая пауза в файле преобладает.
	uint32 frames60 = 0, frames50 = 0;
	for(size_t scan = header.dataOffset; scan < data.size(); scan++)
	{
		if(data[scan] == 0x62) frames60++;
		else if(data[scan] == 0x63) frames50++;
	}
	const uint32 samplesPerRow = frames50 > frames60 ? 882 : 735;

	// Состояние микросхем по ходу потока.
	std::array<std::array<uint8, 256>, 2> registers{};
	std::array<uint16, FMChannels> fnum{};
	std::array<uint8, FMChannels> block{};
	std::array<uint16, PSGChannels> psgPeriod{};
	std::array<uint8, PSGChannels> psgVolume{};
	uint8 psgLatch = 0, psgLatchType = 0;
	std::vector<OPN2Patch> patches;
	std::vector<VgmEvent> events;
	uint32 sample = 0;
	size_t pos = header.dataOffset;

	// Тембр и громкость ноты: снимок регистров канала в момент включения.
	//
	// Громкость в YM2612 это уровень несущих операторов, и драйвер игры двигает
	// именно его. Если оставить уровень в тембре, каждая нота другой громкости
	// заводит новый инструмент, а колонка громкости остаётся пустой и ни на что
	// не влияет. Поэтому общий уровень несущих уходит в колонку, а в тембре
	// остаётся только их баланс между собой: движок потом складывает одно с
	// другим обратно и выдаёт чипу исходный уровень.
	const auto patchAndVolume = [&](uint8 channel) -> std::pair<int, uint8>
	{
		const uint8 port = channel < 3 ? 0 : 1;
		const uint8 slot = channel % 3;
		OPN2Patch patch{};
		patch[0] = registers[port][0xB0 + slot] & 0x3F;
		for(uint8 op = 0; op < 4; op++)
		{
			for(uint8 r = 0; r < 7; r++)
			{
				patch[1 + op * 7 + r] = registers[port][OperatorRegister[r] + OperatorOffset[op] + slot] & RegisterMask[r];
			}
		}
		const uint8 carriers = CarrierMask[patch[0] & 7];
		uint8 base = 127;
		for(uint8 op = 0; op < 4; op++)
		{
			if(carriers & (1u << op))
				base = std::min(base, patch[1 + op * 7 + 1]);
		}
		if(base > 63)
			base = 63;  // Ниже этого колонка громкости не достаёт
		for(uint8 op = 0; op < 4; op++)
		{
			if(carriers & (1u << op))
				patch[1 + op * 7 + 1] = static_cast<uint8>(patch[1 + op * 7 + 1] - base);
		}
		const uint8 volume = static_cast<uint8>(std::clamp(64 - static_cast<int>(base), 1, 64));
		for(size_t i = 0; i < patches.size(); i++)
		{
			if(patches[i] == patch)
				return {static_cast<int>(i), volume};
		}
		patches.push_back(patch);
		return {static_cast<int>(patches.size() - 1), volume};
	};

	while(pos < data.size())
	{
		const uint8 code = data[pos++];
		if(code == 0x66)
			break;
		if(code == 0x52 || code == 0x53)
		{
			if(pos + 2 > data.size())
				break;
			const uint8 reg = data[pos], value = data[pos + 1];
			pos += 2;
			const uint8 port = (code == 0x52) ? 0 : 1;
			registers[port][reg] = value;
			if(code == 0x52 && reg == 0x28)
			{
				const uint8 channel = static_cast<uint8>((value & 3) + ((value & 4) ? 3 : 0));
				if(channel >= FMChannels)
					continue;
				if(value & 0xF0)
				{
					const double hertz = fnum[channel]
						? fnum[channel] * static_cast<double>(header.opn2Clock) / (144.0 * std::pow(2.0, 20 - block[channel]))
						: 0.0;
					const auto note = NoteFromFrequency(hertz);
					if(note != NOTE_NONE)
					{
						const auto [patch, volume] = patchAndVolume(channel);
						events.push_back({sample, channel, note, volume, patch});
					}
				} else
				{
					events.push_back({sample, channel, NOTE_KEYOFF, 255, -1});
				}
			} else if(reg >= 0xA4 && reg <= 0xA6)
			{
				const uint8 channel = static_cast<uint8>((port ? 3 : 0) + reg - 0xA4);
				block[channel] = (value >> 3) & 7;
				fnum[channel] = static_cast<uint16>(((value & 7) << 8) | (fnum[channel] & 0xFF));
			} else if(reg >= 0xA0 && reg <= 0xA2)
			{
				const uint8 channel = static_cast<uint8>((port ? 3 : 0) + reg - 0xA0);
				fnum[channel] = static_cast<uint16>((fnum[channel] & 0x700) | value);
			}
		} else if(code == 0x50)
		{
			if(pos >= data.size())
				break;
			const uint8 value = data[pos++];
			if(value & 0x80)
			{
				psgLatch = (value >> 5) & 3;
				psgLatchType = (value >> 4) & 1;
				if(psgLatchType == 0)
				{
					psgPeriod[psgLatch] = static_cast<uint16>((psgPeriod[psgLatch] & 0x3F0) | (value & 15));
				} else
				{
					const uint8 level = static_cast<uint8>(15 - (value & 15));
					const uint8 channel = static_cast<uint8>(FMChannels + psgLatch);
					if(level && !psgVolume[psgLatch])
					{
						const double hertz = (psgLatch == 3 || !psgPeriod[psgLatch])
							? 0.0 : header.psgClock / (32.0 * psgPeriod[psgLatch]);
						const auto note = (psgLatch == 3) ? static_cast<ModCommand::NOTE>(NOTE_MIDDLEC) : NoteFromFrequency(hertz);
						if(note != NOTE_NONE)
							events.push_back({sample, channel, note, static_cast<uint8>(level * 64 / 15), -1});
					} else if(!level && psgVolume[psgLatch])
					{
						events.push_back({sample, channel, NOTE_KEYOFF, 255, -1});
					} else if(level)
					{
						events.push_back({sample, channel, NOTE_NONE, static_cast<uint8>(level * 64 / 15), -1});
					}
					psgVolume[psgLatch] = level;
				}
			} else
			{
				if(psgLatchType == 0)
					psgPeriod[psgLatch] = static_cast<uint16>((psgPeriod[psgLatch] & 15) | ((value & 0x3F) << 4));
			}
		} else if(code == 0x61)
		{
			if(pos + 2 > data.size())
				break;
			sample += static_cast<uint32>(data[pos] | (data[pos + 1] << 8));
			pos += 2;
		} else if(code == 0x62)
		{
			sample += 735;
		} else if(code == 0x63)
		{
			sample += 882;
		} else if(code >= 0x70 && code <= 0x7F)
		{
			sample += (code & 15) + 1;
		} else if(code >= 0x80 && code <= 0x8F)
		{
			sample += code & 15;  // Поток ЦАП пока только отсчитывает время
		} else if(code == 0x67)
		{
			if(pos + 6 > data.size())
				break;
			const uint32 size = ReadLE32(data, pos + 2) & 0x7FFFFFFF;
			pos += 6 + size;
		} else if(code == 0xE0)
		{
			pos += 4;
		} else if(code == 0x4F || code == 0x54 || (code >= 0x30 && code <= 0x3F))
		{
			pos += (code == 0x54) ? 2 : 1;
		} else if((code >= 0x40 && code <= 0x4E) || (code >= 0x55 && code <= 0x5F) || (code >= 0xA0 && code <= 0xBF))
		{
			pos += 2;
		} else if(code >= 0xC0 && code <= 0xDF)
		{
			pos += 3;
		} else if(code >= 0xE1)
		{
			pos += 4;
		} else
		{
			return false;  // Незнакомая команда: лучше отказаться, чем играть мусор
		}
	}

	if(events.empty())
		return false;

	InitializeGlobals(MOD_TYPE_MPT, VgmChannels);
	m_nSamplePreAmp = m_nVSTiVolume = 48;
	Order().SetDefaultSpeed(1);
	// Строка равна кадру: при скорости 1 темп задаёт число тиков в секунду.
	// В классическом счёте тиков в секунду выходит темп * 0,4, поэтому кадр в
	// секунду переводится в темп умножением на 2,5. В «современном» режиме
	// счёт идёт от долей, и тот же кадр потребовал бы темпа 900.
	Order().SetDefaultTempo(TEMPO(static_cast<double>(VgmRate) / samplesPerRow * 2.5));
	m_nTempoMode = TempoMode::Classic;
	m_nDefaultRowsPerBeat = 4;
	m_nDefaultRowsPerMeasure = 16;
	m_SongFlags.set(SONG_LINEARSLIDES);

	// Тембры FM становятся инструментами, каналы PSG — четырьмя своими.
	for(size_t i = 0; i < patches.size() && m_nSamples < MAX_SAMPLES - 1; i++)
	{
		const SAMPLEINDEX smp = ++m_nSamples;
		ModSample &smpl = Samples[smp];
		smpl.Initialize(MOD_TYPE_MPT);
		smpl.SetAdlib(true);
		smpl.isOPN2 = true;
		// Громкость ноты здесь живёт в строке, а не в канале, поэтому
		// SMP_NODEFAULTVOLUME не нужен: пустая строка должна звучать полно.
		smpl.opn2 = patches[i];
		m_szNames[smp] = "FM " + std::to_string(i + 1);
		const INSTRUMENTINDEX ins = ++m_nInstruments;
		if(auto *instrument = AllocateInstrument(ins, smp); instrument != nullptr)
			instrument->name = m_szNames[smp];
	}
	const INSTRUMENTINDEX firstPSG = m_nInstruments;
	for(uint8 lane = 0; lane < PSGChannels; lane++)
	{
		const SAMPLEINDEX smp = ++m_nSamples;
		ModSample &smpl = Samples[smp];
		smpl.Initialize(MOD_TYPE_MPT);
		smpl.SetAdlib(true);
		smpl.uFlags.set(SMP_NODEFAULTVOLUME);
		auto patch = std::make_shared<PSGPatch>();
		patch->lane = lane;
		patch->chip = 0;
		patch->easyPitch = false;
		patch->furnacePitch = false;
		smpl.psg = std::move(patch);
		m_szNames[smp] = (lane == 3) ? "PSG noise" : ("PSG tone " + std::to_string(lane + 1));
		const INSTRUMENTINDEX ins = ++m_nInstruments;
		if(auto *instrument = AllocateInstrument(ins, smp); instrument != nullptr)
			instrument->name = m_szNames[smp];
	}

	for(CHANNELINDEX chn = 0; chn < VgmChannels; chn++)
	{
		ChnSettings[chn].szName = (chn < FMChannels)
			? "YM2612 " + std::to_string(chn + 1)
			: ((chn == VgmChannels - 1) ? "PSG Noise" : "PSG " + std::to_string(chn - FMChannels + 1));
#ifdef MODPLUG_TRACKER
		ChnSettings[chn].color = (chn < FMChannels) ? 0x5A6AE0u : 0x7AC86Au;
#endif
	}

	if(!(loadFlags & loadPatternData))
		return true;

	const uint32 lastRow = events.back().sample / samplesPerRow;
	const ROWINDEX totalRows = static_cast<ROWINDEX>(std::min<uint32>(lastRow + 2, MAX_PATTERN_ROWS * 1000u));
	constexpr ROWINDEX RowsPerPattern = 64;
	const PATTERNINDEX patternCount = static_cast<PATTERNINDEX>((totalRows + RowsPerPattern - 1) / RowsPerPattern);
	Order().resize(patternCount);
	for(PATTERNINDEX pat = 0; pat < patternCount; pat++)
	{
		const PATTERNINDEX index = Patterns.InsertAny(RowsPerPattern);
		if(index == PATTERNINDEX_INVALID)
			return false;
		Order()[pat] = index;
	}

	for(const auto &event : events)
	{
		const uint32 row = event.sample / samplesPerRow;
		const PATTERNINDEX pat = static_cast<PATTERNINDEX>(row / RowsPerPattern);
		if(pat >= patternCount)
			break;
		ModCommand &cell = *Patterns[Order()[pat]].GetpModCommand(static_cast<ROWINDEX>(row % RowsPerPattern), event.channel);
		if(event.note != NOTE_NONE)
		{
			cell.note = event.note;
			if(event.patch >= 0)
				cell.instr = static_cast<ModCommand::INSTR>(event.patch + 1);
			else if(event.channel >= FMChannels)
				cell.instr = static_cast<ModCommand::INSTR>(firstPSG + event.channel - FMChannels + 1);
		}
		if(event.volume != 255)
		{
			cell.volcmd = VOLCMD_VOLUME;
			cell.vol = std::min<uint8>(event.volume, 64);
		}
	}

	m_modFormat.formatName = UL_("VGM (register log)");
	m_modFormat.type = UL_("vgm");
	m_modFormat.madeWithTracker = UL_("Mega Drive sound driver");
	m_modFormat.charset = mpt::Charset::UTF8;
#ifdef MODPLUG_TRACKER
	AddToLog(LogWarning, MPT_UFORMAT("VGM import is approximate: {} voices and {} FM patches were reconstructed from register writes. Effects and the YM2612 sample channel are not imported yet.")(events.size(), patches.size()));
#endif
	return true;
}

OPENMPT_NAMESPACE_END
