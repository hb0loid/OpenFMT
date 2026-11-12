/*
 * PSGMacroCtrl.h
 * --------------
 * Purpose: Graphical editor for a single PSG macro sequence
 * Notes  : The macro is drawn as a row of bars and painted with the mouse. A
 *          row of numbers in a text field is unreadable past a handful of
 *          steps, and macros are routinely dozens of steps long.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#pragma once

#include "openmpt/all/BuildSettings.hpp"
#include "../soundlib/Snd_defs.h"
#include <functional>

OPENMPT_NAMESPACE_BEGIN

class PSGMacroCtrl : public CWnd
{
public:
	using ChangedFunc = std::function<void()>;

	// Привязывает элемент к последовательности. Сама последовательность живёт в
	// рабочей копии патча у диалога, здесь только указатель на неё.
	void Init(PSGSequence *sequence, int32 low, int32 high, ChangedFunc changed);
	void SetSequence(PSGSequence *sequence);
	// Шаг колеса по значению под курсором. Диалог зовёт это напрямую: пересылать
	// WM_MOUSEWHEEL нельзя, необработанное колесо Windows вернёт родителю, и
	// получится петля.
	bool WheelStep(short delta, CPoint screenPoint);

protected:
	PSGSequence *m_sequence = nullptr;
	ChangedFunc m_changed;
	int32 m_low = 0, m_high = 15;
	int m_lastColumn = -1;
	// Шаг под курсором: без подписи значения попасть в нужный уровень всё равно
	// трудно, особенно у громкости с её шестнадцатью ступенями.
	int m_hoverColumn = -1;
	int32 m_hoverValue = 0;
	bool m_dragging = false;
	bool m_tracking = false;

	int ColumnWidth() const;
	int ColumnAt(int x) const;
	int32 ValueAt(int y) const;
	int ValueTop(int32 value, int height) const;
	void PaintColumn(int column, int32 value);
	void Changed();

	afx_msg void OnPaint();
	afx_msg void OnLButtonDown(UINT flags, CPoint point);
	afx_msg void OnMouseMove(UINT flags, CPoint point);
	afx_msg void OnLButtonUp(UINT flags, CPoint point);
	afx_msg void OnRButtonDown(UINT flags, CPoint point);
	afx_msg BOOL OnMouseWheel(UINT flags, short delta, CPoint point);
	afx_msg void OnSetFocus(CWnd *old);
	afx_msg void OnKillFocus(CWnd *next);
	afx_msg UINT OnGetDlgCode();
	afx_msg BOOL OnEraseBkgnd(CDC *dc) { return TRUE; }
	afx_msg void OnCommandRange(UINT id);
	afx_msg void OnMouseLeave();
	DECLARE_MESSAGE_MAP()
};

OPENMPT_NAMESPACE_END
