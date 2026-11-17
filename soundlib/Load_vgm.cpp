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
 * Громкость идёт отдельно от тембра: драйверы ведут уровень несущих по ходу
 * ноты, и эти записи ложатся в колонку громкости. Удары ЦАПа собираются из
 * блока сэмплов по командам перемотки потока.
 *
 * Что теряется. Плавное изменение прочих регистров в стенограмме неотличимо от
 * нарисованного вручную, поэтому скольжения и вибрато не восстанавливаются:
 * выходит модуль с верными нотами, тембрами и громкостью, но без эффектов.
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
	int pan = -1;           // 0 влево, 128 по центру, 255 вправо, -1 — не менять
};

// Голоса микросхем в этой сборке живут по нотной шкале Furnace, где до первой
// октавы это C-4, а не C-5: импорт из Furnace переносит ноты один в один, и
// шкала досталась от него. У FM это одна октава, а голоса SN76489 звучат на
// две октавы выше своей ноты — ровно это и включает флаг furnacePitch, который
// мы ставим инструментам PSG. Проверено сравнением с эталонным рендером лога:
// без поправки FM получал вдвое более высокую частоту, а PSG звучал октавой
// ниже лога.
constexpr int FMOctave = 12, PSGOctave = 24;
// Шум стоит особняком: его тактует период третьего тона, и на периоде 1 это
// 111 кГц. В шкалу PSG такая частота не влезает — нота выходит за потолок, и
// вся шумовая партия садилась ровно на октаву ниже лога. Инструменту шума мы
// ставим флаг fastNoise, который поднимает шкалу в движке на столько же.
constexpr int NoiseOctave = PSGFastNoiseShift;

ModCommand::NOTE NoteFromFrequency(double hertz, int shift = FMOctave)
{
	if(!(hertz > 8.0) || hertz > 30000.0)
		return NOTE_NONE;
	const int note = static_cast<int>(std::lround(12.0 * std::log2(hertz / MiddleC))) + NOTE_MIDDLEC - shift;
	if(note < NOTE_MIN || note > NOTE_MAX)
		return NOTE_NONE;
	return static_cast<ModCommand::NOTE>(note);
}

