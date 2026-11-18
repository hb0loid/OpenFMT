/*
 * Load_2sf.cpp
 * ------------
 * Назначение: импорт музыки Nintendo DS из 2SF (.mini2sf + .2sflib).
 *
 * 2SF — это PSF версии 0x24: в .2sflib лежит образ картриджа, в .mini2sf —
 * заплатка к нему (обычно два байта: номер последовательности для драйвера,
 * которым снят набор). Музыка в образе хранится стандартным звуковым архивом
 * Nintendo SDAT: последовательности SSEQ (почти MIDI: ноты, паузы, громкость,
 * панорама, изгиб высоты, 48 тиков на четверть), банки инструментов SBNK и
 * архивы сэмплов SWAR (PCM8, PCM16, IMA-ADPCM).
 *
 * Последовательность мы не разбираем по дорожкам, а проигрываем так же, как
 * секвенсор DS: кадр за кадром (192 в секунду), с тем же счётчиком темпа,
 * с огибающими и LFO. Строка модуля — тик секвенсора, тик модуля — кадр DS,
 * поэтому скорость строки (1 или 2 кадра при темпе до 240) повторяет то, как
 * DS сам раскладывает тики по кадрам. Огибающая ADSR становится огибающей
 * громкости инструмента с узлом отпускания: DS отпускает ноту от текущего
 * уровня, и узел отпускания в OpenMPT делает ровно это.
 *
 * Дорожка DS играет аккорды и звенит хвостами, поэтому получает столько
 * каналов, сколько у неё одновременно держится нот, а хвосты после отпускания
 * уходят в фон (NNA «продолжать»). Громкость раскладывается так: скорость
 * нажатия и экспрессия — в колонку громкости, громкость дорожки и общая — в
 * громкость канала (Mxx), громкость последовательности — в общую громкость.
 * Изгиб высоты, вибрато и короткое портаменто — подстройкой +xx с глубиной
 * инструмента, равной диапазону изгиба дорожки.
 */

#include "stdafx.h"
#include "Loaders.h"
#ifdef MPT_WITH_ZLIB
#include "../common/zlib_helper.h"
#include <zlib.h>
#endif
#include <cmath>
#include <functional>
#include <map>
#include <unordered_map>
#ifdef MODPLUG_TRACKER
#include "../mptrack/TrackerSettings.h"
#endif

OPENMPT_NAMESPACE_BEGIN

namespace
{

constexpr double DSFrameSeconds = 64.0 * 2728.0 / 33513982.0;  // кадр секвенсора, ~5,21 мс
constexpr double ARM7Clock = 16756991.0;                        // таймеры каналов
constexpr int32 AmplMin = -723 * 128;                           // тишина огибающей
constexpr double PSGBaseRate = 64.0 * 261.6255653;              // PSG: 64 отсчёта на период, до первой октавы
constexpr size_t MaxProgram = 128u * 1024u * 1024u;
constexpr uint32 MaxFrames = 30u * 60u * 192u;                  // не больше получаса

uint32 RD32(const std::vector<uint8> &d, size_t o)
{
	if(o + 4 > d.size())
		return 0;
	return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32>(d[o + 3]) << 24);
}

uint16 RD16(const std::vector<uint8> &d, size_t o)
{
	if(o + 2 > d.size())
		return 0;
	return static_cast<uint16>(d[o] | (d[o + 1] << 8));
}

// Громкость в сотых долях белла по квадратичному закону, как таблица
// SNDi_DecibelSquareTable: скорость нажатия, громкость, экспрессия и уровень
// поддержки у DS считаются через неё.
int32 SquareDecibel(int x)
{
	if(x <= 0)
		return -723;
	if(x >= 127)
		return 0;
	return std::max(-722, static_cast<int32>(std::lround(400.0 * std::log10(x / 127.0))));
}

int32 CnvAttack(int a)
{
	static constexpr uint8 lut[] = {0x00, 0x01, 0x05, 0x0E, 0x1A, 0x26, 0x33, 0x3F, 0x49, 0x54, 0x5C, 0x64, 0x6D, 0x74, 0x7B, 0x7F, 0x84, 0x89, 0x8F};
	a &= 0x7F;
	return a >= 0x6D ? lut[0x7F - a] : 0xFF - a;
}

int32 CnvFall(int f)
{
	f &= 0x7F;
	if(f == 0x7F)
		return 0xFFFF;
	if(f == 0x7E)
		return 0x3C00;
	if(f < 0x32)
		return f * 2 + 1;
	return 0x1E00 / (0x7E - f);
}

int32 CnvSine(int x)
{
	return static_cast<int32>(std::lround(127.0 * std::sin(6.283185307179586 * (x & 0x7F) / 128.0)));
}

double DecibelToLinear(int32 centibel)
{
	if(centibel <= -723)
		return 0.0;
	return std::pow(10.0, centibel / 200.0);
}

struct PsfFile
{
	std::vector<uint8> program;
	std::map<std::string, std::string> tags;
};

bool InflateProgram(const std::vector<uint8> &input, std::vector<uint8> &output)
{
#ifdef MPT_WITH_ZLIB
	zlib::z_inflate_stream stream{};
	if(!zlib::is_Z_OK(inflateInit(&*stream)))
		return false;
	stream->next_in = const_cast<Bytef *>(input.data());
	stream->avail_in = static_cast<uInt>(input.size());
	std::vector<uint8> block(65536);
	int result = Z_OK;
	while(result == Z_OK)
	{
		stream->next_out = block.data();
		stream->avail_out = static_cast<uInt>(block.size());
		result = inflate(&*stream, Z_NO_FLUSH);
		if(result != Z_OK && result != Z_STREAM_END)
			return false;
		output.insert(output.end(), block.begin(), block.begin() + (block.size() - stream->avail_out));
		if(output.size() > MaxProgram)
			return false;
		if(result == Z_OK && stream->avail_in == 0 && stream->avail_out != 0)
			return false;  // поток оборвался
	}
	return true;
#else
	MPT_UNREFERENCED_PARAMETER(input);
	MPT_UNREFERENCED_PARAMETER(output);
	return false;
#endif
}

bool ReadPsf(FileReader &file, PsfFile &psf)
{
	file.Rewind();
	uint8 magic[4]{};
	if(!file.ReadArray(magic) || magic[0] != 'P' || magic[1] != 'S' || magic[2] != 'F' || magic[3] != 0x24)
		return false;
	const uint32 reservedSize = file.ReadUint32LE(), programSize = file.ReadUint32LE();
	file.Skip(4);  // CRC
	if(!file.Skip(reservedSize) || !file.CanRead(programSize) || programSize > MaxProgram)
		return false;
	if(programSize)
	{
		std::vector<uint8> compressed;
		if(!file.ReadVector(compressed, programSize) || !InflateProgram(compressed, psf.program))
			return false;
	}
	// Теги: «[TAG]», затем строки ключ=значение. Повтор ключа — продолжение.
	char tagMagic[5]{};
	if(file.ReadArray(tagMagic) && !memcmp(tagMagic, "[TAG]", 5))
	{
		std::string text(static_cast<size_t>(std::min<uint64>(file.BytesLeft(), 65536)), '\0');
		file.ReadRaw(mpt::as_span(text));
		size_t start = 0;
		while(start < text.size())
		{
			size_t end = text.find('\n', start);
			if(end == std::string::npos)
				end = text.size();
			const std::string line = text.substr(start, end - start);
			start = end + 1;
			const size_t eq = line.find('=');
			if(eq == std::string::npos)
				continue;
			const auto trim = [](std::string s)
			{
				while(!s.empty() && static_cast<uint8>(s.back()) <= 0x20) s.pop_back();
				size_t i = 0;
				while(i < s.size() && static_cast<uint8>(s[i]) <= 0x20) i++;
				return s.substr(i);
			};
			std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
			for(auto &c : key)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			auto &slot = psf.tags[key];
			slot = slot.empty() ? value : slot + "\n" + value;
		}
	}
	return true;
}

// Программа 2SF: смещение в образе, длина, данные.
bool ApplyProgram(const std::vector<uint8> &program, std::vector<uint8> &rom)
{
	if(program.size() < 8)
		return program.empty();
	const uint32 offset = RD32(program, 0);
	uint32 size = RD32(program, 4);
	size = std::min<uint32>(size, static_cast<uint32>(program.size() - 8));
	if(static_cast<uint64>(offset) + size > MaxProgram)
		return false;
	if(rom.size() < offset + size)
		rom.resize(offset + size);
	std::copy(program.begin() + 8, program.begin() + 8 + size, rom.begin() + offset);
	return true;
}

// Время из тега «length»: [[ч:]м:]с[.доли].
double ParseTime(const std::string &s)
{
	double total = 0.0, part = 0.0, frac = 0.0, scale = 0.1;
	bool inFrac = false, any = false;
	for(char c : s)
	{
		if(c >= '0' && c <= '9')
		{
			any = true;
			if(inFrac) { frac += (c - '0') * scale; scale /= 10; }
			else part = part * 10 + (c - '0');
		} else if(c == ':')
		{
			total = (total + part) * 60.0;
			part = 0;
		} else if(c == '.' || c == ',')
		{
			inFrac = true;
		}
	}
	return any ? total + part + frac : 0.0;
}

struct NoteDef
{
	uint8 type = 0;  // 1 сэмпл, 2 PSG, 3 шум
	uint16 swav = 0, swar = 0;
	uint8 base = 60, attack = 127, decay = 127, sustain = 127, release = 127, pan = 64;
};

struct Region
{
	uint8 low = 0, high = 127;
	NoteDef def;
};

struct Program
{
	uint8 kind = 0;  // 1–15 одиночный, 16 ударные, 17 раскладка по клавишам
	std::vector<Region> regions;
	const NoteDef *Find(int key) const
	{
		for(const auto &r : regions)
		{
			if(key >= r.low && key <= r.high)
				return &r.def;
		}
		return nullptr;
	}
};

// Вариант инструмента: программа банка с одной огибающей и глубиной изгиба.
struct Variant
{
	uint8 program = 0;
	uint8 attack = 0, decay = 0, sustain = 0, release = 0;
	uint8 pwd = 2;
	uint16 tempo = 120;  // огибающая идёт тиками модуля, а это тики DS
	bool operator<(const Variant &o) const
	{
		return std::tie(program, attack, decay, sustain, release, pwd, tempo) < std::tie(o.program, o.attack, o.decay, o.sustain, o.release, o.pwd, o.tempo);
	}
};

struct Cell
{
	uint16 channel = 0;  // дорожка * 256 + голос дорожки
	uint8 note = NOTE_NONE, instr = 0;
	bool hasVolume = false;
	uint8 volume = 0;
	bool fastRelease = false;  // у отпускаемой ноты хвоста почти нет
	std::vector<std::pair<EffectCommand, uint8>> effects;
};

