/*
 * OPN2.h
 * Native four-operator YM2612 voices driven by the tracker mixer.
 */

#pragma once

#include "Snd_defs.h"
#include "ModChannel.h"
#include "../include/ymfm/ymfm_opn.h"
#include "../include/ymfm/ymfm_opm.h"

OPENMPT_NAMESPACE_BEGIN

class OPN2
{
	static constexpr uint32 Clock = 7670454;
	static constexpr uint32 OPMClock = 3579545;
	static constexpr size_t OPN2Voices = 6, OPMVoices = 8;

	struct Voice
	{
		ymfm::ymfm_interface host;
		ymfm::ym2612 chip{host};
		std::unique_ptr<ymfm::ym2151> opm;
		uint32 sampleRate = chip.sample_rate(Clock);
		CHANNELINDEX channel = CHANNELINDEX_INVALID;
		bool keyed = false, hasPlayed = false;
		uint8 volume = 63;
		int32 pan = 128;
		uint64 phase = 0;
		double previous = 0, current = 0, dc = 0;

		void Write(uint8 reg, uint8 value)
		{
			if(opm)
			{
				opm->write_address(reg);
				opm->write_data(value);
			} else
			{
				chip.write_address(reg);
				chip.write_data(value);
			}
		}

		void Key(bool on)
		{
			if(keyed == on)
				return;
			Write(opm ? 0x08 : 0x28, on ? (opm ? 0x78 : 0xF0) : 0);
			keyed = on;
			hasPlayed |= on;
		}

		void Reset(bool useOPM)
		{
			if(useOPM)
			{
				if(!opm)
					opm = std::make_unique<ymfm::ym2151>(host);
				opm->reset();
				sampleRate = opm->sample_rate(OPMClock);
			} else
			{
				chip.reset();
			}
			phase = 0;
			previous = current = 0;
			dc = useOPM ? 0 : 504;  // ymfm idle value of the six summed OPN2 DAC lanes
			keyed = hasPlayed = false;
			volume = 63;
			pan = 128;
		}
	};

	std::array<Voice, OPN2Voices + OPMVoices> m_voices;
	uint32 m_rate;
	std::array<size_t, 2> m_nextVoice{};

	Voice *Find(CHANNELINDEX channel)
	{
		for(auto &voice : m_voices)
			if(voice.channel == channel)
				return &voice;
		return nullptr;
	}

public:
	explicit OPN2(uint32 rate) : m_rate{rate} {}
	void SetSampleRate(uint32 rate) { m_rate = rate; }

	bool HasChannel(CHANNELINDEX channel) const
	{
		for(const auto &voice : m_voices)
			if(voice.channel == channel)
				return true;
		return false;
	}

	void Reset()
	{
		for(auto &voice : m_voices)
			voice.channel = CHANNELINDEX_INVALID;
		m_nextVoice.fill(0);
	}

	void Patch(CHANNELINDEX channel, const OPN2Patch &patch, bool useOPM)
	{
		auto *voice = Find(channel);
		if(voice && static_cast<bool>(voice->opm) != useOPM)
		{
			NoteCut(channel, true);
			voice = nullptr;
		}
		if(!voice)
		{
			const size_t start = useOPM ? OPN2Voices : 0;
			const size_t count = useOPM ? OPMVoices : OPN2Voices;
			const auto first = m_voices.begin() + start;
			const auto last = first + count;
			// Notes for the same tick are assigned before any of them are keyed on.
			// Keep their reservations while there are unused hardware voices.
			const auto free = std::find_if(first, last, [](const Voice &candidate)
				{ return candidate.channel == CHANNELINDEX_INVALID; });
			if(free != last)
				voice = &*free;
			if(!voice)
			{
				const auto released = std::find_if(first, last, [](const Voice &candidate)
					{ return !candidate.keyed && candidate.hasPlayed; });
				if(released != last)
					voice = &*released;
			}
			if(!voice)
			{
				auto &next = m_nextVoice[useOPM];
				voice = &m_voices[start + next];
				next = (next + 1) % count;
			}
			voice->Reset(useOPM);
			voice->channel = channel;
		}

		if(useOPM)
		{
			constexpr uint8 offsets[] = {0, 16, 8, 24};
			constexpr uint8 masks[] = {0x7f, 0x7f, 0xdf, 0x9f, 0xdf, 0xff};
			for(size_t op = 0; op < 4; op++)
				for(size_t reg = 0; reg < std::size(masks); reg++)
					voice->Write(static_cast<uint8>(0x40 + reg * 0x20 + offsets[op]), patch[1 + op * 7 + reg] & masks[reg]);
			voice->Write(0x20, (patch[0] & 0x3f) | 0xc0);
			return;
		}

		static constexpr uint8 OperatorOffset[] = {0, 8, 4, 12};
		static constexpr uint8 RegisterMask[] = {0x7F, 0x7F, 0xDF, 0x9F, 0x1F, 0xFF, 0x0F};
		for(size_t op = 0; op < 4; op++)
			for(size_t reg = 0; reg < 7; reg++)
				voice->Write(static_cast<uint8>(0x30 + reg * 0x10 + OperatorOffset[op]), patch[1 + op * 7 + reg] & RegisterMask[reg]);
		voice->Write(0xB0, patch[0] & 0x3F);
		voice->Write(0xB4, 0xC0);
	}

