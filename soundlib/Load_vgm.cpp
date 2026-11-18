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
#ifdef MODPLUG_TRACKER
#include "../mptrack/TrackerSettings.h"
#endif
#include <cstdlib>
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
constexpr uint8 FMChannels = 6, PSGChannels = 4, OPMChannels = 8;
// Голоса в событиях нумеруются по всем чипам сразу: YM2612 0-5, PSG 6-9,
// YM2151 10-17. Каналы модуля заводятся только под те чипы, что есть в файле.
constexpr uint8 OPMBase = FMChannels + PSGChannels;
// YMZ280B — восемь голосов сэмплов, идут после YM2151.
constexpr uint8 YMZChannels = 8;
constexpr uint8 YMZBase = OPMBase + OPMChannels;
constexpr uint8 VgmChannels = YMZBase + YMZChannels;
// Коды нот YM2151 (младшие 4 бита KC) идут от до-диеза, каждый четвёртый
// пропущен. -1 — кода нет.
constexpr int8 OPMKeyCode[16] = {0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1};
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
	uint32 ymzClock = 0;
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
	header.ymzClock = (0x68 + 4 <= headerEnd) ? (ReadLE32(data, 0x68) & 0x3FFFFFFF) : 0;
	for(uint16 offset : ForeignClockOffset)
	{
		if(offset == 0x30 || offset == 0x68)
			continue;  // YM2151 и YMZ280B у нас есть
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
	// Высота внутри ноты. legato — нота без нового удара (портаменто, которое
	// в режиме слайдов Furnace доезжает за тик), fine — подстройка канала
	// E5xx: 0x80 ровно, шаг 1/128 полутона.
	bool legato = false;
	int fine = -1;
	// Уровни (TL) модуляторов, которые голос должен держать, -1 — не важно.
	// Перед записью в паттерн отсюда остаются только отличия от того, что
	// канал уже держит: они становятся эффектами 12xx–15xx.
	std::array<int8, 4> level{-1, -1, -1, -1};
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

// Нота с дробной частью, в той же шкале, что и NoteFromFrequency.
double ExactNote(double hertz, int shift = FMOctave)
{
	return 12.0 * std::log2(hertz / MiddleC) + static_cast<int>(NOTE_MIDDLEC) - shift;
}

// Подстройка E5xx для отклонения от ноты в полутонах (±1).
int FineFromOffset(double offset)
{
	return std::clamp(0x80 + static_cast<int>(std::lround(offset * 128.0)), 1, 0xFF);
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
	if(header.foreignChip || (!header.opn2Clock && !header.opmClock))
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
	std::array<int, PSGChannels> psgFine;
	psgFine.fill(0x80);
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

	std::vector<Hit> hits;
	const auto flushHit = [&]()
	{
		if(hitOpen && hit.bytes > 64)
			hits.push_back(hit);
		hitOpen = false;
	};
	// ЦАП держит записанное значение до следующей записи, а пишут в него
	// неравномерно: Furnace при прямых записях — то чаще, то реже. Поэтому
	// поток снимается по времени лога, по значению на каждый его отсчёт
	// (44100 Гц): тогда внутри длинного куска удары стоят там же, где в логе.
	// Раньше частота куска считалась как «байты на время» по всему куску, и в
	// Death Egg Robot (104 с одним куском) удары разъезжались.
	// Кусок кончается, когда значение долго не меняется — тишина у ЦАПа не
	// обязательно 128 (там же это 138).
	constexpr uint32 SilenceSamples = VgmRate / 20;
	uint8 dacLevel = 0x80;
	uint32 dacSince = 0, lastChange = 0;
	const auto closeHit = [&]()
	{
		if(!hitOpen)
			return;
		const uint32 keep = std::min<uint32>(hit.bytes, lastChange - hit.first + 1);
		raw.resize(hit.start + keep);
		hit.bytes = keep;
		hit.last = hit.first + keep;
		flushHit();
	};
	const auto appendDAC = [&](uint8 value, uint32 when)
	{
		if(hitOpen)
		{
			if(when - lastChange > SilenceSamples)
			{
				closeHit();
			} else if(when > dacSince)
			{
				raw.insert(raw.end(), when - dacSince, dacLevel);
				hit.bytes += when - dacSince;
				hit.last = when;
			}
		}
		if(value != dacLevel)
		{
			if(!hitOpen)
			{
				hit = Hit{static_cast<uint32>(raw.size()), 0, when, when};
				hitOpen = true;
			}
			lastChange = when;
		}
		dacLevel = value;
		dacSince = when;
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
	// Подстройка канала, которую сейчас держит движок: E5xx переживает ноты,
	// поэтому пишем её только когда она меняется.
	std::array<int, FMChannels> fmFine;
	fmFine.fill(0x80);
	// Нота, от которой движок считает высоту. В отличие от fmSounding она
	// переживает снятие ключа: Furnace ведёт слайды и вибрато и на затухании.
	std::array<ModCommand::NOTE, FMChannels> fmPitchNote{};
	const auto fmHertz = [&](uint8 channel)
	{
		return fnum[channel] ? fnum[channel] * static_cast<double>(header.opn2Clock) / (144.0 * std::pow(2.0, 20 - block[channel])) : 0.0;
	};
	// Панорама: микросхема умеет только «влево», «вправо» и «в оба», и драйверы
	// ею пользуются вовсю — голос, уведённый в одну сторону, звучит на шесть
	// децибел тише того же голоса по центру. Пока мы её не переносили, тихие
	// крайние голоса выходили заметно громче, чем на приставке.
	std::array<uint8, FMChannels> fmPan{3, 3, 3, 3, 3, 3};
	struct FMVoice
	{
		OPN2Patch patch{};
		FMLfo lfo = DefaultFMLfo;
		bool opm = false;
		bool operator==(const FMVoice &other) const { return patch == other.patch && lfo == other.lfo && opm == other.opm; }
	};
	std::vector<FMVoice> patches;
	// Тембры, которые отличаются только уровнем модуляторов, — один
	// инструмент: это тот же звук, у которого макрос или эффект подкрутил
	// яркость. Уровни модуляторов ноты уходят в эффекты 12xx–15xx. Без этого
	// из Death Egg Act 1 выходило 75 инструментов при 28 в самом .fur.
	std::array<int8, 4> noteLevels{-1, -1, -1, -1};
	const auto findPatch = [&](const FMVoice &voice) -> int
	{
		const uint8 carriers = CarrierMask[voice.patch[0] & 7];
		for(uint8 op = 0; op < 4; op++)
			noteLevels[op] = (carriers & (1u << op)) ? int8(-1) : static_cast<int8>(voice.patch[1 + op * 7 + 1] & 0x7F);
		const auto sameButModulators = [carriers](const FMVoice &a, const FMVoice &b)
		{
			if(a.opm != b.opm || a.lfo != b.lfo || a.patch[0] != b.patch[0])
				return false;
			for(size_t i = 1; i < a.patch.size(); i++)
			{
				const size_t op = (i - 1) / 7;
				if((i - 1) % 7 == 1 && !(carriers & (1u << op)))
					continue;
				if(a.patch[i] != b.patch[i])
					return false;
			}
			return true;
		};
		for(size_t i = 0; i < patches.size(); i++)
		{
			if(sameButModulators(patches[i], voice))
				return static_cast<int>(i);
		}
		patches.push_back(voice);
		return static_cast<int>(patches.size() - 1);
	};
	// YM2151: регистры, высота, что звучит.
	std::array<uint8, 256> opm{};
	uint8 opmAMD = 0, opmPMD = 0;
	std::array<ModCommand::NOTE, OPMChannels> opmSounding{}, opmPitchNote{};
	std::array<uint8, OPMChannels> opmLastVolume{};
	std::array<int, OPMChannels> opmFine;
	opmFine.fill(0x80);
	std::array<uint8, OPMChannels> opmPan{3, 3, 3, 3, 3, 3, 3, 3};
	// YMZ280B: образ памяти сэмплов (блок данных 0x86), регистры и то, что
	// происходило с голосами. Ноты из этого собираются после чтения, когда
	// известны все сэмплы и их обычные частоты.
	std::vector<uint8> ymzRom;
	std::array<uint8, 256> ymz{};
	struct YMZSound
	{
		uint32 start = 0, end = 0, loopStart = 0, loopEnd = 0;
		uint8 mode = 0;
		bool loop = false;
		bool operator==(const YMZSound &o) const { return start == o.start && end == o.end && loopStart == o.loopStart && loopEnd == o.loopEnd && mode == o.mode && loop == o.loop; }
	};
	enum class YMZWhat : uint8 { KeyOn, Pitch, KeyOff, Level, Pan };
	struct YMZEvent { uint32 sample = 0; uint8 channel = 0; YMZWhat what = YMZWhat::KeyOn; YMZSound sound; double rate = 0.0; uint8 value = 0; };
	std::vector<YMZEvent> ymzEvents;
	const auto ymzAddress = [&](uint8 base, uint8 channel) -> uint32
	{
		return (ymz[base + channel * 4] << 16) | (ymz[base + 0x20 + channel * 4] << 8) | ymz[base + 0x40 + channel * 4];
	};
	const auto ymzRate = [&](uint8 channel) -> double
	{
		const uint32 fnum = ((ymz[0x01 + channel * 4] & 1) << 8) | ymz[0x00 + channel * 4];
		return (fnum + 1) * static_cast<double>(header.ymzClock) / (384.0 * 256.0);
	};
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
		FMVoice voice;
		voice.patch = patch;
		// LFO канала: чувствительность AMS/FMS в B4, общий LFO в 0x22.
		const uint8 b4 = registers[port][0xB4 + slot];
		voice.lfo[FMLfoAMS] = (b4 >> 4) & 3;
		voice.lfo[FMLfoFMS] = b4 & 7;
		if((registers[0][0x22] & 0x08) && (b4 & 0x37))
		{
			voice.lfo[FMLfoFlags] = FMLfoSetChip;
			voice.lfo[FMLfoRate] = registers[0][0x22] & 7;
		}
		return {findPatch(voice), volume};
	};

	// То же для YM2151: регистры оператора идут через 0x20, операторы
	// M1, C1, M2, C2 лежат со сдвигом 0, 16, 8, 24.
	static constexpr uint8 OPMOperatorOffset[] = {0, 16, 8, 24};
	static constexpr uint8 OPMRegisterMask[] = {0x7F, 0x7F, 0xDF, 0x9F, 0xDF, 0xFF};
	const auto opmCarrierBase = [&](uint8 channel) -> uint8
	{
		const uint8 carriers = CarrierMask[opm[0x20 + channel] & 7];
		uint8 base = 127;
		for(uint8 op = 0; op < 4; op++)
		{
			if(carriers & (1u << op))
				base = std::min<uint8>(base, opm[0x60 + OPMOperatorOffset[op] + channel] & 0x7F);
		}
		return std::min<uint8>(base, 63);
	};
	const auto opmPatchAndVolume = [&](uint8 channel) -> std::pair<int, uint8>
	{
		FMVoice voice;
		voice.opm = true;
		voice.patch[0] = opm[0x20 + channel] & 0x3F;
		for(uint8 op = 0; op < 4; op++)
		{
			for(uint8 r = 0; r < 6; r++)
				voice.patch[1 + op * 7 + r] = opm[0x40 + r * 0x20 + OPMOperatorOffset[op] + channel] & OPMRegisterMask[r];
		}
		const uint8 carriers = CarrierMask[voice.patch[0] & 7];
		const uint8 base = opmCarrierBase(channel);
		for(uint8 op = 0; op < 4; op++)
		{
			if(carriers & (1u << op))
				voice.patch[1 + op * 7 + 1] = static_cast<uint8>(voice.patch[1 + op * 7 + 1] - std::min(voice.patch[1 + op * 7 + 1], base));
		}
		const uint8 pmsAms = opm[0x38 + channel];
		voice.lfo[FMLfoAMS] = pmsAms & 3;
		voice.lfo[FMLfoFMS] = (pmsAms >> 4) & 7;
		if(pmsAms & 0x73)
		{
			voice.lfo[FMLfoFlags] = FMLfoSetChip;
			voice.lfo[FMLfoRate] = opm[0x18];
			voice.lfo[FMLfoAMD] = opmAMD;
			voice.lfo[FMLfoPMD] = opmPMD;
			voice.lfo[FMLfoWave] = opm[0x1B] & 3;
		}
		return {findPatch(voice), volumeFromBase(base)};
	};
	// Высота YM2151 из KC и KF: нота с дробью в шкале FM.
	const auto opmExactNote = [&](uint8 channel) -> double
	{
		const uint8 kc = opm[0x28 + channel];
		const int code = OPMKeyCode[kc & 15];
		if(code < 0)
			return -1.0;
		const double key = (kc >> 4) * 12 + code + (opm[0x30 + channel] >> 2) / 64.0;
		// Ключ 56 — ля первой октавы, 440 Гц при штатной частоте 3,579545 МГц.
		const double hertz = 440.0 * std::pow(2.0, (key - 56.0) / 12.0) * header.opmClock / 3579545.0;
		// Голоса YM2151 в движке звучат на октаву ниже YM2612 при той же ноте
		// (сверено по кадрам с логом), поэтому сдвиг шкалы у них нулевой.
		return ExactNote(hertz, 0);
	};
	const auto opmPitchChanged = [&](uint8 channel)
	{
		const uint8 voiceIndex = static_cast<uint8>(OPMBase + channel);
		if(opmPitchNote[channel] == NOTE_NONE)
			return;
		const double exact = opmExactNote(channel);
		if(exact < static_cast<int>(NOTE_MIN) || exact > static_cast<int>(NOTE_MAX))
			return;
		const bool keyed = opmSounding[channel] != NOTE_NONE;
		VgmEvent event{sample, voiceIndex, NOTE_NONE, 255, -1};
		if(std::abs(exact - opmPitchNote[channel]) > 127.0 / 128.0 && keyed)
		{
			event.note = static_cast<ModCommand::NOTE>(std::lround(exact));
			event.legato = true;
			opmSounding[channel] = event.note;
			opmPitchNote[channel] = event.note;
		}
		const int fine = FineFromOffset(exact - opmPitchNote[channel]);
		if(fine != opmFine[channel])
		{
			event.fine = fine;
			opmFine[channel] = fine;
		}
		if(event.legato || event.fine >= 0)
			events.push_back(event);
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
				if(!dacEnabled)
					closeHit();
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
					const double hertz = fmHertz(channel);
					const auto note = NoteFromFrequency(hertz);
					if(note != NOTE_NONE)
					{
						const auto [patch, volume] = patchAndVolume(channel);
						const int fine = FineFromOffset(ExactNote(hertz) - note);
						events.push_back({sample, channel, note, volume, patch, -1, false, fine != fmFine[channel] ? fine : -1, noteLevels});
						fmFine[channel] = fine;
						fmSounding[channel] = note;
						fmPitchNote[channel] = note;
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
						// Уровень модулятора посреди ноты (яркость) не переносим:
						// пробовал эффектами 12xx–15xx — на слух и по огибающей
						// выходило хуже, чем без них (Mushroom Hill 1: 0.91 -> 0.88).
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
				// Запись младших битов и есть момент смены высоты: старшие чип
				// держит в защёлке до неё. Если нота звучит, это слайд, вибрато
				// или подстройка: в пределах полутона от ноты строки пишем E5xx,
				// дальше — новую ноту без удара.
				if(fmPitchNote[channel] != NOTE_NONE && !(channel == 5 && dacEnabled))
				{
					const double hertz = fmHertz(channel);
					const bool keyed = fmSounding[channel] != NOTE_NONE;
					if(NoteFromFrequency(hertz) != NOTE_NONE)
					{
						const double exact = ExactNote(hertz);
						VgmEvent event{sample, channel, NOTE_NONE, 255, -1};
						const bool farAway = std::abs(exact - fmPitchNote[channel]) > 127.0 / 128.0;
						// Легато только у нажатой ноты: на затухании новая нота
						// заново нажала бы ключ. Там уходим не дальше полутона.
						if(farAway && keyed)
						{
							event.note = NoteFromFrequency(hertz);
							event.legato = true;
							if(keyed)
								fmSounding[channel] = event.note;
							fmPitchNote[channel] = event.note;
						}
						const int fine = FineFromOffset(exact - fmPitchNote[channel]);
						if(fine != fmFine[channel])
						{
							event.fine = fine;
							fmFine[channel] = fine;
						}
						if(event.legato || event.fine >= 0)
							events.push_back(event);
					}
				}
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
			} else if(lane < 3 && note != NOTE_NONE)
			{
				// Тон ведётся как FM: в пределах полутона от ноты строки —
				// подстройка E5xx, чуть дальше — легато (слайд, вибрато), а
				// скачок больше трёх полутонов — новая нота. Ключа у PSG нет,
				// так что новая нота ничего не обрывает, а портаменто до далёкой
				// ноты ехало бы несколько тиков и было бы слышно.
				const double exact = ExactNote(hertz, PSGOctave);
				VgmEvent event{sample, channel, NOTE_NONE, 255, -1};
				const double away = (psgSounding[lane] == NOTE_NONE) ? 99.0 : std::abs(exact - psgSounding[lane]);
				if(away > 3.0)
				{
					event.note = note;
					event.volume = volume;
					psgSounding[lane] = note;
				} else if(away > 127.0 / 128.0)
				{
					event.note = note;
					event.legato = true;
					psgSounding[lane] = note;
				}
				const int fine = FineFromOffset(exact - psgSounding[lane]);
				if(fine != psgFine[lane])
				{
					event.fine = fine;
					psgFine[lane] = fine;
				}
				if(event.volume == 255 && volume != psgLastVolume[lane])
					event.volume = volume;
				if(event.note != NOTE_NONE || event.fine >= 0 || event.volume != 255)
					events.push_back(event);
			} else if(note != NOTE_NONE && note != psgSounding[lane])
			{
				// Шум: новая нота — только когда сменился полутон.
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
			} else if(blockType == 0x86 && size > 8 && pos + 6 + size <= data.size())
			{
				// Образ памяти YMZ280B: полный размер, адрес куска, сам кусок.
				const uint32 offset = ReadLE32(data, pos + 10);
				const uint32 length = size - 8;
				if(offset < (16u << 20) && length <= (16u << 20) - offset)
				{
					if(ymzRom.size() < offset + length)
						ymzRom.resize(offset + length);
					std::copy(data.begin() + pos + 14, data.begin() + pos + 14 + length, ymzRom.begin() + offset);
				}
			}
			pos += 6 + size;
		} else if(code == 0xE0)
		{
			if(pos + 4 > data.size())
				break;
			closeHit();
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
		} else if(code == 0x54)
		{
			if(pos + 2 > data.size())
				break;
			const uint8 reg = data[pos], value = data[pos + 1];
			pos += 2;
			if(reg == 0x19)
			{
				// Один адрес на две глубины: старший бит выбирает PMD или AMD.
				if(value & 0x80)
					opmPMD = value & 0x7F;
				else
					opmAMD = value & 0x7F;
				continue;
			}
			opm[reg] = value;
			if(reg == 0x08)
			{
				const uint8 channel = value & 7;
				const uint8 voiceIndex = static_cast<uint8>(OPMBase + channel);
				if(value & 0x78)
				{
					if(opmSounding[channel] != NOTE_NONE)
						continue;  // Догоняющее нажатие других операторов той же ноты
					const double exact = opmExactNote(channel);
					if(exact < static_cast<int>(NOTE_MIN) || exact > static_cast<int>(NOTE_MAX))
						continue;
					const auto note = static_cast<ModCommand::NOTE>(std::lround(exact));
					const auto [patch, volume] = opmPatchAndVolume(channel);
					const int fine = FineFromOffset(exact - note);
					events.push_back({sample, voiceIndex, note, volume, patch, -1, false, fine != opmFine[channel] ? fine : -1, noteLevels});
					opmFine[channel] = fine;
					opmSounding[channel] = note;
					opmPitchNote[channel] = note;
					opmLastVolume[channel] = volume;
				} else if(opmSounding[channel] != NOTE_NONE)
				{
					events.push_back({sample, voiceIndex, NOTE_KEYOFF, 255, -1});
					opmSounding[channel] = NOTE_NONE;
				}
			} else if(reg >= 0x20 && reg <= 0x27)
			{
				const uint8 channel = reg & 7;
				// RL: бит 6 — левый выход, бит 7 — правый.
				const uint8 sides = (value >> 6) & 3;
				if(sides && sides != opmPan[channel])
				{
					opmPan[channel] = sides;
					const int pan = (sides == 1) ? 0 : ((sides == 2) ? 255 : 128);
					events.push_back({sample, static_cast<uint8>(OPMBase + channel), NOTE_NONE, 255, -1, pan});
				}
			} else if(reg >= 0x28 && reg <= 0x37)
			{
				opmPitchChanged(reg & 7);
			} else if(reg >= 0x60 && reg <= 0x7F)
			{
				const uint8 channel = reg & 7;
				if(opmSounding[channel] != NOTE_NONE)
				{
					const uint8 volume = volumeFromBase(opmCarrierBase(channel));
					if(volume != opmLastVolume[channel])
					{
						events.push_back({sample, static_cast<uint8>(OPMBase + channel), NOTE_NONE, volume, -1});
						opmLastVolume[channel] = volume;
					}
				}
			}
		} else if(code == 0x5D)
		{
			if(pos + 2 > data.size())
				break;
			const uint8 reg = data[pos], value = data[pos + 1];
			pos += 2;
			const uint8 old = ymz[reg];
			ymz[reg] = value;
			if(reg < 0x20)
			{
				const uint8 channel = reg >> 2;
				const bool playing = (ymz[0x01 + channel * 4] & 0x80) != 0;
				switch(reg & 3)
				{
				case 0:
					if(playing)
						ymzEvents.push_back({sample, channel, YMZWhat::Pitch, {}, ymzRate(channel)});
					break;
				case 1:
					if((value & 0x80) && !(old & 0x80) && (value & 0x60))
					{
						YMZSound sound;
						sound.start = ymzAddress(0x20, channel);
						sound.loopStart = ymzAddress(0x21, channel);
						sound.loopEnd = ymzAddress(0x22, channel);
						sound.end = ymzAddress(0x23, channel);
						sound.mode = (value >> 5) & 3;
						sound.loop = (value & 0x10) != 0;
						if(!sound.loop)
							sound.loopStart = sound.loopEnd = 0;
						ymzEvents.push_back({sample, channel, YMZWhat::KeyOn, sound, ymzRate(channel), ymz[0x02 + channel * 4]});
					} else if(!(value & 0x80) && (old & 0x80))
					{
						ymzEvents.push_back({sample, channel, YMZWhat::KeyOff});
					} else if(playing && ((value ^ old) & 1))
					{
						ymzEvents.push_back({sample, channel, YMZWhat::Pitch, {}, ymzRate(channel)});
					}
					break;
				case 2:
					if(playing && value != old)
						ymzEvents.push_back({sample, channel, YMZWhat::Level, {}, 0.0, value});
					break;
				case 3:
					if(value != old)
						ymzEvents.push_back({sample, channel, YMZWhat::Pan, {}, 0.0, static_cast<uint8>(value & 15)});
					break;
				}
			}
		} else if(code == 0x4F || (code >= 0x30 && code <= 0x3F))
		{
			pos += 1;
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

	closeHit();
	if(events.empty())
		return false;

	// Голосов YMZ280B заводим столько, сколько их реально нажималось.
	uint8 ymzVoicesUsed = 0;
	for(const auto &e : ymzEvents)
	{
		if(e.what == YMZWhat::KeyOn)
			ymzVoicesUsed = std::max<uint8>(ymzVoicesUsed, static_cast<uint8>(e.channel + 1));
	}
	// Каналы модуля — только под чипы из файла, в порядке YM2612, PSG, YM2151, YMZ280B.
	std::array<int, VgmChannels> moduleChannel;
	moduleChannel.fill(-1);
	CHANNELINDEX usedChannels = 0;
	for(uint8 voice = 0; voice < VgmChannels; voice++)
	{
		const bool present = (voice < FMChannels) ? (header.opn2Clock != 0)
			: (voice < OPMBase) ? (header.psgClock != 0 && header.opn2Clock != 0)
			: (voice < YMZBase) ? (header.opmClock != 0)
			: (header.ymzClock != 0 && !ymzRom.empty() && voice - YMZBase < ymzVoicesUsed);
		if(present)
			moduleChannel[voice] = usedChannels++;
	}
	InitializeGlobals(MOD_TYPE_MPT, usedChannels);
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
	// Слайды и подстройка по правилам Furnace: портаменто работает и на первом
	// тике (строка здесь — один тик), а E5xx держится в канале через ноты.
	// Остановка у цели обязательна: иначе портаменто Furnace остаётся
	// включённым и после легато, и следующие настоящие ноты тоже становятся
	// легато — голос FM не нажимается заново, громкость проседает (Lava Reef 2).
	m_furnaceSlides = 4 | FurnaceStopAtTarget;
	m_playBehaviour = GetDefaultPlaybackBehaviour(MOD_TYPE_MPT);
	m_playBehaviour.set(kFurnacePersistentFinetune);

	// Тембры FM становятся инструментами, каналы PSG — четырьмя своими.
	for(size_t i = 0; i < patches.size() && m_nSamples < MAX_SAMPLES - 1; i++)
	{
		const SAMPLEINDEX smp = ++m_nSamples;
		ModSample &smpl = Samples[smp];
		smpl.Initialize(MOD_TYPE_MPT);
		smpl.SetAdlib(true);
		smpl.isOPN2 = true;
		smpl.isOPM = patches[i].opm;
		// Громкость ноты здесь живёт в строке, а не в канале, поэтому
		// SMP_NODEFAULTVOLUME не нужен: пустая строка должна звучать полно.
		smpl.opn2 = patches[i].patch;
		smpl.fmLfo = patches[i].lfo;
		m_szNames[smp] = (patches[i].opm ? "OPM " : "FM ") + std::to_string(i + 1);
		const INSTRUMENTINDEX ins = ++m_nInstruments;
		if(auto *instrument = AllocateInstrument(ins, smp); instrument != nullptr)
		{
			instrument->name = m_szNames[smp];
			instrument->midiPWD = 1;  // E5xx: шаг 1/128 полутона
		}
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
		{
			instrument->name = m_szNames[smp];
			instrument->midiPWD = 1;  // E5xx: шаг 1/128 полутона
		}
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
	// YMZ280B: сэмпл на каждый разный кусок памяти, нота по частоте нажатия
	// относительно обычной для этого сэмпла, подстройка и легато — как у FM.
	if(!ymzEvents.empty() && !ymzRom.empty())
	{
		std::vector<YMZSound> sounds;
		std::vector<std::map<int64, int>> rates;
		for(const auto &e : ymzEvents)
		{
			if(e.what != YMZWhat::KeyOn)
				continue;
			auto found = std::find(sounds.begin(), sounds.end(), e.sound);
			if(found == sounds.end())
			{
				sounds.push_back(e.sound);
				rates.emplace_back();
				found = sounds.end() - 1;
			}
			rates[std::distance(sounds.begin(), found)][std::llround(e.rate)]++;
		}
		std::vector<int> soundInstrument(sounds.size(), -1);
		std::vector<double> soundRate(sounds.size(), 0.0);
		for(size_t i = 0; i < sounds.size(); i++)
		{
			if(m_nSamples >= MAX_SAMPLES - 1 || m_nInstruments >= MAX_INSTRUMENTS - 1)
				break;
			const YMZSound &sound = sounds[i];
			// Длина в кадрах по режиму: ADPCM — полбайта, 8 бит — байт, 16 бит — два.
			const auto frames = [&](uint32 from, uint32 to) -> uint32
			{
				if(to <= from)
					return 0;
				return (sound.mode == 1) ? (to - from) * 2 : (sound.mode == 2) ? (to - from) : (to - from) / 2;
			};
			const uint32 length = std::min<uint32>(frames(sound.start, sound.end), MAX_SAMPLE_LENGTH);
			if(!length || sound.start >= ymzRom.size())
				continue;
			const SAMPLEINDEX smp = ++m_nSamples;
			ModSample &smpl = Samples[smp];
			smpl.Initialize(MOD_TYPE_MPT);
			smpl.nLength = length;
			smpl.uFlags.set(CHN_16BIT);
			smpl.chipPanning = true;  // Панорама YMZ280B: центр — полная громкость в оба уха
			const auto best = std::max_element(rates[i].begin(), rates[i].end(), [](const auto &a, const auto &b) { return a.second < b.second; });
			soundRate[i] = static_cast<double>(best->first);
			smpl.nC5Speed = std::clamp<uint32>(static_cast<uint32>(best->first), 1000, 192000);
			if(sound.loop && sound.loopEnd > sound.loopStart && sound.loopStart >= sound.start)
			{
				smpl.nLoopStart = std::min(frames(sound.start, sound.loopStart), length);
				smpl.nLoopEnd = std::min(frames(sound.start, sound.loopEnd), length);
				if(smpl.nLoopEnd > smpl.nLoopStart)
					smpl.uFlags.set(CHN_LOOP);
			}
			if(smpl.AllocateSample())
			{
				int16 *target = smpl.sample16();
				const auto romByte = [&](uint32 address) -> uint8 { return address < ymzRom.size() ? ymzRom[address] : uint8(0); };
				if(sound.mode == 1)
				{
					// ADPCM YMZ280B (как в эмуляторе MAME): старший полубайт первый.
					static constexpr int Diff[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
					static constexpr int Scale[8] = {0x0E6, 0x0E6, 0x0E6, 0x0E6, 0x133, 0x199, 0x200, 0x266};
					int signal = 0, step = 0x7F;
					for(uint32 n = 0; n < length; n++)
					{
						const uint8 byte = romByte(sound.start + n / 2);
						const int nibble = (n & 1) ? (byte & 15) : (byte >> 4);
						signal = std::clamp(signal + step * Diff[nibble] / 8, -32768, 32767);
						step = std::clamp((step * Scale[nibble & 7]) >> 8, 0x7F, 0x6000);
						target[n] = static_cast<int16>(signal);
					}
				} else if(sound.mode == 2)
				{
					for(uint32 n = 0; n < length; n++)
						target[n] = static_cast<int16>(static_cast<int8>(romByte(sound.start + n)) * 256);
				} else
				{
					// 16 бит, старший байт первым (так их раскладывает Furnace).
					for(uint32 n = 0; n < length; n++)
						target[n] = static_cast<int16>((romByte(sound.start + n * 2) << 8) | romByte(sound.start + n * 2 + 1));
				}
				smpl.PrecomputeLoops(*this, false);
			}
			m_szNames[smp] = "YMZ " + std::to_string(i + 1);
			const INSTRUMENTINDEX ins = ++m_nInstruments;
			if(auto *instrument = AllocateInstrument(ins, smp); instrument != nullptr)
			{
				instrument->name = m_szNames[smp];
				instrument->midiPWD = 1;
			}
			soundInstrument[i] = static_cast<int>(m_nInstruments) - 1;
		}
		std::array<ModCommand::NOTE, YMZChannels> sounding{}, pitchNote{};
		std::array<int, YMZChannels> fineNow;
		fineNow.fill(0x80);
		std::array<double, YMZChannels> baseRate{};
		const auto levelToVolume = [](uint8 level) { return static_cast<uint8>((level * 64 + 127) / 255); };
		for(const auto &e : ymzEvents)
		{
			const uint8 voice = static_cast<uint8>(YMZBase + e.channel);
			const uint8 ch = e.channel;
			switch(e.what)
			{
			case YMZWhat::KeyOn:
			{
				const auto found = std::find(sounds.begin(), sounds.end(), e.sound);
				const size_t index = std::distance(sounds.begin(), found);
				if(found == sounds.end() || soundInstrument[index] < 0 || !(e.rate > 0.0))
					break;
				baseRate[ch] = soundRate[index];
				const double exact = static_cast<int>(NOTE_MIDDLEC) + 12.0 * std::log2(e.rate / baseRate[ch]);
				const auto note = static_cast<ModCommand::NOTE>(std::clamp(static_cast<int>(std::lround(exact)), int(NOTE_MIN), int(NOTE_MAX)));
				const int fine = FineFromOffset(exact - note);
				events.push_back({e.sample, voice, note, levelToVolume(e.value), soundInstrument[index], -1, false, fine != fineNow[ch] ? fine : -1});
				fineNow[ch] = fine;
				sounding[ch] = note;
				pitchNote[ch] = note;
				break;
			}
			case YMZWhat::Pitch:
			{
				if(sounding[ch] == NOTE_NONE || !(baseRate[ch] > 0.0) || !(e.rate > 0.0))
					break;
				const double exact = static_cast<int>(NOTE_MIDDLEC) + 12.0 * std::log2(e.rate / baseRate[ch]);
				VgmEvent event{e.sample, voice, NOTE_NONE, 255, -1};
				if(std::abs(exact - pitchNote[ch]) > 127.0 / 128.0)
				{
					event.note = static_cast<ModCommand::NOTE>(std::clamp(static_cast<int>(std::lround(exact)), int(NOTE_MIN), int(NOTE_MAX)));
					event.legato = true;
					pitchNote[ch] = event.note;
				}
				const int fine = FineFromOffset(exact - pitchNote[ch]);
				if(fine != fineNow[ch])
				{
					event.fine = fine;
					fineNow[ch] = fine;
				}
				if(event.legato || event.fine >= 0)
					events.push_back(event);
				break;
			}
			case YMZWhat::KeyOff:
				if(sounding[ch] != NOTE_NONE)
					events.push_back({e.sample, voice, NOTE_NOTECUT, 255, -1});
				sounding[ch] = NOTE_NONE;
				break;
			case YMZWhat::Level:
				if(sounding[ch] != NOTE_NONE)
					events.push_back({e.sample, voice, NOTE_NONE, levelToVolume(e.value), -1});
				break;
			case YMZWhat::Pan:
			{
				// 8 — центр, 1 — крайний левый, 15 — крайний правый.
				const int pan = (e.value == 8) ? 128 : (e.value < 8) ? std::max(0, (e.value - 1) * 128 / 7) : std::min(255, 128 + (e.value - 8) * 127 / 7);
				events.push_back({e.sample, voice, NOTE_NONE, 255, -1, pan});
				break;
			}
			}
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
		{
			instrument->name = m_szNames[smp];
			instrument->midiPWD = 1;  // E5xx: шаг 1/128 полутона
		}
	}

	for(uint8 voice = 0; voice < VgmChannels; voice++)
	{
		if(moduleChannel[voice] < 0)
			continue;
		const CHANNELINDEX chn = static_cast<CHANNELINDEX>(moduleChannel[voice]);
		ChnSettings[chn].szName = (voice < FMChannels) ? "YM2612 " + std::to_string(voice + 1)
			: (voice >= YMZBase) ? "YMZ280B " + std::to_string(voice - YMZBase + 1)
			: (voice >= OPMBase) ? "YM2151 " + std::to_string(voice - OPMBase + 1)
			: (voice == OPMBase - 1) ? "PSG Noise" : "PSG " + std::to_string(voice - FMChannels + 1);
#ifdef MODPLUG_TRACKER
		ChnSettings[chn].color = (voice < FMChannels) ? 0x5A6AE0u : (voice >= YMZBase) ? 0xB070C0u : (voice >= OPMBase) ? 0xC0806Au : 0x7AC86Au;
#endif
	}

	if(!(loadFlags & loadPatternData))
		return true;

	// Уровни модуляторов: оставить только отличия от того, что держит канал.
	// Движок (как Furnace) держит уровни, пока играет тот же инструмент, и
	// сбрасывает их к тембру при смене инструмента.
	{
		std::array<int, VgmChannels> channelPatch;
		channelPatch.fill(-1);
		std::array<std::array<int8, 4>, VgmChannels> held{};
		for(auto &event : events)
		{
			if(event.channel >= OPMBase + OPMChannels || (event.channel >= FMChannels && event.channel < OPMBase))
			{
				event.level.fill(-1);
				continue;
			}
			auto &levels = held[event.channel];
			// Удар ЦАПа на шестом канале — другой инструмент: уровни сброшены.
			if(event.patch >= 0 && static_cast<size_t>(event.patch) >= patches.size())
				channelPatch[event.channel] = -1;
			if(event.patch >= 0 && static_cast<size_t>(event.patch) < patches.size() && ModCommand::IsNote(event.note))
			{
				if(event.patch != channelPatch[event.channel])
				{
					channelPatch[event.channel] = event.patch;
					for(uint8 op = 0; op < 4; op++)
						levels[op] = static_cast<int8>(patches[event.patch].patch[1 + op * 7 + 1] & 0x7F);
				}
			}
			if(channelPatch[event.channel] < 0)
			{
				event.level.fill(-1);
				continue;
			}
			for(uint8 op = 0; op < 4; op++)
			{
				if(event.level[op] < 0)
					continue;
				if(event.level[op] == levels[op])
					event.level[op] = -1;
				else
					levels[op] = event.level[op];
			}
		}
	}

	// Сколько кадров в строке. Кадр на строку точен, но паттерны выходят в 2–3
	// раза длиннее исходных. Компактный режим (по умолчанию) ищет шаг строки
	// по моментам нажатий: самый крупный шаг, на сетку которого ложится
	// большинство нот; остальные ноты получают задержку SDx, так что время
	// нот остаётся точным. Сглаживаются только изменения внутри строки.
#ifdef MODPLUG_TRACKER
	const bool compact = TrackerSettings::Instance().vgmCompactRows;
#else
	const bool compact = (std::getenv("OPENFMT_VGM_COMPACT") != nullptr);  // По умолчанию точный режим, как в трекере
#endif
	uint32 framesPerRow = 1, phase = 0;
	if(compact)
	{
		std::vector<uint32> noteFrames;
		for(const auto &event : events)
		{
			if(ModCommand::IsNote(event.note) && !event.legato)
				noteFrames.push_back(event.sample / samplesPerRow);
		}
		const auto bestPhase = [&](uint32 step) -> std::pair<double, uint32>
		{
			std::vector<uint32> count(step, 0);
			for(uint32 frame : noteFrames)
				count[frame % step]++;
			const auto best = std::max_element(count.begin(), count.end());
			return {noteFrames.empty() ? 0.0 : static_cast<double>(*best) / noteFrames.size(), static_cast<uint32>(std::distance(count.begin(), best))};
		};
		// Крупный шаг берём, только если он ловит почти столько же нот, сколько
		// мелкий: шаг 6 при 85% попаданий хуже шага 3 при 98% — внутри строки
		// громкость и высота сглаживаются, и чем строка длиннее, тем грубее.
		framesPerRow = 2;
		phase = bestPhase(2).second;
		double bestRatio = bestPhase(2).first;
		for(uint32 step = 3; step <= 8; step++)
		{
			const auto [ratio, stepPhase] = bestPhase(step);
			if(ratio >= 0.9 && ratio >= bestRatio - 0.03)
			{
				framesPerRow = step;
				phase = stepPhase;
				bestRatio = std::max(bestRatio, ratio);
			}
		}
		Order().SetDefaultSpeed(framesPerRow);
	}
	// Сетка начинается с кадра phase: первая строка — это он. Сдвигать весь
	// трек позже нельзя — всё звучало бы на кадр-два позже лога; поэтому
	// раньше сетки только самые первые события, и они встают на её начало.
	const auto frameOf = [&](const VgmEvent &event) { return std::max(event.sample / samplesPerRow, phase) - phase; };

	const uint32 lastRow = frameOf(events.back()) / framesPerRow;
	const ROWINDEX totalRows = static_cast<ROWINDEX>(std::min<uint32>(lastRow + 2, MAX_PATTERN_ROWS * 1000u));
	// Кадр на строку — это 60 строк в секунду; паттерн по 128 строк держит
	// около двух секунд. В компактном режиме строки длиннее, хватает 64.
	const ROWINDEX RowsPerPattern = compact ? 64 : 128;
	const PATTERNINDEX patternCount = static_cast<PATTERNINDEX>((totalRows + RowsPerPattern - 1) / RowsPerPattern);
	Order().resize(patternCount);
	for(PATTERNINDEX pat = 0; pat < patternCount; pat++)
	{
		// Последний паттерн — ровно до конца трека, без хвоста тишины.
		const ROWINDEX rows = (pat + 1 == patternCount && totalRows % RowsPerPattern) ? std::max<ROWINDEX>(totalRows % RowsPerPattern, 2) : RowsPerPattern;
		const PATTERNINDEX index = Patterns.InsertAny(rows);
		if(index == PATTERNINDEX_INVALID)
			return false;
		Order()[pat] = index;
	}
	const auto cellAt = [&](uint32 row, uint8 voice) -> ModCommand *
	{
		const PATTERNINDEX pat = static_cast<PATTERNINDEX>(row / RowsPerPattern);
		if(pat >= patternCount || moduleChannel[voice] < 0)
			return nullptr;
		return Patterns[Order()[pat]].GetpModCommand(static_cast<ROWINDEX>(row % RowsPerPattern), static_cast<CHANNELINDEX>(moduleChannel[voice]));
	};
	const auto instrumentOf = [&](const VgmEvent &event) -> int
	{
		if(event.patch >= 0)
			return event.patch + 1;
		if(event.channel >= FMChannels && event.channel < OPMBase && !event.legato && ModCommand::IsNote(event.note))
			return firstPSG + event.channel - FMChannels + 1;
		return 0;
	};

	// Эффект в ячейку: тот же эффект заменяем, иначе берём свободную колонку.
	const auto setEffect = [](ModCommand &cell, EffectCommand command, uint8 param)
	{
		uint8 slot = 0;
		while(slot < ModCommand::Effects() && cell.GetEffect(slot).first != command)
			slot++;
		if(slot == ModCommand::Effects())
			slot = cell.FreeEffectSlot();
		if(slot < ModCommand::Effects())
			cell.SetEffect(slot, command, param);
	};
	// Команда в первую колонку (портаменто и задержку движок ищет только
	// там): что там было, переезжает в свободную.
	const auto setFirstEffect = [&](ModCommand &cell, EffectCommand command, uint8 param)
	{
		const auto [oldCommand, oldParam] = cell.GetEffect(0);
		cell.SetEffect(0, command, param);
		if(oldCommand != CMD_NONE && oldCommand != command)
			setEffect(cell, oldCommand, oldParam);
	};

	if(!compact)
	{
		for(const auto &event : events)
		{
			ModCommand *found = cellAt(frameOf(event), event.channel);
			if(!found)
				continue;
			ModCommand &cell = *found;
			if(event.note != NOTE_NONE)
			{
				cell.note = event.note;
				if(const int instr = instrumentOf(event); instr > 0)
					cell.instr = static_cast<ModCommand::INSTR>(instr);
				// В одной строке-кадре легато может смениться настоящим ударом:
				// тогда портаменто от него убираем, иначе удар потеряется.
				for(uint8 slot = 0; slot < ModCommand::Effects(); slot++)
				{
					if(cell.GetEffect(slot).first == CMD_TONEPORTAMENTO)
						cell.SetEffect(slot, CMD_NONE, 0);
				}
				if(event.legato && !cell.instr)
					setFirstEffect(cell, CMD_TONEPORTAMENTO, 0xFF);
			}
			if(event.fine >= 0)
				setEffect(cell, CMD_FINETUNE, static_cast<uint8>(event.fine));
			for(uint8 op = 0; op < 4; op++)
			{
				if(event.level[op] >= 0)
					setEffect(cell, static_cast<EffectCommand>(CMD_FM_LEVEL1 + op), static_cast<uint8>(event.level[op]));
			}
			if(event.volume != 255)
			{
				cell.volcmd = VOLCMD_VOLUME;
				cell.vol = std::min<uint8>(event.volume, 64);
			}
			if(event.pan >= 0)
				setEffect(cell, CMD_PANNING8, static_cast<uint8>(event.pan));
		}
	} else
	{
		// Что набралось у канала за строку. Ячейка вмещает одну ноту, одну
		// громкость и по одному эффекту каждого вида, поэтому то, что пришло
		// после ноты (второй удар, отпускание, громкость, подстройка), уезжает
		// на первый тик следующей строки.
		struct RowState
		{
			bool used = false;
			uint32 row = 0;
			ModCommand::NOTE note = NOTE_NONE;
			uint32 noteTick = 0;
			int instr = 0;
			bool legato = false;
			int volume = -1, fine = -1, pan = -1;
			bool fineSmooth = false;
			// Громкость по ходу строки (тик, значение): в ячейку идёт начало и
			// плавный спад или подъём Dxy к концу строки.
			std::vector<std::pair<uint32, int>> volumes;
			std::array<int8, 4> level{-1, -1, -1, -1};
			std::vector<VgmEvent> carry;  // что переезжает в следующую строку
		};
		std::array<RowState, VgmChannels> rows;
		std::array<int, VgmChannels> currentVolume;  // что держит канал, -1 — неизвестно
		currentVolume.fill(-1);
		const auto flush = [&](uint8 voice)
		{
			RowState &state = rows[voice];
			if(!state.used)
				return;
			if(ModCommand *found = cellAt(state.row, voice); found != nullptr)
			{
				ModCommand &cell = *found;
				if(state.note != NOTE_NONE)
				{
					cell.note = state.note;
					if(state.instr > 0)
						cell.instr = static_cast<ModCommand::INSTR>(state.instr);
					if(state.legato && !state.instr)
						setFirstEffect(cell, CMD_TONEPORTAMENTO, 0xFF);
					else if(state.noteTick > 0)
						setFirstEffect(cell, CMD_S3MCMDEX, static_cast<uint8>(0xD0 | std::min<uint32>(state.noteTick, 15)));
				}
				// Громкость строки. В ячейку влезает одно значение, а огибающие
				// меняют её каждый кадр. Dxy не годится: в режиме слайдов Furnace
				// он тянется через строки. Поэтому берём значение из середины
				// строки — для затухания оно ближе всего к среднему.
				const bool noteRow = ModCommand::IsNote(state.note) && !state.legato;
				if(noteRow)
				{
					currentVolume[voice] = state.volume;
					if(state.volume >= 0)
					{
						cell.volcmd = VOLCMD_VOLUME;
						cell.vol = static_cast<ModCommand::VOL>(std::min(state.volume, 64));
					}
				} else if(!state.volumes.empty())
				{
					const uint32 middle = (framesPerRow - 1) / 2;
					int value = -1;
					for(const auto &[tick, volume] : state.volumes)
					{
						if(tick <= middle || value < 0)
							value = volume;
					}
					if(value >= 0 && value != currentVolume[voice])
					{
						cell.volcmd = VOLCMD_VOLUME;
						cell.vol = static_cast<ModCommand::VOL>(std::min(value, 64));
						currentVolume[voice] = value;
					}
				}
				// Что громкость сделала после ноты в той же строке — в начало
				// следующей строки (если та не задаст свою).
				if(noteRow && !state.volumes.empty())
				{
					VgmEvent late{};
					late.channel = voice;
					late.volume = static_cast<uint8>(state.volumes.back().second);
					state.carry.insert(state.carry.begin(), late);
				}
				if(state.fine >= 0)
					setEffect(cell, state.fineSmooth ? CMD_FINETUNE_SMOOTH : CMD_FINETUNE, static_cast<uint8>(state.fine));
				if(state.pan >= 0)
					setEffect(cell, CMD_PANNING8, static_cast<uint8>(state.pan));
				for(uint8 op = 0; op < 4; op++)
				{
					if(state.level[op] >= 0)
						setEffect(cell, static_cast<EffectCommand>(CMD_FM_LEVEL1 + op), static_cast<uint8>(state.level[op]));
				}
			}
			std::vector<VgmEvent> carry = std::move(state.carry);
			const uint32 nextRow = state.row + 1;
			state = RowState{};
			if(!carry.empty())
			{
				state.used = true;
				state.row = nextRow;
				for(auto &event : carry)
				{
					// Переехавшее встаёт на начало строки; второй раз уже не
					// переезжает, а уступает тому, что придёт в эту строку.
					const bool realNote = ModCommand::IsNote(event.note) && !event.legato;
					if(event.note != NOTE_NONE)
					{
						state.note = event.note;
						state.noteTick = 0;
						state.instr = instrumentOf(event);
						state.legato = event.legato;
					}
					if(event.volume != 255)
					{
						if(realNote)
							state.volume = event.volume;
						else
							state.volumes.emplace_back(0, event.volume);
					}
					if(event.fine >= 0)
					{
						state.fine = event.fine;
						state.fineSmooth = !realNote && event.note == NOTE_NONE;
					}
					if(event.pan >= 0)
						state.pan = event.pan;
					for(uint8 op = 0; op < 4; op++)
					{
						if(event.level[op] >= 0)
							state.level[op] = event.level[op];
					}
				}
			}
		};
		for(const auto &event : events)
		{
			if(moduleChannel[event.channel] < 0)
				continue;
			const uint32 frame = frameOf(event);
			const uint32 row = frame / framesPerRow;
			const uint32 tick = frame % framesPerRow;
			RowState &state = rows[event.channel];
			if(state.used && state.row != row)
			{
				flush(event.channel);
				// Переехавшее из прошлой строки могло лечь не в эту строку, а
				// раньше неё: тогда его надо записать отдельно.
				if(state.used && state.row != row)
					flush(event.channel);
			}
			if(!state.used)
			{
				state.used = true;
				state.row = row;
			}
			const bool hasRealNote = ModCommand::IsNote(state.note) && !state.legato;
			const bool realNote = ModCommand::IsNote(event.note) && !event.legato;
			// Нота строки уже есть, а пришло событие позже неё: в ячейку оно не
			// поместится, переезжает в следующую строку целиком.
			// Громкость после ноты в той же строке не переезжает, а становится
			// спадом Dxy — если нота на первом тике (с задержкой Dxy не годится).
			if(hasRealNote && tick > state.noteTick && event.note == NOTE_NONE && event.fine < 0 && event.pan < 0
				&& event.volume != 255 && state.noteTick == 0)
			{
				state.volumes.emplace_back(tick, event.volume);
				continue;
			}
			const bool hasLevel = std::any_of(event.level.begin(), event.level.end(), [](int8 l) { return l >= 0; });
			if(hasRealNote && tick > state.noteTick && (event.note != NOTE_NONE || event.volume != 255 || event.fine >= 0 || hasLevel))
			{
				state.carry.push_back(event);
				continue;
			}
			if(event.note != NOTE_NONE)
			{
				state.note = event.note;
				state.noteTick = (realNote || event.note == NOTE_KEYOFF || event.note == NOTE_NOTECUT) ? tick : 0;
				state.instr = instrumentOf(event);
				state.legato = event.legato;
				if(realNote)
					state.volumes.clear();  // громкости до удара относились к прошлой ноте
			}
			if(event.volume != 255)
			{
				if(realNote)
					state.volume = event.volume;
				else
					state.volumes.emplace_back(tick, event.volume);
			}
			if(event.fine >= 0)
			{
				state.fine = event.fine;
				// Подстройка без ноты в строке — это вибрато или слайд: доводим
				// её плавно к концу строки, а не прыжком на первом тике.
				state.fineSmooth = !realNote && !hasRealNote;
			}
			if(event.pan >= 0)
				state.pan = event.pan;
			for(uint8 op = 0; op < 4; op++)
			{
				if(event.level[op] >= 0)
					state.level[op] = event.level[op];
			}
		}
		for(uint8 voice = 0; voice < VgmChannels; voice++)
		{
			flush(voice);
			flush(voice);  // и то, что переехало
		}
	}

	m_modFormat.formatName = UL_("VGM (register log)");
	m_modFormat.type = UL_("vgm");
	m_modFormat.madeWithTracker = UL_("Mega Drive sound driver");
	m_modFormat.charset = mpt::Charset::UTF8;
#ifdef MODPLUG_TRACKER
	AddToLog(LogWarning, MPT_UFORMAT("VGM import is approximate: {} voices and {} FM patches were reconstructed from register writes. Rows are single frames.")(events.size(), patches.size()));
#endif
	return true;
}

OPENMPT_NAMESPACE_END
