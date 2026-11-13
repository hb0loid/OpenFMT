/*
 * ScopeBuffer.h
 * -------------
 * Purpose: Lock-free ring buffer feeding the oscilloscope from the audio thread
 * Notes  : Drawing cannot happen in the audio thread and the audio thread must
 *          never wait, so the mixed output is copied here and picked up by the
 *          UI on a timer.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#pragma once

#include "openmpt/all/BuildSettings.hpp"
#include "../soundlib/Sndfile.h"
#include <array>
#include <atomic>

OPENMPT_NAMESPACE_BEGIN

// Звук для осциллографа: пишет аудиопоток, читает рисование.
class ScopeBuffer
{
public:
	// Степень двойки: тогда обход края это одна маска, без условий и деления.
	// Размера хватает на секунду звука при сорока восьми килогерцах: окно
	// осциллографа доходит до полусекунды, и перед ним ещё нужен запас на
	// поиск начала картинки.
	static constexpr uint32 Size = 65536;
	static_assert((Size & (Size - 1)) == 0);

private:
	std::array<float, Size> m_samples = {};
	// Сколько отсчётов записано за всё время. Пишет только аудиопоток, читает
	// только рисование, поэтому замок не нужен: хватает публикации счётчика.
	std::atomic<uint32> m_written = 0;

public:
	// Сколько отсчётов записано за всё время: по нему видно, как часто звуковая
	// карта приносит новый звук, и обновляется ли вообще картинка.
	uint32 Written() const { return m_written.load(std::memory_order_acquire); }

	void Clear()
	{
		m_samples.fill(0.0f);
		m_written.store(0, std::memory_order_release);
	}

	void Write(const float *mono, uint32 count)
	{
		const uint32 at = m_written.load(std::memory_order_relaxed);
		for(uint32 i = 0; i < count; i++)
			m_samples[(at + i) & (Size - 1)] = mono[i];
		m_written.store(at + count, std::memory_order_release);
	}

	// Копирует последние count отсчётов. Отдаёт false, если звука ещё не
	// набралось или аудиопоток успел переписать начало куска прямо во время
	// копирования: такую картинку склеило бы из двух разных мест.
	bool ReadLatest(float *out, uint32 count) const
	{
		if(count > Size)
			return false;
		const uint32 at = m_written.load(std::memory_order_acquire);
		if(at < count)
			return false;
		const uint32 start = at - count;
		for(uint32 i = 0; i < count; i++)
			out[i] = m_samples[(start + i) & (Size - 1)];
		return m_written.load(std::memory_order_acquire) - start <= Size;
	}
};


// Звук по дороге к индикатору уровня заодно складывается в буфер осциллографа:
// точка съёма у них одна и та же, а CSoundFile::Read принимает один наблюдатель.
class ScopeMonitor final : public IMonitorOutput
{
	IMonitorOutput &m_next;
	ScopeBuffer &m_scope;

	template <typename Span>
	void Feed(Span buffer, float scale)
	{
		const auto channels = buffer.size_channels();
		if(!channels)
			return;
		scale /= static_cast<float>(channels);
		std::array<float, 512> block;
		uint32 at = 0;
		for(std::size_t frame = 0; frame < buffer.size_frames(); frame++)
		{
			float sum = 0.0f;
			for(std::size_t channel = 0; channel < channels; channel++)
				sum += static_cast<float>(buffer(channel, frame));
			block[at++] = sum * scale;
			if(at == block.size())
			{
				m_scope.Write(block.data(), at);
				at = 0;
			}
		}
		if(at)
			m_scope.Write(block.data(), at);
	}

public:
	ScopeMonitor(IMonitorOutput &next, ScopeBuffer &scope)
		: m_next(next), m_scope(scope) { }

	void Process(mpt::audio_span_interleaved<const MixSampleInt> buffer) override
	{
		m_next.Process(buffer);
		Feed(buffer, 1.0f / static_cast<float>(MixSampleIntTraits::mix_clip_max));
	}

	void Process(mpt::audio_span_interleaved<const MixSampleFloat> buffer) override
	{
		m_next.Process(buffer);
		Feed(buffer, 1.0f);
	}
};

OPENMPT_NAMESPACE_END
