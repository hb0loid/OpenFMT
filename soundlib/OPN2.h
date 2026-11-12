/*
 * OPN2.h
 * ------
 * Purpose: Native Yamaha FM voices (YM2612 and YM2151) driven by the tracker mixer.
 * Notes  : One chip instance serves all of its own channels, the way OpenMPT drives
 *          the OPL. Channel volume is applied to the total level of the operators that
 *          reach the output, panning is applied through the chip's own output enable
 *          bits, and the chip's stereo output is mixed once per rendered chunk.
 *
 */

#pragma once

#include "Snd_defs.h"
#include "ModChannel.h"
#include "../include/ymfm/ymfm_opn.h"
#include "../include/ymfm/ymfm_opm.h"

#include <cmath>

OPENMPT_NAMESPACE_BEGIN

class OPN2
{
public:
	static constexpr uint8 OPN2Channels = 6, OPMChannels = 8;

private:
	static constexpr uint32 OPN2Clock = 7670454;  // YM2612 as clocked in the Mega Drive
	static constexpr uint32 OPMClock = 3579545;   // YM2151

	// OpenMPT scales the OPL's 16-bit output by this factor so that a chip voice ends up
	// about as loud as a sample voice (see OPL::Mix). ymfm normalises both FM chips to the
	// same 16-bit full scale, so reusing the identical factor keeps every chip on the
	// mixer's single reference level instead of a constant tuned per chip by ear.
	static constexpr int32 MixerReference = 6169;

	// Patch byte 0 holds algorithm and feedback, followed by seven register values for each
	// of the four operators, in logical operator order 1, 2, 3, 4.
	static constexpr uint8 OPN2OperatorOffset[] = {0, 8, 4, 12};
	static constexpr uint8 OPMOperatorOffset[] = {0, 16, 8, 24};
	static constexpr uint8 OPN2RegisterMask[] = {0x7F, 0x7F, 0xDF, 0x9F, 0x1F, 0xFF, 0x0F};
	static constexpr uint8 OPMRegisterMask[] = {0x7F, 0x7F, 0xDF, 0x9F, 0xDF, 0xFF};
	// Operators that reach the output for each of the eight algorithms, bit 0 = operator 1.
	static constexpr uint8 CarrierMask[] = {0x08, 0x08, 0x08, 0x08, 0x0A, 0x0E, 0x0E, 0x0F};
	static constexpr size_t LevelRegister = 1;  // Total level is the second register of an operator
	static constexpr uint8 MaxLevel = 127;      // Total level is seven bits, 127 is silence
	static constexpr double DCCutoff = 5.0;     // Hz, removes the YM2612 DAC's idle offset

	struct Slot
	{
		OPN2Patch patch = DefaultOPN2Patch;
		CHANNELINDEX channel = CHANNELINDEX_INVALID;
		uint8 volume = 63, modVolume = 63;
		int8 panning = 0;
		bool keyed = false, hasPlayed = false, muted = false;
		// Сколько отсчётов чипа осталось выждать до постановки ключа.
		uint8 keyOnDelay = 0;
	};

	struct Chip
	{
		ymfm::ymfm_interface host;
		// ym3438 — та же YM2612 без разрыва ЦАП. Furnace по умолчанию считает
		// именно так, а ladder effect включается отдельным флагом песни. С
		// обычной ym2612 выход заметно шумит.
		std::unique_ptr<ymfm::ym3438> opn;
		std::unique_ptr<ymfm::ym2151> opm;
		std::array<Slot, OPMChannels> slots;
		uint32 rate = 0;
		uint64 phase = 0;
		std::array<double, 2> previous{}, current{}, dc{};
		double dcCoef = 0.0;
		size_t next = 0;
		bool isOPM = false;
		bool keyPending = false;  // Хоть у одного голоса ключ ждёт постановки

		uint8 NumChannels() const { return isOPM ? OPMChannels : OPN2Channels; }
		bool Exists() const { return opn != nullptr || opm != nullptr; }
	};

