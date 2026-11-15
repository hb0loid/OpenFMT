/*
 * Load_fur.cpp
 * Editable import of Furnace INFO-era modules.
 */
#include "stdafx.h"
#include "Loaders.h"
#ifdef MODPLUG_TRACKER
#include "YM2612Preset.h"
#endif
#ifdef MPT_WITH_ZLIB
#include "../common/zlib_helper.h"
#include <zlib.h>
#endif

OPENMPT_NAMESPACE_BEGIN
namespace
{
constexpr char FurMagic[] = "-Furnace module-";
constexpr size_t MaxFurBytes = 64u * 1024u * 1024u;

template<typename Reader>
bool IsZlib(Reader file)
{
	const auto header = file.ReadUint16BE();
	return (header & 0x8F00) == 0x0800 && (header % 31) == 0 && !(header & 0x20);
}

#ifdef MPT_WITH_ZLIB
template<typename Reader>
bool InflateFur(Reader file, std::vector<uint8> &output, bool prefixOnly)
{
	if(!IsZlib(file) || file.GetLength() > MaxFurBytes)
		return false;
	zlib::z_inflate_stream stream{};
	if(!zlib::is_Z_OK(inflateInit(&*stream)))
		return false;
	std::array<uint8, 16384> input{}, block{};
	int result = Z_OK;
	while(result == Z_OK)
	{
		if(!stream->avail_in && file.BytesLeft())
		{
			const auto length = std::min<size_t>(file.BytesLeft(), input.size());
			file.ReadRaw(mpt::span(input.data(), length));
			stream->next_in = input.data();
			stream->avail_in = static_cast<uInt>(length);
		}
		stream->next_out = block.data();
		stream->avail_out = static_cast<uInt>(prefixOnly ? sizeof(FurMagic) - 1 : block.size());
		const auto capacity = stream->avail_out;
		result = inflate(&*stream, Z_NO_FLUSH);
		const auto produced = capacity - stream->avail_out;
		if(produced > MaxFurBytes - output.size())
			return false;
		output.insert(output.end(), block.begin(), block.begin() + produced);
		if(output.size() >= sizeof(FurMagic) - 1 && std::memcmp(output.data(), FurMagic, sizeof(FurMagic) - 1))
			return false;
		if(prefixOnly && output.size() == sizeof(FurMagic) - 1)
			return true;
	}
	return result == Z_STREAM_END && output.size() >= 32;
}
#endif

bool FurString(FileReader &file, std::string &value)
{
	value.clear();
	while(file.CanRead(1) && value.size() < 1024 * 1024)
	{
		const auto byte = file.ReadUint8();
		if(!byte)
			return true;
		value.push_back(static_cast<char>(byte));
	}
	return false;
}

constexpr uint8 Detune[] = {7, 6, 5, 0, 1, 2, 3, 4};

// Инструмент Furnace, у которого в файле нет FM-блока, всё равно приходит в чип:
// там читаются поля `DivInstrumentFM`, оставшиеся значениями по умолчанию из
// конструктора. Это не «какой-то» патч, а вполне определённый, поэтому он
// выписан здесь по тем же значениям: alg 0, fb 4 и четыре оператора из
// `instrument.h`. Порядок операторов тот же, что у чтения FM-блока: слоты идут
// как op0, op2, op1, op3.
constexpr OPN2Patch FurnaceDefaultPatch = {
	0x20,
	0x25,  42, 0x1F, 0x08, 0x00, 0xF3, 0,
	0x71,  18, 0x1F, 0x0A, 0x00, 0xF4, 0,
	0x21,  48, 0x1F, 0x04, 0x00, 0xB1, 0,
	0x71,   2, 0x1F, 0x09, 0x00, 0xF9, 0,
};

FileReader FurBlock(FileReader file, uint32 offset, std::string &id, bool legacyStandalone = false)
{
	id.clear();
	if(!file.Seek(offset) || !file.CanRead(8))
		return {};
	if(!file.ReadString<mpt::String::maybeNullTerminated>(id, 4))
		return {};
	const auto declaredLength = file.ReadUint32LE();
	// Some standalone INST blocks omit their size and run to the end of the file.
	const auto length = legacyStandalone && id == "INST" && !declaredLength ? file.BytesLeft() : declaredLength;
	if(!file.CanRead(length))
		return {};
	return file.ReadChunk(length);
}

bool ReadPointers(FileReader &file, std::vector<uint32> &ptrs, size_t count)
{
	if(!file.CanRead(count * 4))
		return false;
	ptrs.resize(count);
	for(auto &offset : ptrs)
		offset = file.ReadUint32LE();
	return true;
}

enum class FurVoice { OPN2, OPM, PSG, Noise, PCM };

struct FurInstrument
{
	std::string name;
	uint16 type = 0;
	// `DivInstrumentAmiga::initSample` в Furnace равен нулю, а `getSample` без
	// карты нот возвращает именно его. Поэтому сэмплерный инструмент, у которого
	// в файле нет фичи `SM`, играет не тишину, а сэмпл 0: так в `major boss alt`
	// звучит «crash», у которого своего сэмпла нет вовсе.
	int sample = 0;
	OPN2Patch patch = FurnaceDefaultPatch;
	bool hasFMPatch = false;
	std::array<std::pair<int16, int16>, 120> noteMap{};
	bool mapped = false;
	std::vector<int32> volume;
	uint32 volumeLoop = uint32_max;
	uint32 volumeRelease = uint32_max;
	uint8 volumeStep = 1;
	uint8 volumeDelay = 0;
	PSGPatch psg;
};

void ReadMacro(FileReader file, FurInstrument &ins, size_t &unsupported)
{
	const auto headerSize = file.ReadUint16LE();
	if(headerSize < 8)
		return;
	while(file.CanRead(1))
	{
		const auto code = file.ReadUint8();
		if(code == 255 || !file.CanRead(headerSize - 1))
			break;
		const auto length = file.ReadUint8();
		const auto loop = file.ReadUint8(), release = file.ReadUint8();
		const auto mode = file.ReadUint8();
		const auto flags = file.ReadUint8(), delay = file.ReadUint8(), speed = file.ReadUint8();
		file.Skip(headerSize - 8);
		const uint8 size = flags >> 6;
		const uint8 width = size == 3 ? 4 : size == 2 ? 2 : 1;
		if(!file.CanRead(length * width))
			break;
		PSGSequence *psgMacro = nullptr;
		if(ins.type == 0)
		{
			switch(code)
			{
			case 0: psgMacro = &ins.psg.macros[PSGPatch::Volume]; break;
			case 1: psgMacro = &ins.psg.macros[PSGPatch::Arpeggio]; break;
			case 2: psgMacro = &ins.psg.macros[PSGPatch::Duty]; break;
			case 4: psgMacro = &ins.psg.macros[PSGPatch::Pitch]; break;
			case 14: psgMacro = &ins.psg.macros[PSGPatch::Reset]; break;
			default: break;
			}
		}
		if((code != 0 && !psgMacro) || (flags & 6) != 0)
		{
			if(length)
				unsupported++;
			file.Skip(length * width);
			continue;
		}
		auto &values = psgMacro ? psgMacro->values : ins.volume;
		if(psgMacro)
		{
			psgMacro->loop = loop;
			psgMacro->release = release;
			psgMacro->speed = std::max<uint8>(1, speed);
			psgMacro->delay = delay;
			psgMacro->relative = code == 4 && mode != 0;
			psgMacro->instantRelease = (flags & 8) != 0;
		} else
		{
			ins.volumeLoop = loop;
			ins.volumeRelease = release;
			ins.volumeStep = std::max<uint8>(1, speed);
			ins.volumeDelay = delay;
		}
		for(size_t i = 0; i < length; i++)
		{
			switch(size)
			{
			case 0: values.push_back(file.ReadUint8()); break;
			case 1: values.push_back(file.ReadInt8()); break;
			case 2: values.push_back(file.ReadInt16LE()); break;
			case 3: values.push_back(file.ReadInt32LE()); break;
			}
		}
	}
}

void ReadFurFM(FileReader file, FurInstrument &ins, uint16 version)
{
	const auto flags = file.ReadUint8();
	const auto algorithm = file.ReadUint8();
	file.Skip(2 + (version >= 224 ? 1 : 0));
	if((flags & 15) != 4 || !file.CanRead(32))
		return;
	ins.hasFMPatch = true;
	ins.patch[0] = static_cast<uint8>(((algorithm & 7) << 3) | ((algorithm >> 4) & 7));
	constexpr uint8 map[] = {0, 2, 1, 3};
	for(uint8 op : map)
	{
		std::array<uint8, 8> regs{};
		file.ReadArray(regs);
		const auto i = 1 + op * 7;
		ins.patch[i + 0] = static_cast<uint8>((Detune[(regs[0] >> 4) & 7] << 4) | (regs[0] & 15));
		ins.patch[i + 1] = (flags & (0x10 << map[op])) ? regs[1] & 0x7f : 127;
		ins.patch[i + 2] = regs[2] & 0xdf;
		ins.patch[i + 3] = regs[3] & 0x9f;
		ins.patch[i + 4] = static_cast<uint8>((regs[4] & 31) | (ins.type == 33 ? (regs[7] & 0x18) << 3 : 0));
		ins.patch[i + 5] = regs[5];
		ins.patch[i + 6] = regs[6] & 15;
	}
}

bool ReadFurInstrument(FileReader file, const std::string &id, FurInstrument &ins, size_t &unsupported, bool patchOnly = false)
{
	if(!file.CanRead(4))
		return false;
	const auto version = file.ReadUint16LE();
	ins.type = file.ReadUint16LE();
	if(id == "INS2")
	{
		while(file.CanRead(2))
		{
			std::string feature;
			file.ReadString<mpt::String::maybeNullTerminated>(feature, 2);
			if(feature == "EN")
				break;
			if(!file.CanRead(2))
				return false;
			const auto len = file.ReadUint16LE();
			if(!file.CanRead(len))
				return false;
			FileReader data = file.ReadChunk(len);
			if(feature == "NA")
			{
				data.ReadNullString(ins.name);
			} else if(feature == "FM")
			{
				ReadFurFM(data, ins, version);
			} else if(feature == "SM")
			{
				if(!data.CanRead(4))
					return false;
				ins.sample = data.ReadInt16LE();
				const auto flags = data.ReadUint8();
				data.Skip(1);
				if((flags & 1) && data.CanRead(ins.noteMap.size() * 4))
				{
					ins.mapped = true;
					for(auto &[note, sample] : ins.noteMap)
					{
						note = data.ReadInt16LE();
						sample = data.ReadInt16LE();
					}
				}
			} else if(feature == "MA")
			{
				ReadMacro(data, ins, unsupported);
			} else if(feature == "O1" || feature == "O2" || feature == "O3" || feature == "O4")
			{
				unsupported++;
			}
		}
		return true;
	}
	if(id != "INST" || !FurString(file, ins.name) || !file.CanRead(8 + 4 * 32))
		return false;
	ins.hasFMPatch = true;
	ins.type &= 0xff;
	const auto algorithm = file.ReadUint8(), feedback = file.ReadUint8();
	ins.patch[0] = static_cast<uint8>(((feedback & 7) << 3) | (algorithm & 7));
	file.Skip(6);
	constexpr uint8 map[] = {0, 2, 1, 3};
	for(uint8 op : map)
	{
		std::array<uint8, 32> r{};
		file.ReadArray(r);
		const auto i = 1 + op * 7;
		ins.patch[i + 0] = static_cast<uint8>((Detune[r[9] & 7] << 4) | (r[3] & 15));
		ins.patch[i + 1] = (version < 114 || r[20]) ? r[6] & 127 : 127;
		ins.patch[i + 2] = static_cast<uint8>(((r[8] & 3) << 6) | (r[1] & 31));
		ins.patch[i + 3] = static_cast<uint8>(((r[0] & 1) << 7) | (r[2] & 31));
		ins.patch[i + 4] = r[10] & 31;
		ins.patch[i + 5] = static_cast<uint8>(((r[5] & 15) << 4) | (r[4] & 15));
		ins.patch[i + 6] = (r[11] & 16) ? (r[11] & 7) | 8 : 0;
	}
	if(patchOnly)
		return true;
	if(!file.CanRead(4 + 24 + 16))
		return false;
	file.Skip(4 + 24);
	ins.sample = file.ReadInt16LE();
	file.Skip(14);
	if(!file.CanRead(68))
		return true;
	std::array<uint32, 8> counts{}, loops{};
	for(auto &count : counts)
		count = file.ReadUint32LE();
	for(auto &loop : loops)
		loop = file.ReadUint32LE();
	const auto arpFixed = file.ReadUint8();
	file.Skip(3);
	constexpr int macroMap[] = {PSGPatch::Volume, PSGPatch::Arpeggio, PSGPatch::Duty, -1, PSGPatch::Pitch, -1, -1, -1};
	for(size_t macro = 0; macro < counts.size(); macro++)
	{
		const auto count = counts[macro];
		if(count > 255 || !file.CanRead(count * 4))
			return false;
		if(ins.type == 0 && macroMap[macro] >= 0)
		{
			auto &seq = ins.psg.macros[macroMap[macro]];
			seq.loop = static_cast<uint8>(std::min<uint32>(loops[macro], 255));
			for(size_t i = 0; i < count; i++)
			{
				auto value = file.ReadInt32LE();
				if(macro == 1 && arpFixed)
					value ^= 0x40000000;
				seq.values.push_back(value);
			}
		} else if(macro == 0)
		{
			ins.volumeLoop = loops[macro];
			for(size_t i = 0; i < count; i++)
				ins.volume.push_back(file.ReadInt32LE());
		} else
		{
			unsupported += count != 0;
			file.Skip(count * 4);
		}
	}
	if(version >= 44)
	{
		std::array<uint32, 4> fmCounts{};
		size_t fmSize = 0, opSize = 0;
		for(auto &count : fmCounts)
		{
			count = file.ReadUint32LE();
			if(count > 255)
				return false;
			fmSize += count * 4;
			unsupported += count != 0;
		}
		if(!file.Skip(16 + 12 + fmSize))
			return false;
		for(size_t op = 0; op < 4; op++)
		{
			for(size_t macro = 0; macro < 12; macro++)
			{
				const auto count = file.ReadUint32LE();
				if(count > 255)
					return false;
				opSize += count;
				unsupported += count != 0;
			}
			if(!file.Skip(12 * 5))
				return false;
		}
		if(!file.Skip(opSize) || !file.CanRead(32))
			return false;
		for(size_t macro = 0; macro < 8; macro++)
		{
			const auto release = file.ReadUint32LE();
			if(ins.type == 0 && macroMap[macro] >= 0)
				ins.psg.macros[macroMap[macro]].release = static_cast<uint8>(std::min<uint32>(release, 255));
			else if(macro == 0)
				ins.volumeRelease = release;
		}
	}
	return true;
}

struct FurRow
{
	int note = -1, instrument = -1, volume = -1;
	std::array<std::pair<int16, int16>, 8> effects{};
	FurRow() { effects.fill({-1, -1}); }
};

int ConvertNote(int note)
{
	return note + 12 >= 0 && note + 12 < NOTE_MAX ? NOTE_MIN + note + 12 : NOTE_NONE;
}

bool ReadFurPattern(FileReader file, bool packed, std::vector<FurRow> &data, uint8 columns)
{
	if(!packed)
	{
		if(columns > 8 || !file.CanRead(data.size() * (8 + columns * 4)))
			return false;
		for(auto &row : data)
		{
			const auto note = file.ReadInt16LE(), octave = file.ReadInt16LE();
			if(note >= 100 && note <= 102)
				row.note = note + 80;
			else if(note != 0 || octave != 0)
				row.note = note + 12 * static_cast<int8>(octave) + 60;
			row.instrument = file.ReadInt16LE();
			row.volume = file.ReadInt16LE();
			for(size_t i = 0; i < columns; i++)
				row.effects[i] = {file.ReadInt16LE(), file.ReadInt16LE()};
		}
		return true;
	}
	std::string patternName;
	if(!FurString(file, patternName))
		return false;
	for(size_t i = 0; i < data.size(); i++)
	{
		if(!file.CanRead(1))
			return false;
		const uint8 mask = file.ReadUint8();
		if(mask == 255)
			break;
		if(mask & 128)
		{
			i += (mask & 127) + 1;
			continue;
		}
		if(!file.CanRead(((mask & 32) ? 1 : 0) + ((mask & 64) ? 1 : 0)))
			return false;
		uint16 fx = (mask >> 3) & 3;
		if(mask & 32)
			fx |= file.ReadUint8();
		if(mask & 64)
			fx |= file.ReadUint8() << 8;
		if(!file.CanRead(std::popcount(static_cast<unsigned>(mask & 7)) + std::popcount(fx)))
			return false;
		auto &row = data[i];
		if(mask & 1)
			row.note = file.ReadUint8();
		if(mask & 2)
			row.instrument = file.ReadUint8();
		if(mask & 4)
			row.volume = file.ReadUint8();
		for(size_t j = 0; j < 8; j++)
		{
			if(fx & (1 << (2 * j)))
				row.effects[j].first = file.ReadUint8();
			if(fx & (2 << (2 * j)))
				row.effects[j].second = file.ReadUint8();
		}
	}
	return true;
}

// Нулевой параметр у Furnace и у OpenMPT значит противоположное. В Furnace
// эффект держится сам по себе, пока его не остановят, и `0A00`, `0400`, `0100`
// именно останавливают: `playback.cpp` на нуле обнуляет `volSpeed`, глубину
// вибрато и скорость порта. В OpenMPT эффект действует только на своей строке,
// зато нулевой параметр означает «повторить прежний». Поэтому команда с нулём
// переводится в «ничего»: в OpenMPT остановка и есть отсутствие команды.
//
// Без этого на `knux_alt-ft-am4n` каждая нота тут же гасла: в файле стоит `0A0F`
// (скольжение вниз), а на строке с нотой `0A00`, и мы вместо остановки
// продолжали гасить. Пятнадцать пропаж звука из сорока четырёх по корпусу были
// отсюда.
//
// Так переводится только арпеджио. Всё остальное из непрерывных эффектов —
// скольжения высоты (`01`, `02`, `03`), вибрато (`04`), тремоло (`07`) и
// скольжение громкости (`0A`) — мы проигрываем по-фёрнейсовски сами, и движок
// ждёт команду с нулём как сигнал остановки. Отдать ему «ничего» значит не
// остановить ничего: вибрато качало бы дальше, а слайд уводил бы за собой все
// следующие ноты.
std::pair<EffectCommand, uint8> FurEffect(int code, uint8 value)
{
	switch(code)
	{
	case 0x00: return {value ? CMD_ARPEGGIO : CMD_NONE, value};
	case 0x01: return {CMD_PORTAMENTOUP, value};
	case 0x02: return {CMD_PORTAMENTODOWN, value};
	case 0x03: return {CMD_TONEPORTAMENTO, value};
	case 0x04: return {CMD_VIBRATO, value};
	// `05` и `06` это продолжение вибрато и порта со скольжением громкости:
	// там ноль в параметре означает «без скольжения», а не остановку.
	case 0x05: return {CMD_VIBRATOVOL, value};
	case 0x06: return {CMD_TONEPORTAVOL, value};
	case 0x07: return {CMD_TREMOLO, value};
	case 0x0a: return {CMD_VOLUMESLIDE, value};
	case 0x0b: return {CMD_POSITIONJUMP, value};
	case 0x0c: return {value && value <= 15 ? CMD_RETRIG : CMD_NONE, value};
	case 0x0d: return {CMD_PATTERNBREAK, value};
	case 0xe5: return {CMD_FINETUNE, value};
	case 0xec: return {CMD_S3MCMDEX, static_cast<uint8>(0xc0 | std::min<uint8>(value, 15))};
	case 0xed: return {CMD_S3MCMDEX, static_cast<uint8>(0xd0 | std::min<uint8>(value, 15))};
	default: return {CMD_NONE, 0};
	}
}

// `dacScale` включает логарифмический закон ЦАПа: там макрос громкости идёт по
// шкале 0-64, а сам ЦАП работает в 0-127, и Furnace переводит одну в другую
// прибавлением 63 (`genesis.cpp`: `if (chan[i].dacMode && inVol>0) inVol+=63;`).
// То есть значение 64 это полная громкость, а не половина, и дальше работает та
// же логарифмическая таблица с шагом 0,75 дБ, что и у громкости ноты.
void SetFurEnvelope(ModInstrument &ins, const FurInstrument &fur, int maxVolume, bool dacScale = false)
{
	if(fur.volume.empty())
		return;
	auto &env = ins.VolEnv;
	env.dwFlags.set(ENV_ENABLED);
	const size_t count = std::min<size_t>(fur.volume.size(), 240);
	if(fur.volumeDelay)
		env.push_back(0, 64);
	const uint8 offset = fur.volumeDelay ? 1 : 0;
	for(size_t i = 0; i < count; i++)
	{
		uint8 value;
		if(dacScale)
		{
			const int level = std::clamp<int32>(fur.volume[i], 0, 64);
			const double amplitude = level > 0 ? std::pow(10.0, (level - 64) * 0.75 / 20.0) : 0.0;
			value = static_cast<uint8>(std::clamp<int>(mpt::saturate_round<int>(64.0 * amplitude), 0, 64));
		} else
		{
			value = static_cast<uint8>(std::clamp<int32>(fur.volume[i], 0, maxVolume) * 64 / maxVolume);
		}
		env.push_back(static_cast<uint16>(fur.volumeDelay + i * fur.volumeStep), value);
	}
	if(fur.volumeRelease < count)
	{
		env.nSustainStart = static_cast<uint8>((fur.volumeLoop < fur.volumeRelease ? fur.volumeLoop : fur.volumeRelease) + offset);
		env.nSustainEnd = static_cast<uint8>(fur.volumeRelease + offset);
		env.dwFlags.set(ENV_SUSTAIN);
	} else if(fur.volumeLoop < count)
	{
		env.nLoopStart = static_cast<uint8>(fur.volumeLoop + offset);
		env.nLoopEnd = env.LastPoint();
		env.dwFlags.set(ENV_LOOP);
	} else
	{
		// Макрос без петли и без release в Furnace не кончается: он доходит до
		// последнего значения и держит его, пока нота звучит. Огибающая OpenMPT
		// ведёт себя иначе — дойдя до конца, она по правилам IT включает Fade
		// Out, и нота плавно гаснет за пару секунд. На `life will go on` это и
		// давало «паузы»: у инструмента 19 макрос это одно значение 118, нота
		// уходила на −50 дБ за две секунды, хотя Furnace держал уровень до
		// следующей ноты. Поэтому на последнюю точку ставится sustain: пока
		// нота держится, уровень стоит, а на `===` sustain отпускается,
		// огибающая доходит до конца и нота гаснет, как и положено.
		env.nSustainStart = env.nSustainEnd = env.LastPoint();
		env.dwFlags.set(ENV_SUSTAIN);
	}
}

bool LoadFurSample(ModSample &sample, FileReader data, bool modern, std::string &name, bool decode)
{
	if(!FurString(data, name) || !data.CanRead(modern ? 40 : 20))
		return false;
	const auto length = data.ReadUint32LE();
	auto rate = data.ReadUint32LE();
	if(modern)
		rate = data.ReadUint32LE();
	else
		data.Skip(4);
	const uint8 depth = data.ReadUint8(), loopMode = data.ReadUint8();
	if(modern)
		data.Skip(2);
	else
		rate = data.ReadUint16LE();
	const auto begin = data.ReadInt32LE();
	const auto end = modern ? data.ReadInt32LE() : static_cast<int32>(length);
	if(modern)
		data.Skip(16);
	if(depth != 8 && depth != 16)
		return false;
	const size_t width = depth == 16 ? 2 : 1;
	if(length > MaxFurBytes / width || !data.CanRead(length * width))
		return false;
	sample.Initialize(MOD_TYPE_MPT);
	// Громкость канала у Furnace одинаково живёт у всех чипов, сэмплерных тоже:
	// нота без громкости наследует текущую. Без этого флага на сэмплах каждая
	// нота возвращала бы громкость к умолчанию сэмпла. На athletic rrt, где два
	// YMZ280B, из-за этого вылезло 88 лишних расхождений: флаг стоял только у
	// FM и PSG, а подстановку громкости я убрал у всех разом.
	sample.uFlags.set(SMP_NODEFAULTVOLUME);
	sample.nC5Speed = rate ? rate : 8363;
	sample.nLength = length;
	if(begin >= 0 && end > begin && static_cast<uint32>(end) <= length && loopMode != 1)
	{
		sample.nLoopStart = begin;
		sample.nLoopEnd = end;
		sample.uFlags.set(CHN_LOOP);
		sample.uFlags.set(CHN_PINGPONGLOOP, loopMode == 2);
	}
	if(decode)
		SampleIO(depth == 16 ? SampleIO::_16bit : SampleIO::_8bit, SampleIO::mono, SampleIO::littleEndian, SampleIO::signedPCM).ReadSample(sample, data);
	return true;
}
} // namespace