struct Row
{
	uint32 frame = 0;
	uint32 tick = 0;  // сколько тиков секвенсора прошло к концу кадра
	uint32 tempo = 120;
	std::vector<Cell> cells;
	Cell &At(uint16 channel)
	{
		for(auto &c : cells)
		{
			if(c.channel == channel)
				return c;
		}
		cells.emplace_back();
		cells.back().channel = channel;
		return cells.back();
	}
};

struct Track
{
	bool active = false;
	uint32 pos = 0;
	int32 wait = 0;
	uint32 ranTick = uint32_max;
	std::vector<uint32> calls;
	struct Loop { uint32 pos; int32 count; };
	std::vector<Loop> loops;
	uint8 prg = 0, vol = 127, expr = 127, pan = 64, range = 2;
	int8 transpose = 0, bend = 0;
	bool noteWait = true, tie = false, porta = false, cond = true;
	uint8 portaKey = 60, portaTime = 0;
	uint8 modDepth = 0, modSpeed = 16, modType = 0, modRange = 1;
	uint16 modDelay = 0;
	std::array<uint8, 4> adsr{{0xFF, 0xFF, 0xFF, 0xFF}};
	int16 sweep = 0;
	// Петля: тик, на котором впервые исполнена команда по смещению.
	std::unordered_map<uint32, uint32> firstTick;
	bool looped = false;
	uint32 loopStart = 0, loopEnd = 0;
	uint32 lastNoteTick = uint32_max;
};

struct Voice
{
	uint32 id = 0;
	uint8 track = 0, slot = 0;
	int key = 60;
	uint8 velocity = 127;
	int32 length = 0;
	const NoteDef *def = nullptr;
	int32 attack = 0, decay = 0, sustain = 0, release = 0;
	int32 amp = AmplMin;
	uint8 state = 0;  // 0 атака, 1 спад, 2 поддержка, 3 отпускание
	bool dead = false;
	uint32 modCounter = 0, modDelayCount = 0;
	int32 sweepPitch = 0, sweepLength = 0, sweepCount = 0;
	int32 lfo = 0;
	INSTRUMENTINDEX instrument = 0;
	uint8 pwd = 2;
	bool started = false;
	uint32 startTick = 0, releaseTick = uint32_max;
};

struct Slot
{
	uint32 voice = 0;  // 0 — пусто
	bool held = false;
	uint32 lastEventRow = uint32_max;  // строка модуля с последним нажатием или ===
	// Что уже записано в канал: пишем только изменения.
	int volume = -1, channelVolume = -1, pan = -1, finetune = -1, lfo = -1;
};

// Секвенсор DS и запись его работы в строки модуля.
class DsSequencer
{
public:
	const std::vector<uint8> &data;
	const std::vector<Program> &programs;
	uint8 seqVolume = 127, masterVolume = 127;
	uint32 tempo = 120, counter = 240, tick = 0;
	std::array<Track, 16> tracks;
	std::vector<Voice> voices;
	uint32 nextVoice = 1;
	std::array<std::vector<Slot>, 16> slots;
	std::vector<Row> rows;
	std::map<Variant, INSTRUMENTINDEX> variantIndex;
	std::vector<Variant> variants;
	std::array<int16, 32> variables{};
	uint32 random = 0x12345678;
	uint32 forgetTick = uint32_max;
	uint32 rowTicks = 0;  // тиков DS в строке модуля; 0 — строка = кадр (точный импорт)
	// Все ноты по порядку: дорожка, голос, тик нажатия и отпускания.
	struct NoteLog { uint8 track, slot; uint32 start, release; };
	std::vector<NoteLog> log;

	DsSequencer(const std::vector<uint8> &d, const std::vector<Program> &p) : data(d), programs(p)
	{
		tracks[0].active = true;
	}

	bool Fetch(Track &t, uint8 &v)
	{
		if(t.pos >= data.size())
			return false;
		v = data[t.pos++];
		return true;
	}

	bool ReadArg(Track &t, char kind, int32 &v)
	{
		uint8 b = 0;
		switch(kind)
		{
		case 'b': if(!Fetch(t, b)) return false; v = b; return true;
		case 's': if(!Fetch(t, b)) return false; v = static_cast<int8>(b); return true;
		case 'h':
		case 'H':
		{
			uint8 hi = 0;
			if(!Fetch(t, b) || !Fetch(t, hi)) return false;
			v = b | (hi << 8);
			if(kind == 'H') v = static_cast<int16>(v);
			return true;
		}
		case 't':
		{
			uint8 m = 0, hi = 0;
			if(!Fetch(t, b) || !Fetch(t, m) || !Fetch(t, hi)) return false;
			v = b | (m << 8) | (hi << 16);
			return true;
		}
		case 'v':
			v = 0;
			for(int i = 0; i < 4; i++)
			{
				if(!Fetch(t, b)) return false;
				v = (v << 7) | (b & 0x7F);
				if(!(b & 0x80)) return true;
			}
			return true;
		}
		return false;
	}

	static const char *ArgKinds(uint8 cmd)
	{
		if(cmd < 0x80) return "bv";
		switch(cmd)
		{
		case 0x80: case 0x81: return "v";
		case 0x93: return "bt";
		case 0x94: case 0x95: return "t";
		case 0xC3: case 0xC4: return "s";
		case 0xE0: case 0xE1: case 0xFE: return "h";
		case 0xE3: return "H";
		case 0xFC: case 0xFD: case 0xFF: return "";
		}
		if(cmd >= 0xC0 && cmd <= 0xD6) return "b";
		if(cmd >= 0xB0 && cmd <= 0xBD) return "bH";
		return nullptr;
	}

	uint32 Random()
	{
		random = random * 1664525u + 1013904223u;
		return random >> 16;
	}

	const NoteDef *FindRegion(uint8 prg, int key) const
	{
		if(prg >= programs.size())
			return nullptr;
		return programs[prg].Find(key);
	}

	INSTRUMENTINDEX GetVariant(const Track &t, const NoteDef &def, uint8 pwd)
	{
		Variant v;
		v.program = t.prg;
		v.attack = t.adsr[0] != 0xFF ? t.adsr[0] : def.attack;
		v.decay = t.adsr[1] != 0xFF ? t.adsr[1] : def.decay;
		v.sustain = t.adsr[2] != 0xFF ? t.adsr[2] : def.sustain;
		v.release = t.adsr[3] != 0xFF ? t.adsr[3] : def.release;
		v.pwd = pwd;
		v.tempo = rowTicks ? static_cast<uint16>(std::clamp<uint32>(tempo, 1, 1023)) : 240;  // 240 — тик = кадр
		if(auto it = variantIndex.find(v); it != variantIndex.end())
			return it->second;
		if(variants.size() >= MAX_INSTRUMENTS - 1)
			return 0;
		variants.push_back(v);
		const auto index = static_cast<INSTRUMENTINDEX>(variants.size());
		variantIndex[v] = index;
		return index;
	}

	void NoteOn(uint8 trackIndex, int key, uint8 velocity, int32 duration)
	{
		Track &t = tracks[trackIndex];
		t.lastNoteTick = tick;
		key += t.transpose;
		const NoteDef *def = FindRegion(t.prg, key);
		if(t.noteWait)
			t.wait = duration;
		const int portaFrom = t.portaKey;
		t.portaKey = static_cast<uint8>(std::clamp(key, 0, 127));
		if(def == nullptr || def->type < 1 || def->type > 3 || key < 0 || key >= NOTE_MAX)
			return;
		Voice v;
		v.startTick = tick;
		v.track = trackIndex;
		v.key = key;
		v.velocity = velocity;
		v.length = duration;
		v.def = def;
		v.attack = CnvAttack(t.adsr[0] != 0xFF ? t.adsr[0] : def->attack);
		v.decay = CnvFall(t.adsr[1] != 0xFF ? t.adsr[1] : def->decay);
		v.sustain = SquareDecibel(t.adsr[2] != 0xFF ? t.adsr[2] : def->sustain) * 128;
		v.release = CnvFall(t.adsr[3] != 0xFF ? t.adsr[3] : def->release);
		int maxPitch = 0;
		if(t.porta)
		{
			// Портаменто DS: высота съезжает от прошлой ноты к новой.
			v.sweepPitch = t.sweep + (portaFrom - key) * 64;
			if(t.portaTime)
				v.sweepLength = (t.portaTime * t.portaTime * std::abs(v.sweepPitch)) >> 11;
			else
				v.sweepLength = std::max<int32>(1, duration * 240 / static_cast<int32>(std::max<uint32>(tempo, 1)));
			maxPitch = (std::abs(v.sweepPitch) + 63) / 64;
		}
		// Глубина подстройки инструмента равна диапазону изгиба дорожки:
		// тогда шаг +xx совпадает с шагом изгиба DS (1/128 диапазона). Нота с
		// портаменто требует запаса на ход высоты; запас берётся из короткого
		// ряда, чтобы каждая новая ширина скачка не плодила инструмент.
		int pwd = std::max<int>(t.range, 1);
		if(maxPitch)
		{
			static constexpr uint8 steps[] = {2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96};
			const int need = pwd + maxPitch;
			pwd = 96;
			for(uint8 st : steps)
			{
				if(st >= need) { pwd = st; break; }
			}
		}
		v.pwd = static_cast<uint8>(std::clamp(pwd, 1, 96));
		v.instrument = GetVariant(t, *def, v.pwd);
		if(!v.instrument)
			return;
		// Голос дорожки: любой, где нота не держится. Отзвучивающий хвост
		// новая нота уводит в фон (NNA «отпустить»), а если старая нота
		// отпускается в той же строке, нота модуля просто встаёт на место ===.
		// В одной строке модуля у канала одно событие ноты, поэтому голос,
		// уже нажатый или отпущенный в этой строке, не берём.
		auto &trackSlots = slots[trackIndex];
		const uint32 compactRow = rowTicks ? tick / rowTicks : uint32_max - 1;
		uint8 slot = 0;
		for(; slot < trackSlots.size(); slot++)
		{
			if(!trackSlots[slot].held && (!rowTicks || trackSlots[slot].lastEventRow != compactRow))
				break;
		}
		if(slot == trackSlots.size())
		{
			if(trackSlots.size() >= 16)
				return;
			trackSlots.emplace_back();
		}
		v.id = nextVoice++;
		v.slot = slot;
		log.push_back({trackIndex, slot, tick, uint32_max});
		trackSlots[slot].voice = v.id;
		trackSlots[slot].held = true;
		trackSlots[slot].lastEventRow = compactRow;
		voices.push_back(v);
		Cell &cell = rows.back().At(static_cast<uint16>(trackIndex * 256 + slot));
		cell.note = static_cast<uint8>(key + NOTE_MIN);
		cell.instr = static_cast<uint8>(v.instrument);
	}