	std::array<Chip, 2> m_chips;  // Index 0 drives the YM2612, index 1 the YM2151
	uint32 m_rate;

	static void Write(Chip &chip, uint16 reg, uint8 value)
	{
		// Лог записей в чип: сравнивать его с VGM-экспортом Furnace оказалось
		// самым быстрым способом ловить расхождения, поэтому он остаётся здесь.
		// Путь берётся один раз, а в тихом случае это одна проверка указателя.
		static const char *const logPath = std::getenv("OPN2_LOG");
		if(logPath != nullptr)
		{
			if(FILE *f = std::fopen(logPath, "a"))
			{
				std::fprintf(f, "%03X %02X\n", reg, value);
				std::fclose(f);
			}
		}
		if(chip.opm)
		{
			chip.opm->write_address(static_cast<uint8>(reg));
			chip.opm->write_data(value);
		} else if(chip.opn)
		{
			if(reg & 0x100)
			{
				chip.opn->write_address_hi(static_cast<uint8>(reg));
				chip.opn->write_data_hi(value);
			} else
			{
				chip.opn->write_address(static_cast<uint8>(reg));
				chip.opn->write_data(value);
			}
		}
	}

	// Register address of a channel register such as 0xA0 or 0xB4. The YM2612 keeps its
	// last three channels on the second port, which bit 8 of the returned address selects.
	static uint16 ChannelRegister(const Chip &chip, uint8 channel, uint8 base)
	{
		if(chip.isOPM)
			return static_cast<uint16>(base + channel);
		return static_cast<uint16>((channel >= 3 ? 0x100 : 0x000) + base + (channel % 3));
	}

	static uint16 OperatorRegister(const Chip &chip, uint8 channel, size_t op, size_t reg)
	{
		if(chip.isOPM)
			return static_cast<uint16>(0x40 + reg * 0x20 + OPMOperatorOffset[op] + channel);
		return static_cast<uint16>((channel >= 3 ? 0x100 : 0x000) + 0x30 + reg * 0x10 + OPN2OperatorOffset[op] + (channel % 3));
	}

	// Tracker volume is a linear amplitude, while total level attenuates in steps of
	// 0.75 dB, so the two are related by a logarithm. Volume 63 leaves the patch untouched.
	// Затухание линейно по децибелам: к Total Level добавляется столько шагов по
	// 0,75 дБ, сколько громкости недостаёт до максимума. Шкала взята как в
	// Furnace, то есть на все 127 шагов TL: `VOL_SCALE_LOG` там считает
	// `TL = baseTL + (127 - vol)` при громкости 0..127. Громкость OpenMPT имеет
	// 64 шага, поэтому один её шаг это примерно два шага TL, около 95 дБ на весь
	// ход ползунка и 47 дБ на половине.
	//
	// Раньше здесь была шкала штатного OPL, 63 шага и 23 дБ на половине. Она
	// совпадала с OPL, с которым регулировку сравнивают, но вдвое не дотягивала
	// по динамике до Furnace: в логе регистров YM2151 видно, что TL несущих мы
	// писали на 4 шага меньше, чем сам Furnace. Разница в разрядности TL (6 бит
	// у OPL против 7 у OPN2 и OPM) — это свойство самих микросхем, а не ошибка.
	//
	// Перевод амплитуды в децибелы логарифмом здесь неверен: он даёт на половине
	// громкости 6 дБ, и регулировка почти не работает.
	static uint8 CalcLevel(uint8 baseLevel, uint8 vol)
	{
		baseLevel &= MaxLevel;
		if(vol >= 63)
			return baseLevel;
		const int missing = (static_cast<int>(63 - vol) * MaxLevel + 31) / 63;
		return static_cast<uint8>(std::min<int>(MaxLevel, baseLevel + missing));
	}

