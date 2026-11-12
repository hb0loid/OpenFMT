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
	bool m_running = false;
	// Пока осциллограф рисует, системному таймеру задаётся миллисекундный шаг:
	// по умолчанию он срабатывает примерно раз в 15 мс, и чаще 60 кадров в
	// секунду не выйдет при любом заданном периоде. Отпускается на остановке,
	// потому что мелкий шаг таймера мешает процессору спать.
	std::unique_ptr<Util::MultimediaClock> m_clock;
	CRect m_place;
	bool m_fits = false;
	void Apply();
	// Счётчики для подписи: сколько кадров нарисовано и сколько раз за это
	// время пришёл новый звук. Нужны, чтобы не гадать, во что упирается
	// плавность: в рисование или в размер звукового буфера.
	uint32 m_frames = 0, m_arrivals = 0, m_lastWritten = 0;
	uint32 m_countedAt = 0;
	int m_shownFrames = 0, m_shownArrivals = 0;
	// Холст и буфер отсчётов живут между кадрами: при шестидесяти кадрах в
	// секунду заводить их заново значит занимать процессор впустую.
	CDC m_offScreenDC;
	CBitmap m_offScreenBitmap;
	CSize m_offScreenSize;
	std::vector<float> m_samples;

	// Ищет начало картинки так, чтобы волна стояла на месте, а не ездила.
	static uint32 Trigger(const float *samples, uint32 count, float peak);

	afx_msg void OnPaint();
	afx_msg void OnTimer(UINT_PTR id);
	afx_msg BOOL OnEraseBkgnd(CDC *) { return TRUE; }
	DECLARE_MESSAGE_MAP()
};

OPENMPT_NAMESPACE_END