	void KeyOff(Voice &v)
	{
		if(v.state == 3)
			return;
		v.state = 3;
		v.releaseTick = tick;
		log[v.id - 1].release = tick;
		Slot &s = slots[v.track][v.slot];
		if(s.voice == v.id)
		{
			s.held = false;
			if(rowTicks)
				s.lastEventRow = tick / rowTicks;
			Cell &cell = rows.back().At(static_cast<uint16>(v.track * 256 + v.slot));
			if(cell.note == NOTE_NONE)
			{
				cell.note = NOTE_KEYOFF;
				cell.fastRelease = v.release >= 0x3C00;  // хвост не длиннее 6 кадров
			}
		}
	}

	void Execute(uint8 index, uint8 cmd, const std::array<int32, 3> &a, uint32 cmdPos)
	{
		Track &t = tracks[index];
		if(cmd < 0x80)
		{
			NoteOn(index, cmd, static_cast<uint8>(a[0]), a[1]);
			return;
		}
		switch(cmd)
		{
		case 0x80: t.wait = a[0]; break;
		case 0x81: t.prg = static_cast<uint8>(std::min(a[0], 255)); break;
		case 0x93:
			if(a[0] > 0 && a[0] < 16 && !tracks[a[0]].active)
			{
				Track &n = tracks[a[0]];
				n = Track{};
				n.active = true;
				n.pos = static_cast<uint32>(a[1]);
				Run(static_cast<uint8>(a[0]));
			}
			break;
		case 0x94:
			if(static_cast<uint32>(a[0]) <= cmdPos && !t.looped)
			{
				t.looped = true;
				const auto it = t.firstTick.find(static_cast<uint32>(a[0]));
				t.loopStart = it != t.firstTick.end() ? it->second : 0;
				t.loopEnd = tick;
			}
			t.pos = static_cast<uint32>(a[0]);
			break;
		case 0x95:
			if(t.calls.size() < 16)
			{
				t.calls.push_back(t.pos);
				t.pos = static_cast<uint32>(a[0]);
			}
			break;
		case 0xFD:
			if(!t.calls.empty())
			{
				t.pos = t.calls.back();
				t.calls.pop_back();
			}
			break;
		case 0xC0: t.pan = static_cast<uint8>(a[0]); break;
		case 0xC1: t.vol = static_cast<uint8>(a[0]); break;
		case 0xC2: masterVolume = static_cast<uint8>(a[0]); break;
		case 0xC3: t.transpose = static_cast<int8>(a[0]); break;
		case 0xC4: t.bend = static_cast<int8>(a[0]); break;
		case 0xC5: t.range = static_cast<uint8>(a[0]); break;
		case 0xC7: t.noteWait = a[0] != 0; break;
		case 0xC8: t.tie = a[0] != 0; break;
		case 0xC9: t.portaKey = static_cast<uint8>(std::clamp(a[0] + t.transpose, 0, 127)); t.porta = true; break;
		case 0xCA: t.modDepth = static_cast<uint8>(a[0]); break;
		case 0xCB: t.modSpeed = static_cast<uint8>(a[0]); break;
		case 0xCC: t.modType = static_cast<uint8>(a[0]); break;
		case 0xCD: t.modRange = static_cast<uint8>(a[0]); break;
		case 0xCE: t.porta = a[0] != 0; break;
		case 0xCF: t.portaTime = static_cast<uint8>(a[0]); break;
		case 0xD0: case 0xD1: case 0xD2: case 0xD3: t.adsr[cmd - 0xD0] = static_cast<uint8>(a[0]); break;
		case 0xD4:
			if(t.loops.size() < 8)
				t.loops.push_back({t.pos, a[0]});
			break;
		case 0xFC:
			if(!t.loops.empty())
			{
				auto &lp = t.loops.back();
				if(lp.count == 0)
				{
					if(!t.looped)
					{
						t.looped = true;
						const auto it = t.firstTick.find(lp.pos);
						t.loopStart = it != t.firstTick.end() ? it->second : 0;
						t.loopEnd = tick;
					}
					t.pos = lp.pos;
				} else if(--lp.count > 0)
				{
					t.pos = lp.pos;
				} else
				{
					t.loops.pop_back();
				}
			}
			break;
		case 0xD5: t.expr = static_cast<uint8>(a[0]); break;
		case 0xE0: t.modDelay = static_cast<uint16>(a[0]); break;
		case 0xE1: tempo = static_cast<uint32>(a[0]); break;
		case 0xE3: t.sweep = static_cast<int16>(a[0]); break;
		case 0xFF: t.active = false; break;
		default:
			if(cmd >= 0xB0 && cmd <= 0xBD)
			{
				int16 &var = variables[a[0] & 0x1F];
				const int16 value = static_cast<int16>(a[1]);
				switch(cmd)
				{
				case 0xB0: var = value; break;
				case 0xB1: var = static_cast<int16>(var + value); break;
				case 0xB2: var = static_cast<int16>(var - value); break;
				case 0xB3: var = static_cast<int16>(var * value); break;
				case 0xB4: if(value) var = static_cast<int16>(var / value); break;
				case 0xB5: var = static_cast<int16>(value >= 0 ? var << value : var >> -value); break;
				case 0xB6: var = static_cast<int16>(value ? static_cast<int32>(Random() % (std::abs(value) + 1)) * (value < 0 ? -1 : 1) : 0); break;
				case 0xB8: t.cond = var == value; break;
				case 0xB9: t.cond = var >= value; break;
				case 0xBA: t.cond = var > value; break;
				case 0xBB: t.cond = var <= value; break;
				case 0xBC: t.cond = var < value; break;
				case 0xBD: t.cond = var != value; break;
				}
			}
			break;
		}
	}

	void Run(uint8 index)
	{
		Track &t = tracks[index];
		t.ranTick = tick;
		for(int budget = 0; t.active && t.wait == 0; budget++)
		{
			if(budget > 4096)
			{
				t.active = false;  // зациклилось без пауз
				break;
			}
			const uint32 cmdPos = t.pos;
			t.firstTick.emplace(cmdPos, tick);
			uint8 cmd = 0;
			if(!Fetch(t, cmd))
			{
				t.active = false;
				break;
			}
			bool doIt = true;
			int argMode = 0;  // 1 — последний аргумент случайный, 2 — из переменной
			if(cmd == 0xA2)
			{
				doIt = t.cond;
				if(!Fetch(t, cmd)) { t.active = false; break; }
			}
			if(cmd == 0xA0 || cmd == 0xA1)
			{
				argMode = cmd == 0xA0 ? 1 : 2;
				if(!Fetch(t, cmd)) { t.active = false; break; }
			}
			const char *kinds = ArgKinds(cmd);
			if(kinds == nullptr)
			{
				t.active = false;
				break;
			}
			std::array<int32, 3> args{};
			const size_t count = strlen(kinds);
			bool ok = true;
			for(size_t i = 0; i < count && ok; i++)
			{
				if(i + 1 == count && argMode == 1)
				{
					int32 lo = 0, hi = 0;
					ok = ReadArg(t, 'H', lo) && ReadArg(t, 'H', hi);
					if(hi < lo) std::swap(lo, hi);
					args[i] = lo + static_cast<int32>(Random() % static_cast<uint32>(hi - lo + 1));
				} else if(i + 1 == count && argMode == 2)
				{
					int32 var = 0;
					ok = ReadArg(t, 'b', var);
					args[i] = variables[var & 0x1F];
				} else
				{
					ok = ReadArg(t, kinds[i], args[i]);
				}
			}
			if(!ok)
			{
				t.active = false;
				break;
			}
			if(doIt)
				Execute(index, cmd, args, cmdPos);
		}
	}

	void Tick()
	{
		for(auto &v : voices)
		{
			if(v.length > 0 && v.state != 3)
			{
				if(--v.length == 0)
					KeyOff(v);
			}
		}
		for(uint8 i = 0; i < 16; i++)
		{
			Track &t = tracks[i];
			if(!t.active || t.ranTick == tick)
				continue;
			if(t.wait > 0 && --t.wait > 0)
				continue;
			Run(i);
		}
		tick++;
	}

	// Покадровое обновление голоса: огибающая, LFO, портаменто.
	void UpdateVoice(Voice &v)
	{
		switch(v.state)
		{
		case 0:
			v.amp = v.amp * v.attack / 256;
			if(v.amp == 0)
				v.state = 1;
			break;
		case 1:
			v.amp -= v.decay;
			if(v.amp <= v.sustain)
			{
				v.amp = v.sustain;
				v.state = 2;
			}
			break;
		case 3:
			v.amp -= v.release;
			if(v.amp <= AmplMin)
				v.dead = true;
			break;
		}
		const Track &t = tracks[v.track];
		v.lfo = 0;
		if(t.modDepth)
		{
			if(v.modDelayCount < t.modDelay)
			{
				v.modDelayCount++;
			} else
			{
				v.modCounter = (v.modCounter + (t.modSpeed << 6)) & 0x7FFF;
				v.lfo = CnvSine(static_cast<int>(v.modCounter >> 8)) * t.modRange * t.modDepth;
			}
		}
	}

	int32 PitchOffset(Voice &v) const
	{
		const Track &t = tracks[v.track];
		int32 pitch = (t.bend * t.range) >> 1;
		if(v.sweepLength && v.sweepCount < v.sweepLength)
			pitch += v.sweepPitch * (v.sweepLength - v.sweepCount) / v.sweepLength;
		// В сжатых строках LFO высоты идёт вибрато модуля, в покадровых —
		// прямо подстройкой.
		if(!rowTicks && t.modType == 0)
			pitch += (v.lfo * 60) >> 14;
		return pitch;
	}