	void InitChip(Chip &chip, bool useOPM)
	{
		chip.isOPM = useOPM;
		if(useOPM)
		{
			if(!chip.opm)
				chip.opm = std::make_unique<ymfm::ym2151>(chip.host);
			chip.opm->reset();
			chip.rate = chip.opm->sample_rate(OPMClock);
		} else
		{
			if(!chip.opn)
				chip.opn = std::make_unique<ymfm::ym3438>(chip.host);
			chip.opn->reset();
			chip.rate = chip.opn->sample_rate(OPN2Clock);
		}
		chip.phase = 0;
		chip.previous.fill(0.0);
		chip.current.fill(0.0);
		chip.dc.fill(0.0);
		// One-pole high pass at a fixed audio frequency, derived from the rate the chip is
		// actually clocked at rather than a hand-picked coefficient.
		constexpr double twoPi = 6.283185307179586;
		chip.dcCoef = chip.rate ? 1.0 - std::exp(-twoPi * DCCutoff / chip.rate) : 0.0;
		chip.next = 0;
		for(auto &slot : chip.slots)
			slot = Slot{};
	}

	Slot *Find(CHANNELINDEX channel, Chip **owner = nullptr, uint8 *index = nullptr)
	{
		for(auto &chip : m_chips)
		{
			if(!chip.Exists())
				continue;
			for(uint8 i = 0; i < chip.NumChannels(); i++)
			{
				if(chip.slots[i].channel != channel)
					continue;
				if(owner)
					*owner = &chip;
				if(index)
					*index = i;
				return &chip.slots[i];
			}
		}
		return nullptr;
	}

	static void WriteKey(Chip &chip, uint8 channel, bool on)
	{
		if(chip.isOPM)
			Write(chip, 0x08, static_cast<uint8>((on ? 0x78 : 0x00) | channel));
		else
			Write(chip, 0x28, static_cast<uint8>((on ? 0xF0 : 0x00) | (channel < 3 ? channel : (channel - 3) + 4)));
	}

	void KeyOn(Chip &chip, uint8 channel, bool on)
	{
		Slot &slot = chip.slots[channel];
		if(!on)
		{
			slot.keyOnDelay = 0;
			if(!slot.keyed)
				return;
			WriteKey(chip, channel, false);
			slot.keyed = false;
			return;
		}
		if(slot.keyed || slot.keyOnDelay)
			return;
		// Ключ ставится не сразу, а после того как чип отработает хотя бы один
		// свой отсчёт со снятым ключом. На смене ноты микшер присылает снятие и
		// постановку в одном тике, то есть до того как чип успеет тактироваться,
		// и генератор огибающей перехода не видит: нота тянется дальше вместо
		// того чтобы начаться заново. Пока у каждого голоса был свой экземпляр
		// чипа, это получалось само, потому что выделение голоса сбрасывало весь
		// чип; у общего экземпляра так делать нельзя.
		// hasPlayed ставится только когда ключ действительно записан, в Mix.
		// Иначе голос, который уже занят под ноту этого тика, но ещё не запущен,
		// выглядит освободившимся, и соседний канал его отбирает: в аккорде
		// часть нот просто пропадает.
		slot.keyOnDelay = 1;
		chip.keyPending = true;
	}

	// Algorithm, feedback and the output enable bits share one register on the YM2151,
	// so panning and the algorithm are always written together.
	void WriteAlgorithmAndPanning(Chip &chip, uint8 channel)
	{
		const Slot &slot = chip.slots[channel];
		const uint8 algorithmFeedback = slot.patch[0] & 0x3F;
		if(chip.isOPM)
		{
			const uint8 outputs = slot.muted ? 0x00 : (slot.panning < 0 ? 0x40 : slot.panning > 0 ? 0x80 : 0xC0);
			Write(chip, ChannelRegister(chip, channel, 0x20), static_cast<uint8>(algorithmFeedback | outputs));
		} else
		{
			const uint8 outputs = slot.muted ? 0x00 : (slot.panning < 0 ? 0x80 : slot.panning > 0 ? 0x40 : 0xC0);
			Write(chip, ChannelRegister(chip, channel, 0xB0), algorithmFeedback);
			Write(chip, ChannelRegister(chip, channel, 0xB4), outputs);
		}
	}

