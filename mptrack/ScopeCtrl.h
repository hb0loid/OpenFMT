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
	// Сколько отсчётов видно на экране разом: около двадцати миллисекунд при
	// 48 кГц. Короче — волна дрожит, длиннее — отдельные периоды сливаются.
	static constexpr uint32 Span = 1024;
	// Запас, в котором ищется точка синхронизации.
	static constexpr uint32 Search = Span;

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

	// Ищет начало картинки так, чтобы одиночная нота стояла неподвижно: без
	// этого волна бежит по экрану и разглядеть в ней нечего.
	static uint32 Trigger(const float *samples, uint32 count);

	afx_msg void OnPaint();
	afx_msg void OnTimer(UINT_PTR id);
	afx_msg BOOL OnEraseBkgnd(CDC *) { return TRUE; }
	DECLARE_MESSAGE_MAP()
};

OPENMPT_NAMESPACE_END