#ifdef MODPLUG_TRACKER

YM2612PresetFormat GetYM2612PresetFormat(std::string_view extension)
{
	if(!mpt::CompareNoCaseAscii(extension, ".fui")) return YM2612PresetFormat::Furnace;
	if(!mpt::CompareNoCaseAscii(extension, ".dmp")) return YM2612PresetFormat::DMP;
	if(!mpt::CompareNoCaseAscii(extension, ".tfi")) return YM2612PresetFormat::TFI;
	if(!mpt::CompareNoCaseAscii(extension, ".opni")) return YM2612PresetFormat::OPNI;
	return YM2612PresetFormat::None;
}


std::optional<YM2612Preset> ReadYM2612Preset(FileReader file, YM2612PresetFormat format)
{
	YM2612Preset preset{};
	// These formats store slots in register order (1, 3, 2, 4).
	constexpr uint8 operatorOrder[] = {0, 2, 1, 3};

	switch(format)
	{
	case YM2612PresetFormat::Furnace:
		{
			std::string id;
			if(file.ReadMagic("FINS") || file.ReadMagic("FINB"))
			{
				id = "INS2";
			} else if(file.ReadMagic("-Furnace instr.-") && file.Skip(4) && file.CanRead(4))
			{
				const auto offset = file.ReadUint32LE();
				file = FurBlock(file, offset, id, true);
			} else
			{
				return std::nullopt;
			}
			FurInstrument instrument;
			size_t unsupported = 0;
			if(!ReadFurInstrument(file, id, instrument, unsupported, true) || instrument.type != 1 || !instrument.hasFMPatch)
				return std::nullopt;
			preset.patch = instrument.patch;
			preset.name = std::move(instrument.name);
		}
		break;

	case YM2612PresetFormat::DMP:
		{
			if(file.GetLength() != 51 || file.ReadUint8() != 11)
				return std::nullopt;
			const auto system = file.ReadUint8();
			if((system != 2 && system != 9) || file.ReadUint8() != 1)
				return std::nullopt;
			file.Skip(1);  // FMS
			const auto feedback = file.ReadUint8(), algorithm = file.ReadUint8();
			preset.patch[0] = static_cast<uint8>(((feedback & 7) << 3) | (algorithm & 7));
			file.Skip(1);  // AMS
			for(auto op : operatorOrder)
			{
				const auto reg = file.ReadArray<uint8, 11>();
				const auto i = 1 + op * 7;
				preset.patch[i + 0] = static_cast<uint8>((Detune[reg[8] & 7] << 4) | (reg[0] & 15));
				preset.patch[i + 1] = reg[1] & 127;
				preset.patch[i + 2] = static_cast<uint8>(((reg[7] & 3) << 6) | (reg[2] & 31));
				preset.patch[i + 3] = static_cast<uint8>(((reg[6] & 1) << 7) | (reg[3] & 31));
				preset.patch[i + 4] = reg[9] & 31;
				preset.patch[i + 5] = static_cast<uint8>(((reg[4] & 15) << 4) | (reg[5] & 15));
				preset.patch[i + 6] = reg[10] & 15;
			}
		}
		break;

	case YM2612PresetFormat::TFI:
		{
			if(file.GetLength() != 42)
				return std::nullopt;
			const auto algorithm = file.ReadUint8(), feedback = file.ReadUint8();
			if(algorithm > 7 || feedback > 7)
				return std::nullopt;
			preset.patch[0] = static_cast<uint8>((feedback << 3) | algorithm);
			for(auto op : operatorOrder)
			{
				const auto reg = file.ReadArray<uint8, 10>();
				const auto i = 1 + op * 7;
				preset.patch[i + 0] = static_cast<uint8>((Detune[reg[1] & 7] << 4) | (reg[0] & 15));
				preset.patch[i + 1] = reg[2] & 127;
				preset.patch[i + 2] = static_cast<uint8>(((reg[3] & 3) << 6) | (reg[4] & 31));
				preset.patch[i + 3] = reg[5] & 31;
				preset.patch[i + 4] = reg[6] & 31;
				preset.patch[i + 5] = static_cast<uint8>(((reg[8] & 15) << 4) | (reg[7] & 15));
				preset.patch[i + 6] = reg[9] & 15;
			}
		}
		break;

	case YM2612PresetFormat::OPNI:
		{
			const bool versioned = file.ReadMagic("WOPN2-IN2T");
			if((!versioned && !file.ReadMagic("WOPN2-INST")) || !file.CanRead(1) || file.ReadUint8() != 0)
				return std::nullopt;
			if(versioned && file.ReadUint16LE() != 2)
				return std::nullopt;
			if(!file.CanRead(1 + 32 + 3 + 2 + 28))
				return std::nullopt;
			file.Skip(1);  // Percussion flag
			file.ReadString<mpt::String::spacePadded>(preset.name, 32);
			file.Skip(3);  // MIDI note offset and percussion key
			preset.patch[0] = file.ReadUint8() & 63;
			file.Skip(1);  // LFO sensitivity
			constexpr uint8 masks[] = {0x7f, 0x7f, 0xdf, 0x9f, 0x1f, 0xff, 0x0f};
			for(auto op : operatorOrder)
				for(size_t reg = 0; reg < std::size(masks); reg++)
					preset.patch[1 + op * 7 + reg] = file.ReadUint8() & masks[reg];
		}
		break;

	default:
		return std::nullopt;
	}
	return preset;
}