	void WriteLevels(Chip &chip, uint8 channel)
	{
		const Slot &slot = chip.slots[channel];
		const uint8 algorithm = slot.patch[0] & 0x07;
		for(size_t op = 0; op < 4; op++)
		{
			const uint8 baseLevel = slot.patch[1 + op * 7 + LevelRegister] & MaxLevel;
			const bool carrier = (CarrierMask[algorithm] & (1u << op)) != 0;
			uint8 level = CalcLevel(baseLevel, carrier ? slot.volume : slot.modVolume);
			if(slot.muted)
				level = MaxLevel;
			Write(chip, OperatorRegister(chip, channel, op, LevelRegister), level);
		}
	}

	void WritePatch(Chip &chip, uint8 channel)
	{
		const Slot &slot = chip.slots[channel];
		const size_t registers = chip.isOPM ? std::size(OPMRegisterMask) : std::size(OPN2RegisterMask);
		for(size_t op = 0; op < 4; op++)
		{
			for(size_t reg = 0; reg < registers; reg++)
			{
				if(reg == LevelRegister)
					continue;  // Written by WriteLevels, which folds in the channel volume
				const uint8 mask = chip.isOPM ? OPMRegisterMask[reg] : OPN2RegisterMask[reg];
				Write(chip, OperatorRegister(chip, channel, op, reg), slot.patch[1 + op * 7 + reg] & mask);
			}
		}
		WriteLevels(chip, channel);
		WriteAlgorithmAndPanning(chip, channel);
	}

public:
	explicit OPN2(uint32 rate) : m_rate{rate} {}
	void SetSampleRate(uint32 rate) { m_rate = rate; }

	bool HasChannel(CHANNELINDEX channel) const
	{
		for(const auto &chip : m_chips)
		{
			if(!chip.Exists())
				continue;
			for(uint8 i = 0; i < chip.NumChannels(); i++)
				if(chip.slots[i].channel == channel)
					return true;
		}
		return false;
	}

	void Reset()
	{
		for(auto &chip : m_chips)
		{
			if(chip.Exists())
				InitChip(chip, chip.isOPM);
		}
	}

	void Patch(CHANNELINDEX channel, const OPN2Patch &patch, bool useOPM)
	{
		Chip &chip = m_chips[useOPM ? 1 : 0];
		if(!chip.Exists())
			InitChip(chip, useOPM);

		// A channel that moves between the two chips has to release its old voice first.
		Chip *previousChip = nullptr;
		uint8 previousIndex = 0;
		if(Find(channel, &previousChip, &previousIndex) && previousChip != &chip)
			NoteCut(channel, true);

		const uint8 count = chip.NumChannels();
		uint8 found = count;
		for(uint8 i = 0; i < count; i++)
		{
			if(chip.slots[i].channel == channel)
			{
				found = i;
				break;
			}
		}
		if(found == count)
		{
			// Notes starting on the same tick are assigned before any of them are keyed on,
			// so keep their reservations while there are unused channels left.
			for(uint8 i = 0; i < count; i++)
			{
				if(chip.slots[i].channel == CHANNELINDEX_INVALID)
				{
					found = i;
					break;
				}
			}
		}
		if(found == count)
		{
			for(uint8 i = 0; i < count; i++)
			{
				// Голос с ожидающим ключом занят нотой этого тика, отбирать его
				// нельзя: иначе в аккорде часть нот пропадёт.
				if(!chip.slots[i].keyed && chip.slots[i].hasPlayed && !chip.slots[i].keyOnDelay)
				{
					found = i;
					break;
				}
			}
		}
		if(found == count)
		{
			found = static_cast<uint8>(chip.next);
			chip.next = (chip.next + 1) % count;
		}

		Slot &slot = chip.slots[found];
		if(slot.channel != channel)
		{
			KeyOn(chip, found, false);
			slot = Slot{};
			slot.channel = channel;
		}
		slot.patch = patch;
		WritePatch(chip, found);
	}