	// LFO DS как вибрато (Hxy/Uxy) или панбрелло (Yxy). Период LFO — 512/скорость
	// кадров, у вибрато модуля — 64/x тиков, у панбрелло — 256/x тиков.
	// 0 — LFO молчит. LFO громкости остаётся в колонке громкости.
	int LfoEffect(const Voice &v) const
	{
		const Track &t = tracks[v.track];
		if(!rowTicks || !t.modDepth || !t.modRange || !t.modSpeed || v.modDelayCount < t.modDelay || t.modType == 1)
			return 0;
		const double periodTicks = 512.0 / t.modSpeed * static_cast<double>(std::max<uint32>(tempo, 1)) / 240.0;
		const double amplitude = 127.0 * t.modRange * t.modDepth;
		if(t.modType == 0)
		{
			const double fine = amplitude * 60.0 / 16384.0;  // в 1/64 полутона
			const int x = std::clamp(static_cast<int>(std::lround(64.0 / periodTicks)), 1, 15);
			if(fine < 15.5)
				return (CMD_FINEVIBRATO << 8) | (x << 4) | std::clamp(static_cast<int>(std::lround(fine)), 1, 15);
			return (CMD_VIBRATO << 8) | (x << 4) | std::clamp(static_cast<int>(std::lround(fine / 4.0)), 1, 15);
		}
		const double pan = 2.0 * amplitude * 64.0 / 16384.0;  // в единицах панорамы 0–256
		const int x = std::clamp(static_cast<int>(std::lround(256.0 / periodTicks)), 1, 15);
		return (CMD_PANBRELLO << 8) | (x << 4) | std::clamp(static_cast<int>(std::lround(pan / 8.0)), 1, 15);
	}

	// Состояние голоса в строку: только то, что изменилось.
	void Emit(Voice &v, bool noteOn)
	{
		const Track &t = tracks[v.track];
		Slot &s = slots[v.track][v.slot];
		if(s.voice != v.id)
			return;  // голос уже ушёл в фон
		int32 volDb = SquareDecibel(v.velocity) + SquareDecibel(t.expr);
		if(t.modType == 1)
			volDb += (v.lfo * 60) >> 14;
		const int volume = static_cast<int>(std::lround(64.0 * DecibelToLinear(std::min(volDb, 0))));
		const int channelVolume = static_cast<int>(std::lround(64.0 * DecibelToLinear(SquareDecibel(t.vol) + SquareDecibel(masterVolume))));
		const int panLfo = (!rowTicks && t.modType == 2) ? ((v.lfo * 64) >> 14) : 0;
		const int pan = std::clamp(t.pan + v.def->pan - 64 + panLfo, 0, 127) * 2;
		const int lfo = LfoEffect(v);
		const int finetune = std::clamp(0x80 + static_cast<int>(std::lround(PitchOffset(v) * 2.0 / v.pwd)), 0, 255);
		const uint16 key = static_cast<uint16>(v.track * 256 + v.slot);
		Cell *cell = noteOn ? &rows.back().At(key) : nullptr;
		const auto get = [&]() -> Cell & { if(!cell) cell = &rows.back().At(key); return *cell; };
		if(noteOn || volume != s.volume)
		{
			get().hasVolume = true;
			get().volume = static_cast<uint8>(volume);
			s.volume = volume;
		}
		if(finetune != s.finetune)
		{
			get().effects.emplace_back(CMD_FINETUNE, static_cast<uint8>(finetune));
			s.finetune = finetune;
		}
		if(pan != s.pan)
		{
			get().effects.emplace_back(CMD_PANNING8, static_cast<uint8>(pan));
			s.pan = pan;
		}
		if(channelVolume != s.channelVolume)
		{
			get().effects.emplace_back(CMD_CHANNELVOLUME, static_cast<uint8>(channelVolume));
			s.channelVolume = channelVolume;
		}
		if(lfo != s.lfo)
		{
			get().effects.emplace_back(lfo ? static_cast<EffectCommand>(lfo >> 8) : CMD_DUMMY, static_cast<uint8>(lfo & 0xFF));
			s.lfo = lfo;
		}
	}

	// Один кадр DS — одна строка модуля.
	void Frame(uint32 frame)
	{
		rows.emplace_back();
		rows.back().frame = frame;
		while(counter >= 240)
		{
			counter -= 240;
			Tick();
		}
		counter += tempo;
		rows.back().tick = tick;
		rows.back().tempo = tempo;
		if(tick > forgetTick)
		{
			ForgetEmitted();
			forgetTick = uint32_max;
		}
		for(auto &v : voices)
		{
			UpdateVoice(v);
			Emit(v, !v.started);
			v.started = true;
			if(v.sweepLength && v.sweepCount < v.sweepLength)
				v.sweepCount++;
		}
		for(auto &v : voices)
		{
			if(v.dead)
			{
				Slot &s = slots[v.track][v.slot];
				if(s.voice == v.id)
				{
					s.voice = 0;
					if(s.lfo > 0)
					{
						rows.back().At(static_cast<uint16>(v.track * 256 + v.slot)).effects.emplace_back(CMD_DUMMY, uint8(0));
						s.lfo = 0;
					}
				}
			}
		}
		voices.erase(std::remove_if(voices.begin(), voices.end(), [](const Voice &v) { return v.dead; }), voices.end());
	}

	bool AllDone() const
	{
		for(const auto &t : tracks)
		{
			if(t.active && !t.looped)
				return false;
		}
		return true;
	}

	// Сбросить память записанного: на начале петли каналы могут прийти в
	// любом состоянии, поэтому следующая нота пишет всё заново.
	void ForgetEmitted()
	{
		for(auto &trackSlots : slots)
		{
			for(auto &s : trackSlots)
				s.volume = s.channelVolume = s.pan = s.finetune = s.lfo = -1;
		}
	}
};

// Огибающая громкости из ADSR: покадровая кривая, прореженная до точек.
void BuildEnvelope(InstrumentEnvelope &env, const Variant &v)
{
	const int32 attack = CnvAttack(v.attack), decay = CnvFall(v.decay), release = CnvFall(v.release);
	const int32 sustain = SquareDecibel(v.sustain) * 128;
	const auto level = [](int32 amp) { return 64.0 * DecibelToLinear(amp >> 7); };
	std::vector<double> curve;
	int32 amp = AmplMin;
	int state = 0;
	while(curve.size() < 60000)
	{
		if(state == 0)
		{
			amp = amp * attack / 256;
			if(amp == 0) state = 1;
		} else
		{
			amp -= decay;
			if(amp <= sustain) { amp = sustain; state = 2; }
		}
		curve.push_back(level(amp));
		if(state == 2)
			break;
	}
	std::vector<double> tail;
	for(int32 a = -release; a > AmplMin && tail.size() < 60000; a -= release)
		tail.push_back(level(a));
	tail.push_back(0.0);
	// Тик модуля — тик DS, а огибающая DS идёт кадрами: 240/темп кадров на тик.
	const double framesPerTick = 240.0 / std::max<int>(v.tempo, 1);
	const auto toTicks = [framesPerTick](const std::vector<double> &frames)
	{
		std::vector<double> ticks;
		for(size_t k = 0; ; k++)
		{
			const size_t f = static_cast<size_t>(k * framesPerTick);
			if(f >= frames.size())
			{
				if(ticks.empty() || ticks.back() != frames.back())
					ticks.push_back(frames.back());
				break;
			}
			ticks.push_back(frames[f]);
		}
		return ticks;
	};
	curve = toTicks(curve);
	tail = toTicks(tail);

	// Точки ломаной: жадно, пока отклонение не больше tol.
	const auto reduce = [](const std::vector<double> &c, size_t budget)
	{
		std::vector<size_t> points;
		for(double tol = 0.4; ; tol *= 1.6)
		{
			points.assign(1, 0);
			size_t i = 0;
			while(i + 1 < c.size())
			{
				size_t best = i + 1;
				for(size_t j = i + 2; j < c.size() && j - i <= 4000; j++)
				{
					const double a = std::round(c[i]), b = std::round(c[j]);
					bool fits = true;
					const size_t stride = std::max<size_t>(1, (j - i) / 256);
					for(size_t k = i + 1; k < j && fits; k += stride)
					{
						const double expect = a + (b - a) * static_cast<double>(k - i) / static_cast<double>(j - i);
						fits = std::abs(expect - c[k]) <= tol;
					}
					if(fits)
						best = j;
					else if(j - best > 64)
						break;
				}
				points.push_back(best);
				i = best;
			}
			if(points.size() <= budget)
				return points;
		}
	};
	const auto value = [](double x) { return static_cast<EnvelopeNode::value_t>(std::clamp(std::lround(x), 0l, 64l)); };
	env.clear();
	for(size_t p : reduce(curve, 100))
		env.push_back(static_cast<EnvelopeNode::tick_t>(p), value(curve[p]));
	env.nSustainStart = env.nSustainEnd = env.LastPoint();
	env.dwFlags.set(ENV_ENABLED | ENV_SUSTAIN);
	// Отпускание — отдельный хвост с узлом: OpenMPT прыгает к узлу по ===
	// и берёт значения хвоста относительно уровня в момент прыжка. Уровень
	// при прыжке он читает на тик дальше точки поддержки, поэтому за ней
	// стоит её повтор: иначе там оказался бы сам узел, и отпускание тихой
	// ноты начиналось бы с полной громкости.
	const uint32 nodeTick = static_cast<uint32>(curve.size()) + 1;
	env.push_back(static_cast<EnvelopeNode::tick_t>(nodeTick - 1), env.back().value);
	env.push_back(static_cast<EnvelopeNode::tick_t>(nodeTick), 64);
	env.nReleaseNode = env.LastPoint();
	std::vector<double> released{64.0};
	released.insert(released.end(), tail.begin(), tail.end());
	for(size_t p : reduce(released, 100))
	{
		if(p == 0)
			continue;
		env.push_back(static_cast<EnvelopeNode::tick_t>(std::min<size_t>(nodeTick + p, 65000)), value(released[p]));
	}
}

// Строки модуля из покадровых строк секвенсора. Тик модуля — тик DS, строка —
// rowTicks тиков. Событие ноты не на сетке ставится задержкой SDx, короткая
// нота с мгновенным отпусканием — задержкой с обрезкой (:xy). Изгиб,
// меняющийся внутри строки, идёт плавной подстройкой (*xx), одиночная смена
// в конце строки — уже в следующей. Громкость без ноты тоже ставится
// задержкой. LFO высоты и панорамы повторяется в каждой строке, пока звучит.
struct OutCell
{
	uint8 note = NOTE_NONE, instr = 0;
	bool hasVolume = false;
	uint8 volume = 0;
	std::vector<std::pair<EffectCommand, uint8>> effects;
};

struct Compact
{
	std::vector<std::vector<std::pair<uint16, OutCell>>> rows;
	std::vector<uint32> tempo;  // темп DS в начале строки
	uint32 deferred = 0;        // отпускания, сдвинутые к следующей строке
};

