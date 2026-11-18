/*
 * SN76489.h
 * Clocked Sega PSG tone and noise generators for imported Furnace patches.
 */

#pragma once

#include "ModChannel.h"
#include "Snd_defs.h"
#include "ChannelScopes.h"
#include "BlipKernel.h"

OPENMPT_NAMESPACE_BEGIN

class SN76489
{
	static constexpr uint32 Clock = 3579545;
	static constexpr uint16 NoiseSeed = 0x8000;

	struct MacroState
	{
		size_t next = 0;
		uint8 wait = 0;
		int32 value = 0;

		bool Tick(const PSGSequence &seq, bool released)
		{
			if(wait)
			{
				wait--;
				return false;
			}
			if(next >= seq.values.size())
				return false;
			if(!released && seq.release < seq.values.size() && next > seq.release)
			{
				if(seq.loop >= next)
					return false;
				next = seq.loop;
			}
			value = seq.values[next++];
			wait = seq.speed - 1;
			if(next == seq.values.size() && seq.loop < next
				&& (seq.release >= seq.values.size() || seq.loop > seq.release))
				next = seq.loop;
			return true;
		}
	};

	struct Voice
	{
		std::shared_ptr<const PSGPatch> patch;
		CHANNELINDEX channel = CHANNELINDEX_INVALID;
		std::array<MacroState, PSGPatch::Count> macros;
		bool keyed = false, released = false, gate = false;
		uint16 period = 1, counter = 1, lastPeriod = 0;
		bool high = false;
		uint8 volume = 15;
		int16 pitchOffset = 0;
		int32 pan = 128;
		double dc = 0;
		// Ступеньки, ещё не дошедшие до выхода.
		Blip::Lane lane;
	};

	struct Chip
	{
		std::array<Voice, 4> voices;
		uint64 phase = 0;
		uint16 noise = NoiseSeed, noiseCounter = 1;
		uint8 mode = 3, preset = 0;

		void SetNoise(uint8 newMode, uint8 newPreset, bool reset)
		{
			if(mode != newMode || (!(newMode & 2) && preset != newPreset) || reset)
				noise = NoiseSeed;
			mode = newMode;
			preset = newPreset;
		}

		// Возвращает, что изменилось на этом такте: +1 если дорожка поднялась,
		// -1 если опустилась. Раньше тики просто считались и усреднялись внутри
		// выходного отсчёта, а усреднение — слабый фильтр: меандр несёт
		// гармоники далеко за половину частоты микшера, и они заворачивались
		// обратно.
		void ClockTick(std::array<int, 4> &change)
		{
			bool thirdRising = false;
			for(size_t lane = 0; lane < 3; lane++)
			{
				auto &voice = voices[lane];
				if(--voice.counter == 0)
				{
					voice.counter = voice.period;
					voice.high = !voice.high;
					thirdRising = lane == 2 && voice.high;
					change[lane] = voice.high ? 1 : -1;
				}
			}
			const bool noiseClock = (mode & 2) ? thirdRising : --noiseCounter == 0;
			if(noiseClock)
			{
				noiseCounter = static_cast<uint16>(32u << preset);
				const uint16 feedback = static_cast<uint16>((noise ^ ((mode & 1) ? noise >> 3 : 0)) & 1);
				const uint16 before = noise & 1;
				noise = static_cast<uint16>((noise >> 1) | (feedback << 15));
				if(before != (noise & 1))
					change[3] = (noise & 1) ? 1 : -1;
			}
		}
	};

	std::array<Chip, 32> m_chips;
	std::array<Voice *, MAX_CHANNELS> m_channels{};
	uint32 m_rate;
	// Полифония: при нескольких чипах нота берёт любой голос своего вида (тон
	// или шум) на первых m_chipCount чипах. При одном чипе — как раньше, голос
	// задан инструментом (чип и дорожка), как у импорта Furnace.
	uint8 m_chipCount = 1;
	uint8 m_nextSteal = 0;
	uint64 m_keyCounter = 0;