	void OperatorLevel(CHANNELINDEX channel, uint8 op, uint8 level)
	{
		if(auto *voice = Find(channel))
		{
			constexpr uint8 opmOffset[] = {0, 16, 8, 24};
			constexpr uint8 opnOffset[] = {0, 8, 4, 12};
			voice->Write(voice->opm ? 0x60 + opmOffset[op] : 0x40 + opnOffset[op], level);
		}
	}

	bool NoteOff(CHANNELINDEX channel)
	{
		auto *voice = Find(channel);
		if(!voice)
			return false;
		voice->Key(false);
		return true;
	}

	bool NoteCut(CHANNELINDEX channel, bool unassign)
	{
		auto *voice = Find(channel);
		if(!voice)
			return false;
		voice->Key(false);
		voice->volume = 0;
		if(unassign)
			voice->channel = CHANNELINDEX_INVALID;
		return true;
	}

	bool MoveChannel(CHANNELINDEX from, CHANNELINDEX to)
	{
		auto *voice = Find(from);
		if(!voice)
			return false;
		if(from != to)
		{
			NoteCut(to, true);
			voice->channel = to;
		}
		return true;
	}

	bool Frequency(CHANNELINDEX channel, uint32 milliHertz, bool keyOff)
	{
		auto *voice = Find(channel);
		if(!voice)
			return false;
		if(milliHertz && voice->opm)
		{
			// Key code starts at C-sharp; the remaining quarter-octave codes are skipped.
			constexpr uint8 codes[] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
			// OPM key fraction at concert A for the 3.579545 MHz clock.
			const double key = 56.0 + 12.0 * std::log2(milliHertz / 440000.0);
			const int fraction = std::clamp(static_cast<int>(std::lround(key * 64)), 0, 96 * 64 - 1);
			const int note = fraction / 64;
			voice->Write(0x28, static_cast<uint8>((note / 12) * 16 + codes[note % 12]));
			voice->Write(0x30, static_cast<uint8>((fraction % 64) * 4));
		} else if(milliHertz)
		{
			// The OPN2 phase accumulator advances F-number * 2^block / 2 at the chip rate.
			const uint64 numerator = static_cast<uint64>(milliHertz) * (uint64{1} << 21) * 144;
			const uint64 denominator = static_cast<uint64>(Clock) * 1000;
			uint8 block = 0;
			uint64 fnum = (numerator + denominator / 2) / denominator;
			while(fnum > 2047 && block < 7)
			{
				block++;
				const uint64 scaled = denominator << block;
				fnum = (numerator + scaled / 2) / scaled;
			}
			fnum = std::clamp<uint64>(fnum, 1, 2047);
			voice->Write(0xA4, static_cast<uint8>((block << 3) | (fnum >> 8)));
			voice->Write(0xA0, static_cast<uint8>(fnum & 0xFF));
		}
		voice->Key(!keyOff && milliHertz != 0);
		return true;
	}

	bool Volume(CHANNELINDEX channel, uint8 volume, bool applyToModulator)
	{
		auto *voice = Find(channel);
		if(!voice)
			return false;
		if(!applyToModulator)
			voice->volume = std::min<uint8>(volume, 63);
		return true;
	}

	bool Pan(CHANNELINDEX channel, int32 pan)
	{
		auto *voice = Find(channel);
		if(!voice)
			return false;
		voice->pan = std::clamp<int32>(pan, 0, 256);
		return true;
	}

	void Mix(int32 *target, size_t count, uint32 volumeFactorQ16, const ModChannel *channels)
	{
		for(auto &voice : m_voices)
		{
			if(voice.channel == CHANNELINDEX_INVALID)
				continue;
			const bool muted = channels[voice.channel].dwFlags[CHN_MUTE | CHN_SYNCMUTE];
			const double chipGain = voice.opm ? 240.0 : 1200.0;
			const double gain = muted ? 0.0 : (chipGain / (63.0 * 65536.0)) * volumeFactorQ16 * voice.volume;
			const double left = gain * std::min<int32>(256 - voice.pan, 128) / 128.0;
			const double right = gain * std::min<int32>(voice.pan, 128) / 128.0;
			for(size_t i = 0; i < count; i++)
			{
				voice.phase += voice.sampleRate;
				while(voice.phase >= m_rate)
				{
					voice.phase -= m_rate;
					voice.previous = voice.current;
					if(voice.opm)
					{
						ymfm::ym2151::output_data sample;
						voice.opm->generate(&sample);
						voice.current = sample.data[0];
					} else
					{
						ymfm::ym2612::output_data sample;
						voice.chip.generate(&sample);
						// Remove the YM2612 DAC's idle offset without changing its waveform.
						voice.dc += 0.001 * (sample.data[0] - voice.dc);
						voice.current = sample.data[0] - voice.dc;
					}
				}
				const double sample = voice.previous + (voice.current - voice.previous) * (static_cast<double>(voice.phase) / m_rate);
				target[2 * i] += static_cast<int32>(sample * left);
				target[2 * i + 1] += static_cast<int32>(sample * right);
			}
		}
	}
};

OPENMPT_NAMESPACE_END

