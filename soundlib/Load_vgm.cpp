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

#include <functional>
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
// Прочие PCM-чипы: RF5C164 (Mega CD) — восемь голосов, PWM (32X) — левый и
// правый, OKIM6295 — до двух чипов по четыре голоса.
constexpr uint8 RFChannels = 8, PWMChannels = 2, OKIChannels = 8;
constexpr uint8 RFBase = YMZBase + YMZChannels;
constexpr uint8 PWMBase = RFBase + RFChannels;
constexpr uint8 OKIBase = PWMBase + PWMChannels;
constexpr uint8 VgmChannels = OKIBase + OKIChannels;
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
	uint32 rfClock = 0, pwmClock = 0, okiClock = 0;
	bool okiPin7 = false;
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
	header.rfClock = (0x6C + 4 <= headerEnd) ? (ReadLE32(data, 0x6C) & 0x3FFFFFFF) : 0;
	header.pwmClock = (0x70 + 4 <= headerEnd) ? (ReadLE32(data, 0x70) & 0x3FFFFFFF) : 0;
	header.okiClock = (0x98 + 4 <= headerEnd) ? (ReadLE32(data, 0x98) & 0x3FFFFFFF) : 0;
	header.okiPin7 = (0x98 + 4 <= headerEnd) && (ReadLE32(data, 0x98) & 0x80000000u);
	for(uint16 offset : ForeignClockOffset)
	{
		if(offset == 0x30 || offset == 0x68 || offset == 0x98)
			continue;  // YM2151, YMZ280B и OKIM6295 у нас есть
		if(offset + 4u <= headerEnd && (ReadLE32(data, offset) & 0x3FFFFFFF))
			header.foreignChip = true;
	}
	return header.psgClock || header.opn2Clock || header.opmClock || header.rfClock || header.pwmClock || header.okiClock;
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
	// Режим шума в понятиях движка (бит 0 — белый, бит 1 — от третьего тона),
	// -1 — не шум.
	int8 noiseMode = -1;
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
	if(header.foreignChip || (!header.opn2Clock && !header.opmClock && !header.psgClock && !header.rfClock && !header.pwmClock && !header.okiClock))
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
	int8 psgSoundingMode = -1;  // режим, в котором звучит шум
	std::array<bool, 4> noiseModesUsed{};
	std::array<uint8, PSGChannels> psgLastVolume{};
	std::array<int, PSGChannels> psgPan{128, 128, 128, 128};
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
	struct Hit { uint32 start = 0, bytes = 0, first = 0, last = 0, writeStart = 0, writeCount = 0; };
	// Сами записанные значения удара, по порядку. Звук снимается по времени
	// лога, и два одинаковых удара из-за дрожания записей дают чуть разные
	// байты, а последовательность записей у одного барабана одна и та же.
	std::vector<uint8> dacWrites;
	Hit hit;
	bool hitOpen = false;

	std::vector<Hit> hits;
	const auto flushHit = [&]()
	{
		if(hitOpen && hit.bytes > 64)
		{
			hit.writeCount = static_cast<uint32>(dacWrites.size()) - hit.writeStart;
			hits.push_back(hit);
		}
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
				hit = Hit{static_cast<uint32>(raw.size()), 0, when, when, static_cast<uint32>(dacWrites.size()), 0};
				hitOpen = true;
			}
			dacWrites.push_back(value);
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
	// Потоки других чипов: 0x90 задаёт чип и регистр, 0x91 — банк данных и шаг.
	std::array<uint8, 256> streamChip;
	streamChip.fill(0x02);  // По умолчанию — ЦАП YM2612, как пишет Furnace
	std::array<uint8, 256> streamRegister{}, streamBank{}, streamStepBase{};
	std::array<uint8, 256> streamStepSize;
	streamStepSize.fill(1);
	// Данные потоков по типу блока (0x00–0x3F), сжатые блоки распакованы.
	std::array<std::vector<uint8>, 0x40> streamData;
	std::array<std::vector<DataBlock>, 0x40> streamBlocks;
	// Таблицы распаковки (блок 0x7F) по виду сжатия и подтипу.
	std::map<uint16, std::vector<uint16>> unpackTables;
	const auto unpackBlock = [&](const uint8 *src, uint32 size, std::vector<uint8> &out) -> bool
	{
		if(size < 10)
			return false;
		const uint8 method = src[0];
		const uint32 outSize = src[1] | (src[2] << 8) | (src[3] << 16) | (static_cast<uint32>(src[4]) << 24);
		const uint8 bitsOut = src[5], bitsIn = src[6], subType = src[7];
		const uint16 addValue = static_cast<uint16>(src[8] | (src[9] << 8));
		if(!bitsIn || bitsIn > 16 || !bitsOut || bitsOut > 16 || outSize > (64u << 20))
			return false;
		const std::vector<uint16> *table = nullptr;
		if(method == 1 || subType == 2)
		{
			const auto found = unpackTables.find(static_cast<uint16>((method << 8) | subType));
			if(found == unpackTables.end())
				return false;
			table = &found->second;
		}
		const uint8 bytesOut = (bitsOut + 7) / 8;
		const uint8 *in = src + 10;
		const uint64 inBits = static_cast<uint64>(size - 10) * 8;
		uint64 bitPos = 0;
		uint32 state = addValue;  // DPCM: начальное значение
		const size_t base = out.size();
		out.reserve(base + outSize);
		while(out.size() - base + bytesOut <= outSize && bitPos + bitsIn <= inBits)
		{
			uint32 value = 0;
			for(uint8 b = 0; b < bitsIn; b++, bitPos++)
				value = (value << 1) | ((in[bitPos >> 3] >> (7 - (bitPos & 7))) & 1);
			if(method == 1)
			{
				if(value >= table->size())
					return false;
				state = (state + (*table)[value]) & ((1u << bitsOut) - 1);
				value = state;
			} else if(subType == 0)
			{
				value += addValue;
			} else if(subType == 1)
			{
				value = (value << (bitsOut - bitsIn)) + addValue;
			} else if(subType == 2)
			{
				if(value >= table->size())
					return false;
				value = (*table)[value];
			}
			for(uint8 b = 0; b < bytesOut; b++)
				out.push_back(static_cast<uint8>(value >> (8 * b)));
		}
		return true;
	};
	// PWM (32X): регистры и куски потоков. Кусок — непрерывное проигрывание:
	// перезапуск потока с того же места, где он и так был, куском не считается.
	uint16 pwmCycle = 0;
	struct PwmSegment { uint8 voice = 0, bank = 0, stepSize = 1, stepBase = 0; uint32 startSample = 0, endSample = uint32_max, byteOffset = 0, frequency = 0; uint16 cycle = 0; };
	std::vector<PwmSegment> pwmSegments;
	std::array<int, 256> streamSegment;
	streamSegment.fill(-1);
	const auto closeSegment = [&](uint8 stream, uint32 when)
	{
		if(streamSegment[stream] >= 0)
		{
			PwmSegment &seg = pwmSegments[streamSegment[stream]];
			seg.endSample = std::min(seg.endSample, when);
			streamSegment[stream] = -1;
		}
	};
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
	// Прочие PCM-чипы (OKIM6295, RF5C164, PWM) сводятся к одному: голос
	// играет звук — кусок PCM, который чип реально прочитал, — с какой-то
	// частотой. Одинаковые звуки (по содержимому) становятся одним сэмплом,
	// частота — нотой и подстройкой относительно обычной для этого звука.
	struct PcmSound { std::vector<int16> data; double rate = 0.0; uint32 loopStart = uint32_max; std::string name; };
	std::vector<PcmSound> pcmSounds;
	std::multimap<uint64, size_t> pcmSoundByHash;
	// Звуки, где нота всегда обрывается в конце (RF5C164): короткий звук,
	// который совпадает с началом длинного, — тот же звук. Ключ — первые 64
	// отсчёта.
	std::multimap<uint64, size_t> pcmSoundByHead;
	const auto headHash = [](const std::vector<int16> &d)
	{
		uint64 hash = 1469598103934665603ull;
		for(size_t i = 0; i < std::min<size_t>(d.size(), 64); i++)
			hash = (hash ^ static_cast<uint16>(d[i])) * 1099511628211ull;
		return hash;
	};
	const auto addPrefixSound = [&](std::vector<int16> &&pcmData, double rate, const char *prefix) -> int
	{
		if(pcmData.size() < 64)
			return -1;
		const uint64 head = headHash(pcmData);
		for(auto [it, end] = pcmSoundByHead.equal_range(head); it != end; ++it)
		{
			PcmSound &known = pcmSounds[it->second];
			const size_t common = std::min(known.data.size(), pcmData.size());
			if(!std::equal(pcmData.begin(), pcmData.begin() + common, known.data.begin()))
				continue;
			if(pcmData.size() > known.data.size())
				known.data = std::move(pcmData);
			return static_cast<int>(it->second);
		}
		PcmSound sound;
		sound.data = std::move(pcmData);
		sound.rate = rate;
		sound.name = prefix;
		pcmSounds.push_back(std::move(sound));
		pcmSoundByHead.emplace(head, pcmSounds.size() - 1);
		return static_cast<int>(pcmSounds.size() - 1);
	};
	const auto addPcmSound = [&](std::vector<int16> &&pcmData, double rate, uint32 loopStart, const char *prefix) -> int
	{
		if(pcmData.empty())
			return -1;
		uint64 hash = 1469598103934665603ull ^ pcmData.size() ^ (static_cast<uint64>(loopStart) << 32);
		for(int16 v : pcmData)
			hash = (hash ^ static_cast<uint16>(v)) * 1099511628211ull;
		for(auto [it, end] = pcmSoundByHash.equal_range(hash); it != end; ++it)
		{
			const PcmSound &known = pcmSounds[it->second];
			if(known.loopStart == loopStart && known.data == pcmData)
				return static_cast<int>(it->second);
		}
		PcmSound sound;
		sound.data = std::move(pcmData);
		sound.rate = rate;
		sound.loopStart = loopStart;
		sound.name = prefix;
		pcmSounds.push_back(std::move(sound));
		pcmSoundByHash.emplace(hash, pcmSounds.size() - 1);
		return static_cast<int>(pcmSounds.size() - 1);
	};
	enum class PcmWhat : uint8 { On, Pitch, Off, Volume, Pan };
	struct PcmEvent { uint32 sample = 0; uint8 voice = 0; PcmWhat what = PcmWhat::On; int sound = -1; double rate = 0.0; int value = 0; };
	std::vector<PcmEvent> pcmEvents;
	std::array<bool, VgmChannels> pcmVoiceUsed{};
	// OKIM6295: образ ROM (блок 0x8B), банки NMK112 и запуск фраз.
	struct OkiChip
	{
		std::vector<uint8> rom;
		std::array<uint8, 4> nmkBank{};
		uint8 nmkFlags = 0, bank = 0;
		int pendingPhrase = -1;
		bool pin7 = false;
	};
	std::array<OkiChip, 2> oki;
	for(auto &chip : oki)
		chip.pin7 = header.okiPin7;
	// RF5C164 (Mega CD): 64 КБ памяти волн, восемь голосов. Голос шагает по
	// памяти с дробным шагом, байт 0xFF — переход на точку петли. Драйвер
	// часто подкачивает звук в память на ходу (кольцевой буфер), поэтому мы
	// повторяем чтение чипа по времени и собираем для каждого нажатия байты,
	// которые голос реально прочитал, — по одному на адрес. Это и есть звук
	// ноты на родной скорости, а шаг — её частота.
	struct RfVoice
	{
		uint8 env = 0xFF, pan = 0xFF, start = 0;
		uint16 step = 0, loop = 0;
		bool enabled = false;
		uint16 position = 0;
		uint32 fraction = 0;
		std::vector<int16> heard;
		std::vector<uint8> heardRaw;
		// Снимок сэмпла в момент нажатия: от начала до 0xFF и точка петли.
		std::vector<uint8> snapshot;
		uint32 snapshotLoop = uint32_max;
		int onEvent = -1;
		double noteRate = 0.0;
		int volume = -1, panning = -1;
	};
	std::array<RfVoice, RFChannels> rf;
	std::map<uint32, int> rfSoundAt;  // место в банке сэмплов -> звук
	std::vector<uint8> rfRam(65536, 0);
	uint8 rfBank = 0, rfSelected = 0;
	bool rfRunning = false;
	double rfTicks = 0.0;
	const double rfRate = header.rfClock / 384.0;
	const auto rfByte = [](uint8 b) { return static_cast<int16>(((b & 0x80) ? (b & 0x7F) : -(b & 0x7F)) * 256); };
	const auto rfAdvance = [&](uint32 when)
	{
		if(!header.rfClock)
			return;
		const double target = when * rfRate / VgmRate;
		const uint64 ticks = static_cast<uint64>(std::max(0.0, std::floor(target - rfTicks)));
		rfTicks += static_cast<double>(ticks);
		if(!rfRunning)
			return;
		for(auto &v : rf)
		{
			if(!v.enabled || v.onEvent < 0 || !v.step)
				continue;
			for(uint64 t = 0; t < ticks; t++)
			{
				v.fraction += v.step;
				while(v.fraction >= 2048)
				{
					v.fraction -= 2048;
					v.position++;
					uint8 b = rfRam[v.position];
					if(b == 0xFF)
					{
						v.position = v.loop;
						b = rfRam[v.position];
					}
					if(v.heard.size() < MAX_SAMPLE_LENGTH)
					{
						v.heard.push_back(rfByte(b == 0xFF ? 0 : b));
						v.heardRaw.push_back(b);
					}
				}
			}
		}
	};
	const auto rfRateOf = [&](const RfVoice &v) { return rfRate * v.step / 2048.0; };
	const auto rfLevel = [&](const RfVoice &v, int &volume, int &panning)
	{
		const int left = v.pan & 15, right = v.pan >> 4;
		volume = std::clamp((v.env * std::max(left, right) * 64 + 255 * 15 / 2) / (255 * 15), 0, 64);
		panning = (left + right) ? (right * 255 + (left + right) / 2) / (left + right) : 128;
	};
	// Нота RF5C164 разбирается после чтения всего файла: только тогда
	// известно, какие страницы памяти драйвер переписывал на ходу.
	struct RfNote
	{
		std::vector<int16> heard;
		std::vector<uint8> heardRaw, snapshot;
		uint32 snapshotLoop = uint32_max;
		uint8 start = 0;
		double noteRate = 0.0;
		int onEvent = -1;
	};
	std::vector<RfNote> rfNotes;
	std::array<uint32, 256> rfPageWrites{};  // сколько раз писали в страницу 256 байт
	const auto rfTouch = [&](uint32 from, uint32 count)
	{
		if(!count)
			return;
		const uint32 last = std::min<uint32>(from + count - 1, 0xFFFF);
		for(uint32 page = (from & 0xFFFF) >> 8; page <= (last >> 8); page++)
			rfPageWrites[page]++;
	};
	// Страница, в которую писали много раз, — буфер подкачки, а не сэмпл.
	const auto rfStreamed = [&](uint8 start, size_t length)
	{
		const uint32 first = static_cast<uint32>(start), last = std::min<uint32>((static_cast<uint32>(start) * 256 + static_cast<uint32>(length)) >> 8, 255);
		for(uint32 page = first; page <= last; page++)
		{
			if(rfPageWrites[page] > 8)
				return true;
		}
		return false;
	};
	const auto rfResolve = [&](RfNote &n, bool loopsOnly) -> bool
	{
		int sound = -1;
		// Драйвер подкачивает в память куски своего банка сэмплов (блок
		// данных типа 2) подряд. Если услышанное нашлось в банке, звук — это
		// место в банке: одно и то же место — один инструмент, даже если ноты
		// были разной длины или начинались в разных местах памяти.
		// Память под нотой не менялась: услышанное — это снимок, а дальше
		// повторы его петли. Тогда инструмент — сам сэмпл с петлёй, как в
		// памяти чипа, и звучит как надо при любой длине ноты.
		if(!n.snapshot.empty() && n.snapshot.size() >= 16 && !rfStreamed(n.start, n.snapshot.size()))
		{
			const size_t length = n.snapshot.size();
			bool same = true;
			for(size_t i = 0; i < n.heardRaw.size() && same; i++)
			{
				size_t at = i;
				if(at >= length)
				{
					if(n.snapshotLoop == uint32_max)
					{
						same = false;
						break;
					}
					at = n.snapshotLoop + (at - length) % (length - n.snapshotLoop);
				}
				same = (n.heardRaw[i] == n.snapshot[at]);
			}
			if(same)
			{
				std::vector<int16> sampleData(length);
				for(size_t i = 0; i < length; i++)
					sampleData[i] = rfByte(n.snapshot[i]);
				sound = addPcmSound(std::move(sampleData), n.noteRate, n.snapshotLoop, "RF");
			}
		}
		const auto &bank = streamData[0x02];
		// Петля, которую делает драйвер: он подкачивает из банка сэмпл целиком,
		// а потом раз за разом его петлевой кусок. Раскладываем услышанное на
		// куски банка; вид «[X..Y), затем [L..Y), [L..Y)…» — это сэмпл X..Y с
		// петлёй от L, и инструмент получает настоящую петлю.
		if(sound < 0 && n.heardRaw.size() >= 64 && !bank.empty())
		{
			std::vector<std::pair<uint32, uint32>> pieces;  // начало в банке, длина
			size_t i = 0;
			bool ok = true;
			while(i < n.heardRaw.size() && ok)
			{
				const size_t left = n.heardRaw.size() - i;
				// Продолжение прошлого куска или новое место: сначала пробуем
				// петлю (начало второго куска), потом поиск по банку.
				uint32 at = uint32_max;
				const auto runFrom = [&](uint32 candidate)
				{
					size_t len = 0;
					while(len < left && candidate + len < bank.size() && bank[candidate + len] == n.heardRaw[i + len])
						len++;
					return len;
				};
				if(pieces.size() >= 2 && runFrom(pieces[1].first) >= std::min<size_t>(left, 16))
					at = pieces[1].first;
				if(at == uint32_max)
				{
					if(left < 16)
					{
						ok = false;
						break;
					}
					const std::boyer_moore_horspool_searcher searcher(n.heardRaw.begin() + i, n.heardRaw.begin() + i + 16);
					const auto found = std::search(bank.begin(), bank.end(), searcher);
					if(found == bank.end())
					{
						ok = false;
						break;
					}
					at = static_cast<uint32>(std::distance(bank.begin(), found));
				}
				const size_t len = runFrom(at);
				if(!len)
				{
					ok = false;
					break;
				}
				pieces.emplace_back(at, static_cast<uint32>(len));
				i += len;
				if(pieces.size() > 4096)
					ok = false;
			}
			// Петля — первый прыжок на место банка, которое уже звучало. Сэмпл —
			// всё до прыжка, петля — от того места; остаток ноты обязан по ней
			// циклиться, иначе это не петля.
			if(ok && pieces.size() >= 2)
			{
				size_t offset = pieces[0].second;
				for(size_t k = 1; k < pieces.size() && sound < 0; k++)
				{
					const uint32 target = pieces[k].first;
					size_t loopAt = SIZE_MAX, earlier = 0;
					for(size_t m = 0; m < k; m++)
					{
						if(target >= pieces[m].first && target < pieces[m].first + pieces[m].second)
						{
							loopAt = earlier + (target - pieces[m].first);
							break;
						}
						earlier += pieces[m].second;
					}
					if(loopAt != SIZE_MAX)
					{
						const size_t loopEnd = offset, loopLength = loopEnd - loopAt;
						bool cycles = loopLength > 0;
						for(size_t t = loopEnd; t < n.heardRaw.size() && cycles; t++)
							cycles = n.heardRaw[t] == n.heardRaw[loopAt + (t - loopEnd) % loopLength];
						if(cycles)
						{
							std::vector<int16> sampleData(n.heard.begin(), n.heard.begin() + loopEnd);
							sound = addPcmSound(std::move(sampleData), n.noteRate, static_cast<uint32>(loopAt), "RF");
						}
						break;
					}
					offset += pieces[k].second;
				}
			}
		}
		if(loopsOnly)
		{
			if(sound >= 0)
				pcmEvents[n.onEvent].sound = sound;
			return sound >= 0;
		}
		// Нота не дошла до петли, но это начало уже известного зацикленного
		// звука: берём его, тогда и у короткой ноты инструмент с петлёй.
		for(size_t k = 0; k < pcmSounds.size() && sound < 0; k++)
		{
			const PcmSound &known = pcmSounds[k];
			if(known.loopStart != uint32_max && known.name == "RF" && n.heard.size() >= 64 && n.heard.size() <= known.data.size()
				&& std::equal(n.heard.begin(), n.heard.end(), known.data.begin()))
				sound = static_cast<int>(k);
		}
		if(sound < 0 && n.heardRaw.size() >= 32 && !bank.empty())
		{
			// Ищем по первому окну, где звук меняется: начало из одинаковых
			// байт (тишина) нашлось бы в первом же пустом месте банка.
			size_t window = 0;
			while(window + 32 <= n.heardRaw.size()
				&& std::all_of(n.heardRaw.begin() + window, n.heardRaw.begin() + window + 32, [&](uint8 x) { return x == n.heardRaw[window]; }))
				window += 32;
			if(window + 32 <= n.heardRaw.size())
			{
				const std::boyer_moore_horspool_searcher searcher(n.heardRaw.begin() + window, n.heardRaw.begin() + window + 32);
				auto from = bank.begin();
				while(sound < 0)
				{
					const auto found = std::search(from, bank.end(), searcher);
					if(found == bank.end())
						break;
					from = found + 1;
					const size_t foundAt = static_cast<size_t>(std::distance(bank.begin(), found));
					if(foundAt < window)
						continue;
					const uint32 at = static_cast<uint32>(foundAt - window);
					// Склеиваем, только если вся нота совпала с банком байт в байт.
					if(at + n.heardRaw.size() > bank.size() || !std::equal(n.heardRaw.begin(), n.heardRaw.end(), bank.begin() + at))
						continue;
					if(const auto known = rfSoundAt.find(at); known != rfSoundAt.end())
					{
						sound = known->second;
						if(n.heard.size() > pcmSounds[sound].data.size())
							pcmSounds[sound].data = std::move(n.heard);
					} else
					{
						PcmSound made;
						made.data = std::move(n.heard);
						made.rate = n.noteRate;
						made.name = "RF";
						pcmSounds.push_back(std::move(made));
						sound = static_cast<int>(pcmSounds.size() - 1);
						rfSoundAt[at] = sound;
					}
				}
			}
		}
		if(sound < 0)
			sound = addPrefixSound(std::move(n.heard), n.noteRate, "RF");
		pcmEvents[n.onEvent].sound = sound;
		return true;
	};
	const auto rfFinish = [&](uint8 index, uint32 when)
	{
		RfVoice &v = rf[index];
		if(v.onEvent < 0)
			return;
		RfNote note;
		note.heard = std::move(v.heard);
		note.heardRaw = std::move(v.heardRaw);
		note.snapshot = std::move(v.snapshot);
		note.snapshotLoop = v.snapshotLoop;
		note.start = v.start;
		note.noteRate = v.noteRate;
		note.onEvent = v.onEvent;
		rfNotes.push_back(std::move(note));
		v.heard.clear();
		v.heardRaw.clear();
		v.snapshot.clear();
		pcmEvents.push_back({when, static_cast<uint8>(RFBase + index), PcmWhat::Off});
		v.onEvent = -1;
	};
	const auto rfKeyOn = [&](uint8 index, uint32 when)
	{
		RfVoice &v = rf[index];
		rfFinish(index, when);
		v.position = static_cast<uint16>(v.start << 8);
		v.fraction = 0;
		v.heard.clear();
		uint8 b = rfRam[v.position];
		if(b == 0xFF)
		{
			v.position = v.loop;
			b = rfRam[v.position];
		}
		v.heardRaw.clear();
		v.snapshot.clear();
		v.snapshotLoop = uint32_max;
		{
			const uint32 first = static_cast<uint32>(v.start) << 8;
			for(uint32 a = first; a < 0x10000 && rfRam[a] != 0xFF; a++)
				v.snapshot.push_back(rfRam[a]);
			if(v.loop >= first && v.loop < first + v.snapshot.size())
				v.snapshotLoop = v.loop - first;
		}
		v.heard.push_back(rfByte(b == 0xFF ? 0 : b));
		v.heardRaw.push_back(b);
		v.noteRate = rfRateOf(v);
		rfLevel(v, v.volume, v.panning);
		const uint8 voice = static_cast<uint8>(RFBase + index);
		pcmVoiceUsed[voice] = true;
		pcmEvents.push_back({when, voice, PcmWhat::Pan, -1, 0.0, v.panning});
		pcmEvents.push_back({when, voice, PcmWhat::On, -1, v.noteRate, v.volume});
		v.onEvent = static_cast<int>(pcmEvents.size() - 1);
	};
	const auto okiByte = [](const OkiChip &chip, uint8 chipIndex, uint32 address) -> uint8
	{
		address &= 0x3FFFF;
		uint32 physical;
		if(chip.nmkFlags & 0x80)
		{
			// NMK112: четыре окна по 64 КБ, у каждого свой банк. Если таблица фраз
			// постраничная, её четверти по 256 байт берутся из банков окон.
			if(address < 0x400 && (chip.nmkFlags & (1 << chipIndex)))
				physical = (static_cast<uint32>(chip.nmkBank[(address >> 8) & 3]) << 16) | address;
			else
				physical = (static_cast<uint32>(chip.nmkBank[(address >> 16) & 3]) << 16) | (address & 0xFFFF);
		} else
		{
			physical = (static_cast<uint32>(chip.bank) << 18) | address;
		}
		return physical < chip.rom.size() ? chip.rom[physical] : uint8(0);
	};
	// АДИКМ OKI (как у MAME): 49 шагов, 12-битный сигнал, старший полубайт первый.
	const auto okiDecode = [&](const OkiChip &chip, uint8 chipIndex, uint32 start, uint32 end) -> std::vector<int16>
	{
		static const auto diffTable = []()
		{
			std::array<int, 49 * 16> table{};
			for(int step = 0; step < 49; step++)
			{
				const int value = static_cast<int>(std::floor(16.0 * std::pow(11.0 / 10.0, step)));
				for(int nib = 0; nib < 16; nib++)
				{
					int diff = value / 8;
					if(nib & 1)
						diff += value / 4;
					if(nib & 2)
						diff += value / 2;
					if(nib & 4)
						diff += value;
					table[step * 16 + nib] = (nib & 8) ? -diff : diff;
				}
			}
			return table;
		}();
		static constexpr int IndexShift[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
		std::vector<int16> out;
		if(end < start)
			return out;
		const uint32 bytes = std::min<uint32>(end - start + 1, 0x40000);
		out.reserve(bytes * 2);
		int signal = -2, step = 0;
		for(uint32 n = 0; n < bytes * 2; n++)
		{
			const uint8 byte = okiByte(chip, chipIndex, start + n / 2);
			const int nibble = (n & 1) ? (byte & 15) : (byte >> 4);
			signal = std::clamp(signal + diffTable[step * 16 + nibble], -2048, 2047);
			step = std::clamp(step + IndexShift[nibble & 7], 0, 48);
			out.push_back(static_cast<int16>(signal * 16));
		}
		return out;
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
			} else if(note != NOTE_NONE && lane == 3)
			{
				// Шум. Режим у движка свой: бит 0 — белый, бит 1 — частота от
				// третьего тона. На фиксированной скорости (в играх Master System
				// так почти всегда) движок берёт скорость по ноте C, C# или D, и
				// третий тон тогда не трогает; иначе шум переписывал высоту
				// третьего голоса, и тот вдруг пищал высоко.
				const int8 engineMode = static_cast<int8>(((noiseMode & 4) ? 1 : 0) | (((noiseMode & 3) == 3) ? 2 : 0));
				const auto noiseNote = (engineMode & 2) ? note : static_cast<ModCommand::NOTE>(NOTE_MIDDLEC + 2 - (noiseMode & 3));
				// Новая нота — только когда сменился полутон или режим.
				if(noiseNote != psgSounding[lane] || engineMode != psgSoundingMode)
				{
					VgmEvent event{sample, channel, noiseNote, volume, -1};
					event.noiseMode = engineMode;
					events.push_back(event);
					psgSounding[lane] = noiseNote;
					psgSoundingMode = engineMode;
					noiseModesUsed[engineMode] = true;
				} else if(volume != psgLastVolume[lane])
				{
					events.push_back({sample, channel, NOTE_NONE, volume, -1});
				}
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
			if(blockType >= 0x01 && blockType <= 0x3F && pos + 6 + size <= data.size())
			{
				streamBlocks[blockType].push_back({static_cast<uint32>(streamData[blockType].size()), size});
				streamData[blockType].insert(streamData[blockType].end(), data.begin() + pos + 6, data.begin() + pos + 6 + size);
			} else if(blockType >= 0x41 && blockType <= 0x7E && pos + 6 + size <= data.size())
			{
				// Сжатый поток: распаковываем сразу.
				auto &target = streamData[blockType - 0x40];
				const uint32 before = static_cast<uint32>(target.size());
				if(unpackBlock(data.data() + pos + 6, size, target))
					streamBlocks[blockType - 0x40].push_back({before, static_cast<uint32>(target.size()) - before});
				else
					target.resize(before);
			} else if(blockType == 0x7F && size >= 6 && pos + 6 + size <= data.size())
			{
				// Таблица распаковки: вид, подтип, биты, число значений, значения.
				const uint8 *t = data.data() + pos + 6;
				const uint8 bytes = (t[2] + 7) / 8;
				const uint16 count = static_cast<uint16>(t[4] | (t[5] << 8));
				std::vector<uint16> values;
				for(uint32 i = 0; i < count && 6u + (i + 1) * bytes <= size; i++)
					values.push_back(static_cast<uint16>(bytes == 1 ? t[6 + i] : (t[6 + i * 2] | (t[7 + i * 2] << 8))));
				unpackTables[static_cast<uint16>((t[0] << 8) | t[1])] = std::move(values);
			} else if(blockType == 0x00 && pos + 6 + size <= data.size())
			{
				// Тип 0 — сэмплы для ЦАПа YM2612. Блоков бывает несколько, и
				// указатель потока считает их как один непрерывный кусок.
				dataBlocks.push_back({static_cast<uint32>(pcm.size()), size});
				pcm.insert(pcm.end(), data.begin() + pos + 6, data.begin() + pos + 6 + size);
			} else if(blockType == 0xC1 && size > 2 && pos + 6 + size <= data.size())
			{
				// Прямая запись в память RF5C164: адрес и байты.
				rfAdvance(sample);
				const uint32 to = (data[pos + 6] | (data[pos + 7] << 8)) | (static_cast<uint32>(rfBank) << 12);
				rfTouch(to, size - 2);
				for(uint32 i = 0; i + 2 < size && to + i < 0x10000; i++)
					rfRam[to + i] = data[pos + 8 + i];
			} else if(blockType == 0x8B && size > 8 && pos + 6 + size <= data.size())
			{
				// ROM OKIM6295: полный размер, адрес куска, сам кусок; старший бит
				// размера блока — второй чип.
				OkiChip &chip = oki[(ReadLE32(data, pos + 2) & 0x80000000u) ? 1 : 0];
				const uint32 offset = ReadLE32(data, pos + 10);
				const uint32 length = size - 8;
				if(offset < (16u << 20) && length <= (16u << 20) - offset)
				{
					if(chip.rom.size() < offset + length)
						chip.rom.resize(offset + length);
					std::copy(data.begin() + pos + 14, data.begin() + pos + 14 + length, chip.rom.begin() + offset);
				}
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
			const bool pwm = (streamChip[stream] & 0x7F) == 0x11;
			// Куда пишет поток PWM: регистр 3 — правый, 2 и 4 (моно) — левый голос.
			const auto pwmVoice = [&]() { return static_cast<uint8>(PWMBase + ((streamRegister[stream] == 3) ? 1 : 0)); };
			const auto startPwm = [&](uint32 byteOffset)
			{
				// Продолжение того же проигрывания — не новый кусок.
				if(streamSegment[stream] >= 0)
				{
					const PwmSegment &seg = pwmSegments[streamSegment[stream]];
					const double expected = seg.byteOffset + (sample - seg.startSample) * static_cast<double>(seg.frequency) / VgmRate * seg.stepSize * 2.0;
					if(std::abs(expected - byteOffset) <= seg.stepSize * 2.0 * (seg.frequency / 1000.0 + 8.0) && seg.frequency == streamFrequency[stream])
						return;
				}
				closeSegment(stream, sample);
				PwmSegment seg;
				seg.voice = pwmVoice();
				seg.bank = streamBank[stream] & 0x3F;
				seg.stepSize = std::max<uint8>(streamStepSize[stream], 1);
				seg.stepBase = streamStepBase[stream];
				seg.startSample = sample;
				seg.byteOffset = byteOffset;
				seg.frequency = streamFrequency[stream];
				seg.cycle = pwmCycle;
				pwmSegments.push_back(seg);
				streamSegment[stream] = static_cast<int>(pwmSegments.size() - 1);
				pcmVoiceUsed[seg.voice] = true;
			};
			if(code == 0x90)
			{
				streamChip[stream] = data[pos + 1];
				streamRegister[stream] = data[pos + 3];
			} else if(code == 0x91)
			{
				streamBank[stream] = data[pos + 1];
				streamStepSize[stream] = data[pos + 2];
				streamStepBase[stream] = data[pos + 3];
			} else if(code == 0x92)
			{
				streamFrequency[stream] = ReadLE32(data, pos + 1);
			} else if(code == 0x95)
			{
				const uint32 blockID = data[pos + 1] | (data[pos + 2] << 8);
				if(pwm)
				{
					const auto &blocks = streamBlocks[streamBank[stream] & 0x3F];
					if(blockID < blocks.size())
						startPwm(blocks[blockID].start);
				} else if(blockID < dataBlocks.size())
				{
					blockStarts.push_back({sample, blockID, streamFrequency[stream], (data[pos + 3] & 1) != 0, false});
				}
			} else if(code == 0x93)
			{
				const uint32 offset = ReadLE32(data, pos + 1);
				if(pwm)
				{
					if(offset != uint32_max)
						startPwm(offset);
				} else
				{
					// Запуск с произвольного места. Furnace так не пишет, но если
					// место совпадает с началом блока, это тот же удар.
					const auto found = std::find_if(dataBlocks.begin(), dataBlocks.end(),
						[offset](const DataBlock &b) { return b.start == offset; });
					if(found != dataBlocks.end())
						blockStarts.push_back({sample, static_cast<uint32>(std::distance(dataBlocks.begin(), found)), streamFrequency[stream], false, false});
				}
			} else if(code == 0x94)
			{
				if(pwm || stream == 0xFF)
				{
					for(int s2 = 0; s2 < 256; s2++)
					{
						if(stream == 0xFF || s2 == stream)
							closeSegment(static_cast<uint8>(s2), sample);
					}
				}
				if(!pwm && !blockStarts.empty() && !blockStarts.back().stop)
					blockStarts.push_back({sample, 0, 0, false, true});
			}
			pos += size;
		} else if(code == 0xB2)
		{
			// PWM напрямую: нужен только цикл (регистр 1) — по нему середина.
			if(pos + 2 > data.size())
				break;
			const uint8 reg = data[pos] >> 4;
			const uint16 value = static_cast<uint16>(((data[pos] & 15) << 8) | data[pos + 1]);
			pos += 2;
			if(reg == 1)
				pwmCycle = value;
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
		} else if(code == 0xB1)
		{
			if(pos + 2 > data.size())
				break;
			const uint8 reg = data[pos], value = data[pos + 1];
			pos += 2;
			rfAdvance(sample);
			RfVoice &v = rf[rfSelected];
			const uint8 voice = static_cast<uint8>(RFBase + rfSelected);
			switch(reg)
			{
			case 0:
			case 1:
				(reg == 0 ? v.env : v.pan) = value;
				if(v.onEvent >= 0)
				{
					int volume, panning;
					rfLevel(v, volume, panning);
					if(volume != v.volume)
						pcmEvents.push_back({sample, voice, PcmWhat::Volume, -1, 0.0, volume});
					if(panning != v.panning)
						pcmEvents.push_back({sample, voice, PcmWhat::Pan, -1, 0.0, panning});
					v.volume = volume;
					v.panning = panning;
				}
				break;
			case 2:
			case 3:
				v.step = (reg == 2) ? static_cast<uint16>((v.step & 0xFF00) | value) : static_cast<uint16>((v.step & 0x00FF) | (value << 8));
				if(v.onEvent >= 0 && v.step)
					pcmEvents.push_back({sample, voice, PcmWhat::Pitch, -1, rfRateOf(v)});
				break;
			case 4:
				v.loop = static_cast<uint16>((v.loop & 0xFF00) | value);
				break;
			case 5:
				v.loop = static_cast<uint16>((v.loop & 0x00FF) | (value << 8));
				break;
			case 6:
				v.start = value;
				break;
			case 7:
				rfRunning = (value & 0x80) != 0;
				if(value & 0x40)
					rfSelected = value & 7;
				else
					rfBank = value & 15;
				break;
			case 8:
				for(uint8 i = 0; i < RFChannels; i++)
				{
					const bool enable = !((value >> i) & 1);
					if(enable && !rf[i].enabled)
						rfKeyOn(i, sample);
					else if(!enable && rf[i].enabled)
						rfFinish(i, sample);
					rf[i].enabled = enable;
				}
				break;
			}
		} else if(code == 0xC2)
		{
			if(pos + 3 > data.size())
				break;
			rfAdvance(sample);
			const uint16 offset = static_cast<uint16>(data[pos] | (data[pos + 1] << 8));
			rfRam[(offset | (rfBank << 12)) & 0xFFFF] = data[pos + 2];
			rfTouch((offset | (rfBank << 12)) & 0xFFFF, 1);
			pos += 3;
		} else if(code == 0x68)
		{
			// Запись в память PCM из блока данных: тип блока, откуда, куда, сколько.
			if(pos + 11 > data.size())
				break;
			const uint8 type = data[pos + 1] & 0x3F;
			const uint32 from = data[pos + 2] | (data[pos + 3] << 8) | (data[pos + 4] << 16);
			const uint32 to = data[pos + 5] | (data[pos + 6] << 8) | (data[pos + 7] << 16);
			uint32 count = data[pos + 8] | (data[pos + 9] << 8) | (data[pos + 10] << 16);
			if(!count)
				count = 0x01000000;
			pos += 11;
			if(type == 0x02)
			{
				rfAdvance(sample);
				const auto &source = streamData[0x02];
				// Адрес записи — внутри окна памяти, выбранного регистром 7 (как в libvgm).
				const uint32 base = to | (static_cast<uint32>(rfBank) << 12);
				rfTouch(base, count);
				for(uint32 i = 0; i < count && from + i < source.size() && base + i < 0x10000; i++)
					rfRam[base + i] = source[from + i];
			}
		} else if(code == 0xB8)
		{
			if(pos + 2 > data.size())
				break;
			const uint8 chipIndex = data[pos] >> 7, reg = data[pos] & 0x7F, value = data[pos + 1];
			pos += 2;
			OkiChip &chip = oki[chipIndex];
			if(reg == 0x00)
			{
				const double rate = header.okiClock / (chip.pin7 ? 132.0 : 165.0);
				if(chip.pendingPhrase >= 0)
				{
					// Второй байт: какие голоса (биты 4–7) и ослабление.
					static constexpr uint8 Volume[16] = {64, 44, 32, 22, 16, 12, 8, 6, 4, 0, 0, 0, 0, 0, 0, 0};
					const uint32 entry = static_cast<uint32>(chip.pendingPhrase) * 8;
					const auto address = [&](uint32 at)
					{
						return (static_cast<uint32>(okiByte(chip, chipIndex, at) & 3) << 16) | (okiByte(chip, chipIndex, at + 1) << 8) | okiByte(chip, chipIndex, at + 2);
					};
					const uint32 start = address(entry), end = address(entry + 3);
					const int sound = addPcmSound(okiDecode(chip, chipIndex, start, end), rate, uint32_max, "OKI");
					for(uint8 v = 0; v < 4; v++)
					{
						if(!(value & (0x10 << v)) || sound < 0)
							continue;
						const uint8 voice = static_cast<uint8>(OKIBase + chipIndex * 4 + v);
						pcmVoiceUsed[voice] = true;
						pcmEvents.push_back({sample, voice, PcmWhat::On, sound, rate, Volume[value & 15]});
					}
					chip.pendingPhrase = -1;
				} else if(value & 0x80)
				{
					chip.pendingPhrase = value & 0x7F;
				} else
				{
					for(uint8 v = 0; v < 4; v++)
					{
						if(value & (0x08 << v))
							pcmEvents.push_back({sample, static_cast<uint8>(OKIBase + chipIndex * 4 + v), PcmWhat::Off});
					}
				}
			} else if(reg == 0x0C)
			{
				chip.pin7 = value != 0;
			} else if(reg == 0x0E)
			{
				chip.nmkFlags = value;
			} else if(reg == 0x0F)
			{
				chip.bank = value;
			} else if(reg >= 0x10 && reg <= 0x13)
			{
				chip.nmkBank[reg - 0x10] = value;
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
		} else if(code == 0x4F)
		{
			// Стерео Game Gear: биты 4–7 — левый выход голосов PSG, 0–3 — правый.
			if(pos >= data.size())
				break;
			const uint8 value = data[pos++];
			for(uint8 lane = 0; lane < PSGChannels; lane++)
			{
				const bool left = (value >> (4 + lane)) & 1, right = (value >> lane) & 1;
				const int pan = (left && right) ? 128 : left ? 0 : right ? 255 : -1;
				if(pan >= 0 && pan != psgPan[lane])
				{
					psgPan[lane] = pan;
					events.push_back({sample, static_cast<uint8>(FMChannels + lane), NOTE_NONE, 255, -1, pan});
				}
			}
		} else if(code >= 0x30 && code <= 0x3F)
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
	rfAdvance(sample);
	for(uint8 i = 0; i < RFChannels; i++)
		rfFinish(i, sample);
	// Сначала ноты с петлёй, потом остальные — им уже есть с кем сверяться.
	std::vector<bool> rfResolved(rfNotes.size(), false);
	for(size_t i = 0; i < rfNotes.size(); i++)
		rfResolved[i] = rfResolve(rfNotes[i], true);
	for(size_t i = 0; i < rfNotes.size(); i++)
	{
		if(!rfResolved[i])
			rfResolve(rfNotes[i], false);
	}
	rfNotes.clear();
	// Куски PWM: значения из данных потока по шагу, относительно середины цикла.
	for(int stream = 0; stream < 256; stream++)
		closeSegment(static_cast<uint8>(stream), sample);
	std::array<bool, VgmChannels> pwmPanned{};
	for(const auto &seg : pwmSegments)
	{
		const auto &source = streamData[seg.bank];
		if(!seg.frequency || seg.endSample <= seg.startSample)
			continue;
		const double center = (seg.cycle ? seg.cycle : 0x1000) / 2.0;
		const uint64 commands = static_cast<uint64>((seg.endSample - seg.startSample) * static_cast<double>(seg.frequency) / VgmRate);
		std::vector<int16> pcmData;
		pcmData.reserve(static_cast<size_t>(std::min<uint64>(commands, MAX_SAMPLE_LENGTH)));
		// Выход PWM идёт через разделительный конденсатор: постоянную
		// составляющую (до запуска звука значения стоят у нуля, а не в
		// середине) убираем фильтром около 20 Гц, иначе на краях щелчки.
		const double keep = 1.0 - 2.0 * 3.14159265358979323846 * 20.0 / seg.frequency;
		double previous = center, filtered = 0.0;
		for(uint64 n = 0; n < commands && pcmData.size() < MAX_SAMPLE_LENGTH; n++)
		{
			const uint64 at = seg.byteOffset + (seg.stepBase + n * seg.stepSize) * 2u;
			if(at + 1 >= source.size())
				break;
			const double value = (source[at] | (source[at + 1] << 8)) & 0xFFF;
			filtered = value - previous + keep * filtered;
			previous = value;
			pcmData.push_back(static_cast<int16>(std::clamp(filtered / center * 32767.0, -32768.0, 32767.0)));
		}
		const uint32 played = static_cast<uint32>(pcmData.size());
		const int sound = addPcmSound(std::move(pcmData), seg.frequency, uint32_max, "PWM");
		if(sound < 0)
			continue;
		if(!pwmPanned[seg.voice])
		{
			pcmEvents.push_back({seg.startSample, seg.voice, PcmWhat::Pan, -1, 0.0, (seg.voice == PWMBase) ? 0 : 255});
			pwmPanned[seg.voice] = true;
		}
		pcmEvents.push_back({seg.startSample, seg.voice, PcmWhat::On, sound, static_cast<double>(seg.frequency), 64});
		const uint32 naturalEnd = seg.startSample + static_cast<uint32>(played * static_cast<double>(VgmRate) / seg.frequency);
		if(seg.endSample < naturalEnd)
			pcmEvents.push_back({seg.endSample, seg.voice, PcmWhat::Off});
	}
	std::stable_sort(pcmEvents.begin(), pcmEvents.end(), [](const PcmEvent &a, const PcmEvent &b) { return a.sample < b.sample; });
	if(events.empty() && hits.empty() && pcmEvents.empty() && ymzEvents.empty() && blockStarts.empty())
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
			: (voice < OPMBase) ? (header.psgClock != 0)
			: (voice < YMZBase) ? (header.opmClock != 0)
			: (voice < RFBase) ? (header.ymzClock != 0 && !ymzRom.empty() && voice - YMZBase < ymzVoicesUsed)
			: pcmVoiceUsed[voice];
		if(present)
			moduleChannel[voice] = usedChannels++;
	}
	InitializeGlobals(MOD_TYPE_MPT, usedChannels);
	m_nSamplePreAmp = m_nVSTiVolume = 48;
	// Баланс чипов как у плат в MAME. Сэмплы (PCM) идут через общее
	// усиление сэмплов, синтезаторы — через громкость своего чипа.
	// Battle Garrega: YM2151 0,3, OKIM6295 0,6; при выходах самих чипов это
	// OKI на 24 дБ громче, чем у нас вышло бы по умолчанию.
	// 32X: PWM через 12-битный ЦАП с усилением 0,4, YM2612 приглушён до 0,25;
	// PWM на 6 дБ громче, чем по умолчанию, и запас от перегруза: YM2612 x1/4,
	// PSG x1/2, сэмплы x1/2 (ЦАП YM2612 — ещё x1/2 в инструменте, вровень с FM).
	const bool pwmSong = !pwmSegments.empty();
	if(header.okiClock && std::any_of(pcmVoiceUsed.begin() + OKIBase, pcmVoiceUsed.begin() + OKIBase + OKIChannels, [](bool b) { return b; }))
	{
		// Ещё -6 дБ на оба: удары OKI иначе упираются в потолок.
		m_chipVolume[ChipOPM] = ChipVolumeUnity / 8;
		m_nSamplePreAmp = 48 * 2;
	}
	if(pwmSong)
	{
		m_chipVolume[ChipOPN2] = ChipVolumeUnity / 4;
		m_chipVolume[ChipPSG] = ChipVolumeUnity / 2;
		m_nSamplePreAmp = 24;
	}
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
	for(size_t i = 0; i < patches.size() && m_nSamples < MAX_SAMPLES - 1 && m_nInstruments < MAX_INSTRUMENTS - 1; i++)
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
	// Удары ЦАПа, записанные побайтно: одинаковые удары становятся одним
	// сэмплом, а каждый удар — нотой на шестом канале. Одинаковость — по
	// последовательности записанных значений: звук снят по времени лога, и два
	// одинаковых удара из-за дрожания записей дают чуть разные байты.
	// Звук сэмпла — у самого длинного удара; удар короче обрывается там же,
	// где в логе. Тот же удар, сыгранный быстрее или медленнее (томы), — тот же
	// сэмпл другой нотой.
	const INSTRUMENTINDEX firstDAC = m_nInstruments;
	constexpr size_t MaxDacSamples = 200;
	struct DacSample { uint32 hit = 0; double writeRate = 0.0; };
	std::vector<DacSample> dacSamples;
	std::vector<int> hitSample(hits.size(), -1);
	std::vector<int> hitNote(hits.size(), NOTE_MIDDLEC);
	const auto writeRate = [&](const Hit &h) { return (h.last > h.first) ? static_cast<double>(h.writeCount) * VgmRate / (h.last - h.first) : 0.0; };
	// Тот же ли это звук и на сколько полутонов он выше. Драйвер играет один
	// барабан на разной высоте, шагая по сэмплу, а не меняя скорость записей,
	// поэтому высоту ищем подбором: сжимаем звук на целое число полутонов и
	// сравниваем средние по блокам в 8 отсчётов (записи дрожат на отсчёт-два).
	// Возвращает сдвиг в полутонах или NoMatch.
	constexpr int NoMatch = 999;
	const auto soundShift = [&](const Hit &h, const Hit &k) -> int
	{
		const auto error = [&](double ratio) -> double  // время h -> время k
		{
			const uint32 length = static_cast<uint32>(std::min<double>(h.bytes, k.bytes / ratio));
			if(length < 16)
				return 0.0;
			double diff = 0.0, energy = 0.0;
			for(uint32 block = 0; block + 8 <= length; block += 8)
			{
				int x = 0, y = 0;
				for(uint32 t = block; t < block + 8; t++)
				{
					x += raw[h.start + t] - 128;
					y += raw[k.start + std::min<uint32>(static_cast<uint32>(t * ratio), k.bytes - 1)] - 128;
				}
				diff += std::abs(x - y);
				energy += std::abs(x) + std::abs(y);
			}
			return (energy > 0.0) ? diff / energy : 0.0;
		};
		constexpr double Limit = 0.3;
		if(error(1.0) < Limit)
			return 0;
		// Шаг в десятую полутона: драйвер ещё и плавает по скорости на
		// несколько процентов, и за полсекунды удара волна уезжает.
		int best = NoMatch;
		double bestError = Limit;
		for(int tenths = -120; tenths <= 120; tenths++)
		{
			if(tenths == 0)
				continue;
			const double e = error(std::exp2(tenths / 120.0));
			if(e < bestError)
			{
				bestError = e;
				best = static_cast<int>(std::lround(tenths / 10.0));
			}
		}
		return best;
	};
	for(size_t i = 0; i < hits.size(); i++)
	{
		const Hit &h = hits[i];
		int match = -1;
		for(size_t s = 0; s < dacSamples.size() && match < 0; s++)
		{
			DacSample &known = dacSamples[s];
			const Hit &k = hits[known.hit];
			// Тот же удар начинается с тех же записей (первые 16, короткий
			// кусок — целиком). Но одинаковое начало ещё не тот же звук: у
			// Furnace прямой ЦАП — смесь всех PCM-каналов, и дальше куски разные;
			// а склейка «удар + следующий» тоже начинается как удар. Поэтому
			// нужно совпадение записей на всей общей длине, а при совпавшем
			// лишь начале — сверка самого звука.
			const uint32 common = std::min<uint32>(16, std::min(h.writeCount, k.writeCount));
			if(common < 4 || std::memcmp(dacWrites.data() + h.writeStart, dacWrites.data() + k.writeStart, common))
				continue;
			const uint32 shorter = std::min(h.writeCount, k.writeCount), longer = std::max(h.writeCount, k.writeCount);
			const bool similar = shorter * 10 >= longer * 9;
			const bool whole = shorter >= 16 && !std::memcmp(dacWrites.data() + h.writeStart, dacWrites.data() + k.writeStart, shorter);
			int shift = NoMatch;
			// Записи совпали на всей общей длине — тот же удар (короткий —
			// оборванное начало длинного); высота — по скорости записей.
			if(whole)
			{
				const double rh = writeRate(h), rk = writeRate(k);
				shift = (rh > 0.0 && rk > 0.0) ? mpt::saturate_round<int>(12.0 * std::log2(rh / rk)) : 0;
				if(std::abs(shift) > 1)
					shift = soundShift(h, k);
			} else if(similar)
			{
				// Совпало лишь начало: решает сам звук, и только у кусков
				// похожей длины — иначе это склейка с чужим продолжением.
				shift = soundShift(h, k);
			}
			if(shift == NoMatch)
				continue;
			match = static_cast<int>(s);
			hitNote[i] = std::clamp(NOTE_MIDDLEC + shift, int(NOTE_MIN), int(NOTE_MAX));
			// Звук берём у более длинного удара той же высоты.
			if(shift == 0 && h.bytes > k.bytes)
				known.hit = static_cast<uint32>(i);
		}
		if(match < 0 && dacSamples.size() < MaxDacSamples)
		{
			match = static_cast<int>(dacSamples.size());
			dacSamples.push_back({static_cast<uint32>(i), writeRate(h)});
		}
		if(match >= 0)
			hitSample[i] = match;
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
			if(pwmSong)
				instrument->nGlobalVol = 32;  // См. баланс 32X выше
		}
		return true;
	};
	size_t dacMade = 0;
	for(const auto &known : dacSamples)
	{
		// Звук снят по времени лога, на его частоте; отличие скорости удара от
		// эталонного учтено нотой.
		const Hit &h = hits[known.hit];
		if(!addDacSample(raw.data() + h.start, h.bytes, VgmRate, false, "DAC " + std::to_string(dacMade + 1)))
			break;
		dacMade++;
	}
	for(size_t i = 0; i < hits.size(); i++)
	{
		if(hitSample[i] < 0 || static_cast<size_t>(hitSample[i]) >= dacMade)
			continue;
		events.push_back({hits[i].first, 5, static_cast<ModCommand::NOTE>(hitNote[i]), 64,
			static_cast<int>(firstDAC) + hitSample[i]});
		// Удар короче звука своего сэмпла — обрываем там же, где в логе.
		const Hit &sound = hits[dacSamples[hitSample[i]].hit];
		if(hits[i].bytes + 64 < sound.bytes)
			events.push_back({hits[i].last, 5, NOTE_NOTECUT, 255, -1});
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
	// Прочие PCM-чипы: сэмпл на каждый разный звук, нота по частоте нажатия.
	if(!pcmEvents.empty())
	{
		std::vector<int> soundInstrument(pcmSounds.size(), -1);
		// Место под тембры FM, голоса PSG и режимы шума оставляем.
		const size_t reserve = patches.size() + PSGChannels + 3;
		for(size_t i = 0; i < pcmSounds.size(); i++)
		{
			if(m_nSamples + reserve >= MAX_SAMPLES - 1 || m_nInstruments + reserve >= MAX_INSTRUMENTS - 1)
				break;
			const PcmSound &sound = pcmSounds[i];
			const SAMPLEINDEX smp = ++m_nSamples;
			ModSample &smpl = Samples[smp];
			smpl.Initialize(MOD_TYPE_MPT);
			smpl.nLength = std::min<SmpLength>(static_cast<SmpLength>(sound.data.size()), MAX_SAMPLE_LENGTH);
			smpl.uFlags.set(CHN_16BIT);
			smpl.nC5Speed = std::clamp<uint32>(mpt::saturate_round<uint32>(sound.rate), 1000, 192000);
			if(sound.loopStart < smpl.nLength)
			{
				smpl.nLoopStart = sound.loopStart;
				smpl.nLoopEnd = smpl.nLength;
				smpl.uFlags.set(CHN_LOOP);
			}
			if(smpl.AllocateSample() && smpl.sample16() != nullptr)
			{
				std::copy(sound.data.begin(), sound.data.begin() + smpl.nLength, smpl.sample16());
				smpl.PrecomputeLoops(*this, false);
			}
			m_szNames[smp] = sound.name + " " + std::to_string(i + 1);
			const INSTRUMENTINDEX ins = ++m_nInstruments;
			if(auto *instrument = AllocateInstrument(ins, smp); instrument != nullptr)
			{
				instrument->name = m_szNames[smp];
				instrument->midiPWD = 1;
			}
			soundInstrument[i] = static_cast<int>(m_nInstruments) - 1;
		}
		std::array<ModCommand::NOTE, VgmChannels> sounding{}, pitchNote{};
		std::array<int, VgmChannels> fineNow;
		fineNow.fill(0x80);
		std::array<double, VgmChannels> baseRate{};
		for(const auto &e : pcmEvents)
		{
			const uint8 voice = e.voice;
			switch(e.what)
			{
			case PcmWhat::On:
			{
				if(e.sound < 0 || soundInstrument[e.sound] < 0 || !(e.rate > 0.0))
					break;
				baseRate[voice] = pcmSounds[e.sound].rate;
				const double exact = static_cast<int>(NOTE_MIDDLEC) + 12.0 * std::log2(e.rate / baseRate[voice]);
				const auto note = static_cast<ModCommand::NOTE>(std::clamp(static_cast<int>(std::lround(exact)), int(NOTE_MIN), int(NOTE_MAX)));
				const int fine = FineFromOffset(exact - note);
				events.push_back({e.sample, voice, note, static_cast<uint8>(std::clamp(e.value, 0, 64)), soundInstrument[e.sound], -1, false, fine != fineNow[voice] ? fine : -1});
				fineNow[voice] = fine;
				sounding[voice] = note;
				pitchNote[voice] = note;
				break;
			}
			case PcmWhat::Pitch:
			{
				if(sounding[voice] == NOTE_NONE || !(baseRate[voice] > 0.0) || !(e.rate > 0.0))
					break;
				const double exact = static_cast<int>(NOTE_MIDDLEC) + 12.0 * std::log2(e.rate / baseRate[voice]);
				VgmEvent event{e.sample, voice, NOTE_NONE, 255, -1};
				if(std::abs(exact - pitchNote[voice]) > 127.0 / 128.0)
				{
					event.note = static_cast<ModCommand::NOTE>(std::clamp(static_cast<int>(std::lround(exact)), int(NOTE_MIN), int(NOTE_MAX)));
					event.legato = true;
					pitchNote[voice] = event.note;
				}
				const int fine = FineFromOffset(exact - pitchNote[voice]);
				if(fine != fineNow[voice])
				{
					event.fine = fine;
					fineNow[voice] = fine;
				}
				if(event.legato || event.fine >= 0)
					events.push_back(event);
				break;
			}
			case PcmWhat::Off:
				if(sounding[voice] != NOTE_NONE)
					events.push_back({e.sample, voice, NOTE_NOTECUT, 255, -1});
				sounding[voice] = NOTE_NONE;
				break;
			case PcmWhat::Volume:
				if(sounding[voice] != NOTE_NONE)
					events.push_back({e.sample, voice, NOTE_NONE, static_cast<uint8>(std::clamp(e.value, 0, 64)), -1});
				break;
			case PcmWhat::Pan:
				events.push_back({e.sample, voice, NOTE_NONE, 255, -1, std::clamp(e.value, 0, 255)});
				break;
			}
		}
	}
	std::stable_sort(events.begin(), events.end(),
		[](const VgmEvent &a, const VgmEvent &b) { return a.sample < b.sample; });

	const INSTRUMENTINDEX firstPSG = m_nInstruments;
	for(uint8 lane = 0; lane < PSGChannels && m_nSamples < MAX_SAMPLES - 1 && m_nInstruments < MAX_INSTRUMENTS - 1; lane++)
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
			// Если в песне есть шум других режимов, режим основного задаём
			// явно: иначе чип оставался бы в режиме предыдущего удара.
			if(noiseModesUsed[0] || noiseModesUsed[1] || noiseModesUsed[2])
				patch->macros[PSGPatch::Duty].values = {3};
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
	// Шум в других режимах — свои инструменты, режим задан постоянным
	// значением макроса Duty. Основной («PSG noise») — белый от третьего тона.
	std::array<INSTRUMENTINDEX, 4> noiseInstrument{};
	for(uint8 mode = 0; mode < 3; mode++)
	{
		if(!noiseModesUsed[mode] || m_nSamples >= MAX_SAMPLES - 1 || m_nInstruments >= MAX_INSTRUMENTS - 1)
			continue;
		const SAMPLEINDEX smp = ++m_nSamples;
		ModSample &smpl = Samples[smp];
		smpl.Initialize(MOD_TYPE_MPT);
		smpl.SetAdlib(true);
		smpl.uFlags.set(SMP_NODEFAULTVOLUME);
		auto patch = std::make_shared<PSGPatch>();
		patch->lane = 3;
		patch->chip = 0;
		patch->easyPitch = false;
		patch->fastNoise = true;
		patch->macros[PSGPatch::Duty].values = {mode};
		smpl.psg = std::move(patch);
		m_szNames[smp] = std::string("PSG noise") + ((mode & 1) ? " white" : " periodic") + ((mode & 2) ? "" : " fixed");
		const INSTRUMENTINDEX ins = ++m_nInstruments;
		if(auto *instrument = AllocateInstrument(ins, smp); instrument != nullptr)
		{
			instrument->name = m_szNames[smp];
			instrument->midiPWD = 1;
		}
		noiseInstrument[mode] = ins;
	}

	for(uint8 voice = 0; voice < VgmChannels; voice++)
	{
		if(moduleChannel[voice] < 0)
			continue;
		const CHANNELINDEX chn = static_cast<CHANNELINDEX>(moduleChannel[voice]);
		ChnSettings[chn].szName = (voice < FMChannels) ? "YM2612 " + std::to_string(voice + 1)
			: (voice >= OKIBase) ? "OKIM6295 " + std::to_string(voice - OKIBase + 1)
			: (voice >= PWMBase) ? std::string((voice == PWMBase) ? "PWM L" : "PWM R")
			: (voice >= RFBase) ? "RF5C164 " + std::to_string(voice - RFBase + 1)
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
	// Где внутри кадра драйвер пишет регистры. Игровой драйвер работает по
	// кадровому прерыванию, и его записи за кадр идут пачкой в одном месте
	// кадра, но лог VGM начат не с края кадра: у многих треков пачка ложится
	// ровно на границу 735 отсчётов. Тогда ноты одного кадра разлетались то в
	// этот кадр, то в следующий, и ритм дрожал на кадр. Сетку сдвигаем так,
	// чтобы пачка оказалась посередине кадра. Сдвиг один на все каналы (так
	// точнее, чем по каналу: пачка одна на весь драйвер) и выбирается из двух
	// вариантов — раньше или позже — тот, что ближе к времени лога, чтобы трек
	// не съезжал больше чем на полкадра. Драйверу, который идёт не от кадра
	// (нажатия по всему кадру), сдвиг не поможет — там сетка как есть.
	int32 gridShift = 0;
	{
		constexpr double TwoPi = 2.0 * 3.14159265358979323846;
		double sumSin = 0.0, sumCos = 0.0;
		uint32 count = 0;
		for(const auto &event : events)
		{
			if(event.note == NOTE_NONE || event.legato)
				continue;
			const double angle = TwoPi * (event.sample % samplesPerRow) / samplesPerRow;
			sumSin += std::sin(angle);
			sumCos += std::cos(angle);
			count++;
		}
		// Насколько записи сбиты в одно место кадра: 1 — все в одной точке,
		// 0 — размазаны по всему кадру.
		const double focus = count ? std::hypot(sumSin, sumCos) / count : 0.0;
		if(count >= 8 && focus >= 0.7)
		{
			double center = std::atan2(sumSin, sumCos) / TwoPi * samplesPerRow;
			if(center < 0.0)
				center += samplesPerRow;
			gridShift = mpt::saturate_round<int32>(samplesPerRow / 2.0 - center);
			// Строка звучит с начала кадра, а пачка теперь в его середине: при
			// отрицательном сдвиге трек ушёл бы почти на кадр раньше лога.
			if(gridShift < 0)
				gridShift += samplesPerRow;
		}
	}
	const auto frameIndex = [&](const VgmEvent &event) -> uint32
	{
		return static_cast<uint32>(std::max<int64>(int64(event.sample) + gridShift, 0) / samplesPerRow);
	};

	uint32 framesPerRow = 1, phase = 0;
	if(compact)
	{
		std::vector<uint32> noteFrames;
		for(const auto &event : events)
		{
			if(ModCommand::IsNote(event.note) && !event.legato)
				noteFrames.push_back(frameIndex(event));
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
	const auto frameOf = [&](const VgmEvent &event) { return std::max(frameIndex(event), phase) - phase; };

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
		if(event.noiseMode >= 0 && event.noiseMode < 3 && noiseInstrument[event.noiseMode])
			return noiseInstrument[event.noiseMode];
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
		// Удар длиной в кадр (хет шумом, щелчок FM) и его отпускание из-за
		// дрожания времени лога попадают в один кадр, и отпускание затирало
		// ноту — удар пропадал целиком. Тогда отпускание встаёт на кадр позже.
		// Сдвигается только отпускание и без накопления: два удара в одном кадре
		// (быстрое арпеджио у драйвера, который чаще кадра) так и остаются —
		// второй побеждает, иначе всё дальше уползало бы.
		// Если же в том же кадре нота нажата снова, отпускание отменяется:
		// иначе оно оборвало бы эту, держащуюся ноту. Нажатие и отпускание
		// в один миг (драйвер так сбрасывает голос при старте) — не удар, его
		// по-прежнему затирает отпускание.
		constexpr uint32 NoFrame = std::numeric_limits<uint32>::max();
		std::array<uint32, VgmChannels> notedFrame;  // кадр последнего удара канала
		notedFrame.fill(NoFrame);
		std::array<uint32, VgmChannels> pushedFrame;  // куда отложено отпускание
		std::array<uint32, VgmChannels> notedSample{};  // когда был последний удар
		pushedFrame.fill(NoFrame);
		for(const auto &event : events)
		{
			uint32 frame = frameOf(event);
			const bool strike = ModCommand::IsNote(event.note) && !event.legato;
			if((event.note == NOTE_KEYOFF || event.note == NOTE_NOTECUT) && notedFrame[event.channel] == frame
				&& event.sample >= notedSample[event.channel] + samplesPerRow / 4)
			{
				frame++;
				pushedFrame[event.channel] = frame;
			} else if(strike && pushedFrame[event.channel] == frame + 1)
			{
				if(ModCommand *pushed = cellAt(frame + 1, event.channel); pushed != nullptr && (pushed->note == NOTE_KEYOFF || pushed->note == NOTE_NOTECUT))
					pushed->note = NOTE_NONE;
				pushedFrame[event.channel] = NoFrame;
			}
			if(strike)
			{
				notedFrame[event.channel] = frame;
				notedSample[event.channel] = event.sample;
			}
			ModCommand *found = cellAt(frame, event.channel);
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
			// Отпускание или новый удар на том же тике, что и нота строки,
			// затёр бы её: короткий удар в кадр (хет шумом, щелчок FM) и его
			// отпускание из-за дрожания времени лога попадают в одну строку, и
			// удар пропадал целиком. Такое тоже переезжает дальше.
			const bool sameTickNote = hasRealNote && tick == state.noteTick
				&& (realNote || event.note == NOTE_KEYOFF || event.note == NOTE_NOTECUT);
			if(sameTickNote || (hasRealNote && tick > state.noteTick && (event.note != NOTE_NONE || event.volume != 255 || event.fine >= 0 || hasLevel)))
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

	// У каждого вида эффекта в канале — своя постоянная колонка. Иначе одна и
	// та же подстройка (+xx) стояла то в первой колонке, то во второй, смотря
	// что ещё было в строке, и читать узор было нельзя. Портаменто и задержка
	// всегда в первой (движок ищет их только там), остальные — по частоте
	// употребления. Колонок в редакторе показывается столько, сколько нужно.
	for(CHANNELINDEX chn = 0; chn < GetNumChannels(); chn++)
	{
		const auto firstColumn = [](EffectCommand command) { return command == CMD_TONEPORTAMENTO || command == CMD_S3MCMDEX; };
		std::map<EffectCommand, uint32> uses;
		bool anyFirst = false;
		for(const auto &pat : Patterns)
		{
			for(ROWINDEX row = 0; row < pat.GetNumRows(); row++)
			{
				const ModCommand &cell = *pat.GetpModCommand(row, chn);
				for(uint8 slot = 0; slot < ModCommand::Effects(); slot++)
				{
					const EffectCommand command = cell.GetEffect(slot).first;
					if(command == CMD_NONE)
						continue;
					if(firstColumn(command))
						anyFirst = true;
					else
						uses[command]++;
				}
			}
		}
		std::vector<std::pair<uint32, EffectCommand>> order;
		for(const auto &[command, count] : uses)
			order.emplace_back(count, command);
		std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
		std::map<EffectCommand, uint8> column;
		uint8 next = anyFirst ? 1 : 0;
		for(const auto &[count, command] : order)
		{
			if(next < ModCommand::Effects())
				column[command] = next++;
		}
		uint8 used = 1;
		for(auto &pat : Patterns)
		{
			for(ROWINDEX row = 0; row < pat.GetNumRows(); row++)
			{
				ModCommand &cell = *pat.GetpModCommand(row, chn);
				std::array<std::pair<EffectCommand, uint8>, ModCommand::Effects()> effects;
				for(uint8 slot = 0; slot < ModCommand::Effects(); slot++)
				{
					effects[slot] = cell.GetEffect(slot);
					cell.SetEffect(slot, CMD_NONE, 0);
				}
				std::vector<std::pair<EffectCommand, uint8>> homeless;
				for(const auto &[command, param] : effects)
				{
					if(command == CMD_NONE)
						continue;
					uint8 slot = ModCommand::Effects();
					if(firstColumn(command))
						slot = 0;
					else if(auto it = column.find(command); it != column.end())
						slot = it->second;
					if(slot < ModCommand::Effects() && cell.GetEffect(slot).first == CMD_NONE)
						cell.SetEffect(slot, command, param);
					else
						homeless.emplace_back(command, param);
				}
				for(const auto &[command, param] : homeless)
				{
					if(const uint8 slot = cell.FreeEffectSlot(); slot < ModCommand::Effects())
						cell.SetEffect(slot, command, param);
				}
				for(uint8 slot = 0; slot < ModCommand::Effects(); slot++)
				{
					if(cell.GetEffect(slot).first != CMD_NONE)
						used = std::max<uint8>(used, slot + 1);
				}
			}
		}
#ifdef MODPLUG_TRACKER
		ChnSettings[chn].effectColumns = used;
#else
		MPT_UNUSED(used);
#endif
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
