/*
 * ScopeCtrl.cpp
 * -------------
 * Purpose: Oscilloscope showing the shape of the sound that is playing
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#include "stdafx.h"
#include "ScopeCtrl.h"
#include "Mainfrm.h"
#include "TrackerSettings.h"
#include "../common/mptStringBuffer.h"
#include <cmath>

OPENMPT_NAMESPACE_BEGIN

static constexpr UINT_PTR ScopeTimer = 1;

BEGIN_MESSAGE_MAP(ScopeCtrl, CWnd)
	ON_WM_PAINT()
	ON_WM_TIMER()
	ON_WM_ERASEBKGND()
END_MESSAGE_MAP()


bool ScopeCtrl::Create(CWnd *parent, UINT id)
{
	m_buffer = &CMainFrame::GetMainFrame()->GetScopeBuffer();
	const auto wndClass = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW, nullptr, nullptr, nullptr);
	return CWnd::CreateEx(0, wndClass, nullptr, WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), parent, id) != FALSE;
}


void ScopeCtrl::Start()
{
	if(!m_running && m_hWnd)
	{
		// Двадцать пять кадров в секунду: глазу этого хватает, а считать
		// картинку чаще значит занимать процессор ради незаметного.
		SetTimer(ScopeTimer, 40, nullptr);
		m_running = true;
	}
}


void ScopeCtrl::Stop()
{
	if(m_running && m_hWnd)
	{
		KillTimer(ScopeTimer);
		m_running = false;
	}
}


void ScopeCtrl::OnTimer(UINT_PTR id)
{
	if(id == ScopeTimer && IsWindowVisible())
		Invalidate(FALSE);
}


uint32 ScopeCtrl::Trigger(const float *samples, uint32 count)
{
	// Переход через ноль снизу вверх: у любой периодической волны он случается
	// раз за период, поэтому картинка от кадра к кадру встаёт на одно место.
	// Берём самый поздний из найденных, чтобы показывать как можно более свежее.
	uint32 found = 0;
	for(uint32 i = 1; i < count; i++)
	{
		if(samples[i - 1] <= 0.0f && samples[i] > 0.0f)
			found = i;
	}
	return found;
}


void ScopeCtrl::OnPaint()
{
	CPaintDC dc(this);
	CRect rect;
	GetClientRect(&rect);
	if(rect.IsRectEmpty() || !m_buffer)
		return;

	const auto &colors = TrackerSettings::Instance().rgbCustomColors;
	const COLORREF back = colors[MODCOLOR_BACKSAMPLE];
	const COLORREF wave = colors[MODCOLOR_SAMPLE];

	CDC memDC;
	CBitmap bitmap;
	memDC.CreateCompatibleDC(&dc);
	bitmap.CreateCompatibleBitmap(&dc, rect.Width(), rect.Height());
	auto oldBitmap = memDC.SelectObject(&bitmap);
	memDC.FillSolidRect(&rect, back);

	std::vector<float> samples(Span + Search);
	const bool got = m_buffer->ReadLatest(samples.data(), static_cast<uint32>(samples.size()));

	// Средняя линия рисуется всегда: по ней видно, что окно живое, а звука нет.
	const int middle = rect.Height() / 2;
	CPen zeroPen(PS_SOLID, 1, RGB(GetRValue(wave) / 3, GetGValue(wave) / 3, GetBValue(wave) / 3));
	auto oldPen = memDC.SelectObject(&zeroPen);
	memDC.MoveTo(0, middle);
	memDC.LineTo(rect.Width(), middle);

	if(got)
	{
		const uint32 start = Trigger(samples.data(), Search);
		const float *shown = samples.data() + start;

		float peak = 0.0f;
		for(uint32 i = 0; i < Span; i++)
			peak = std::max(peak, std::abs(shown[i]));
		m_peak = peak;

		// Громкое ужимаем сразу, тихое растягиваем не спеша: иначе картинка
		// дёргается по высоте на каждой ноте и читать её невозможно.
		const float target = std::clamp(peak > 0.0001f ? 0.85f / peak : 1.0f, 1.0f, 64.0f);
		m_gain = (target < m_gain) ? target : m_gain + (target - m_gain) * 0.1f;

		CPen wavePen(PS_SOLID, 1, wave);
		memDC.SelectObject(&wavePen);
		const int height = rect.Height() - 2;
		const int width = rect.Width();
		for(int x = 0; x < width; x++)
		{
			const uint32 from = static_cast<uint32>(static_cast<int64>(x) * Span / width);
			const uint32 to = std::max(from + 1, static_cast<uint32>(static_cast<int64>(x + 1) * Span / width));
			float low = shown[from], high = shown[from];
			for(uint32 i = from; i < to && i < Span; i++)
			{
				low = std::min(low, shown[i]);
				high = std::max(high, shown[i]);
			}
			const auto place = [&](float value)
			{
				return middle - static_cast<int>(std::clamp(value * m_gain, -1.0f, 1.0f) * height / 2);
			};
			memDC.MoveTo(x, place(high));
			memDC.LineTo(x, place(low) + 1);
		}

		// Уровень подписывается настоящий, до усиления: иначе цифра врала бы.
		const CString text = (peak > 0.0f)
			? mpt::cfmt::fix(20.0 * std::log10(static_cast<double>(peak)), 1) + _T(" dB")
			: CString(_T("-inf dB"));
		memDC.SetBkMode(TRANSPARENT);
		memDC.SetTextColor(wave);
		memDC.SelectObject(CMainFrame::GetGUIFont());
		CRect textRect = rect;
		textRect.DeflateRect(4, 2);
		memDC.DrawText(text, &textRect, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
	}

	memDC.SelectObject(oldPen);
	dc.BitBlt(0, 0, rect.Width(), rect.Height(), &memDC, 0, 0, SRCCOPY);
	memDC.SelectObject(oldBitmap);
}

OPENMPT_NAMESPACE_END