	Voice *Find(CHANNELINDEX c) const { return c < m_channels.size() ? m_channels[c] : nullptr; }

	// Чип, которому принадлежит голос: при полифонии это не обязательно чип
	// из инструмента.
	Chip &ChipOf(const Voice *voice)
	{
		for(auto &chip : m_chips)
		{
			if(voice >= chip.voices.data() && voice < chip.voices.data() + chip.voices.size())
				return chip;
		}
		return m_chips[0];
	}

public:
	explicit SN76489(uint32 rate) : m_rate(rate) {}
	void SetSampleRate(uint32 rate) { m_rate = rate; }
	bool HasChannel(CHANNELINDEX c) const { return Find(c) != nullptr; }

	void Reset()
	{
		m_channels.fill(nullptr);
		m_chips = {};
		m_nextSteal = 0;
	}

	void SetChipCount(uint8 count) { m_chipCount = std::clamp<uint8>(count, 1, 4); }

	// Голос для ноты при полифонии: свой прежний, если он того же вида; иначе
	// свободный (без ноты), потом отпущенный, и только потом по кругу отбираем.
	Voice &PickVoice(CHANNELINDEX c, const PSGPatch &patch)
	{
		const bool noise = patch.lane == 3;
		const auto suits = [&](uint8 lane) { return noise ? lane == 3 : lane < 3; };
		if(Voice *own = Find(c); own != nullptr)
		{
			for(uint8 chip = 0; chip < m_chipCount; chip++)
			{
				for(uint8 lane = 0; lane < 4; lane++)
				{
					if(&m_chips[chip].voices[lane] == own && suits(lane))
						return *own;
				}
			}
		}
		Voice *released = nullptr;
		for(uint8 chip = 0; chip < m_chipCount; chip++)
		{
			for(uint8 lane = 0; lane < 4; lane++)
			{
				Voice &voice = m_chips[chip].voices[lane];
				if(!suits(lane))
					continue;
				if(voice.channel == CHANNELINDEX_INVALID || (!voice.keyed && !voice.gate))
					return voice;
				if(voice.released && released == nullptr)
					released = &voice;
			}
		}
		if(released)
			return *released;
		const uint8 lanes = noise ? 1 : 3;
		const uint8 total = static_cast<uint8>(m_chipCount * lanes);
		const uint8 pick = static_cast<uint8>(m_nextSteal++ % total);
		return m_chips[pick / lanes].voices[noise ? 3 : pick % lanes];
	}

	void Patch(CHANNELINDEX c, std::shared_ptr<const PSGPatch> patch)
	{
		auto &voice = (m_chipCount > 1) ? PickVoice(c, *patch) : m_chips[patch->chip].voices[patch->lane];
		if(Find(c) != &voice)
		{
			NoteCut(c, true);
			if(voice.channel != CHANNELINDEX_INVALID)
				NoteCut(voice.channel, true);
			voice.channel = c;
			m_channels[c] = &voice;
		}
		voice.patch = std::move(patch);
		voice.keyed = false;
	}

	bool MoveChannel(CHANNELINDEX from, CHANNELINDEX to)
	{
		auto *voice = Find(from);
		if(!voice)
			return false;
		if(from != to)
		{
			NoteCut(to, true);
			m_channels[from] = nullptr;
			m_channels[to] = voice;
			voice->channel = to;
		}
		return true;
	}

	bool NoteOff(CHANNELINDEX c)
	{
		auto *voice = Find(c);
		if(!voice)
			return false;
		if(voice->released)
			return true;
		voice->released = true;
		const auto &patch = *voice->patch;
		voice->gate = voice->gate && patch.macros[PSGPatch::Volume].release < patch.macros[PSGPatch::Volume].values.size();
		for(size_t i = 0; i < PSGPatch::Count; i++)
		{
			const auto &seq = patch.macros[i];
			if(seq.release < seq.values.size())
			{
				auto &state = voice->macros[i];
				state.next = std::max<size_t>(state.next, seq.release + 1u);
				if(seq.instantRelease)
					state.wait = 0;
			}
		}
		return true;
	}