Compact CompactRows(const std::vector<Row> &frames, uint32 rowTicks, uint32 endTick, uint32 restartTick)
{
	Compact out;
	if(frames.empty())
		return out;
	struct Acc
	{
		int ev = 0;  // 0 — нет, 1 — нажатие, 2 — ===
		uint8 note = NOTE_NONE, instr = 0, noteVolume = 0;
		uint32 sub = 0;
		int cutSub = -1;
		bool volumeChanged = false;
		uint32 volumeSub = 0;
		int fineCount = 0, panCount = 0, cvCount = 0;
		uint32 fineSub = 0, panSub = 0, cvSub = 0;
	};
	struct State { int volume = -1, cv = -1, pan = -1, fine = -1, lfo = 0; };
	struct Chan { Acc acc; State want, have; bool pendingOff = false; };
	std::map<uint16, Chan> chans;
	for(const auto &f : frames)
	{
		for(const auto &c : f.cells)
			chans[c.channel];
	}
	const uint32 totalTicks = std::min(endTick, frames.back().tick);
	const uint32 numRows = (totalTicks + rowTicks - 1) / rowTicks;
	out.rows.resize(numRows);
	out.tempo.assign(numRows, 0);
	const uint32 restartRow = restartTick != uint32_max ? restartTick / rowTicks : uint32_max;

	const auto startRow = [&](uint32 r)
	{
		for(auto &[key, ch] : chans)
		{
			ch.acc = {};
			if(ch.pendingOff)
			{
				ch.acc.ev = 2;
				ch.pendingOff = false;
			}
			if(r == restartRow)
				ch.have = {};
		}
	};
	const auto finalize = [&](uint32 r)
	{
		for(auto &[key, ch] : chans)
		{
			Acc &a = ch.acc;
			State &want = ch.want, &have = ch.have;
			OutCell cell;
			int delay = 0;
			if(a.ev == 1)
			{
				cell.note = a.note;
				cell.instr = a.instr;
				cell.hasVolume = true;
				cell.volume = a.noteVolume;
				have.volume = a.noteVolume;
				if(a.cutSub >= 0)
					cell.effects.emplace_back(CMD_DELAYCUT, static_cast<uint8>((a.sub << 4) | std::min<uint32>(15, a.cutSub - a.sub)));
				else
					delay = static_cast<int>(a.sub);
			} else if(a.ev == 2)
			{
				cell.note = NOTE_KEYOFF;
				delay = static_cast<int>(a.sub);
			}
			if(a.ev != 1 && want.volume >= 0 && want.volume != have.volume)
			{
				cell.hasVolume = true;
				cell.volume = static_cast<uint8>(want.volume);
				have.volume = want.volume;
				if(a.ev == 0 && a.volumeChanged)
					delay = static_cast<int>(a.volumeSub);
			}
			if(delay > 0)
				cell.effects.insert(cell.effects.begin(), {CMD_S3MCMDEX, static_cast<uint8>(0xD0 | std::min(delay, 15))});
			const auto late = [&](int count, uint32 sub) { return count == 1 && sub * 2 > rowTicks && a.ev != 1; };
			const auto room = [&]() { return cell.effects.size() < ModCommand::Effects(); };
			if(want.fine >= 0 && want.fine != have.fine && !late(a.fineCount, a.fineSub) && room())
			{
				cell.effects.emplace_back((a.fineCount >= 2 && have.fine >= 0) ? CMD_FINETUNE_SMOOTH : CMD_FINETUNE, static_cast<uint8>(want.fine));
				have.fine = want.fine;
			}
			if(want.lfo && room())
				cell.effects.emplace_back(static_cast<EffectCommand>(want.lfo >> 8), static_cast<uint8>(want.lfo & 0xFF));
			// Панбрелло в OpenMPT оставляет сдвиг панорамы до следующей ноты
			// или команды панорамы: когда LFO панорамы смолкает, пишем её заново.
			if((have.lfo >> 8) == CMD_PANBRELLO && (want.lfo >> 8) != CMD_PANBRELLO)
				have.pan = -1;
			have.lfo = want.lfo;
			if(want.pan >= 0 && want.pan != have.pan && !late(a.panCount, a.panSub) && room())
			{
				cell.effects.emplace_back(CMD_PANNING8, static_cast<uint8>(want.pan));
				have.pan = want.pan;
			}
			if(want.cv >= 0 && want.cv != have.cv && !late(a.cvCount, a.cvSub) && room())
			{
				cell.effects.emplace_back(CMD_CHANNELVOLUME, static_cast<uint8>(want.cv));
				have.cv = want.cv;
			}
			if(cell.note != NOTE_NONE || cell.hasVolume || !cell.effects.empty())
				out.rows[r].emplace_back(key, std::move(cell));
		}
	};

	uint32 curRow = 0, prevTick = 0;
	startRow(0);
	for(const auto &f : frames)
	{
		const bool ticked = f.tick > prevTick;
		const uint32 tick = ticked ? prevTick : (prevTick ? prevTick - 1 : 0);
		prevTick = f.tick;
		if(tick >= totalTicks)
			break;
		const uint32 row = tick / rowTicks, sub = tick % rowTicks;
		while(curRow < row)
		{
			finalize(curRow);
			startRow(++curRow);
		}
		if(!out.tempo[row])
			out.tempo[row] = f.tempo;
		for(const auto &c : f.cells)
		{
			Chan &ch = chans[c.channel];
			Acc &a = ch.acc;
			const bool noteOn = c.note >= NOTE_MIN && c.note <= NOTE_MAX;
			if(noteOn)
			{
				if(a.ev != 1)
				{
					a.ev = 1;
					a.note = c.note;
					a.instr = c.instr;
					a.sub = sub;
					a.noteVolume = c.volume;
					a.cutSub = -1;
				}
				ch.want.volume = c.volume;
			} else if(c.note == NOTE_KEYOFF)
			{
				if(a.ev == 1)
				{
					if(c.fastRelease && sub > a.sub)
					{
						a.cutSub = static_cast<int>(sub);
					} else
					{
						ch.pendingOff = true;
						out.deferred++;
					}
				} else if(a.ev == 0)
				{
					a.ev = 2;
					a.sub = sub;
				}
			}
			if(c.hasVolume && !noteOn)
			{
				ch.want.volume = c.volume;
				a.volumeChanged = true;
				a.volumeSub = sub;
			}
			for(const auto &[cmd, param] : c.effects)
			{
				switch(cmd)
				{
				case CMD_FINETUNE: ch.want.fine = param; a.fineCount++; a.fineSub = sub; break;
				case CMD_PANNING8: ch.want.pan = param; a.panCount++; a.panSub = sub; break;
				case CMD_CHANNELVOLUME: ch.want.cv = param; a.cvCount++; a.cvSub = sub; break;
				case CMD_VIBRATO: case CMD_FINEVIBRATO: case CMD_PANBRELLO: ch.want.lfo = (cmd << 8) | param; break;
				case CMD_DUMMY: ch.want.lfo = 0; break;
				default: break;
				}
			}
		}
	}
	while(curRow < numRows)
	{
		finalize(curRow);
		if(++curRow < numRows)
			startRow(curRow);
	}
	for(uint32 r = 1; r < numRows; r++)
	{
		if(!out.tempo[r])
			out.tempo[r] = out.tempo[r - 1];
	}
	return out;
}

}  // namespace


CSoundFile::ProbeResult CSoundFile::ProbeFileHeader2SF(MemoryFileReader file, const uint64 *pfilesize)
{
	MPT_UNREFERENCED_PARAMETER(pfilesize);
	uint8 magic[4]{};
	if(!file.ReadArray(magic))
		return ProbeWantMoreData;
	if(magic[0] != 'P' || magic[1] != 'S' || magic[2] != 'F' || magic[3] != 0x24)
		return ProbeFailure;
	return ProbeSuccess;
}