ModCommand::NOTE ClampedNote(double hertz, int shift = FMOctave)
{
	if(!(hertz > 0.0))
		return NOTE_NONE;
	const int note = static_cast<int>(std::lround(12.0 * std::log2(hertz / MiddleC))) + NOTE_MIDDLEC - shift;
	return static_cast<ModCommand::NOTE>(std::clamp(note, static_cast<int>(NOTE_MIN), static_cast<int>(NOTE_MAX)));
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
	// Что сейчас звучит на голосе и с какой громкостью: по ним и решается,
	// нужна ли новая нота, смена громкости или снятие.
	std::array<ModCommand::NOTE, PSGChannels> psgSounding{};
	std::array<uint8, PSGChannels> psgLastVolume{};
	uint8 psgLatch = 0, psgLatchType = 0;
	uint8 noiseMode = 0;
	// Поток ЦАП: блок сэмплов из файла, указатель чтения и текущий удар.
	// Команда перемотки стоит ровно в начале каждого удара, поэтому удар —
	// это кусок блока от одной перемотки до следующей, и одинаковые куски
	// становятся одним сэмплом.
	std::vector<uint8> pcm;
	uint32 pcmPointer = 0;
	bool dacEnabled = false;
	// Всё, что ушло в ЦАП побайтно (командами 0x8n или прямой записью в
	// регистр 0x2A), копится в raw, а удар — это кусок raw. Furnace при
	// выгрузке без потоков пишет в 0x2A уже готовую смесь всех своих
	// PCM-каналов, непрерывно и около 32 кГц, поэтому кусок режется не только
	// по паузе в записях, но и по тишине внутри них.
	std::vector<uint8> raw;
	struct Hit { uint32 start = 0, bytes = 0, first = 0, last = 0; };
	Hit hit;
	bool hitOpen = false;
	uint32 silentRun = 0, silentFrom = 0;
	std::vector<Hit> hits;
	const auto flushHit = [&]()
	{
		if(hitOpen && hit.bytes > 64)
			hits.push_back(hit);
		hitOpen = false;
	};
	constexpr uint32 SilenceBytes = 1600;  // ~50 мс на 32 кГц
	const auto appendDAC = [&](uint8 value, uint32 when)
	{
		const bool silent = value >= 127 && value <= 129;
		if(hitOpen && when - hit.last > VgmRate / 20)
			flushHit();
		if(!hitOpen)
		{
			if(silent)
				return;
			hit = Hit{static_cast<uint32>(raw.size()), 0, when, when};
			hitOpen = true;
			silentRun = 0;
		}
		raw.push_back(value);
		hit.bytes++;
		hit.last = when;
		if(silent)
		{
			if(silentRun++ == 0)
				silentFrom = when;
			if(silentRun >= SilenceBytes)
			{
				hit.bytes -= silentRun;
				hit.last = silentFrom;
				flushHit();
			}
		} else
		{
			silentRun = 0;
		}
	};
	// Потоковый ЦАП (команды 0x90-0x95): так выгружает Furnace. Каждый блок
	// данных типа 0 — один сэмпл, команда 0x95 запускает блок с частотой,
	// заданной перед ней командой 0x92, а 0x94 останавливает.
	struct DataBlock { uint32 start = 0, size = 0; };
	std::vector<DataBlock> dataBlocks;
	std::array<uint32, 256> streamFrequency{};
	struct BlockStart { uint32 sample = 0, block = 0, frequency = 0; bool loop = false, stop = false; };
	std::vector<BlockStart> blockStarts;
	// То же, что у PSG: что звучит на голосе FM и с какой громкостью. Нужно,
	// чтобы ловить ведение уровня по ходу ноты, а не только в момент нажатия.
	std::array<ModCommand::NOTE, FMChannels> fmSounding{};
	std::array<uint8, FMChannels> fmLastVolume{};
	// Панорама: микросхема умеет только «влево», «вправо» и «в оба», и драйверы
	// ею пользуются вовсю — голос, уведённый в одну сторону, звучит на шесть
	// децибел тише того же голоса по центру. Пока мы её не переносили, тихие
	// крайние голоса выходили заметно громче, чем на приставке.
	std::array<uint8, FMChannels> fmPan{3, 3, 3, 3, 3, 3};
	std::vector<OPN2Patch> patches;
	std::vector<VgmEvent> events;
	uint32 sample = 0;
	size_t pos = header.dataOffset;

	// Громкость канала FM — это уровень его несущих операторов. Берём самый
	// громкий из них: он и слышен, а разницу между несущими несёт тембр.
	const auto carrierBase = [&](uint8 channel) -> uint8
	{
		const uint8 port = channel < 3 ? 0 : 1;
		const uint8 slot = channel % 3;
		const uint8 carriers = CarrierMask[registers[port][0xB0 + slot] & 7];
		uint8 base = 127;
		for(uint8 op = 0; op < 4; op++)
		{
			if(carriers & (1u << op))
				base = std::min<uint8>(base, registers[port][0x40 + OperatorOffset[op] + slot] & 0x7F);
		}
		return std::min<uint8>(base, 63);  // Ниже этого колонка громкости не достаёт
	};
	const auto volumeFromBase = [](uint8 base) -> uint8
	{
		return static_cast<uint8>(std::clamp(64 - static_cast<int>(base), 1, 64));
	};

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
		const uint8 base = carrierBase(channel);
		for(uint8 op = 0; op < 4; op++)
		{
			if(carriers & (1u << op))
				patch[1 + op * 7 + 1] = static_cast<uint8>(patch[1 + op * 7 + 1] - std::min(patch[1 + op * 7 + 1], base));
		}
		const uint8 volume = volumeFromBase(base);
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
			if(code == 0x52 && reg == 0x2B)
				dacEnabled = (value & 0x80) != 0;
			if(code == 0x52 && reg == 0x2A)
			{
				if(dacEnabled)
					appendDAC(value, sample);
				continue;
			}
			if(code == 0x52 && reg == 0x28)
			{
				const uint8 channel = static_cast<uint8>((value & 3) + ((value & 4) ? 3 : 0));
				if(channel >= FMChannels)
					continue;
				// Под ЦАПом шестой канал играет сэмплы, а не ноты FM.
				if(channel == 5 && dacEnabled)
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
						fmSounding[channel] = note;
						fmLastVolume[channel] = volume;
					}
				} else
				{
					events.push_back({sample, channel, NOTE_KEYOFF, 255, -1});
					fmSounding[channel] = NOTE_NONE;
				}
			} else if(reg >= 0xB4 && reg <= 0xB6)
			{
				const uint8 channel = static_cast<uint8>((port ? 3 : 0) + reg - 0xB4);
				const uint8 sides = (value >> 6) & 3;
				// Ноль — голос отключён от обоих выходов; это не положение в
				// панораме, и прежнее положение мы сохраняем.
				if(sides && sides != fmPan[channel])
				{
					fmPan[channel] = sides;
					const int pan = (sides == 1) ? 255 : ((sides == 2) ? 0 : 128);
					events.push_back({sample, channel, NOTE_NONE, 255, -1, pan});
				}
			} else if(reg >= 0x40 && reg <= 0x4E)
			{
				// Ведение уровня по ходу ноты. Драйвер плавно двигает уровень
				// несущих (в логе это идёт подряд: 92, 93, 94, 95…), и на слух
				// это затухание или подъём внутри ноты. Снимка в момент нажатия
				// для такого мало, поэтому каждая запись уровня при звучащей
				// ноте выносится в колонку громкости. Записи до нажатия — это
				// ещё настройка тембра, в поток они не идут.
				const uint8 slot = reg & 3;
				if(slot < 3)
				{
					const uint8 channel = static_cast<uint8>((port ? 3 : 0) + slot);
					if(fmSounding[channel] != NOTE_NONE && !(channel == 5 && dacEnabled))
					{
						const uint8 volume = volumeFromBase(carrierBase(channel));
						if(volume != fmLastVolume[channel])
						{
							events.push_back({sample, channel, NOTE_NONE, volume, -1});
							fmLastVolume[channel] = volume;
						}
					}
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
					if(psgLatch == 3)
						noiseMode = value & 7;  // Биты 0-1 — скорость, бит 2 — белый шум
				} else
				{
					psgVolume[psgLatch] = static_cast<uint8>(15 - (value & 15));
				}
			} else if(psgLatchType == 0)
			{
				psgPeriod[psgLatch] = static_cast<uint16>((psgPeriod[psgLatch] & 15) | ((value & 0x3F) << 4));
			} else
			{
				psgVolume[psgLatch] = static_cast<uint8>(15 - (value & 0x0F));
			}

			// Нота PSG складывается из двух независимых записей: период и
			// громкость, и порядок у драйверов разный. Поэтому решение
			// принимается после каждой записи по текущему состоянию голоса, а
			// не по той единственной, которая оказалась последней. Из-за
			// прежней проверки «громкость появилась» канал, у которого драйвер
			// ставил громкость раньше периода, молчал всю песню: ноты в тот
			// миг ещё не было, а потом событие уже не возникало.
			// Период третьего голоса правит высоту шума, если тот на него
			// настроен, поэтому решение принимается и за шум тоже.
			const uint8 lane = (psgLatch == 2 && (noiseMode & 3) == 3 && !psgVolume[2]) ? 3 : psgLatch;
			const uint8 channel = static_cast<uint8>(FMChannels + lane);
			const uint8 level = psgVolume[lane];
			// Высота шума. Три значения скорости задают её сами, а четвёртое
			// берёт период третьего тонального голоса — тогда этот голос
			// обычно и не звучит вовсе, он работает подстройкой шума. Без
			// этого все удары выходили одной высоты.
			double hertz = 0.0;
			if(lane == 3)
			{
				// Нулевой период микросхема считает единицей, и драйверы этим
				// пользуются: период третьего голоса они вообще не ставят, а шум
				// идёт на предельной скорости — то высокое шипение, которое и
				// слышно вместо хета. Такую частоту мы прежде отбрасывали, и весь
				// голос шума молчал всю песню.
				const uint16 period = ((noiseMode & 3) == 3)
					? std::max<uint16>(psgPeriod[2], 1)
					: static_cast<uint16>(0x10u << (noiseMode & 3));
				hertz = header.psgClock / (32.0 * period);
			} else if(psgPeriod[lane])
			{
				hertz = header.psgClock / (32.0 * psgPeriod[lane]);
			}
			// У шума крайние частоты законны: на предельной скорости он уходит
			// далеко за слышимый тон, и это шипение, а не промах. Поэтому
			// нота поджимается к краю шкалы, а не отбрасывается.
			const auto note = (lane == 3) ? ClampedNote(hertz, NoiseOctave) : NoteFromFrequency(hertz, PSGOctave);
			const uint8 volume = static_cast<uint8>(level * 64 / 15);
			if(!level)
			{
				if(psgSounding[lane] != NOTE_NONE)
				{
					events.push_back({sample, channel, NOTE_KEYOFF, 255, -1});
					psgSounding[lane] = NOTE_NONE;
				}
			} else if(note != NOTE_NONE && note != psgSounding[lane])
			{
				// Новая нота — только когда сменился полутон: мелкие правки
				// периода внутри ноты это вибрато, и каждую из них в строку
				// выносить незачем.
				events.push_back({sample, channel, note, volume, -1});
				psgSounding[lane] = note;
			} else if(volume != psgLastVolume[lane])
			{
				events.push_back({sample, channel, NOTE_NONE, volume, -1});
			}
			psgLastVolume[lane] = volume;
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
			// Байт из блока в ЦАП и пауза. Из таких команд и состоит удар.
			appendDAC(pcmPointer < pcm.size() ? pcm[pcmPointer] : uint8(0x80), sample);
			pcmPointer++;
			sample += code & 15;
		} else if(code == 0x67)
		{
			if(pos + 6 > data.size())
				break;
			const uint8 blockType = data[pos + 1];
			const uint32 size = ReadLE32(data, pos + 2) & 0x7FFFFFFF;
			if(blockType == 0x00 && pos + 6 + size <= data.size())
			{
				// Тип 0 — сэмплы для ЦАПа YM2612. Блоков бывает несколько, и
				// указатель потока считает их как один непрерывный кусок.
				dataBlocks.push_back({static_cast<uint32>(pcm.size()), size});
				pcm.insert(pcm.end(), data.begin() + pos + 6, data.begin() + pos + 6 + size);
			}
			pos += 6 + size;
		} else if(code == 0xE0)
		{
			if(pos + 4 > data.size())
				break;
			flushHit();
			pcmPointer = ReadLE32(data, pos);
			pos += 4;
		} else if(code >= 0x90 && code <= 0x95)
		{
			static constexpr uint8 StreamCommandSize[] = {4, 4, 5, 10, 1, 4};
			const uint8 size = StreamCommandSize[code - 0x90];
			if(pos + size > data.size())
				break;
			const uint8 stream = data[pos];
			if(code == 0x92)
			{
				streamFrequency[stream] = ReadLE32(data, pos + 1);
			} else if(code == 0x95)
			{
				const uint32 blockID = data[pos + 1] | (data[pos + 2] << 8);
				if(blockID < dataBlocks.size())
					blockStarts.push_back({sample, blockID, streamFrequency[stream], (data[pos + 3] & 1) != 0, false});
			} else if(code == 0x93)
			{
				// Запуск с произвольного места. Furnace так не пишет, но если
				// место совпадает с началом блока, это тот же удар.
				const uint32 offset = ReadLE32(data, pos + 1);
				const auto found = std::find_if(dataBlocks.begin(), dataBlocks.end(),
					[offset](const DataBlock &b) { return b.start == offset; });
				if(found != dataBlocks.end())
					blockStarts.push_back({sample, static_cast<uint32>(std::distance(dataBlocks.begin(), found)), streamFrequency[stream], false, false});
			} else if(code == 0x94)
			{
				if(!blockStarts.empty() && !blockStarts.back().stop)
					blockStarts.push_back({sample, 0, 0, false, true});
			}
			pos += size;
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

	flushHit();
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
	// Удары ЦАПа, записанные побайтно: одинаковые куски становятся одним
	// сэмплом (сравниваем содержимое: у прямых записей адреса в блоке нет), а
	// каждый удар — нотой на шестом канале. Частота считается по самому удару:
	// сколько байт он занял и сколько времени играл.
	const INSTRUMENTINDEX firstDAC = m_nInstruments;
	constexpr size_t MaxDacSamples = 200;
	struct DacSample { uint32 start = 0, length = 0; double rate = 0.0; int count = 0; };
	std::vector<DacSample> dacSamples;
	std::vector<int> hitSample(hits.size(), -1);
	for(size_t i = 0; i < hits.size(); i++)
	{
		const Hit &h = hits[i];
		int match = -1;
		for(size_t s = 0; s < dacSamples.size() && match < 0; s++)
		{
			DacSample &known = dacSamples[s];
			const uint32 common = std::min(h.bytes, known.length);
			if(!std::memcmp(raw.data() + h.start, raw.data() + known.start, common))
			{
				match = static_cast<int>(s);
				if(h.bytes > known.length)
				{
					known.start = h.start;
					known.length = h.bytes;
				}
			}
		}
		if(match < 0 && dacSamples.size() < MaxDacSamples)
		{
			match = static_cast<int>(dacSamples.size());
			dacSamples.push_back({h.start, h.bytes});
		}
		if(match < 0)
			continue;
		hitSample[i] = match;
		if(h.last > h.first)
		{
			dacSamples[match].rate += static_cast<double>(h.bytes) * VgmRate / (h.last - h.first);
			dacSamples[match].count++;
		}
	}
	const auto addDacSample = [&](const uint8 *source, uint32 length, uint32 rate, bool loop, std::string name) -> bool
	{
		if(m_nSamples >= MAX_SAMPLES - 1 || m_nInstruments >= MAX_INSTRUMENTS - 1)
			return false;
		const SAMPLEINDEX smp = ++m_nSamples;
		ModSample &smpl = Samples[smp];
		smpl.Initialize(MOD_TYPE_MPT);
		smpl.nLength = length;
		smpl.nC5Speed = std::clamp<uint32>(rate, 1000, 96000);
		if(loop)
		{
			smpl.nLoopStart = 0;
			smpl.nLoopEnd = length;
			smpl.uFlags.set(CHN_LOOP);
		}
		if(smpl.AllocateSample())
		{
			int8 *target = smpl.sample8();
			for(uint32 i = 0; i < length; i++)
				target[i] = static_cast<int8>(static_cast<int>(source[i]) - 128);
			smpl.PrecomputeLoops(*this, false);
		}
		m_szNames[smp] = std::move(name);
		const INSTRUMENTINDEX ins = ++m_nInstruments;
		if(auto *instrument = AllocateInstrument(ins, smp); instrument != nullptr)
			instrument->name = m_szNames[smp];
		return true;
	};
	size_t dacMade = 0;
	for(const auto &known : dacSamples)
	{
		const uint32 rate = known.count ? mpt::saturate_round<uint32>(known.rate / known.count) : 16000;
		if(!addDacSample(raw.data() + known.start, known.length, rate, false, "DAC " + std::to_string(dacMade + 1)))
			break;
		dacMade++;
	}
	for(size_t i = 0; i < hits.size(); i++)
	{
		if(hitSample[i] < 0 || static_cast<size_t>(hitSample[i]) >= dacMade)
			continue;
		events.push_back({hits[i].first, 5, static_cast<ModCommand::NOTE>(NOTE_MIDDLEC), 64,
			static_cast<int>(firstDAC) + hitSample[i]});
	}

	// Потоковый ЦАП: сэмпл на каждый использованный блок. Его частота — та,
	// с которой блок запускали чаще всего; другие частоты становятся другой
	// нотой того же сэмпла.
	{
		std::vector<int> blockInstrument(dataBlocks.size(), -1);
		std::vector<uint32> blockRate(dataBlocks.size(), 0);
		for(size_t b = 0; b < dataBlocks.size(); b++)
		{
			std::map<uint32, int> rates;
			bool loop = false;
			for(const auto &start : blockStarts)
			{
				if(!start.stop && start.block == b && start.frequency)
				{
					rates[start.frequency]++;
					loop = loop || start.loop;
				}
			}
			if(rates.empty() || !dataBlocks[b].size)
				continue;
			const uint32 rate = std::max_element(rates.begin(), rates.end(),
				[](const auto &x, const auto &y) { return x.second < y.second; })->first;
			if(!addDacSample(pcm.data() + dataBlocks[b].start, dataBlocks[b].size, rate, loop, "DAC block " + std::to_string(b)))
				break;
			blockInstrument[b] = static_cast<int>(m_nInstruments) - 1;
			blockRate[b] = rate;
		}
		for(const auto &start : blockStarts)
		{
			if(start.stop)
			{
				events.push_back({start.sample, 5, NOTE_NOTECUT, 255, -1});
				continue;
			}
			if(blockInstrument[start.block] < 0 || !start.frequency)
				continue;
			const int shift = mpt::saturate_round<int>(12.0 * std::log2(static_cast<double>(start.frequency) / blockRate[start.block]));
			const int note = std::clamp(NOTE_MIDDLEC + shift, int(NOTE_MIN), int(NOTE_MAX));
			events.push_back({start.sample, 5, static_cast<ModCommand::NOTE>(note), 64, blockInstrument[start.block]});
		}
	}
	std::stable_sort(events.begin(), events.end(),
		[](const VgmEvent &a, const VgmEvent &b) { return a.sample < b.sample; });

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
		// Периоды мы знаем точно, поэтому фёрнейсовское отображение высоких
		// нот здесь только мешает: с ним шум уходит ещё на две октавы вниз
		// (проверено сверкой с логом).
		patch->easyPitch = false;
		if(lane == 3)
		{
			// Своя шкала у шума: см. NoiseOctave. Режим голос берёт из
			// значения по умолчанию — белый шум от периода третьего тона,
			// а другого в логах Mega Drive и не встречается.
			patch->fastNoise = true;
		} else
		{
			patch->furnacePitch = true;  // Те же две октавы, что и у импорта из Furnace
		}
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
		if(event.pan >= 0)
		{
			cell.command = CMD_PANNING8;
			cell.param = static_cast<ModCommand::PARAM>(event.pan);
		}
	}

	m_modFormat.formatName = UL_("VGM (register log)");
	m_modFormat.type = UL_("vgm");
	m_modFormat.madeWithTracker = UL_("Mega Drive sound driver");
	m_modFormat.charset = mpt::Charset::UTF8;
#ifdef MODPLUG_TRACKER
	AddToLog(LogWarning, MPT_UFORMAT("VGM import is approximate: {} voices and {} FM patches were reconstructed from register writes. Slides and vibrato are not imported yet.")(events.size(), patches.size()));
#endif
	return true;
}

OPENMPT_NAMESPACE_END
