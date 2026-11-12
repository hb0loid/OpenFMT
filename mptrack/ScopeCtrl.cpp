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

static std::vector<ScopeCtrl *> s_scopes;

BEGIN_MESSAGE_MAP(ScopeCtrl, CWnd)
	ON_WM_PAINT()
	ON_WM_TIMER()
	ON_WM_ERASEBKGND()
END_MESSAGE_MAP()


ScopeCtrl::~ScopeCtrl()
{
	s_scopes.erase(std::remove(s_scopes.begin(), s_scopes.end(), this), s_scopes.end());
}


void ScopeCtrl::Place(const CRect &rect, bool fits)
{
	m_place = rect;
	m_fits = fits;
	Apply();
}


void ScopeCtrl::Apply()
{
	if(!m_hWnd)
		return;
	const bool show = m_fits && TrackerSettings::Instance().showOscilloscope;
	SetWindowPos(nullptr, m_place.left, m_place.top, m_place.Width(), m_place.Height(),
		SWP_NOZORDER | SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
	if(show)
		Start();
	else
		Stop();
}


void ScopeCtrl::RefreshAll()
{
	for(ScopeCtrl *scope : s_scopes)
		scope->Apply();
}


bool ScopeCtrl::Create(CWnd *parent, UINT id)
{
	s_scopes.push_back(this);
	m_buffer = &CMainFrame::GetMainFrame()->GetScopeBuffer();
	const auto wndClass = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW, nullptr, nullptr, nullptr);
	return CWnd::CreateEx(0, wndClass, nullptr, WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), parent, id) != FALSE;
}


void ScopeCtrl::Start()
{
	if(!m_running && m_hWnd)
	{
		// Кадров столько, сколько показывает экран: рисовать чаще его частоты
		// обновления бессмысленно, лишние кадры всё равно будут выброшены.
		// Если частоту узнать не удалось, берём обычные шестьдесят.
		int refresh = 0;
		if(CDC *dc = GetDC(); dc != nullptr)
		{
			refresh = dc->GetDeviceCaps(VREFRESH);
			ReleaseDC(dc);
		}
		if(refresh < 24 || refresh > 1000)
			refresh = 60;
		// Выше ста двадцати разницы уже не видно, а процессор занят: на экране
		// с большей частотой лишние кадры просто не нужны.
		refresh = std::min(refresh, 120);
		m_clock = std::make_unique<Util::MultimediaClock>(1);
		SetTimer(ScopeTimer, std::max(1, 1000 / refresh), nullptr);
		m_running = true;
	}
}


void ScopeCtrl::Stop()
{
	if(m_running && m_hWnd)
	{
		KillTimer(ScopeTimer);
		m_clock.reset();
		m_running = false;
	}
}


void ScopeCtrl::OnTimer(UINT_PTR id)
{
	if(id == ScopeTimer && IsWindowVisible())
		Invalidate(FALSE);
}


uint32 ScopeCtrl::Trigger(const float *samples, uint32 count, float peak)
{
	// Картинка начинается с перехода через ноль снизу вверх: у периодической
	// волны он случается раз за период, поэтому кадры встают на одно место.
	//
	// Но у волны FM переходов за период бывает несколько: мелкая рябь на спаде
	// пересекает ноль не хуже основного фронта. Цепляясь то за один, то за
	// другой, картинка ездит влево-вправо. Поэтому переход засчитывается только
	// после того, как волна сходила достаточно глубоко вниз: рябь столько не
	// проходит, а настоящий период проходит всегда. Так в осциллографах и
	// сделано, и порог там тоже берётся от размаха сигнала, а не абсолютный —
	// иначе на тихой ноте он не сработает вовсе.
	const float level = peak * 0.3f;
	if(level <= 0.0f)
		return 0;
	bool armed = false;
	uint32 fallback = 0;
	for(uint32 i = 1; i < count; i++)
	{
		if(samples[i] < -level)
		{
			armed = true;
		} else if(samples[i - 1] <= 0.0f && samples[i] > 0.0f)
		{
			if(armed)
				return i;
			if(!fallback)
				fallback = i;
		}
	}
	// Волна, которая не уходит ниже порога, бывает: например смещённая вверх.
	// Для неё берём первый же переход, это всё равно лучше, чем нулевой сдвиг.
	return fallback;
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

	// Раз в секунду пересчитываем, сколько кадров нарисовано и сколько раз
	// пришёл новый звук: если второе число мало, рисовать чаще бессмысленно,
	// кадры просто повторяют друг друга.
	m_frames++;
	if(const uint32 written = m_buffer->Written(); written != m_lastWritten)
	{
		m_lastWritten = written;
		m_arrivals++;
	}
	const uint32 now = static_cast<uint32>(GetTickCount64());
	if(now - m_countedAt >= 1000)
	{
		m_shownFrames = static_cast<int>(m_frames);
		m_shownArrivals = static_cast<int>(m_arrivals);
		m_frames = m_arrivals = 0;
		m_countedAt = now;
	}

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
		float whole = 0.0f;
		for(const float value : m_samples)
			whole = std::max(whole, std::abs(value));
		const uint32 start = Trigger(m_samples.data(), search, whole);
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
		CString text = (peak > 0.0f)
			? mpt::cfmt::fix(20.0 * std::log10(static_cast<double>(peak)), 1) + _T(" dB")
			: CString(_T("-inf dB"));
		if(m_shownFrames)
			text = mpt::cfmt::val(m_shownFrames) + _T(" кадр/с, звук ") + mpt::cfmt::val(m_shownArrivals) + _T("/с   ") + text;
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
