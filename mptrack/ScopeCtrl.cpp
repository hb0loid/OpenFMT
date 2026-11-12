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
#include "HighDPISupport.h"
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
		// Шестьдесят кадров в секунду: на волне, которая бежит и меняется от
		// ручки, двадцать пять уже видно как рывки.
		SetTimer(ScopeTimer, 16, nullptr);
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
	const int width = rect.Width(), height = rect.Height();

	// Холст заводится один раз и переживает кадры: при шестидесяти кадрах в
	// секунду создавать его заново значит занимать процессор впустую.
	if(!m_offScreenDC.m_hDC)
		m_offScreenDC.CreateCompatibleDC(&dc);
	if(m_offScreenSize != rect.Size())
	{
		m_offScreenBitmap.DeleteObject();
		m_offScreenBitmap.CreateCompatibleBitmap(&dc, width, height);
		m_offScreenSize = rect.Size();
		m_offScreenDC.SelectObject(&m_offScreenBitmap);
	}
	m_offScreenDC.FillSolidRect(&rect, back);

	// На столбец пикселей должно приходиться не меньше двух отсчётов: иначе
	// соседние столбцы берут один и тот же, и линия рассыпается в точки.
	const uint32 span = std::clamp<uint32>(static_cast<uint32>(width) * 2, MinSpan, MaxSpan);
	const uint32 search = span / 2;
	m_samples.resize(span + search);
	const bool got = m_buffer->ReadLatest(m_samples.data(), static_cast<uint32>(m_samples.size()));

	const int middle = height / 2;
	const int thickness = std::max(1, HighDPISupport::ScalePixels(2, m_hWnd));
	CPen zeroPen(PS_SOLID, 1, RGB(GetRValue(wave) / 3, GetGValue(wave) / 3, GetBValue(wave) / 3));
	auto oldPen = m_offScreenDC.SelectObject(&zeroPen);
	m_offScreenDC.MoveTo(0, middle);
	m_offScreenDC.LineTo(width, middle);

	if(got)
	{
		const uint32 start = Trigger(m_samples.data(), search);
		const float *shown = m_samples.data() + start;

		float peak = 0.0f;
		for(uint32 i = 0; i < span; i++)
			peak = std::max(peak, std::abs(shown[i]));
		m_peak = peak;

		// Громкое ужимаем сразу, тихое растягиваем не спеша: иначе картинка
		// дёргается по высоте на каждой ноте и читать её невозможно.
		const float target = std::clamp(peak > 0.0001f ? 0.85f / peak : 1.0f, 1.0f, 64.0f);
		m_gain = (target < m_gain) ? target : m_gain + (target - m_gain) * 0.1f;

		const auto place = [&](float value)
		{
			return middle - static_cast<int>(std::clamp(value * m_gain, -1.0f, 1.0f) * (height - thickness * 2) / 2);
		};

		CPen wavePen(PS_SOLID, thickness, wave);
		m_offScreenDC.SelectObject(&wavePen);
		int previous = place(shown[0]);
		for(int x = 0; x < width; x++)
		{
			const uint32 from = static_cast<uint32>(static_cast<int64>(x) * span / width);
			const uint32 to = std::clamp<uint32>(static_cast<uint32>(static_cast<int64>(x + 1) * span / width), from + 1, span);
			float low = shown[from], high = shown[from];
			for(uint32 i = from; i < to; i++)
			{
				low = std::min(low, shown[i]);
				high = std::max(high, shown[i]);
			}
			// Сначала соединяем со столбцом слева, иначе на широком окне, где
			// отсчётов меньше, чем столбцов, линия распадается на точки.
			const int top = place(high), bottom = place(low);
			m_offScreenDC.MoveTo(x, previous);
			m_offScreenDC.LineTo(x, std::clamp(previous, top, bottom));
			m_offScreenDC.MoveTo(x, top);
			m_offScreenDC.LineTo(x, bottom + 1);
			previous = place(shown[to - 1]);
		}

		// Уровень подписывается настоящий, до усиления: иначе цифра врала бы.
		const CString text = (peak > 0.0f)
			? mpt::cfmt::fix(20.0 * std::log10(static_cast<double>(peak)), 1) + _T(" dB")
			: CString(_T("-inf dB"));
		m_offScreenDC.SetBkMode(TRANSPARENT);
		m_offScreenDC.SetTextColor(wave);
		m_offScreenDC.SelectObject(CMainFrame::GetGUIFont());
		CRect textRect = rect;
		textRect.DeflateRect(4, 2);
		m_offScreenDC.DrawText(text, &textRect, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
	}

	m_offScreenDC.SelectObject(oldPen);
	dc.BitBlt(0, 0, width, height, &m_offScreenDC, 0, 0, SRCCOPY);
}


OPENMPT_NAMESPACE_END
