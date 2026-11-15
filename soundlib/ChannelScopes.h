/*
 * ChannelScopes.h
 * ---------------
 * Назначение: звук каждого канала отдельно, для осциллографов по каналам.
 *
 * Общий осциллограф показывает сумму, в которой голоса не разобрать. Здесь у
 * каждого канала своё кольцо. За блок микширования источники — чипы и
 * сэмплы — прибавляют свой вклад в накопитель, в конце блока он уходит в
 * кольцо, рисование забирает последние отсчёты. Замка нет и быть не должно — аудиопоток ждать
 * не может, — поэтому счётчик публикуется один раз на блок, а не на отсчёт.
 *
 * Пока приёмник не включён, вся работа сводится к проверке указателя: платить
 * за то, чего не видно на экране, незачем.
 */

#pragma once

#include "openmpt/all/BuildSettings.hpp"
#include "Snd_defs.h"
#include <algorithm>
#include <array>
#include <atomic>

OPENMPT_NAMESPACE_BEGIN

class ChannelScopes
{
public:
	// Столько каналов показываем: модули с микросхемами укладываются с запасом,
	// а память — это Channels * Size * 4 байта.
	static constexpr uint32 Channels = 32;
	// Степень двойки: обход края это маска, без деления.
	static constexpr uint32 Size = 4096;
	static_assert((Size & (Size - 1)) == 0);

private:
	struct Lane
	{
		std::array<float, Size> samples{};
		uint32 head = 0;                 // Пишет только аудиопоток
		std::atomic<uint32> written{0};  // Публикуется раз в блок
	};
	std::array<Lane, Channels> m_lanes;
	// Смотрящих бывает несколько — сетка на вкладке Chips и шапки каналов в
	// редакторе, — поэтому не флаг, а счётчик: иначе закрытие одного окна
	// гасило бы картинку в другом.
	std::atomic<int> m_users{0};
	// Накопитель текущего блока; трогает только аудиопоток.
	std::array<std::array<float, 512>, Channels> m_pending{};
	std::array<bool, Channels> m_touched{};
	uint32 m_blockLength = 0;

public:
	void AddUser() { m_users.fetch_add(1, std::memory_order_acq_rel); }
	void RemoveUser() { if(m_users.load(std::memory_order_acquire) > 0) m_users.fetch_sub(1, std::memory_order_acq_rel); }
	bool IsEnabled() const { return m_users.load(std::memory_order_acquire) > 0; }

	// Блок микширования: сюда каждый источник прибавляет свой вклад в канал —
	// и чип, и сэмплы, и несколько голосов одного канала (фоновые ноты NNA).
	// Поэтому не «дописать отсчёт», а «прибавить к i-му отсчёту блока».
	static constexpr uint32 MaxBlock = 512;  // MIXBUFFERSIZE

	void BeginBlock(uint32 count)
	{
		m_blockLength = std::min(count, MaxBlock);
		m_touched.fill(false);
	}

	// Канал звучит в этом блоке, даже если вклад нулевой: картинка должна
	// показать тишину, а не застыть на прошлом.
	inline void Touch(CHANNELINDEX channel)
	{
		if(channel >= Channels || m_touched[channel])
			return;
		m_touched[channel] = true;
		std::fill(m_pending[channel].begin(), m_pending[channel].begin() + m_blockLength, 0.0f);
	}

	inline void Add(CHANNELINDEX channel, size_t index, float value)
	{
		if(channel >= Channels || index >= m_blockLength)
			return;
		Touch(channel);
		m_pending[channel][index] += value;
	}

	// Конец блока: отдать рисованию то, что набралось.
	void Publish()
	{
		for(uint32 channel = 0; channel < Channels; channel++)
		{
			Lane &lane = m_lanes[channel];
			// Канал, который в этом блоке ничего не дал, звучит тишиной: пишем
			// нули. Раньше его кольцо просто стояло, и осциллограф держал
			// последнюю волну, хотя канал давно замолчал.
			const bool touched = m_touched[channel];
			for(uint32 i = 0; i < m_blockLength; i++)
				lane.samples[(lane.head + i) & (Size - 1)] = touched ? m_pending[channel][i] : 0.0f;
			lane.head += m_blockLength;
			lane.written.store(lane.head, std::memory_order_release);
		}
		m_touched.fill(false);
	}

	void Clear()
	{
		for(auto &lane : m_lanes)
		{
			lane.samples.fill(0.0f);
			lane.head = 0;
			lane.written.store(0, std::memory_order_release);
		}
	}

	uint32 Written(CHANNELINDEX channel) const
	{
		return (channel < Channels) ? m_lanes[channel].written.load(std::memory_order_acquire) : 0;
	}

	// Последние count отсчётов канала. false, если их ещё не набралось или
	// аудиопоток успел переписать начало куска прямо во время копирования.
	bool ReadLatest(CHANNELINDEX channel, float *out, uint32 count) const
	{
		if(channel >= Channels || count > Size)
			return false;
		const Lane &lane = m_lanes[channel];
		const uint32 end = lane.written.load(std::memory_order_acquire);
		if(end < count)
			return false;
		const uint32 start = end - count;
		for(uint32 i = 0; i < count; i++)
			out[i] = lane.samples[(start + i) & (Size - 1)];
		return lane.written.load(std::memory_order_acquire) - start <= Size;
	}
};

OPENMPT_NAMESPACE_END