	bool NoteCut(CHANNELINDEX c, bool unassign)
	{
		auto *voice = Find(c);
		if(!voice)
			return false;
		voice->keyed = voice->released = voice->gate = false;
		if(unassign)
		{
			voice->channel = CHANNELINDEX_INVALID;
			voice->patch.reset();
			m_channels[c] = nullptr;
		}
		return true;
	}

	void NoiseMode(CHANNELINDEX c, uint8 mode)
	{
		if(auto *voice = Find(c); voice && voice->patch->lane == 3)
		{
			auto &chip = ChipOf(voice);
			chip.SetNoise(mode, chip.preset, true);
		}
	}

	bool Frequency(CHANNELINDEX c, uint32 milliHertz, bool keyOff)
	{
		auto *voice = Find(c);
		if(!voice)
			return false;
		if(keyOff)
			NoteOff(c);
		if(!milliHertz || (!voice->gate && keyOff))
			return true;

		const auto &patch = *voice->patch;
		const bool triggered = !keyOff && (!voice->keyed || voice->released);
		if(triggered)
		{
			voice->keyed = voice->gate = true;
			voice->released = false;
			voice->macros = {};
			voice->macros[PSGPatch::Volume].value = 15;
			for(size_t i = 0; i < PSGPatch::Count; i++)
				voice->macros[i].wait = patch.macros[i].delay;
			voice->pitchOffset = 0;
		}

		std::array<bool, PSGPatch::Count> changed{};
		for(size_t i = 0; i < PSGPatch::Count; i++)
			changed[i] = voice->macros[i].Tick(patch.macros[i], voice->released);
		const int32 arp = voice->macros[PSGPatch::Arpeggio].value;
		const bool fixed = ((arp < 0) != ((arp & 0x40000000) != 0));
		const int32 semitones = std::clamp(fixed ? (arp ^ 0x40000000) : arp, -128, 180);
		if(changed[PSGPatch::Pitch])
		{
			const int64 offset = voice->macros[PSGPatch::Pitch].value;
			const int64 pitch = offset + (patch.macros[PSGPatch::Pitch].relative ? voice->pitchOffset : 0);
			voice->pitchOffset = static_cast<int16>(std::clamp<int64>(pitch, -32768, 32767));
		}
		const double frequency = (fixed ? 16351.6 : milliHertz) * std::exp2((semitones + voice->pitchOffset / 128.0) / 12.0);
		// Furnace's Sega PSG note scale sounds two octaves above this tracker's note scale.
		const double psgFrequency = patch.fastNoise
			? frequency * std::exp2(PSGFastNoiseShift / 12.0)
			: (patch.furnacePitch ? frequency * 4.0 : frequency);
		const double rawPeriod = Clock * 1000.0 / (32.0 * psgFrequency);
		// Furnace's optional high-note shortcut uses successive hardware periods above period 16.
		const double mappedPeriod = patch.easyPitch && rawPeriod < 16.0 ? 16.0 - 12.0 * std::log2(16.0 / rawPeriod) : rawPeriod;
		const uint16 period = static_cast<uint16>(std::lround(std::clamp(mappedPeriod, 1.0, 1023.0)));
		const bool pitchChanged = triggered || voice->lastPeriod != period;
		voice->lastPeriod = period;
		if(patch.lane < 3)
		{
			if(pitchChanged)
				voice->period = period;
		} else
		{
			auto &chip = ChipOf(voice);
			const auto mode = changed[PSGPatch::Duty] ? static_cast<uint8>(voice->macros[PSGPatch::Duty].value & 3) : chip.mode;
			auto preset = chip.preset;
			const int note = static_cast<int>(std::lround(12.0 * std::log2(frequency / 16351.6)));
			const int noteClass = (note % 12 + 12) % 12;
			if(noteClass <= 2)
				preset = static_cast<uint8>(2 - noteClass);
			const bool reset = (patch.resetNoise && pitchChanged)
				|| (changed[PSGPatch::Reset] && voice->macros[PSGPatch::Reset].value == 1);
			chip.SetNoise(mode, preset, reset);
			if((mode & 2) && (pitchChanged || changed[PSGPatch::Duty]))
				chip.voices[2].period = period;
		}
		return true;
	}