#endif // MODPLUG_TRACKER

CSoundFile::ProbeResult CSoundFile::ProbeFileHeaderFUR(MemoryFileReader file, const uint64 *)
{
	if(!file.CanRead(16))
		return ProbeWantMoreData;
	if(file.ReadMagic(FurMagic))
		return ProbeSuccess;
	file.Rewind();
#ifdef MPT_WITH_ZLIB
	std::vector<uint8> prefix;
	if(IsZlib(file))
	{
		if(InflateFur(file, prefix, true))
			return ProbeSuccess;
		if(file.GetLength() < 64)
			return ProbeWantMoreData;
	}
#endif
	return ProbeFailure;
}

bool CSoundFile::ReadFUR(FileReader &source, ModLoadingFlags flags)
{
	source.Rewind();
	FileReader file = source;
	std::vector<uint8> unpacked;
	if(!file.ReadMagic(FurMagic))
	{
#ifdef MPT_WITH_ZLIB
		if(!InflateFur(source, unpacked, flags == onlyVerifyHeader))
			return false;
		file = FileReader(mpt::as_span(unpacked));
		if(!file.ReadMagic(FurMagic))
			return false;
#else
		return false;
#endif
	}
	if(flags == onlyVerifyHeader)
		return true;
	if(!file.CanRead(16))
		return false;
	const uint16 version = file.ReadUint16LE();
	file.Skip(2);
	const auto infoPointer = file.ReadUint32LE();
	std::string id;
	auto info = FurBlock(file, infoPointer, id);
	if(version < 95 || version >= 240 || id != "INFO" || !info.CanRead(248))
		return false;
	const unsigned timeBase = info.ReadUint8() + 1u;
	const unsigned firstSpeed = info.ReadUint8(), secondSpeed = info.ReadUint8();
	info.Skip(1);
	const double tickRate = info.ReadFloatLE();
	const ROWINDEX rows = info.ReadUint16LE();
	const ORDERINDEX orders = info.ReadUint16LE();
	const auto highlight1 = info.ReadUint8(), highlight2 = info.ReadUint8();
	const auto instrumentCount = info.ReadUint16LE(), waveCount = info.ReadUint16LE(), sampleCount = info.ReadUint16LE();
	const auto patternCount = info.ReadUint32LE();
	if(!rows || rows > 256 || !orders || orders > 256 || instrumentCount > 255 || sampleCount > 256 || waveCount > 256
		|| patternCount > 65536 || !std::isfinite(tickRate) || tickRate <= 0)
		return false;

	std::vector<FurVoice> voices;
	std::vector<std::pair<uint8, uint8>> psgLanes;
	std::vector<uint8> chipSlots;
	std::vector<std::string> channelNames;
	for(size_t i = 0; i < 32; i++)
	{
		const auto chip = info.ReadUint8();
		uint8 channels = 0;
		const char *name = "";
		FurVoice voice = FurVoice::PCM;
		switch(chip)
		{
		case 0x83: channels = 6; name = "YM2612"; voice = FurVoice::OPN2; break;
		case 0x03: channels = 4; name = "PSG"; voice = FurVoice::PSG; break;
		case 0x82: channels = 8; name = "YM2151"; voice = FurVoice::OPM; break;
		case 0xb8: channels = 8; name = "YMZ280B"; break;
		case 0: break;
		default: return false;
		}
		if(channels)
			chipSlots.push_back(static_cast<uint8>(i));
		for(uint8 c = 0; c < channels; c++)
		{
			const bool noise = voice == FurVoice::PSG && c == 3;
			voices.push_back(noise ? FurVoice::Noise : voice);
			psgLanes.emplace_back(static_cast<uint8>(i), c);
			channelNames.push_back(noise ? "PSG Noise" : std::string(name) + " " + std::to_string(c + 1));
		}
	}
	if(voices.empty() || voices.size() + 1 > MAX_BASECHANNELS)
		return false;
	std::array<uint16, 32> chipGains{};
	for(auto &gain : chipGains)
		gain = static_cast<uint16>(std::abs(static_cast<int>(info.ReadInt8())) * 4);
	if(!info.Skip(32))
		return false;
	std::array<uint32, 32> chipFlags{};
	for(auto &offset : chipFlags)
		offset = info.ReadUint32LE();
	std::array<bool, 32> noEasyNoise{}, noPhaseReset{};
	if(version >= 119)
	{
		for(size_t chip = 0; chip < chipFlags.size(); chip++)
		{
			if(!chipFlags[chip])
				continue;
			auto data = FurBlock(file, chipFlags[chip], id);
			if(id != "FLAG")
				continue;
			std::string settings;
			FurString(data, settings);
			noEasyNoise[chip] = settings.find("noEasyNoise=true") != std::string::npos;
			noPhaseReset[chip] = settings.find("noPhaseReset=true") != std::string::npos;
		}
	}
	std::string title, author;
	if(!FurString(info, title) || !FurString(info, author) || !info.Skip(4))
		return false;
	std::array<uint8, 20> compat{};
	if(!info.ReadArray(compat))
		return false;
	std::vector<uint32> instrumentOffsets, waveOffsets, sampleOffsets, patternOffsets;
	if(!ReadPointers(info, instrumentOffsets, instrumentCount) || !ReadPointers(info, waveOffsets, waveCount)
		|| !ReadPointers(info, sampleOffsets, sampleCount) || !ReadPointers(info, patternOffsets, patternCount))
		return false;
	std::vector<uint8> arrangement, columns;
	if(!info.ReadVector(arrangement, voices.size() * orders) || !info.ReadVector(columns, voices.size()))
		return false;
	if(!info.Skip(voices.size() * 2))
		return false;
	for(auto &name : channelNames)
	{
		std::string custom;
		if(!FurString(info, custom))
			return false;
		if(!custom.empty())
			name = std::move(custom);
	}
	for(size_t i = 0; i < voices.size(); i++)
	{
		std::string shortName;
		if(!FurString(info, shortName))
			return false;
	}
	std::string comment;
	FurString(info, comment);
	const float master = info.ReadFloatLE();
	std::array<uint8, 28> extraCompat{};
	info.ReadArray(extraCompat);
	const auto virtualNumerator = info.ReadUint16LE(), virtualDenominator = info.ReadUint16LE();
	std::string subsongName, subsongComment;
	FurString(info, subsongName);
	FurString(info, subsongComment);
	const uint8 extraSubsongs = info.ReadUint8();
	info.Skip(3 + 4 * extraSubsongs);
	if(version >= 103)
		for(int i = 0; i < 6; i++)
		{
			std::string metadata;
			FurString(info, metadata);
		}
	std::vector<uint8> speeds{static_cast<uint8>(std::max(1u, firstSpeed)), static_cast<uint8>(std::max(1u, secondSpeed))};
	// INFO includes a speed sequence and groove table in newer Furnace revisions.
	if(version >= 135)
	{
		if(!info.CanRead(12 * chipSlots.size()))
			return false;
		for(uint8 slot : chipSlots)
		{
			const auto gain = info.ReadFloatLE();
			if(std::isfinite(gain))
				chipGains[slot] = static_cast<uint16>(std::lround(std::clamp(std::abs(gain), 0.0f, 2.0f) * 256.0f));
			info.Skip(8);
		}
		const auto patchCount = info.ReadUint32LE();
		if(patchCount > info.BytesLeft() / 4 || !info.Skip(patchCount * 4u))
			return false;
	}
	if(version >= 136) info.Skip(1);
	if(version >= 138 && !info.Skip(8))
		return false;
	std::vector<std::vector<uint8>> grooves;
	if(version >= 139)
	{
		const auto count = info.ReadUint8();
		std::array<uint8, 16> sequence{};
		if(!count || count > sequence.size() || !info.ReadArray(sequence) || !info.CanRead(1))
			return false;
		speeds.assign(sequence.begin(), sequence.begin() + count);
		const auto grooveCount = info.ReadUint8();
		for(size_t i = 0; i < grooveCount; i++)
		{
			const auto length = info.ReadUint8();
			if(!length || length > sequence.size() || !info.ReadArray(sequence))
				return false;
			grooves.emplace_back(sequence.begin(), sequence.begin() + length);
		}
	}
	// Furnace держит набор скоростей и чередует их по строкам, а эффекты 09xx и
	// 0Fxx меняют их по отдельности. Если песня начинается с равных скоростей,
	// а эффект потом их разводит, чередование обязан вести служебный канал: без
	// него импорт ставит на строку одну скорость, и темп уезжает на всю песню.
	// Поэтому паттерны просматриваются заранее, до выбора числа каналов.
	bool speedEffects = false;
	// Шкала громкости у Furnace задаётся каналом, а не инструментом: у YM2612
	// она 0-127, у YMZ280B 0-255, у PSG 0-15. Макрос громкости живёт в
	// инструменте, поэтому его шкалу можно узнать только по тому, на каком
	// канале инструмент играет. Запоминаем это здесь же, заодно с просмотром
	// на эффекты скорости: второй проход по паттернам не нужен.
	std::vector<uint8> instrumentVoice(instrumentCount, 0xff);
	// Канал ЦАПа живёт по своим правилам громкости, поэтому отмечается отдельно.
	std::vector<bool> instrumentOnDac(instrumentCount, false);
	if(flags & loadPatternData)
	{
		std::vector<FurRow> scan;
		for(auto offset : patternOffsets)
		{
			std::string blockID;
			auto block = FurBlock(file, offset, blockID);
			const bool packed = blockID == "PATN";
			if(!packed && blockID != "PATR")
				continue;
			const auto subsong = packed ? block.ReadUint8() : 0;
			const uint16 channel = packed ? block.ReadUint8() : block.ReadUint16LE();
			block.Skip(2);
			const uint16 oldSubsong = packed ? 0 : block.ReadUint16LE();
			if(!packed)
				block.Skip(2);
			if(subsong || oldSubsong || channel >= voices.size())
				continue;
			scan.assign(rows, FurRow{});
			if(!ReadFurPattern(block, packed, scan, columns[channel]))
				continue;
			for(const auto &row : scan)
			{
				if(row.instrument >= 0 && static_cast<size_t>(row.instrument) < instrumentVoice.size())
				{
					if(instrumentVoice[row.instrument] == 0xff)
						instrumentVoice[row.instrument] = static_cast<uint8>(voices[channel]);
					if(voices[channel] == FurVoice::OPN2 && channel % 6 == 5)
						instrumentOnDac[row.instrument] = true;
				}
				for(const auto &[code, value] : row.effects)
				{
					if(code == 0x09 || code == 0x0f)
						speedEffects = true;
				}
			}
			if(speedEffects)
				break;
		}
	}
	// Поведение задержки ноты Furnace выбирает по файлу: 0 отбрасывает задержку
	// от скорости и больше, 1 только больше скорости, 2 не отбрасывает ничего.
	// У версий ниже 110 он принудительно ставит 1, у остальных читает байт из
	// блока совместимости. Отброшенная задержка не роняет ноту: она играет на
	// своей строке сразу, потому что Furnace просто не откладывает строку.
	const uint8 delayBehavior = (version >= 110) ? extraCompat[22] : 1;
	const bool timing = !grooves.empty() || speedEffects
		|| std::adjacent_find(speeds.begin(), speeds.end(), std::not_equal_to<uint8>()) != speeds.end();
	InitializeGlobals(MOD_TYPE_MPT, static_cast<CHANNELINDEX>(voices.size() + (timing ? 1 : 0)));
	m_SongFlags.set(SONG_IMPORTED | SONG_LINEARSLIDES);
	// Громкость микросхем из chipGains здесь намеренно не применяется. Furnace
	// уже учитывает её в выходе самого чипа, поэтому второй раз её накручивать
	// нельзя. Измерено на раздельном экспорте Furnace: без неё партия YM2612
	// совпадала с эталоном на +0,1 дБ в `the toilet lock` и +1,6 дБ в
	// `mushroom hill alt`, а с ней уходила на −5,5 и −3,8 дБ соответственно.
	// Уровни m_chipVolume остаются единичными и служат поправкой микшера.
	// Legacy partial and hardware-period slides use chip-specific units.
	if(compat[1] == 2)
	{
		m_furnaceSlides = extraCompat[13] ? extraCompat[13] : 4;
		if(compat[0]) m_furnaceSlides |= FurnaceLimitSlides;
		if(compat[8]) m_furnaceSlides |= FurnaceStopOnNoteOff;
		if(compat[9]) m_furnaceSlides |= FurnaceStopAtTarget;
		if(extraCompat[1]) m_furnaceSlides |= FurnaceSkipFirstTick;
		if(extraCompat[4]) m_furnaceSlides |= FurnaceIgnoreSameNote;
	}
	m_playBehaviour = GetDefaultPlaybackBehaviour(MOD_TYPE_MPT);
	// E5xx у Furnace — не разовая правка ноты, а подстройка канала.
	m_playBehaviour.set(kFurnacePersistentFinetune);
	Order().SetDefaultTempo(TEMPO(std::clamp(tickRate * 2.5 * (virtualNumerator && virtualDenominator ? double(virtualNumerator) / virtualDenominator : 1.0), 32.0, 512.0)));
	Order().SetDefaultSpeed(std::clamp<unsigned>(speeds.front() * timeBase, 1, 255));
	if(highlight1 && highlight2 >= highlight1)
	{
		m_nDefaultRowsPerBeat = highlight1;
		m_nDefaultRowsPerMeasure = highlight2;
	}
	m_songName = title;
	m_songArtist = mpt::ToUnicode(mpt::Charset::UTF8, author);
	m_songMessage.Read(reinterpret_cast<const std::byte *>(comment.data()), comment.size(), SongMessage::leAutodetect);
	if(std::isfinite(master) && master >= 0)
		m_nDefaultGlobalVolume = static_cast<uint16>(std::clamp(master, 0.0f, 1.0f) * MAX_GLOBAL_VOLUME);
#ifdef MODPLUG_TRACKER
	// Цвет шапки канала по микросхеме. Радужная раскладка OpenMPT красит каналы
	// по именам, а у нас все имена разные, поэтому получался бледный градиент,
	// в котором соседние чипы не отличить. Здесь каждое семейство берёт свой
	// цвет, а каналы внутри семейства — свой оттенок, чтобы соседей было видно.
	{
		const auto chipColor = [](FurVoice voice) -> std::array<int, 3>
		{
			switch(voice)
			{
			case FurVoice::OPN2:  return {0xE0, 0x6A, 0x5A};  // YM2612 — тёплый красный
			case FurVoice::OPM:   return {0xC8, 0x82, 0xE8};  // YM2151 — сиреневый
			case FurVoice::PSG:   return {0x6A, 0xC8, 0x7A};  // SN76489 — зелёный
			case FurVoice::Noise: return {0x8E, 0xA6, 0xC8};  // шум — стальной
			case FurVoice::PCM:   return {0x58, 0xC0, 0xDC};  // сэмплы — голубой
			}
			return {0xB0, 0xB0, 0xB0};
		};
		std::array<int, 5> seen{};
		for(CHANNELINDEX c = 0; c < voices.size(); c++)
		{
			const auto voice = voices[c];
			const auto base = chipColor(voice);
			const size_t index = static_cast<size_t>(voice) < seen.size() ? static_cast<size_t>(voice) : 0;
			const int step = seen[index]++ % 3;
			const auto shade = [step](int v) { return static_cast<uint32>(std::clamp(v - step * 22, 0, 255)); };
			ChnSettings[c].color = shade(base[0]) | (shade(base[1]) << 8) | (shade(base[2]) << 16);
		}
		if(timing)
			ChnSettings[static_cast<CHANNELINDEX>(voices.size())].color = 0x909090;
	}
#endif  // MODPLUG_TRACKER
	for(CHANNELINDEX c = 0; c < voices.size(); c++)
		ChnSettings[c].szName = channelNames[c];
	if(timing)
		ChnSettings[static_cast<CHANNELINDEX>(voices.size())].szName = "Furnace timing";

	size_t unsupported = 0;
	for(SAMPLEINDEX i = 0; i < sampleOffsets.size(); i++)
	{
		auto data = FurBlock(file, sampleOffsets[i], id);
		std::string name;
		if((id != "SMP2" && id != "SMPL") || !LoadFurSample(Samples[i + 1], data, id == "SMP2", name, (flags & loadSampleData) != 0))
			return false;
		m_szNames[i + 1] = name;
	}
	m_nSamples = sampleCount;

	std::vector<FurInstrument> furInstruments(instrumentCount);
	std::map<std::pair<CHANNELINDEX, int>, INSTRUMENTINDEX> psgInstruments;
	std::vector<bool> usedPSGInstrument(instrumentCount);
	std::vector<INSTRUMENTINDEX> rawSamples(sampleCount);
	m_nInstruments = instrumentCount;
	const auto createInstrument = [&](INSTRUMENTINDEX index, SAMPLEINDEX smp, const std::string &name) -> ModInstrument *
	{
		auto *ins = AllocateInstrument(index, smp);
		if(ins)
		{
			ins->name = name;
			ins->midiPWD = 1;  // Furnace E5xx changes pitch in 1/128-semitone steps.
		}
		return ins;
	};
	for(INSTRUMENTINDEX i = 0; i < instrumentCount; i++)
	{
		auto data = FurBlock(file, instrumentOffsets[i], id);
		auto &fur = furInstruments[i];
		if(!ReadFurInstrument(data, id, fur, unsupported))
			return false;
		SAMPLEINDEX mappedSample = 0;
		if(fur.type == 1 || fur.type == 33)
		{
			mappedSample = ++m_nSamples;
			auto &sample = Samples[mappedSample];
			sample.Initialize(MOD_TYPE_MPT);
			sample.SetAdlib(true);
			sample.isOPN2 = true;
			// Нота не задаёт громкость канала заново: у Furnace она живёт в
			// канале и переживает ноты, а скольжение её двигает. Без этого
			// флага каждая нота возвращала бы громкость к умолчанию сэмпла и
			// затирала всё, что набрало скольжение.
			sample.uFlags.set(SMP_NODEFAULTVOLUME);
			sample.isOPM = fur.type == 33;
			sample.opn2 = fur.patch;
			m_szNames[mappedSample] = fur.name;
		} else if(fur.type == 4 || fur.type == 41)
		{
			if(fur.sample >= 0 && fur.sample < sampleCount)
				mappedSample = static_cast<SAMPLEINDEX>(fur.sample + 1);
		} else if(fur.type != 0)
		{
			unsupported++;
		}
		auto *ins = createInstrument(i + 1, mappedSample, fur.name);
		if(!ins)
			return false;
		// После note off хвост ноты у синтезных чипов отпускает сам чип своим
		// release. OpenMPT же вдобавок гасит канал fade out, и это слышно: в
		// логе регистров видно, как TL несущих продолжает расти уже после
		// снятия ключа, тогда как Furnace регистры не трогает. Поэтому
		// инструментам без сэмпла fade out снимается. У сэмплерных он остаётся:
		// там note off обрывает голос, и без затухания сэмпл звучал бы дальше.
		if(fur.type == 0 || fur.type == 1 || fur.type == 33)
			ins->nFadeOut = 0;
		// Шкала берётся у канала, на котором инструмент играет; если он нигде не
		// встретился, остаётся догадка по типу инструмента.
		int envelopeMax = fur.type == 0 ? 15 : (fur.type == 1 || fur.type == 33) ? 127 : 255;
		if(i < instrumentVoice.size() && instrumentVoice[i] != 0xff)
		{
			switch(static_cast<FurVoice>(instrumentVoice[i]))
			{
			case FurVoice::PSG:
			case FurVoice::Noise: envelopeMax = 15; break;
			case FurVoice::PCM: envelopeMax = 255; break;
			default: envelopeMax = 127; break;
			}
		}
		SetFurEnvelope(*ins, fur, envelopeMax, i < instrumentOnDac.size() && instrumentOnDac[i]);
		if(fur.mapped)
		{
			for(size_t n = 0; n < fur.noteMap.size() && n + 12 < ins->Keyboard.size(); n++)
			{
				const auto [note, smp] = fur.noteMap[n];
				ins->Keyboard[n + 12] = (smp >= 0 && smp < sampleCount) ? static_cast<SAMPLEINDEX>(smp + 1) : 0;
				if(version >= 152 && note >= 0 && note < 108)
					ins->NoteMap[n + 12] = static_cast<ModCommand::NOTE>(ConvertNote(note));
			}
		}
	}

	const auto assignPSG = [&](CHANNELINDEX channel, int furIns) -> INSTRUMENTINDEX
	{
		auto &mapped = psgInstruments[{channel, furIns}];
		if(mapped)
			return mapped;
		const bool realInstrument = furIns >= 0 && furInstruments[furIns].type == 0;
		const bool reuse = realInstrument && !usedPSGInstrument[furIns];
		if((!reuse && m_nInstruments >= 255) || m_nSamples >= MAX_SAMPLES - 1)
			return 0;
		const auto [chip, lane] = psgLanes[channel];
		auto patch = realInstrument ? std::make_shared<PSGPatch>(furInstruments[furIns].psg) : std::make_shared<PSGPatch>();
		patch->chip = chip;
		patch->lane = lane;
		// Громкость микросхемы сюда больше не кладётся: Furnace уже учёл её в
		// выходе чипа, и прежний импорт накручивал её второй раз. Измерено на
		// раздельном экспорте: PSG был тише эталона на 10,9 дБ при громкости
		// чипа 21 (что даёт −9,7 дБ), на 8,1 дБ при той же 21 и на 7,0 дБ при
		// громкости 32 (что даёт −6,0 дБ). Дефицит следовал за громкостью чипа,
		// то есть и был этим двойным применением. Поле остаётся в патче ради
		// старых .fmt, где оно уже записано.
		patch->mixGain = ChipVolumeUnity;
		patch->easyPitch = !noEasyNoise[chip];
		patch->resetNoise = !noPhaseReset[chip];
		patch->furnacePitch = true;
		const auto smp = ++m_nSamples;
		auto &sample = Samples[smp];
		sample.Initialize(MOD_TYPE_MPT);
		sample.SetAdlib(true);
		sample.uFlags.set(SMP_NODEFAULTVOLUME);
		sample.psg = std::move(patch);
		const std::string name = realInstrument ? furInstruments[furIns].name : "SN76489";
		m_szNames[smp] = name + (lane == 3 ? " (noise)" : " (tone " + std::to_string(lane + 1) + ")");
		if(reuse)
		{
			mapped = static_cast<INSTRUMENTINDEX>(furIns + 1);
			Instruments[mapped]->Keyboard.fill(smp);
			usedPSGInstrument[furIns] = true;
		} else
		{
			mapped = ++m_nInstruments;
			if(!createInstrument(mapped, smp, name))
				return 0;
		}
		return mapped;
	};

	if(flags & loadPatternData)
	{
		std::map<std::pair<uint16, uint16>, std::vector<FurRow>> patterns;
		for(auto offset : patternOffsets)
		{
			auto pattern = FurBlock(file, offset, id);
			const bool packed = id == "PATN";
			if(!packed && id != "PATR")
				return false;
			const auto subsong = packed ? pattern.ReadUint8() : 0;
			const uint16 channel = packed ? pattern.ReadUint8() : pattern.ReadUint16LE();
			const uint16 index = pattern.ReadUint16LE();
			const uint16 oldSubsong = packed ? 0 : pattern.ReadUint16LE();
			if(!packed) pattern.Skip(2);
			if(subsong || oldSubsong)
				continue;
			if(channel >= voices.size() || index >= 256)
				return false;
			auto &data = patterns[{channel, index}];
			data.resize(rows);
			if(!ReadFurPattern(pattern, packed, data, columns[channel]))
				return false;
		}
		std::vector<int> currentInstrument(voices.size(), -1);
		std::vector<unsigned> bank(voices.size(), 0);
		std::vector<bool> legacyDAC(voices.size(), false);
		std::vector<int> lastInstrument(voices.size(), 0);
		// Громкость канала в Furnace живёт сама по себе: note on её не трогает,
		// нота звучит тем уровнем, что стоит на канале сейчас, а меняют его
		// отдельные строки с одной только громкостью. OpenMPT же на каждой новой
		// ноте ставит громкость сэмпла, то есть полную, и после тихого места
		// нота прорывается громкой. Поэтому текущая громкость канала ведётся
		// здесь и выписывается в колонку громкости на каждой ноте, у которой
		// своей громкости нет. Нот без громкости в треках кратно больше, чем с
		// ней: на `mushroom hill alt 2` первый канал даёт 23 ноты с громкостью
		// против 342 без.
		// Громкость канала считается здесь же и проставляется каждой ноте явно.
		// У Furnace нота без громкости играет на текущей громкости канала, и
		// пустая колонка значит «как было». Так точнее по оригиналу, но модуль
		// перестаёт описывать сам себя: глядя на строку, нельзя сказать, как
		// громко она прозвучит, надо знать всю историю канала. Это дважды
		// подводило — при перемотке и при подстановке, — и мешает править
		// вручную. Поэтому импорт ведёт громкость сам, теми же единицами и той
		// же арифметикой, что и движок (0-256 с дробным остатком), и пишет её в
		// колонку. Повторно используемых паттернов у нас нет: на каждую позицию
		// в порядке создаётся свой, поэтому записанное число верно всегда.
		std::vector<int> currentVolume(voices.size(), 256);
		std::vector<int> volumeFraction(voices.size(), 0);
		std::vector<uint8> volumeSlide(voices.size(), 0);
		Order().resize(orders);
		size_t phase = 0;
		for(ORDERINDEX order = 0; order < orders; order++)
		{
			const auto pat = Patterns.InsertAny(rows);
			if(pat == PATTERNINDEX_INVALID)
				return false;
			Order()[order] = pat;
			auto &pattern = Patterns[pat];
			// Ноты, чья задержка длиннее строки, переезжают на ту строку, где
			// они и прозвучат. Раскладываем их не сразу, а после прохода:
			// целевую строку цикл ещё не видел и затёр бы перенесённую ноту.
			std::vector<std::tuple<ROWINDEX, ROWINDEX, CHANNELINDEX, ModCommand>> delayed;
			for(ROWINDEX row = 0; row < rows; row++, phase++)
			{
				for(CHANNELINDEX c = 0; c < voices.size(); c++)
				{
					const auto p = patterns.find({c, arrangement[c * orders + order]});
					if(p == patterns.end())
						continue;
					const auto &sourceRow = p->second[row];
					auto &cell = *pattern.GetpModCommand(row, c);
					const auto voice = voices[c];
					if(sourceRow.instrument >= 0 && sourceRow.instrument < instrumentCount)
						currentInstrument[c] = sourceRow.instrument;
					const int furIns = currentInstrument[c];
					const bool dacChannel = voice == FurVoice::OPN2 && c % 6 == 5;
					// Канал 6 бывает и обычным FM-каналом: сэмпл играет либо старый
					// режим DAC, либо сэмплерный инструмент на нём.
					const bool dacNote = dacChannel && (legacyDAC[c] || (furIns >= 0
						&& (furInstruments[furIns].type == 4 || furInstruments[furIns].type == 41)));
					INSTRUMENTINDEX ins = furIns >= 0 ? static_cast<INSTRUMENTINDEX>(furIns + 1) : 0;
					const int note = sourceRow.note;
					if(note >= 0 && note < 180)
						cell.note = static_cast<ModCommand::NOTE>(ConvertNote(note - 60));
					else if(note == 180 || note == 181)
						// В Furnace и note off, и note release снимают ключ, то есть
						// отпускают огибающую: у YM2612 команда ставит keyOff, а не
						// глушит канал. Поэтому обоим соответствует `===`, а не `^^^`.
						// Разница между ними только в освобождении макросов.
						cell.note = NOTE_KEYOFF;
					else if(note == 182)
						unsupported++;
					const bool psg = voice == FurVoice::PSG || voice == FurVoice::Noise;
					const bool noiseMode = voice == FurVoice::Noise && std::any_of(sourceRow.effects.begin(), sourceRow.effects.end(),
						[](const auto &effect) { return effect.first == 0x20; });
					if(psg && (sourceRow.instrument >= 0 || cell.IsNote() || noiseMode))
						ins = assignPSG(c, furIns);
					const int maxVolume = voice == FurVoice::PSG || voice == FurVoice::Noise ? 15 : voice == FurVoice::PCM ? 255 : 127;
					if(sourceRow.volume >= 0)
					{
						const int value = std::min(sourceRow.volume, maxVolume);
						cell.volcmd = VOLCMD_VOLUME;
						// У нот, которые играет DAC, громкость в Furnace логарифмическая:
						// `dacVolTable[127 - i] = 128 * 10^(-i * 0,75 / 20)`, те же 0,75 дБ
						// на шаг, что и у Total Level. Остальные чипы к такой шкале
						// приводят сами (PSG трактует громкость как индекс аттенюатора,
						// FM как TL), а сэмпл идёт обычным путём OpenMPT, где громкость
						// линейна по амплитуде. Поэтому для DAC она переводится здесь.
						// Громкость YMZ280B не трогается: там регистр линейный.
						if(dacNote)
						{
							const double amplitude = std::pow(10.0, (value - maxVolume) * 0.75 / 20.0);
							cell.vol = static_cast<uint8>(std::clamp<int>(mpt::saturate_round<int>(64.0 * amplitude), value > 0 ? 1 : 0, 64));
						} else if(!psg && voice != FurVoice::PCM)
						{
							// У FM шаг колонки равен шагу Total Level, 0,75 дБ, как у
							// фёрнейсовской громкости, но значений в колонке 64 против
							// 127. Поэтому берётся верхняя половина шкалы: громкость
							// 64 это самый тихий различимый уровень, 127 — полный.
							// Ниже 64 корпус не опускается (минимум за 24 трека — 39,
							// и таких значений 3 из 3821), так что теряется только
							// то, чем никто не пользуется.
							cell.vol = static_cast<uint8>(std::clamp(value - 63, value > 0 ? 1 : 0, 64));
						} else
						{
							cell.vol = static_cast<uint8>((value * 64 + maxVolume / 2) / maxVolume);
						}
						currentVolume[c] = cell.vol * 4;
						volumeFraction[c] = 0;
					} else if(cell.IsNote())
					{
						// Нота без громкости: проставляем ту, что набралась.
						cell.volcmd = VOLCMD_VOLUME;
						cell.vol = static_cast<ModCommand::VOL>(std::clamp((currentVolume[c] + 2) / 4, 0, 64));
						currentVolume[c] = cell.vol * 4;
						volumeFraction[c] = 0;
					}
					for(const auto &[fx, param] : sourceRow.effects)
					{
						if(fx < 0) continue;
						const auto value = static_cast<uint8>(std::max<int16>(0, param));
						if(fx == 0x08)
						{
							const unsigned l = value >> 4, r = value & 15;
							const auto pan = static_cast<uint8>((l || r) ? (r * 255 + (l + r) / 2) / (l + r) : 128);
							if(cell.volcmd == VOLCMD_NONE)
							{
								cell.volcmd = VOLCMD_PANNING;
								cell.vol = (pan + 2) / 4;
							} else if(cell.command == CMD_NONE)
							{
								cell.command = CMD_PANNING8;
								cell.param = pan;
							} else unsupported++;
							continue;
						}
						if(fx == 0xeb)
						{
							bank[c] = value;
							continue;
						}
						if(fx == 0x17 && dacChannel)
						{
							legacyDAC[c] = value != 0;
							continue;
						}
						auto [command, argument] = FurEffect(fx, value);
						if(fx == 0x20 && voice == FurVoice::Noise)
						{
							command = CMD_PSG_NOISE;
							argument = (value & 1) | ((value & 0x10) >> 3);
						} else if(fx >= 0x12 && fx <= 0x15 && (voice == FurVoice::OPN2 || voice == FurVoice::OPM))
						{
							command = static_cast<EffectCommand>(CMD_FM_LEVEL1 + fx - 0x12);
							argument = std::min<uint8>(value, 127);
						}
						if(fx == 0x09 || fx == 0x0f)
						{
							if(fx == 0x09 && !grooves.empty())
							{
								if(value < grooves.size())
								{
									speeds = grooves[value];
									phase = 0;
								}
								continue;
							}
							if(!value)
								continue;
							const size_t slot = fx == 0x0f && speeds.size() == 2 && grooves.empty() ? 1 : 0;
							speeds[slot] = value;
							if(timing)
								continue;
							command = CMD_SPEED;
							argument = static_cast<uint8>(std::min<unsigned>(255, value * timeBase));
						} else if(fx == 0xff && orders < 256)
						{
							command = CMD_POSITIONJUMP;
							argument = static_cast<uint8>(orders);
						}
						// На строку Furnace может прийти до восьми эффектов, а в
						// ячейке OpenMPT колонка эффекта одна. Выживает более
						// важный: команду чипа или скорость заменить нечем,
						// панораму же можно увести в колонку громкости или
						// потерять почти без вреда.
						const auto priority = [](ModCommand::COMMAND cmd) -> int
						{
							switch(cmd)
							{
							case CMD_NONE:
								return 0;
							case CMD_PANNING8:
								return 1;
							case CMD_ARPEGGIO:
							case CMD_VIBRATO:
							case CMD_TREMOLO:
								return 2;
							case CMD_VOLUMESLIDE:
							case CMD_VOLUMEDOWN_DURATION:
								return 3;
							case CMD_PORTAMENTOUP:
							case CMD_PORTAMENTODOWN:
							case CMD_TONEPORTAMENTO:
							case CMD_TONEPORTAVOL:
								return 4;
							case CMD_PSG_NOISE:
							case CMD_FM_LEVEL1:
							case CMD_FM_LEVEL2:
							case CMD_FM_LEVEL3:
							case CMD_FM_LEVEL4:
								return 5;
							case CMD_SPEED:
							case CMD_TEMPO:
							case CMD_POSITIONJUMP:
							case CMD_PATTERNBREAK:
								return 6;
							default:
								return 3;
							}
						};
						if(command == CMD_NONE)
						{
							if(fx != 0 || value)
								unsupported++;
						} else if(const uint8 slot = cell.FreeEffectSlot(); slot < ModCommand::Effects())
						{
							// Свободный слот есть — эффект сохраняется целиком,
							// бороться за колонку не нужно.
							cell.SetEffect(slot, command, argument);
						} else
						{
							// Слоты кончились: тогда выживает более важный, а
							// слабейший из всех уходит. Панораму, проигравшую
							// борьбу, ещё можно сохранить в колонке громкости.
							uint8 weakest = 0;
							for(uint8 i = 1; i < ModCommand::Effects(); i++)
							{
								if(priority(cell.GetEffect(i).first) < priority(cell.GetEffect(weakest).first))
									weakest = i;
							}
							const auto [loserCommand, loserParam] = cell.GetEffect(weakest);
							if(priority(command) > priority(loserCommand))
							{
								if(loserCommand == CMD_PANNING8 && cell.volcmd == VOLCMD_NONE)
								{
									cell.volcmd = VOLCMD_PANNING;
									cell.vol = static_cast<ModCommand::VOL>((loserParam + 2) / 4);
								} else
								{
									unsupported++;
								}
								cell.SetEffect(weakest, command, argument);
							} else
							{
								unsupported++;
							}
						}
					}
					if(sourceRow.instrument >= 0 && furIns >= 0 && furInstruments[furIns].type == 4)
						legacyDAC[c] = false;
					if(dacChannel && legacyDAC[c] && cell.IsNote() && sampleCount)
					{
						const size_t sample = bank[c] * 12 + static_cast<unsigned>(note % 12);
						if(sample < rawSamples.size())
						{
							auto &mapped = rawSamples[sample];
							if(!mapped && m_nInstruments < 255)
							{
								mapped = ++m_nInstruments;
								createInstrument(mapped, static_cast<SAMPLEINDEX>(sample + 1), "DAC sample " + std::to_string(sample + 1));
							}
							ins = mapped;
							cell.note = NOTE_MIDDLEC;
						}
					}
					// Номер инструмента ставится только там, где есть нота. У Furnace
					// смена инструмента без ноты лишь запоминает его до следующей
					// ноты и громкость не трогает, а у OpenMPT такая строка
					// сбрасывает громкость канала к умолчанию сэмпла. На
					// `flying battery alt act 2` из-за этого затирался рост
					// громкости PSG: ноты идут через строку, а между ними стоят
					// строки с одним номером инструмента, и каждая отменяла
					// набранное. До следующей ноты инструмент всё равно доедет,
					// его помнит currentInstrument.
					if(ins && (cell.IsNote() || noiseMode) && (sourceRow.instrument >= 0 || lastInstrument[c] != ins))
					{
						cell.instr = static_cast<ModCommand::INSTR>(ins);
						lastInstrument[c] = ins;
					}
					// Скольжение громкости едет каждый тик строки, и считать его
					// надо той же арифметикой, что в движке: шаг там мельче нашей
					// единицы громкости, поэтому копится дробный остаток, а шкала
					// берётся у чипа — 15 у PSG, 255 у сэмплерного, 127 у
					// остальных.
					if(cell.command == CMD_VOLUMESLIDE)
						volumeSlide[c] = cell.param;
					if(volumeSlide[c])
					{
						const int up = volumeSlide[c] >> 4, down = volumeSlide[c] & 0x0f;
						const int step = up ? up : -down;
						const int chipScale = (voice == FurVoice::PSG || voice == FurVoice::Noise) ? 15
							: (voice == FurVoice::PCM) ? 255 : 127;
						const unsigned ticks = std::max(1u, static_cast<unsigned>(speeds[phase % speeds.size()]) * timeBase);
						for(unsigned tick = 0; tick < ticks; tick++)
						{
							volumeFraction[c] += step * 16384 / chipScale;
							const int whole = volumeFraction[c] / 256;
							if(whole)
							{
								volumeFraction[c] -= whole * 256;
								currentVolume[c] = std::clamp(currentVolume[c] + whole, 0, 256);
							}
						}
					}
					// Задержка ноты у Furnace отсчитывается тиками и спокойно
					// переваливает за строку: по умолчанию там не отбрасывается
					// ни одна задержка. У OpenMPT задержка длиной со строку и
					// больше просто съедает ноту, поэтому такую ноту переносим
					// на ту строку, где она прозвучит, а в задержке оставляем
					// остаток. Заодно переводим тики Furnace в наши: строка у
					// нас длиннее во столько же раз, во сколько умножена скорость.
					if(cell.command == CMD_S3MCMDEX && (cell.param & 0xf0) == 0xd0)
					{
						const unsigned speed = std::max(1u, static_cast<unsigned>(speeds[phase % speeds.size()]));
						const unsigned value = cell.param & 0x0f;
						// Сравнение идёт в тиках Furnace, не в наших: наша строка
						// длиннее ровно во столько раз, во сколько умножена скорость.
						const bool honoured = (delayBehavior == 2) || (delayBehavior == 1 ? value <= speed : value < speed);
						if(!honoured)
						{
							cell.command = CMD_NONE;
							cell.param = 0;
							continue;
						}
						const unsigned ticksPerRow = std::max(1u, speed * timeBase);
						const unsigned delayTicks = value * timeBase;
						const ROWINDEX ahead = static_cast<ROWINDEX>(delayTicks / ticksPerRow);
						const unsigned rest = delayTicks % ticksPerRow;
						ModCommand moved = cell;
						if(rest)
							moved.param = static_cast<ModCommand::PARAM>(0xd0 | std::min(rest, 15u));
						else
							moved.command = CMD_NONE, moved.param = 0;
						if(!ahead)
							cell = moved;
						else if(row + ahead < rows)
							delayed.emplace_back(row, row + ahead, c, moved);
					}
				}
				if(timing)
				{
					auto &clock = *pattern.GetpModCommand(row, static_cast<CHANNELINDEX>(voices.size()));
					clock.command = CMD_SPEED;
					clock.param = static_cast<uint8>(std::clamp<unsigned>(speeds[phase % speeds.size()] * timeBase, 1, 255));
				}
			}
			// Перенесённая нота встаёт только на пустое место: занятую строку
			// она бы затёрла, а это уже не перенос, а потеря другой ноты.
			// Исходную строку очищаем здесь же, и только если нота доехала:
			// очистка сразу при переносе теряла ноту всякий раз, когда место
			// оказывалось занято.
			for(const auto &[from, to, c, moved] : delayed)
			{
				auto &target = *pattern.GetpModCommand(to, c);
				if(!target.IsEmpty())
					continue;
				target = moved;
				pattern.GetpModCommand(from, c)->Clear();
			}
		}
	}
	m_modFormat.formatName = UL_("Furnace (editable import)");
	m_modFormat.type = UL_("fur");
	m_modFormat.madeWithTracker = UL_("Furnace");
	m_modFormat.charset = mpt::Charset::UTF8;
#ifdef MODPLUG_TRACKER
	if(unsupported || extraSubsongs || std::find(voices.begin(), voices.end(), FurVoice::PCM) != voices.end())
		AddToLog(LogWarning, MPT_UFORMAT("Furnace import is approximate. {} unsupported effects or macros, {} extra subsongs. YMZ280B is represented by PCM samples.")(unsupported, extraSubsongs));
#endif
	return true;
}
OPENMPT_NAMESPACE_END