	// Furnace commands 12xx to 15xx set the level of a single operator.
	void OperatorLevel(CHANNELINDEX channel, uint8 op, uint8 level)
	{
		Chip *chip = nullptr;
		uint8 index = 0;
		if(Slot *slot = Find(channel, &chip, &index); slot != nullptr && op < 4)
		{
			slot->patch[1 + op * 7 + LevelRegister] = level & MaxLevel;
			WriteLevels(*chip, index);
		}
	}

	bool NoteOff(CHANNELINDEX channel)
	{
		Chip *chip = nullptr;
		uint8 index = 0;
		if(!Find(channel, &chip, &index))
			return false;
		KeyOn(*chip, index, false);
		return true;
	}

	bool NoteCut(CHANNELINDEX channel, bool unassign)
	{
		Chip *chip = nullptr;
		uint8 index = 0;
		Slot *slot = Find(channel, &chip, &index);
		if(!slot)
			return false;
		KeyOn(*chip, index, false);
		// Only the output volume is silenced here. The modulator level is the mixer's
		// "cutoff doubles as modulator intensity" control, which ordinary modules never
		// write, so zeroing it would strip the FM timbre for good after the first cut.
		slot->volume = 0;
		WriteLevels(*chip, index);
		if(unassign)
			slot->channel = CHANNELINDEX_INVALID;
		return true;
	}

	bool MoveChannel(CHANNELINDEX from, CHANNELINDEX to)
	{
		Chip *chip = nullptr;
		uint8 index = 0;
		Slot *slot = Find(from, &chip, &index);
		if(!slot)
			return false;
		if(from != to)
		{
			NoteCut(to, true);
			slot->channel = to;
		}
		return true;
	}

	bool Frequency(CHANNELINDEX channel, uint32 milliHertz, bool keyOff)
	{
		Chip *chip = nullptr;
		uint8 index = 0;
		Slot *slot = Find(channel, &chip, &index);
		if(!slot)
			return false;
		if(milliHertz && chip->isOPM)
		{
			// Key code starts at C sharp; the remaining quarter-octave codes are skipped.
			constexpr uint8 codes[] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
			// OPM key fraction at concert A for the 3.579545 MHz clock.
			const double key = 56.0 + 12.0 * std::log2(milliHertz / 440000.0);
			const int fraction = std::clamp(static_cast<int>(std::lround(key * 64)), 0, 96 * 64 - 1);
			const int note = fraction / 64;
			Write(*chip, ChannelRegister(*chip, index, 0x28), static_cast<uint8>((note / 12) * 16 + codes[note % 12]));
			Write(*chip, ChannelRegister(*chip, index, 0x30), static_cast<uint8>((fraction % 64) * 4));
		} else if(milliHertz)
		{
			// The OPN2 phase accumulator advances F-number * 2^block / 2 at the chip rate.
			const uint64 numerator = static_cast<uint64>(milliHertz) * (uint64{1} << 21) * 144;
			const uint64 denominator = static_cast<uint64>(OPN2Clock) * 1000;
			uint8 block = 0;
			uint64 fnum = (numerator + denominator / 2) / denominator;
			while(fnum > 2047 && block < 7)
			{
				block++;
				const uint64 scaled = denominator << block;
				fnum = (numerator + scaled / 2) / scaled;
			}
			fnum = std::clamp<uint64>(fnum, 1, 2047);
			Write(*chip, ChannelRegister(*chip, index, 0xA4), static_cast<uint8>((block << 3) | (fnum >> 8)));
			Write(*chip, ChannelRegister(*chip, index, 0xA0), static_cast<uint8>(fnum & 0xFF));
		}
		KeyOn(*chip, index, !keyOff && milliHertz != 0);
		return true;
	}

	bool Volume(CHANNELINDEX channel, uint8 volume, bool applyToModulator)
	{
		Chip *chip = nullptr;
		uint8 index = 0;
		Slot *slot = Find(channel, &chip, &index);
		if(!slot)
			return false;
		// The mixer reuses filter cutoff as "modulator intensity", which is well defined
		// for the OPL's single modulator and carrier. These chips chain up to three
		// modulators per algorithm, where attenuating all of them by the same amount is
		// not the same thing, so the request is accepted and ignored, as before.
		// Mapping cutoff onto a four operator patch needs its own work.
		if(applyToModulator)
			return true;
		const uint8 clamped = std::min<uint8>(volume, 63);
		if(slot->volume != clamped)
		{
			slot->volume = clamped;
			WriteLevels(*chip, index);
		}
		return true;
	}

