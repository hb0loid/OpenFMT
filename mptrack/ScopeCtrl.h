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

protected:
	const ScopeBuffer *m_buffer = nullptr;
	// Усиление подбирается само: одна нота чипа тише полной шкалы в десятки раз
	// и без этого была бы неразличимой чертой посреди экрана.
	float m_gain = 1.0f;
	float m_peak = 0.0f;
	bool m_running = false;
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
