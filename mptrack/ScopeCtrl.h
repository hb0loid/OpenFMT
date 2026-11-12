/*
 * ScopeCtrl.h
 * -----------
 * Purpose: Oscilloscope showing the shape of the sound that is playing
 * Notes  : Sits in the unused right-hand part of the Patterns and Samples
 *          control bars, so the waveform is visible while the chip editors are
 *          being tweaked.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#pragma once

#include "openmpt/all/BuildSettings.hpp"
#include "ScopeBuffer.h"
#include "../misc/mptClock.h"
#include <memory>
#include <atomic>
#include <thread>

OPENMPT_NAMESPACE_BEGIN

class ScopeCtrl : public CWnd
{
public:
	// Сколько отсчётов видно разом: не меньше двух на столбец пикселей, иначе
	// соседние столбцы берут один и тот же отсчёт и линия рассыпается в точки.
	static constexpr uint32 MinSpan = 1024, MaxSpan = 8192;

	bool Create(CWnd *parent, UINT id);
	void Start();
	void Stop();
	// Панель вкладки говорит, где осциллографу стоять и есть ли для него место;
	// показаться он решает сам, с оглядкой на переключатель в меню.
	void Place(const CRect &rect, bool fits);
	// Переключатель в меню один, а панелей несколько, поэтому каждая живая
	// копия перекладывается сама: лезть за раскладкой в чужой диалог нельзя,
	// она там не для посторонних.
	static void RefreshAll();
	~ScopeCtrl();

protected:
	const ScopeBuffer *m_buffer = nullptr;
	// Усиление подбирается само: одна нота чипа тише полной шкалы в десятки раз
	// и без этого была бы неразличимой чертой посреди экрана.
	float m_gain = 1.0f;
	float m_peak = 0.0f;
	// Пока осциллограф рисует, системному таймеру задаётся миллисекундный шаг:
	// по умолчанию он срабатывает примерно раз в 15 мс, и чаще 60 кадров в
	// секунду не выйдет при любом заданном периоде. Отпускается на остановке,
	// потому что мелкий шаг таймера мешает процессору спать.
	std::unique_ptr<Util::MultimediaClock> m_clock;
	// Таймеры окон не умеют чаще сотни раз в секунду, и на деле упираются в
	// шаг системного таймера — выходит шестьдесят четыре кадра, сколько ни
	// проси. Поэтому такты шлёт отдельный поток: он только спит и отправляет
	// сообщение, рисование по-прежнему происходит в потоке окна.
	std::thread m_ticker;
	std::atomic<bool> m_ticking = false;
	// Поток может обогнать рисование, и тогда сообщения копились бы в очереди.
	// Пока кадр не нарисован, следующий такт не отправляется.
	std::atomic<bool> m_pending = false;
	int m_interval = 8;
	CRect m_place;
	bool m_fits = false;
	void Apply();
	// Холст и буфер отсчётов живут между кадрами: при шестидесяти кадрах в
	// секунду заводить их заново значит занимать процессор впустую.
	CDC m_offScreenDC;
	CBitmap m_offScreenBitmap;
	CSize m_offScreenSize;
	std::vector<float> m_samples;
	std::vector<CPoint> m_points;

	// Ищет начало картинки так, чтобы волна стояла на месте, а не ездила.
	static uint32 Trigger(const float *samples, uint32 count, float peak);

	afx_msg LRESULT OnFrame(WPARAM, LPARAM);
	afx_msg void OnDestroy();
	afx_msg void OnPaint();
	afx_msg void OnTimer(UINT_PTR id);
	afx_msg BOOL OnEraseBkgnd(CDC *) { return TRUE; }
	DECLARE_MESSAGE_MAP()
};

OPENMPT_NAMESPACE_END