	// These chips only choose between left, right and both, which is what the mixer expects
	// back from OPL::Pan as -1, 0 and 1.
	bool Pan(CHANNELINDEX channel, int32 pan, int8 &result)
	{
		Chip *chip = nullptr;
		uint8 index = 0;
		Slot *slot = Find(channel, &chip, &index);
		if(!slot)
			return false;
		const int8 panning = static_cast<int8>(pan <= 84 ? -1 : pan >= 171 ? 1 : 0);
		if(slot->panning != panning)
		{
			slot->panning = panning;
			WriteAlgorithmAndPanning(*chip, index);
		}
		result = panning;
		return true;
	}

	// Muted channels stop receiving volume updates, so the chip has to be silenced here.
	bool Mute(CHANNELINDEX channel, bool muted)
	{
		Chip *chip = nullptr;
		uint8 index = 0;
		Slot *slot = Find(channel, &chip, &index);
		if(!slot)
			return false;
		if(slot->muted != muted)
		{
			slot->muted = muted;
			WriteLevels(*chip, index);
			WriteAlgorithmAndPanning(*chip, index);
		}
		return true;
	}

	void Mix(int32 *target, size_t count, uint32 volumeFactorQ16, const uint16 *chipVolume)
	{
		for(auto &chip : m_chips)
		{
			if(!chip.Exists() || !chip.rate)
				continue;
			bool assigned = false;
			for(uint8 i = 0; i < chip.NumChannels(); i++)
				assigned |= chip.slots[i].channel != CHANNELINDEX_INVALID;
			if(!assigned)
				continue;

			const double gain = static_cast<double>(MixerReference) * volumeFactorQ16 / 65536.0
				* chipVolume[chip.isOPM ? ChipOPM : ChipOPN2] / ChipVolumeUnity;
			for(size_t i = 0; i < count; i++)
			{
				chip.phase += chip.rate;
				while(chip.phase >= m_rate)
				{
					chip.phase -= m_rate;
					chip.previous = chip.current;
					std::array<double, 2> raw{};
					if(chip.opm)
					{
						ymfm::ym2151::output_data sample;
						chip.opm->generate(&sample);
						raw[0] = sample.data[0];
						raw[1] = sample.data[1];
					} else
					{
						ymfm::ym2612::output_data sample;
						chip.opn->generate(&sample);
						raw[0] = sample.data[0];
						raw[1] = sample.data[1];
					}
					for(size_t ch = 0; ch < 2; ch++)
					{
						// Remove the DAC's idle offset without changing the waveform.
						chip.dc[ch] += chip.dcCoef * (raw[ch] - chip.dc[ch]);
						chip.current[ch] = raw[ch] - chip.dc[ch];
					}
					// Отсчёт со снятым ключом отработан, теперь можно ставить
					// ключ: огибающая начнётся заново.
					if(chip.keyPending)
					{
						bool pending = false;
						for(uint8 slot = 0; slot < chip.NumChannels(); slot++)
						{
							if(!chip.slots[slot].keyOnDelay)
								continue;
							if(--chip.slots[slot].keyOnDelay == 0)
							{
								WriteKey(chip, slot, true);
								chip.slots[slot].keyed = true;
								chip.slots[slot].hasPlayed = true;
							} else
								pending = true;
						}
						chip.keyPending = pending;
					}
				}
				const double fraction = static_cast<double>(chip.phase) / m_rate;
				for(size_t ch = 0; ch < 2; ch++)
				{
					const double sample = chip.previous[ch] + (chip.current[ch] - chip.previous[ch]) * fraction;
					target[2 * i + ch] += static_cast<int32>(sample * gain);
				}
			}
		}
	}
};

OPENMPT_NAMESPACE_END