bool CSoundFile::Read2SF(FileReader &file, ModLoadingFlags loadFlags)
{
	PsfFile psf;
	if(!ReadPsf(file, psf))
		return false;
	if(loadFlags == onlyVerifyHeader)
		return true;

	// Образ: сначала библиотека (_lib), поверх неё — своя программа.
	std::vector<uint8> rom;
	if(auto lib = psf.tags.find("_lib"); lib != psf.tags.end())
	{
#ifdef MPT_EXTERNAL_SAMPLES
		const auto fileName = file.GetOptionalFileName();
		if(!fileName)
			return false;
		std::function<bool(const mpt::PathString &, const std::string &, int)> loadLib = [&](const mpt::PathString &dir, const std::string &name, int depth) -> bool
		{
			if(depth > 4)
				return false;
			const mpt::PathString path = dir + mpt::PathString::FromUnicode(mpt::ToUnicode(mpt::Charset::UTF8, name));
			if(!mpt::native_fs{}.is_file(path))
				return false;
			mpt::IO::InputFile f{path, SettingCacheCompleteFileBeforeLoading()};
			if(!f.IsValid())
				return false;
			FileReader libFile = GetFileReader(f);
			PsfFile libPsf;
			if(!ReadPsf(libFile, libPsf))
				return false;
			if(auto nested = libPsf.tags.find("_lib"); nested != libPsf.tags.end())
			{
				if(!loadLib(dir, nested->second, depth + 1))
					return false;
			}
			return ApplyProgram(libPsf.program, rom);
		};
		if(!loadLib(fileName->GetDirectoryWithDrive(), lib->second, 0))
		{
			AddToLog(LogError, U_("2SF library not found: ") + mpt::ToUnicode(mpt::Charset::UTF8, lib->second));
			return false;
		}
#else
		return false;
#endif
	}
	if(!ApplyProgram(psf.program, rom))
		return false;

	// Звуковой архив SDAT внутри образа.
	size_t sdat = std::string::npos;
	for(size_t i = 0; i + 0x40 <= rom.size(); i += 4)
	{
		if(rom[i] == 'S' && rom[i + 1] == 'D' && rom[i + 2] == 'A' && rom[i + 3] == 'T' && rom[i + 4] == 0xFF && rom[i + 5] == 0xFE)
		{
			const uint32 info = RD32(rom, i + 0x18), fat = RD32(rom, i + 0x20);
			if(info && fat && i + info + 0x40 <= rom.size() && i + fat + 12 <= rom.size())
			{
				sdat = i;
				break;
			}
		}
	}
	if(sdat == std::string::npos)
		return false;
	const size_t infoBase = sdat + RD32(rom, sdat + 0x18), fatBase = sdat + RD32(rom, sdat + 0x20), symbBase = RD32(rom, sdat + 0x10) ? sdat + RD32(rom, sdat + 0x10) : 0;
	const uint32 fatCount = RD32(rom, fatBase + 8);
	const auto record = [&](int type, uint32 index) -> size_t
	{
		const size_t list = infoBase + RD32(rom, infoBase + 8 + 4 * type);
		if(index >= RD32(rom, list))
			return 0;
		const uint32 off = RD32(rom, list + 4 + 4 * index);
		return off ? infoBase + off : 0;
	};
	const auto fileSpan = [&](uint32 id, size_t &offset, size_t &size) -> bool
	{
		if(id >= fatCount)
			return false;
		offset = sdat + RD32(rom, fatBase + 12 + 16 * id);
		size = RD32(rom, fatBase + 16 + 16 * id);
		return offset + size <= rom.size() && size >= 16;
	};
	const auto isFile = [&](size_t offset, const char *magic) { return !memcmp(rom.data() + offset, magic, 4); };

	// Номер последовательности. Заплатка .mini2sf — это и есть номер (драйвер,
	// которым снимают наборы, держит его в образе). У цельного файла номер
	// ищем там же, где его держит этот драйвер, а не найдя — берём первую.
	const size_t seqList = infoBase + RD32(rom, infoBase + 8);
	const uint32 seqCount = RD32(rom, seqList);
	const auto validSeq = [&](uint32 n)
	{
		const size_t r = record(0, n);
		size_t off = 0, size = 0;
		return r && fileSpan(RD16(rom, r), off, size) && isFile(off, "SSEQ");
	};
	uint32 seqNo = uint32_max;
	if(psf.program.size() >= 9 && psf.program.size() <= 12)
		seqNo = psf.program.size() >= 10 ? RD16(psf.program, 8) : psf.program[8];
	else if(rom.size() >= 0xD0FC2)
		seqNo = RD16(rom, 0xD0FC0);
	if(seqNo == uint32_max || !validSeq(seqNo))
	{
		seqNo = uint32_max;
		for(uint32 n = 0; n < seqCount && n < 4096; n++)
		{
			if(validSeq(n)) { seqNo = n; break; }
		}
	}
	if(seqNo == uint32_max)
		return false;

	const size_t seqInfo = record(0, seqNo);
	if(seqInfo + 12 > rom.size())
		return false;
	size_t seqOff = 0, seqSize = 0;
	fileSpan(RD16(rom, seqInfo), seqOff, seqSize);
	const uint16 bankId = RD16(rom, seqInfo + 4);
	const uint8 seqVolume = rom[seqInfo + 6];
	const uint32 dataOffset = RD32(rom, seqOff + 0x18);
	if(dataOffset >= seqSize)
		return false;
	const std::vector<uint8> seqData(rom.begin() + seqOff + dataOffset, rom.begin() + seqOff + seqSize);

	// Банк: программы и четыре архива сэмплов.
	const size_t bankInfo = record(2, bankId);
	size_t bankOff = 0, bankSize = 0;
	if(!bankInfo || bankInfo + 12 > rom.size() || !fileSpan(RD16(rom, bankInfo), bankOff, bankSize) || !isFile(bankOff, "SBNK") || bankSize < 0x3C)
		return false;
	std::vector<Program> programs;
	{
		const uint32 count = std::min<uint32>(RD32(rom, bankOff + 0x38), static_cast<uint32>((bankSize - 0x3C) / 4));
		const auto readDef = [&](size_t at, uint8 type, NoteDef &def) -> bool
		{
			if(at + 10 > bankOff + bankSize)
				return false;
			def.type = type;
			def.swav = RD16(rom, at);
			def.swar = RD16(rom, at + 2);
			def.base = rom[at + 4];
			def.attack = rom[at + 5];
			def.decay = rom[at + 6];
			def.sustain = rom[at + 7];
			def.release = rom[at + 8];
			def.pan = rom[at + 9];
			return true;
		};
		programs.resize(std::min<uint32>(count, 256));
		for(uint32 i = 0; i < programs.size(); i++)
		{
			const size_t rec = bankOff + 0x3C + 4 * i;
			const uint8 type = rom[rec];
			const size_t at = bankOff + RD16(rom, rec + 1);
			Program &p = programs[i];
			p.kind = type;
			if(type >= 1 && type < 16)
			{
				Region r;
				if(readDef(at, type, r.def))
					p.regions.push_back(r);
			} else if(type == 16 && at + 2 <= bankOff + bankSize)
			{
				const uint8 low = rom[at], high = rom[at + 1];
				for(int key = low; key <= high; key++)
				{
					const size_t e = at + 2 + 12 * (key - low);
					Region r;
					r.low = r.high = static_cast<uint8>(key);
					if(e + 12 > bankOff + bankSize || !readDef(e + 2, rom[e], r.def))
						break;
					p.regions.push_back(r);
				}
			} else if(type == 17 && at + 8 <= bankOff + bankSize)
			{
				int low = 0;
				for(int k = 0; k < 8; k++)
				{
					const uint8 high = rom[at + k];
					if(!high)
						break;
					const size_t e = at + 8 + 12 * k;
					Region r;
					r.low = static_cast<uint8>(low);
					r.high = high;
					if(e + 12 > bankOff + bankSize || !readDef(e + 2, rom[e], r.def))
						break;
					p.regions.push_back(r);
					low = high + 1;
				}
			}
		}
	}
	std::array<std::vector<size_t>, 4> waveArchives;  // абсолютные смещения SWAV
	std::array<size_t, 4> waveArchiveEnd{};
	for(int w = 0; w < 4; w++)
	{
		const uint16 id = RD16(rom, bankInfo + 4 + 2 * w);
		const size_t arcInfo = id != 0xFFFF ? record(3, id) : 0;
		size_t off = 0, size = 0;
		if(!arcInfo || !fileSpan(RD16(rom, arcInfo), off, size) || !isFile(off, "SWAR") || size < 0x3C)
			continue;
		const uint32 count = std::min<uint32>(RD32(rom, off + 0x38), static_cast<uint32>((size - 0x3C) / 4));
		for(uint32 i = 0; i < count; i++)
			waveArchives[w].push_back(off + RD32(rom, off + 0x3C + 4 * i));
		waveArchiveEnd[w] = off + size;
	}

	// Проигрываем последовательность. Первый прогон находит петлю, второй
	// пишет строки, зная, где начало петли: там каналы приходят с конца петли
	// в другом состоянии, и строка начала петли пишет всё заново.
	const double tagLength = psf.tags.count("length") ? ParseTime(psf.tags["length"]) : 0.0;
	uint32 stopTick = uint32_max, loopStartTick = uint32_max, loopEndTick = uint32_max;
	bool heldAcross = false, consistent = false, finished = false;
	const auto play = [&](DsSequencer &seq, uint32 forgetTick, uint32 rowTicks)
	{
	stopTick = loopStartTick = loopEndTick = uint32_max;
	consistent = finished = false;
	seq.seqVolume = seqVolume;
	seq.forgetTick = forgetTick;
	seq.rowTicks = rowTicks;
	uint32 tailFrames = 0;
	for(uint32 frame = 0; frame < MaxFrames; frame++)
	{
		seq.Frame(frame);
		// Дорожка, которая не кончается и не петляет, иначе держала бы нас до
		// предела: дальше длины из тега (с запасом) не идём.
		if(stopTick == uint32_max && tagLength > 0 && frame * DSFrameSeconds > tagLength + 30.0)
		{
			stopTick = seq.tick;
			break;
		}
		if(stopTick == uint32_max && seq.AllDone())
		{
			// Петля общая, если все зациклившиеся дорожки зациклились на одном
			// и том же отрезке. Петля из одних пауз — это конец песни: драйвер
			// просто ждёт, и модулю незачем крутить тишину.
			// Дорожка, крутящая одни паузы, на петлю не влияет.
			consistent = false;
			for(const auto &t : seq.tracks)
			{
				const bool idle = t.lastNoteTick == uint32_max || t.lastNoteTick < t.loopStart;
				if(!t.looped || idle)
					continue;
				if(loopStartTick == uint32_max)
				{
					loopStartTick = t.loopStart;
					loopEndTick = t.loopEnd;
					consistent = true;
				} else if(t.loopStart != loopStartTick || t.loopEnd != loopEndTick)
				{
					consistent = false;
				}
			}
			if(loopStartTick == uint32_max)
			{
				stopTick = seq.tick;  // петли нет: доигрываем хвосты
				finished = true;
			} else if(consistent && loopEndTick > loopStartTick)
			{
				// Прогоняем петлю второй раз: если ноты держатся через её
				// конец, петлёй станет второй проход, где они уже учтены.
				stopTick = loopEndTick + (loopEndTick - loopStartTick) + 1;
			} else
			{
				consistent = false;
				uint32 longest = 0;
				for(const auto &t : seq.tracks)
					longest = std::max(longest, t.loopEnd);
				stopTick = std::max<uint32>(longest, static_cast<uint32>(tagLength / DSFrameSeconds * seq.tempo / 240.0));
			}
		}
		if(stopTick != uint32_max && seq.tick >= stopTick)
		{
			if(!finished && !consistent)
				break;
			if(consistent)
				break;
			// Без петли — ждём, пока отзвучат хвосты.
			if(seq.voices.empty() || ++tailFrames > 192 * 10)
				break;
		}
	}
	};
	DsSequencer scout(seqData, programs);
	play(scout, uint32_max, 1);
	if(scout.rows.empty() || scout.variants.empty())
		return false;

	// Шаг строки: самый крупный из 1/16, 1/32, ... доли такта, при котором
	// почти все события ложатся в строки без сдвига (вне сетки — задержкой).
	// Сдвиг бывает у короткой ноты с долгим хвостом: нажатие и отпускание
	// попадают в одну строку, и отпускание уходит в следующую. Петля должна
	// начинаться и кончаться на границе строки.
	// Сжатые строки — только в трекере и по настройке: тик модуля там — тик
	// DS, а на DS тик ложится на границу кадра, так что время нот плывёт в
	// пределах кадра (5 мс). Покадровый импорт точен, но строк в нём много.
#ifdef MODPLUG_TRACKER
	const bool compactRows = TrackerSettings::Instance().twoSFCompactRows;
#else
	const bool compactRows = false;
#endif
	uint32 rowTicks = compactRows ? 1 : 0;
	for(uint32 candidate : {12u, 6u, 4u, 3u, 2u})
	{
		if(!compactRows)
			break;
		if(consistent && (loopStartTick % candidate || loopEndTick % candidate))
			continue;
		DsSequencer test(seqData, programs);
		play(test, uint32_max, candidate);
		const Compact trial = CompactRows(test.rows, candidate, uint32_max, uint32_max);
		if(trial.deferred * 100 <= test.log.size())
		{
			rowTicks = candidate;
			break;
		}
	}
	DsSequencer layout(seqData, programs);
	play(layout, uint32_max, rowTicks);

	// Ноты, которые держатся через конец петли. Их отпускание приходится уже
	// на следующий проход: переносим его в начало петли, на тот же отступ.
	// Если в этом месте голос на первом проходе занят другой нотой, перенос
	// невозможен, и петлёй берём второй проход: в нём отпускания нот из
	// первого прохода уже стоят на своих местах.
	struct Crossing { uint8 track, slot; uint32 offset; };
	std::vector<Crossing> crossing;
	bool doubled = false;
	if(consistent)
	{
		for(const auto &n : layout.log)
		{
			if(n.start >= loopEndTick || n.release < loopEndTick)
				continue;
			if(n.release == uint32_max || n.release - loopEndTick >= loopEndTick - loopStartTick)
				doubled = true;  // держится дольше целой петли
			else
				crossing.push_back({n.track, n.slot, n.release - loopEndTick});
		}
		for(const auto &c : crossing)
		{
			const uint32 at = loopStartTick + c.offset;
			for(const auto &n : layout.log)
			{
				// Голос занят на первом проходе — или им же что-то нажимается
				// в той же строке, куда легло бы отпускание.
				if(n.track == c.track && n.slot == c.slot && n.start < loopEndTick && n.release >= loopStartTick
					&& (n.start < at || (rowTicks && n.start / rowTicks == at / rowTicks)))
					doubled = true;
			}
		}
	}
	// Тик модуля — тик DS, поэтому разная кадровая фаза тиков на первом
	// проходе петли и на повторах модуля не касается.
	DsSequencer seq(seqData, programs);
	const uint32 restartTick = consistent ? (doubled ? loopEndTick : loopStartTick) : uint32_max;
	const uint32 endTick = consistent ? (doubled ? 2 * loopEndTick - loopStartTick : loopEndTick) : uint32_max;
	play(seq, restartTick, rowTicks);
	heldAcross = doubled;
	if(seq.rows.empty() || seq.variants.empty())
		return false;
	if(consistent && !doubled)
	{
		const auto frameOfTick = [&](uint32 tk) -> size_t
		{
			const auto it = std::upper_bound(seq.rows.begin(), seq.rows.end(), tk, [](uint32 value, const Row &row) { return value < row.tick; });
			return static_cast<size_t>(it - seq.rows.begin());
		};
		for(const auto &c : crossing)
		{
			const size_t f = frameOfTick(loopStartTick + c.offset);
			if(f >= seq.rows.size())
				continue;
			Cell &cell = seq.rows[f].At(static_cast<uint16>(c.track * 256 + c.slot));
			if(cell.note == NOTE_NONE)
				cell.note = NOTE_KEYOFF;
		}
	}
	// Строки модуля и тик, с которого каждая начинается.
	Compact compact;
	std::vector<uint32> rowStartTick;
	ROWINDEX restartRow = ROWINDEX_INVALID;
	if(rowTicks)
	{
		compact = CompactRows(seq.rows, rowTicks, endTick, restartTick);
		for(uint32 r = 0; r < compact.rows.size(); r++)
			rowStartTick.push_back(r * rowTicks);
		if(consistent && restartTick / rowTicks < compact.rows.size())
			restartRow = static_cast<ROWINDEX>(restartTick / rowTicks);
	} else
	{
		// Строка — кадр: строки секвенсора идут как есть.
		const auto frameOfTick = [&](uint32 tk) -> size_t
		{
			const auto it = std::upper_bound(seq.rows.begin(), seq.rows.end(), tk, [](uint32 value, const Row &row) { return value < row.tick; });
			return static_cast<size_t>(it - seq.rows.begin());
		};
		const size_t frames = std::min(seq.rows.size(), consistent ? frameOfTick(endTick) : seq.rows.size());
		compact.rows.resize(frames);
		compact.tempo.assign(frames, 0);
		for(size_t f = 0; f < frames; f++)
		{
			for(const auto &cell : seq.rows[f].cells)
			{
				OutCell out;
				out.note = cell.note;
				out.instr = cell.instr;
				out.hasVolume = cell.hasVolume;
				out.volume = cell.volume;
				out.effects = cell.effects;
				compact.rows[f].emplace_back(cell.channel, std::move(out));
			}
			rowStartTick.push_back(f ? seq.rows[f - 1].tick : 0);
		}
		if(consistent && frameOfTick(restartTick) < frames)
			restartRow = static_cast<ROWINDEX>(frameOfTick(restartTick));
	}
	const ROWINDEX numRows = static_cast<ROWINDEX>(compact.rows.size());
	if(!numRows)
		return false;

	// Каналы: дорожки по порядку, у каждой столько, сколько держалось нот.
	std::array<CHANNELINDEX, 16> trackBase{};
	CHANNELINDEX numChannels = 0;
	for(uint8 t = 0; t < 16; t++)
	{
		trackBase[t] = numChannels;
		numChannels = static_cast<CHANNELINDEX>(numChannels + seq.slots[t].size());
	}
	if(!numChannels || numChannels > MAX_BASECHANNELS)
		return false;

	InitializeGlobals(MOD_TYPE_MPT, numChannels);
	m_playBehaviour = GetDefaultPlaybackBehaviour(MOD_TYPE_MPT);
	m_playBehaviour.set(kFurnacePersistentFinetune);  // изгиб — состояние дорожки, переживает ноты
	m_SongFlags.set(SONG_LINEARSLIDES);
	// Тик модуля — тик DS, 48 на долю: скорость — тиков в строке, строк на
	// долю 48 / скорость. Темп модуля почти равен темпу DS (тик DS длится
	// 240 / темп кадров). Классический режим округлял бы длину тика до целых
	// отсчётов, и модуль уходил бы вперёд; современный копит дробную часть.
	// В покадровом импорте строка и тик — кадр DS, 96 строк на долю: при
	// темпе DS 120 доля ровно 96 кадров, и темп модуля выходит те же ~120.
	const double tempoFactor = 60.0 / (240.0 * DSFrameSeconds * 48.0);
	m_nTempoMode = TempoMode::Modern;
	if(rowTicks)
	{
		m_nDefaultRowsPerBeat = 48 / rowTicks;
		m_nDefaultRowsPerMeasure = 192 / rowTicks;
		Order().SetDefaultTempo(TEMPO(std::clamp(compact.tempo[0] * tempoFactor, 32.0, 1000.0)));
		Order().SetDefaultSpeed(rowTicks);
	} else
	{
		m_nDefaultRowsPerBeat = 96;
		m_nDefaultRowsPerMeasure = 384;
		Order().SetDefaultTempo(TEMPO(60.0 / (DSFrameSeconds * m_nDefaultRowsPerBeat)));
		Order().SetDefaultSpeed(1);
	}
	m_nDefaultGlobalVolume = static_cast<uint32>(std::lround(MAX_GLOBAL_VOLUME * DecibelToLinear(SquareDecibel(seqVolume))));
	m_nSamplePreAmp = 80;  // DS: полный сэмпл на одном канале — около -4 дБ от потолка

	for(uint8 t = 0; t < 16; t++)
	{
		for(size_t s = 0; s < seq.slots[t].size(); s++)
		{
			const CHANNELINDEX chn = static_cast<CHANNELINDEX>(trackBase[t] + s);
			std::string name = "Track " + std::to_string(t);
			if(s)
				name += "." + std::to_string(s + 1);
			ChnSettings[chn].szName = name;
			ChnSettings[chn].nPan = 128;
			ChnSettings[chn].nVolume = 64;
		}
	}

	// Сэмплы — по требованию инструментов.
	std::map<uint32, SAMPLEINDEX> sampleIndex;
	const auto getSample = [&](const NoteDef &def) -> SAMPLEINDEX
	{
		const uint32 key = def.type == 1 ? ((static_cast<uint32>(def.swar & 3) << 16) | def.swav) : def.type == 2 ? (0x1000000u | std::min<uint16>(def.swav, 7)) : 0x2000000u;
		if(auto it = sampleIndex.find(key); it != sampleIndex.end())
			return it->second;
		if(m_nSamples >= MAX_SAMPLES - 1)
			return 0;
		const SAMPLEINDEX smp = ++m_nSamples;
		sampleIndex[key] = smp;
		ModSample &sample = Samples[smp];
		sample.Initialize(MOD_TYPE_MPT);
		if(def.type == 2)
		{
			// Прямоугольник PSG: скважность (n+1)/8, у n=7 — тишина.
			const int duty = def.swav < 7 ? def.swav + 1 : 0;
			sample.nLength = 64;
			sample.nLoopStart = 0;
			sample.nLoopEnd = 64;
			sample.uFlags.set(CHN_16BIT | CHN_LOOP);
			sample.nC5Speed = static_cast<uint32>(std::lround(PSGBaseRate));
			if(sample.AllocateSample())
			{
				for(int i = 0; i < 64; i++)
					sample.sample16()[i] = static_cast<int16>(duty == 0 ? 0 : (i < duty * 8 ? 32767 : -32767));
				sample.PrecomputeLoops(*this, false);
			}
			m_szNames[smp] = "PSG " + std::to_string(duty * 125 / 10) + "." + std::to_string(duty * 125 % 10) + "%";
			return smp;
		}
		if(def.type == 3)
		{
			// Шум: 15-битный регистр сдвига, период 32767 отсчётов.
			sample.nLength = 32767;
			sample.nLoopStart = 0;
			sample.nLoopEnd = 32767;
			sample.uFlags.set(CHN_16BIT | CHN_LOOP);
			sample.nC5Speed = static_cast<uint32>(std::lround(PSGBaseRate / 8.0));
			if(sample.AllocateSample())
			{
				uint32 x = 0x7FFF;
				for(uint32 i = 0; i < 32767; i++)
				{
					const bool bit = x & 1;
					x = bit ? (x >> 1) ^ 0x6000 : x >> 1;
					sample.sample16()[i] = static_cast<int16>(bit ? -32767 : 32767);
				}
				sample.PrecomputeLoops(*this, false);
			}
			m_szNames[smp] = "Noise";
			return smp;
		}
		const int arc = def.swar & 3;
		if(def.swav >= waveArchives[arc].size())
			return smp;
		const size_t at = waveArchives[arc][def.swav];
		if(at + 12 > waveArchiveEnd[arc])
			return smp;
		const uint8 format = rom[at], loop = rom[at + 1];
		const uint16 timer = RD16(rom, at + 4), loopStart = RD16(rom, at + 6);
		const uint32 loopLength = RD32(rom, at + 8);
		size_t bytes = (static_cast<size_t>(loopStart) + loopLength) * 4;
		bytes = std::min(bytes, waveArchiveEnd[arc] - (at + 12));
		const uint8 *src = rom.data() + at + 12;
		sample.nC5Speed = timer ? static_cast<uint32>(std::lround(ARM7Clock / timer)) : 8363;
		SmpLength loopFrom = 0;
		if(format == 0)
		{
			sample.nLength = static_cast<SmpLength>(bytes);
			loopFrom = loopStart * 4u;
			if(sample.AllocateSample())
			{
				for(size_t i = 0; i < bytes; i++)
					sample.sample8()[i] = static_cast<int8>(src[i]);
			}
		} else if(format == 1)
		{
			sample.uFlags.set(CHN_16BIT);
			sample.nLength = static_cast<SmpLength>(bytes / 2);
			loopFrom = loopStart * 2u;
			if(sample.AllocateSample())
			{
				for(size_t i = 0; i < bytes / 2; i++)
					sample.sample16()[i] = static_cast<int16>(src[i * 2] | (src[i * 2 + 1] << 8));
			}
		} else if(format == 2 && bytes > 4)
		{
			// IMA-ADPCM DS: заголовок с начальным значением и шагом, затем
			// полубайты, младший первым.
			static constexpr int8 indexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
			static constexpr uint16 stepTable[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
			sample.uFlags.set(CHN_16BIT);
			sample.nLength = static_cast<SmpLength>((bytes - 4) * 2);
			loopFrom = loopStart ? (loopStart * 4u - 4u) * 2u : 0;
			if(sample.AllocateSample())
			{
				int32 predictor = static_cast<int16>(src[0] | (src[1] << 8));
				int index = std::min<int>(src[2], 88);
				int16 *out = sample.sample16();
				for(size_t i = 4; i < bytes; i++)
				{
					for(int half = 0; half < 2; half++)
					{
						const int n = half ? (src[i] >> 4) : (src[i] & 0x0F);
						const int step = stepTable[index];
						int diff = step >> 3;
						if(n & 1) diff += step >> 2;
						if(n & 2) diff += step >> 1;
						if(n & 4) diff += step;
						predictor = (n & 8) ? std::max(-32767, predictor - diff) : std::min(32767, predictor + diff);
						index = std::clamp(index + indexTable[n & 7], 0, 88);
						*out++ = static_cast<int16>(predictor);
					}
				}
			}
		}
		if(loop && loopFrom < sample.nLength)
		{
			sample.nLoopStart = loopFrom;
			sample.nLoopEnd = sample.nLength;
			sample.uFlags.set(CHN_LOOP);
		}
		if(sample.HasSampleData())
			sample.PrecomputeLoops(*this, false);
		m_szNames[smp] = "SWAR " + std::to_string(arc) + " / " + std::to_string(def.swav);
		return smp;
	};

	// Инструменты: вариант = программа банка + огибающая + глубина изгиба.
	std::map<uint8, int> variantsOfProgram;
	std::map<std::tuple<uint8, uint8, uint8, uint8, uint8>, int> variantsOfEnvelope;
	for(const auto &v : seq.variants)
	{
		variantsOfProgram[v.program]++;
		variantsOfEnvelope[{v.program, v.attack, v.decay, v.sustain, v.release}]++;
	}
	for(size_t i = 0; i < seq.variants.size(); i++)
	{
		const Variant &v = seq.variants[i];
		const INSTRUMENTINDEX ins = static_cast<INSTRUMENTINDEX>(i + 1);
		ModInstrument *instrument = AllocateInstrument(ins, 0);
		if(instrument == nullptr)
			return false;
		m_nInstruments = std::max(m_nInstruments, ins);
		const Program &p = programs[v.program];
		std::string name = "Prg " + std::to_string(v.program) + (p.kind == 16 ? " drums" : p.kind == 17 ? " split" : "");
		if(variantsOfProgram[v.program] > 1)
			name += " A" + std::to_string(v.attack) + " D" + std::to_string(v.decay) + " S" + std::to_string(v.sustain) + " R" + std::to_string(v.release);
		if(variantsOfEnvelope[{v.program, v.attack, v.decay, v.sustain, v.release}] > 1)
			name += " PB" + std::to_string(v.pwd);
		instrument->name = name;
		instrument->nNNA = NewNoteAction::NoteOff;
		instrument->nFadeOut = 0;
		instrument->midiPWD = static_cast<int8>(v.pwd);
		for(uint8 n = 0; n < NOTE_MAX; n++)
		{
			const NoteDef *def = p.Find(n);  // n — клавиша DS, нота модуля n + 1
			if(def == nullptr || def->type < 1 || def->type > 3)
				continue;
			instrument->Keyboard[n] = getSample(*def);
			instrument->NoteMap[n] = static_cast<uint8>(std::clamp(n + NOTE_MIDDLEC - def->base, int(NOTE_MIN), int(NOTE_MAX)));
		}
		BuildEnvelope(instrument->VolEnv, v);
	}

	// Узоры: такт DS (192 тика) — узор, но не длиннее 1024 строк. Такт
	// открывает строка, в которой исполняется его первый тик.
	std::vector<ROWINDEX> patternStart{0};
	{
		uint32 nextBar = 192;
		for(ROWINDEX r = 1; r < numRows; r++)
		{
			const bool bar = rowStartTick[r] >= nextBar && rowStartTick[r - 1] < nextBar;
			if(r == restartRow || bar || r - patternStart.back() >= 1024)
				patternStart.push_back(r);
			while(rowStartTick[r] >= nextBar)
				nextBar += 192;
		}
	}
	Order().resize(static_cast<ORDERINDEX>(patternStart.size()));
	std::vector<ModCommand *> rowPointer(numRows);
	for(size_t p = 0; p < patternStart.size(); p++)
	{
		const ROWINDEX first = patternStart[p], last = p + 1 < patternStart.size() ? patternStart[p + 1] : numRows;
		const PATTERNINDEX pat = Patterns.InsertAny(last - first);
		if(pat == PATTERNINDEX_INVALID)
			return false;
		Order()[static_cast<ORDERINDEX>(p)] = pat;
		for(ROWINDEX r = first; r < last; r++)
			rowPointer[r] = Patterns[pat].GetpModCommand(r - first, 0);
		if(first == restartRow)
			Order().SetRestartPos(static_cast<ORDERINDEX>(p));
	}
	std::vector<uint8> used(numChannels, 1);
	for(ROWINDEX r = 0; r < numRows; r++)
	{
		for(const auto &[key, cell] : compact.rows[r])
		{
			const uint8 track = static_cast<uint8>(key >> 8), slot = static_cast<uint8>(key & 0xFF);
			if(slot >= seq.slots[track].size())
				continue;
			const CHANNELINDEX c = static_cast<CHANNELINDEX>(trackBase[track] + slot);
			ModCommand &m = rowPointer[r][c];
			m.note = cell.note;
			m.instr = cell.instr;
			if(cell.hasVolume)
			{
				m.volcmd = VOLCMD_VOLUME;
				m.vol = cell.volume;
			}
			uint8 slotIndex = 0;
			for(const auto &[cmd, param] : cell.effects)
			{
				if(slotIndex >= ModCommand::Effects())
					break;
				m.SetEffect(slotIndex++, cmd, param);
			}
			used[c] = std::max(used[c], slotIndex);
		}
		// Смена темпа DS — в любой свободной колонке строки.
		const bool tempoChanged = rowTicks && r > 0 && compact.tempo[r] != compact.tempo[r - 1];
		const bool tempoAtRestart = rowTicks && r == restartRow && compact.tempo[r] != compact.tempo[numRows - 1];
		if(tempoChanged || tempoAtRestart)
		{
			const uint8 param = static_cast<uint8>(std::clamp(static_cast<int>(std::lround(compact.tempo[r] * tempoFactor)), 32, 255));
			for(CHANNELINDEX c = 0; c < numChannels; c++)
			{
				ModCommand &m = rowPointer[r][c];
				if(const uint8 slot = m.FreeEffectSlot(); slot < ModCommand::Effects())
				{
					m.SetEffect(slot, CMD_TEMPO, param);
					used[c] = std::max<uint8>(used[c], slot + 1);
					break;
				}
			}
		}
	}
#ifdef MODPLUG_TRACKER
	for(CHANNELINDEX c = 0; c < numChannels; c++)
		ChnSettings[c].effectColumns = used[c];
#endif

	// Названия и сведения из тегов.
	const auto tag = [&](const char *key) -> std::string
	{
		auto it = psf.tags.find(key);
		return it != psf.tags.end() ? it->second : std::string();
	};
	const mpt::Charset charset = tag("utf8").empty() ? mpt::Charset::Windows1252 : mpt::Charset::UTF8;
	std::string seqName;
	if(symbBase)
	{
		const size_t list = symbBase + RD32(rom, symbBase + 8);
		if(seqNo < RD32(rom, list))
		{
			const uint32 off = RD32(rom, list + 4 + 4 * seqNo);
			for(size_t i = symbBase + off; off && i < rom.size() && rom[i] && seqName.size() < 64; i++)
				seqName += static_cast<char>(rom[i]);
		}
	}
	m_songName = !tag("title").empty() ? mpt::ToCharset(mpt::Charset::UTF8, mpt::ToUnicode(charset, tag("title"))) : seqName;
	m_songArtist = mpt::ToUnicode(charset, tag("artist"));
	std::string message;
	for(const char *key : {"game", "year", "copyright", "comment", "2sfby"})
	{
		if(!tag(key).empty())
			message += std::string(key) + ": " + tag(key) + "\n";
	}
	if(!seqName.empty())
		message += "sequence: " + seqName + " (" + std::to_string(seqNo) + ")\n";
	const std::string utf8Message = mpt::ToCharset(mpt::Charset::UTF8, mpt::ToUnicode(charset, message));
	m_songMessage.Read(reinterpret_cast<const std::byte *>(utf8Message.data()), utf8Message.size(), SongMessage::leAutodetect);

	const mpt::ustring rowText = rowTicks ? MPT_UFORMAT("{} ticks per row")(rowTicks) : U_("one frame per row");
	if(consistent)
		AddToLog(LogInformation, MPT_UFORMAT("2SF: sequence {} loops from tick {} to {}{}; {}.")(seqNo, heldAcross ? loopEndTick : loopStartTick, heldAcross ? 2 * loopEndTick - loopStartTick : loopEndTick, heldAcross ? U_(" (second pass: notes held across the loop end)") : U_(""), rowText));
	else
		AddToLog(LogInformation, finished
			? MPT_UFORMAT("2SF: sequence {} does not loop ({} ticks, {}).")(seqNo, seq.tick, rowText)
			: MPT_UFORMAT("2SF: sequence {} has no common loop, {} ticks are imported ({}).")(seqNo, seq.tick, rowText));
	m_modFormat.formatName = UL_("Nintendo DS Sound Format");
	m_modFormat.type = UL_("2sf");
	m_modFormat.madeWithTracker = UL_("Nintendo DS sequence (SSEQ)");
	m_modFormat.charset = mpt::Charset::UTF8;
	return true;
}

OPENMPT_NAMESPACE_END