	bool Volume(CHANNELINDEX c, uint8 volume, bool applyToModulator)
	{
		auto *voice = Find(c);
		if(!voice)
			return false;
		if(!applyToModulator)
			voice->volume = static_cast<uint8>((volume * 15 + 31) / 63);
		return true;
	}

	bool Pan(CHANNELINDEX c, int32 pan)
	{
		auto *voice = Find(c);
		if(!voice)
			return false;
		voice->pan = std::clamp<int32>(pan, 0, 256);
		return true;
	}

	void Mix(int32 *target, size_t count, uint32 factor, const ModChannel *channels, uint16 chipVolume, ChannelScopes *scopes = nullptr)
	{
		constexpr double level[] = {0.0, 0.03981, 0.05012, 0.06310, 0.07943, 0.1, 0.12589, 0.15849,
			0.19953, 0.25119, 0.31623, 0.39811, 0.50119, 0.63096, 0.79433, 1.0};
		for(auto &chip : m_chips)
		{
			std::array<double, 4> left{}, right{};
			bool assigned = false;
			for(size_t lane = 0; lane < 4; lane++)
			{
				auto &voice = chip.voices[lane];
				if(voice.channel == CHANNELINDEX_INVALID)
					continue;
				assigned = true;
				if(!voice.gate || channels[voice.channel].dwFlags[CHN_MUTE | CHN_SYNCMUTE])
					continue;
				const int vol = voice.volume + std::clamp(voice.macros[PSGPatch::Volume].value, 0, 15) - 15;
				// Match the Genesis PSG level to the existing OPN2 mixer calibration.
				// Громкость микросхемы из песни Furnace лежит в patch->mixGain,
				// а chipVolume — поправка микшера поверх неё.
				const double gain = level[std::max(0, vol)] * (18800000.0 / (65536.0 * 256.0)) * factor
					* voice.patch->mixGain * chipVolume / ChipVolumeUnity;
				left[lane] = gain * std::min<int32>(256 - voice.pan, 128) / 128.0;
				right[lane] = gain * std::min<int32>(voice.pan, 128) / 128.0;
			}
			if(!assigned)
				continue;
			for(size_t i = 0; i < count; i++)
			{
				chip.phase += Clock;
				while(chip.phase >= static_cast<uint64>(m_rate) * 16)
				{
					chip.phase -= static_cast<uint64>(m_rate) * 16;
					std::array<int, 4> change{};
					chip.ClockTick(change);
					// Где внутри выходного отсчёта случился этот такт: остаток
					// накопителя — время, прошедшее с него.
					const double position = std::clamp(
						1.0 - static_cast<double>(chip.phase) / Clock, 0.0, 0.999999);
					for(size_t lane = 0; lane < 4; lane++)
					{
						if(change[lane])
							chip.voices[lane].lane.Add(change[lane], position);
					}
				}
				for(size_t lane = 0; lane < 4; lane++)
				{
					auto &voice = chip.voices[lane];
					const double signal = voice.lane.Pop();
					voice.dc += 0.002 * (signal - voice.dc);
					target[2 * i] += static_cast<int32>((signal - voice.dc) * left[lane]);
					target[2 * i + 1] += static_cast<int32>((signal - voice.dc) * right[lane]);
					if(scopes != nullptr && voice.channel != CHANNELINDEX_INVALID)
					{
						// В тех же единицах, что и общий осциллограф.
						scopes->Add(voice.channel, i, static_cast<float>(
							(signal - voice.dc) * (left[lane] + right[lane]) * 0.5 / 134217728.0));
					}
				}
			}
		}
	}
};

OPENMPT_NAMESPACE_END

