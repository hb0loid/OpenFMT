/*
 * ChannelScopes.h
 * ---------------
 * Назначение: звук каждого канала отдельно, для осциллографов по каналам.
 *
 * Общий осциллограф показывает сумму, в которой голоса не разобрать. Здесь у
 * каждого канала своё кольцо: микшер складывает в него свой вклад, рисование
 * забирает последние отсчёты. Замка нет и быть не должно — аудиопоток ждать
 * не может, — поэтому счётчик публикуется один раз на блок, а не на отсчёт.
 *
 * Пока приёмник не включён, вся работа сводится к проверке указателя: платить
 * за то, чего не видно на экране, незачем.
 */

#pragma once

#include "openmpt/all/BuildSettings.hpp"
#include "Snd_defs.h"
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
	std::atomic<bool> m_enabled{false};

public:
	void SetEnabled(bool enabled) { m_enabled.store(enabled, std::memory_order_release); }
	bool IsEnabled() const { return m_enabled.load(std::memory_order_acquire); }

	// Добавить отсчёт канала. Вызывается из микшера на каждый отсчёт, поэтому
	// здесь нет ни атомарных операций, ни ветвлений сверх необходимого.
	inline void Push(CHANNELINDEX channel, float value)
	{
		if(channel >= Channels)
			return;
		Lane &lane = m_lanes[channel];
		lane.samples[lane.head & (Size - 1)] = value;
		lane.head++;
	}

	// Конец блока: отдать рисованию то, что набралось.
	void Publish()
	{
		for(auto &lane : m_lanes)
			lane.written.store(lane.head, std::memory_order_release);
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
