/*
 * PSGMacroCtrl.cpp
 * ----------------
 * Purpose: Graphical editor for a single PSG macro sequence
 * Notes  : (currently none)
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#include "stdafx.h"
#include "PSGMacroCtrl.h"
#include "resource.h"

OPENMPT_NAMESPACE_BEGIN

enum
{
	MacroMenuLoop = 1,
	MacroMenuRelease,
	MacroMenuClearLoop,
	MacroMenuClearRelease,
	MacroMenuCut,
	MacroMenuClear,
};

BEGIN_MESSAGE_MAP(PSGMacroCtrl, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_LBUTTONDOWN()
	ON_WM_MOUSEMOVE()
	ON_WM_LBUTTONUP()
	ON_WM_RBUTTONDOWN()
	ON_WM_MOUSEWHEEL()
	ON_WM_SETFOCUS()
	ON_WM_KILLFOCUS()
	ON_WM_GETDLGCODE()
	ON_COMMAND_RANGE(MacroMenuLoop, MacroMenuClear, &PSGMacroCtrl::OnCommandRange)
END_MESSAGE_MAP()


void PSGMacroCtrl::Init(PSGSequence *sequence, int32 low, int32 high, ChangedFunc changed)
{
	m_sequence = sequence;
	m_low = low;
	m_high = high;
	m_changed = std::move(changed);
	Invalidate(FALSE);
}


void PSGMacroCtrl::SetSequence(PSGSequence *sequence)
{
	m_sequence = sequence;
	Invalidate(FALSE);
}


int PSGMacroCtrl::ColumnWidth() const
{
	CRect rect;
	GetClientRect(rect);
	const size_t length = m_sequence ? m_sequence->values.size() : 0;
	// Пустой макрос рисуется сеткой по умолчанию, чтобы было куда щёлкнуть.
	const int columns = static_cast<int>(std::max<size_t>(length, 16));
	return std::clamp(rect.Width() / std::max(columns, 1), 2, 14);
}


int PSGMacroCtrl::ColumnAt(int x) const
{
	return std::max(0, x) / std::max(ColumnWidth(), 1);
}


int32 PSGMacroCtrl::ValueAt(int y) const
{
	CRect rect;
	GetClientRect(rect);
	const int height = std::max(rect.Height() - 2, 1);
	const int32 span = m_high - m_low;
	const int32 value = m_high - static_cast<int32>((static_cast<int64>(std::clamp(y - 1, 0, height - 1)) * (span + 1)) / height);
	return std::clamp(value, m_low, m_high);
}


int PSGMacroCtrl::ValueTop(int32 value, int height) const
{
	const int32 span = std::max<int32>(m_high - m_low, 1);
	const int64 offset = static_cast<int64>(m_high - std::clamp(value, m_low, m_high)) * (height - 2);
	return 1 + static_cast<int>(offset / span);
}


void PSGMacroCtrl::Changed()
{
	Invalidate(FALSE);
	if(m_changed)
		m_changed();
}


void PSGMacroCtrl::PaintColumn(int column, int32 value)
{
	if(!m_sequence || column < 0 || column > 254)
		return;
	auto &values = m_sequence->values;
	if(static_cast<size_t>(column) >= values.size())
	{
		// Рисование правее конца удлиняет макрос: иначе длину пришлось бы
		// задавать отдельным полем, а это ровно та возня, от которой уходим.
		const int32 fill = values.empty() ? value : values.back();
		values.resize(static_cast<size_t>(column) + 1, fill);
	}
	values[column] = std::clamp(value, m_low, m_high);
}


void PSGMacroCtrl::OnPaint()
{
	CPaintDC dc(this);
	CRect rect;
	GetClientRect(rect);

	const COLORREF back = GetSysColor(COLOR_WINDOW);
	const COLORREF frame = GetSysColor(COLOR_BTNSHADOW);
	const COLORREF bar = GetSysColor(COLOR_HIGHLIGHT);
	const COLORREF grid = GetSysColor(COLOR_BTNFACE);
	const COLORREF zero = GetSysColor(COLOR_GRAYTEXT);

	CBitmap bitmap;
	CDC memDC;
	memDC.CreateCompatibleDC(&dc);
	bitmap.CreateCompatibleBitmap(&dc, rect.Width(), rect.Height());
	auto *oldBitmap = memDC.SelectObject(&bitmap);

	memDC.FillSolidRect(rect, back);
	const int width = ColumnWidth();
	const size_t length = m_sequence ? m_sequence->values.size() : 0;
	const int columns = rect.Width() / std::max(width, 1);

	for(int column = 0; column <= columns; column++)
	{
		const int x = column * width;
		memDC.FillSolidRect(x, 0, 1, rect.Height(), grid);
	}
	// Нулевая линия нужна там, где значения ходят в обе стороны.
	if(m_low < 0)
		memDC.FillSolidRect(0, ValueTop(0, rect.Height()), rect.Width(), 1, zero);

	for(size_t i = 0; i < length && static_cast<int>(i) <= columns; i++)
	{
		const int32 value = m_sequence->values[i];
		const int x = static_cast<int>(i) * width;
		const int top = ValueTop(value, rect.Height());
		const int base = (m_low < 0) ? ValueTop(0, rect.Height()) : rect.Height() - 1;
		const int from = std::min(top, base), to = std::max(top, base);
		memDC.FillSolidRect(x + 1, from, std::max(width - 1, 1), std::max(to - from, 1), bar);
	}

	if(m_sequence)
	{
		const auto marker = [&](uint8 at, COLORREF colour)
		{
			if(at >= length)
				return;
			memDC.FillSolidRect(static_cast<int>(at) * width, 0, 1, rect.Height(), colour);
		};
		marker(m_sequence->loop, RGB(0, 160, 0));
		marker(m_sequence->release, RGB(200, 0, 0));
	}

	memDC.Draw3dRect(rect, frame, frame);
	if(GetFocus() == this)
	{
		CRect focusRect = rect;
		focusRect.DeflateRect(1, 1);
		memDC.DrawFocusRect(focusRect);
	}

	dc.BitBlt(0, 0, rect.Width(), rect.Height(), &memDC, 0, 0, SRCCOPY);
	memDC.SelectObject(oldBitmap);
}


void PSGMacroCtrl::OnLButtonDown(UINT, CPoint point)
{
	if(!m_sequence)
		return;
	SetFocus();
	SetCapture();
	m_dragging = true;
	m_lastColumn = ColumnAt(point.x);
	PaintColumn(m_lastColumn, ValueAt(point.y));
	Changed();
}


void PSGMacroCtrl::OnMouseMove(UINT, CPoint point)
{
	if(!m_dragging || !m_sequence)
		return;
	const int column = ColumnAt(point.x);
	const int32 value = ValueAt(point.y);
	// Между двумя событиями мыши может остаться пропуск: заполняем его, иначе
	// быстрый штрих оставляет дыры.
	const int from = (m_lastColumn < 0) ? column : m_lastColumn;
	const int step = (column >= from) ? 1 : -1;
	for(int c = from; c != column + step; c += step)
		PaintColumn(c, value);
	m_lastColumn = column;
	Changed();
}


void PSGMacroCtrl::OnLButtonUp(UINT, CPoint)
{
	if(m_dragging)
	{
		ReleaseCapture();
		m_dragging = false;
		m_lastColumn = -1;
	}
}


void PSGMacroCtrl::OnRButtonDown(UINT, CPoint point)
{
	if(!m_sequence)
		return;
	SetFocus();
	m_lastColumn = ColumnAt(point.x);
	CMenu menu;
	menu.CreatePopupMenu();
	const bool inside = static_cast<size_t>(m_lastColumn) < m_sequence->values.size();
	menu.AppendMenu(MF_STRING | (inside ? 0 : MF_GRAYED), MacroMenuLoop, _T("Loop from here"));
	menu.AppendMenu(MF_STRING | (inside ? 0 : MF_GRAYED), MacroMenuRelease, _T("Release here"));
	menu.AppendMenu(MF_STRING | (m_sequence->loop < m_sequence->values.size() ? 0 : MF_GRAYED), MacroMenuClearLoop, _T("No loop"));
	menu.AppendMenu(MF_STRING | (m_sequence->release < m_sequence->values.size() ? 0 : MF_GRAYED), MacroMenuClearRelease, _T("No release"));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING | (inside ? 0 : MF_GRAYED), MacroMenuCut, _T("End macro here"));
	menu.AppendMenu(MF_STRING, MacroMenuClear, _T("Clear macro"));
	CPoint screen = point;
	ClientToScreen(&screen);
	menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, screen.x, screen.y, this);
}


void PSGMacroCtrl::OnCommandRange(UINT id)
{
	if(!m_sequence)
		return;
	auto &seq = *m_sequence;
	switch(id)
	{
	case MacroMenuLoop: seq.loop = static_cast<uint8>(m_lastColumn); break;
	case MacroMenuRelease: seq.release = static_cast<uint8>(m_lastColumn); break;
	case MacroMenuClearLoop: seq.loop = 255; break;
	case MacroMenuClearRelease: seq.release = 255; break;
	case MacroMenuCut:
		seq.values.resize(static_cast<size_t>(m_lastColumn) + 1);
		if(seq.loop >= seq.values.size()) seq.loop = 255;
		if(seq.release >= seq.values.size()) seq.release = 255;
		break;
	case MacroMenuClear:
		seq.values.clear();
		seq.loop = seq.release = 255;
		break;
	default: return;
	}
	Changed();
}


bool PSGMacroCtrl::WheelStep(short delta, CPoint screenPoint)
{
	if(!m_sequence || !delta)
		return false;
	CPoint client = screenPoint;
	ScreenToClient(&client);
	const int column = ColumnAt(client.x);
	if(static_cast<size_t>(column) >= m_sequence->values.size())
		return false;
	PaintColumn(column, m_sequence->values[column] + (delta > 0 ? 1 : -1));
	Changed();
	return true;
}


BOOL PSGMacroCtrl::OnMouseWheel(UINT, short delta, CPoint point)
{
	WheelStep(delta, point);
	// Всегда «обработано»: иначе колесо уйдёт родителю, тот вернёт его сюда, и
	// выйдет бесконечная пересылка.
	return TRUE;
}


void PSGMacroCtrl::OnSetFocus(CWnd *old)
{
	CWnd::OnSetFocus(old);
	Invalidate(FALSE);
}


void PSGMacroCtrl::OnKillFocus(CWnd *next)
{
	CWnd::OnKillFocus(next);
	Invalidate(FALSE);
}


UINT PSGMacroCtrl::OnGetDlgCode()
{
	return DLGC_WANTARROWS;
}

OPENMPT_NAMESPACE_END
