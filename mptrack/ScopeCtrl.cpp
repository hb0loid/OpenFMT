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

static constexpr UINT WM_SCOPE_FRAME = WM_APP + 1;

static std::vector<ScopeCtrl *> s_scopes;

BEGIN_MESSAGE_MAP(ScopeCtrl, CWnd)
	ON_WM_PAINT()
	ON_WM_DESTROY()
	ON_WM_ERASEBKGND()
	ON_WM_RBUTTONUP()
	ON_MESSAGE(WM_SCOPE_FRAME, &ScopeCtrl::OnFrame)
END_MESSAGE_MAP()


// Пункты меню по правой кнопке. Меню своё и никуда не уходит, поэтому номера
// нужны только внутри этого файла: TrackPopupMenu с TPM_RETURNCMD возвращает
// выбранный номер прямо на месте, без похода через карту сообщений.
namespace ScopeMenu
{
	enum : UINT
	{
		WindowAuto = 100, Window5, Window10, Window20, Window50, Window100,
		StyleLine = 200, StyleFilled, StyleDots,
		RateAuto = 300, Rate30, Rate60, Rate120, Rate240,
		Trigger = 400, AutoGain, Level,
	};

	// Миллисекунды окна по номеру пункта и обратно.
	static int WindowOf(UINT id)
	{
		switch(id)
		{
		case Window5: return 5;
		case Window10: return 10;
		case Window20: return 20;
		case Window50: return 50;
		case Window100: return 100;
		default: return 0;
		}
	}

	static int RateOf(UINT id)
	{
		switch(id)
		{
		case Rate30: return 30;
		case Rate60: return 60;
		case Rate120: return 120;
		case Rate240: return 240;
		default: return 0;
		}
	}
}


ScopeCtrl::~ScopeCtrl()
{
	Stop();
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
	if(m_ticking || !m_hWnd)
		return;
	// Кадров столько, сколько показывает экран, но не больше ста двадцати:
	// выше разницы не видно, а процессор занят.
	int refresh = TrackerSettings::Instance().oscilloscopeFrameRate;
	if(refresh <= 0)
	{
		if(CDC *dc = GetDC(); dc != nullptr)
		{
			refresh = dc->GetDeviceCaps(VREFRESH);
			ReleaseDC(dc);
		}
		if(refresh < 24 || refresh > 1000)
			refresh = 60;
		refresh = std::min(refresh, 120);
	}
	m_interval = std::max(1, 1000 / refresh);
	// Без мелкого шага системного таймера сон потока округляется до
	// пятнадцати миллисекунд, и весь смысл отдельного потока теряется.
	m_clock = std::make_unique<Util::MultimediaClock>(1);
	m_pending = false;
	m_ticking = true;
	m_ticker = std::thread([this]()
	{
		while(m_ticking)
		{
			Sleep(static_cast<DWORD>(m_interval));
			// Пока прошлый кадр не нарисован, новый такт не нужен: иначе
			// сообщения копятся в очереди и окно отстаёт всё сильнее.
			if(m_ticking && !m_pending.exchange(true))
				PostMessage(WM_SCOPE_FRAME);
			else
				m_pending = m_pending.load();
		}
	});
}


void ScopeCtrl::Stop()
{
	if(!m_ticking)
		return;
	m_ticking = false;
	if(m_ticker.joinable())
		m_ticker.join();
	m_clock.reset();
}


void ScopeCtrl::Restart()
{
	const bool wasTicking = m_ticking;
	Stop();
	if(wasTicking)
		Start();
	Invalidate(FALSE);
}


uint32 ScopeCtrl::Span(int width) const
{
	const int window = TrackerSettings::Instance().oscilloscopeWindow;
	if(window <= 0)
	{
		// По ширине окна: на столбец пикселей не меньше двух отсчётов, иначе
		// соседние столбцы берут один и тот же и линия рассыпается в точки.
		return std::clamp<uint32>(static_cast<uint32>(width) * 2, MinSpan, MaxSpan);
	}
	// Заданное окно считается от частоты микшера, чтобы на экране была именно
	// та доля секунды, которую выбрали, независимо от настроек звука.
	uint32 rate = TrackerSettings::Instance().GetMixerSettings().gdwMixingFreq;
	if(rate < 8000 || rate > 192000)
		rate = 48000;
	const uint64 samples = static_cast<uint64>(rate) * static_cast<uint32>(window) / 1000u;
	return static_cast<uint32>(std::clamp<uint64>(samples, 64, 10000));
}


void ScopeCtrl::OnRButtonUp(UINT, CPoint point)
{
	auto &settings = TrackerSettings::Instance();
	const auto check = [](bool on) -> UINT { return MF_STRING | (on ? MF_CHECKED : 0); };
	const int window = settings.oscilloscopeWindow;
	const int style = settings.oscilloscopeStyle;
	const int rate = settings.oscilloscopeFrameRate;

	CMenu menu, windowMenu, styleMenu, rateMenu;
	menu.CreatePopupMenu();
	windowMenu.CreatePopupMenu();
	windowMenu.AppendMenu(check(window <= 0), ScopeMenu::WindowAuto, _T("&By width"));
	windowMenu.AppendMenu(check(window == 5), ScopeMenu::Window5, _T("&5 ms"));
	windowMenu.AppendMenu(check(window == 10), ScopeMenu::Window10, _T("1&0 ms"));
	windowMenu.AppendMenu(check(window == 20), ScopeMenu::Window20, _T("&20 ms"));
	windowMenu.AppendMenu(check(window == 50), ScopeMenu::Window50, _T("5&0 ms"));
	windowMenu.AppendMenu(check(window == 100), ScopeMenu::Window100, _T("&100 ms"));
	styleMenu.CreatePopupMenu();
	styleMenu.AppendMenu(check(style == 0), ScopeMenu::StyleLine, _T("&Line"));
	styleMenu.AppendMenu(check(style == 1), ScopeMenu::StyleFilled, _T("&Filled"));
	styleMenu.AppendMenu(check(style == 2), ScopeMenu::StyleDots, _T("&Dots"));
	rateMenu.CreatePopupMenu();
	rateMenu.AppendMenu(check(rate <= 0), ScopeMenu::RateAuto, _T("&As the screen shows"));
	rateMenu.AppendMenu(check(rate == 30), ScopeMenu::Rate30, _T("&30 fps"));
	rateMenu.AppendMenu(check(rate == 60), ScopeMenu::Rate60, _T("&60 fps"));
	rateMenu.AppendMenu(check(rate == 120), ScopeMenu::Rate120, _T("1&20 fps"));
	rateMenu.AppendMenu(check(rate == 240), ScopeMenu::Rate240, _T("2&40 fps"));
	menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(windowMenu.m_hMenu), _T("&Time window"));
	menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(styleMenu.m_hMenu), _T("&Shape"));
	menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(rateMenu.m_hMenu), _T("&Frame rate"));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(check(settings.oscilloscopeTrigger), ScopeMenu::Trigger, _T("S&tanding wave"));
	menu.AppendMenu(check(settings.oscilloscopeAutoGain), ScopeMenu::AutoGain, _T("Auto &gain"));
	menu.AppendMenu(check(settings.oscilloscopeLevel), ScopeMenu::Level, _T("Show le&vel"));

	ClientToScreen(&point);
	const UINT chosen = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, this);
	switch(chosen)
	{
	case 0:
		return;
	case ScopeMenu::StyleLine:
	case ScopeMenu::StyleFilled:
	case ScopeMenu::StyleDots:
		settings.oscilloscopeStyle = static_cast<int32>(chosen - ScopeMenu::StyleLine);
		break;
	case ScopeMenu::Trigger:
		settings.oscilloscopeTrigger = !settings.oscilloscopeTrigger;
		break;
	case ScopeMenu::AutoGain:
		settings.oscilloscopeAutoGain = !settings.oscilloscopeAutoGain;
		m_gain = 1.0f;
		break;
	case ScopeMenu::Level:
		settings.oscilloscopeLevel = !settings.oscilloscopeLevel;
		break;
	default:
		if(chosen >= ScopeMenu::WindowAuto && chosen <= ScopeMenu::Window100)
			settings.oscilloscopeWindow = ScopeMenu::WindowOf(chosen);
		else if(chosen >= ScopeMenu::RateAuto && chosen <= ScopeMenu::Rate240)
			settings.oscilloscopeFrameRate = ScopeMenu::RateOf(chosen);
		else
			return;
		break;
	}
	// Частота кадров задаётся потоку при пуске, поэтому его надо перезавести;
	// остальным настройкам хватило бы перерисовки, но лишний раз не повредит.
	Restart();
}


LRESULT ScopeCtrl::OnFrame(WPARAM, LPARAM)
{
	m_pending = false;
	if(IsWindowVisible())
		Invalidate(FALSE);
	return 0;
}


void ScopeCtrl::OnDestroy()
{
	Stop();
	CWnd::OnDestroy();
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

	const uint32 span = Span(width);
	// Поиск начала картинки идёт по куску перед ней, и ему нужно не меньше
	// периода низкой ноты: на мелком окне половины span на это не хватит.
	const uint32 search = TrackerSettings::Instance().oscilloscopeTrigger
		? std::clamp<uint32>(span, 1024, ScopeBuffer::Size - span - 1024) : 0;
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
		const uint32 start = search ? Trigger(m_samples.data(), search, whole) : 0;
		const float *shown = m_samples.data() + start;

		float peak = 0.0f;
		for(uint32 i = 0; i < span; i++)
			peak = std::max(peak, std::abs(shown[i]));
		m_peak = peak;

		// Громкое ужимаем сразу, тихое растягиваем не спеша: иначе картинка
		// дёргается по высоте на каждой ноте и читать её невозможно.
		if(TrackerSettings::Instance().oscilloscopeAutoGain)
		{
			const float target = std::clamp(peak > 0.0001f ? 0.85f / peak : 1.0f, 1.0f, 64.0f);
			m_gain = (target < m_gain) ? target : m_gain + (target - m_gain) * 0.1f;
		} else
		{
			m_gain = 1.0f;
		}

		const auto place = [&](float value)
		{
			return middle - static_cast<int>(std::clamp(value * m_gain, -1.0f, 1.0f) * (height - thickness * 2) / 2);
		};

		// Перо ровно в один пиксель: толстое включает в GDI совсем другой,
		// медленный путь, и кадров выходило двадцать пять вместо сорока трёх.
		// Толщина набирается повторной отрисовкой со сдвигом — это всё равно
		// считанные вызовы против тысяч.
		CPen wavePen(PS_SOLID, 1, wave);
		m_offScreenDC.SelectObject(&wavePen);
		const int style = TrackerSettings::Instance().oscilloscopeStyle;
		// Ломаная идёт зигзагом через верх и низ каждого столбца, поэтому она
		// же соединяет столбцы между собой: на широком окне, где отсчётов
		// меньше, чем столбцов, линия иначе распадается на точки. Там, где
		// столбец вырождается в точку, вторая вершина не нужна.
		m_points.clear();
		m_points.reserve(static_cast<size_t>(width) * 2);
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
			const int top = place(high), bottom = place(low);
			if(style == 1)
			{
				// Заполненная область: столбец от нулевой линии до волны, так
				// что тихое остаётся тонкой полоской, а громкое заливает окно.
				const int up = std::min(top, middle), down = std::max(bottom, middle);
				m_offScreenDC.FillSolidRect(x, up, 1, std::max(1, down - up + 1), wave);
			} else if(style == 2)
			{
				// Точки: размер берётся той же толщины, что и линия, иначе на
				// экране с высокой плотностью их просто не видно.
				m_offScreenDC.FillSolidRect(x, top, thickness, thickness, wave);
				if(top != bottom)
					m_offScreenDC.FillSolidRect(x, bottom, thickness, thickness, wave);
			} else
			{
				const bool downwards = (x & 1) != 0;
				m_points.emplace_back(x, downwards ? top : bottom);
				if(top != bottom)
					m_points.emplace_back(x, downwards ? bottom : top);
			}
		}
		if(const int count = static_cast<int>(m_points.size()); count > 1)
		{
			m_offScreenDC.Polyline(m_points.data(), count);
			for(int pass = 1; pass < thickness; pass++)
			{
				for(CPoint &point : m_points)
					point.y++;
				m_offScreenDC.Polyline(m_points.data(), count);
			}
		}

		// Уровень подписывается настоящий, до усиления: иначе цифра врала бы.
		if(TrackerSettings::Instance().oscilloscopeLevel)
		{
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
	}

	m_offScreenDC.SelectObject(oldPen);
	dc.BitBlt(0, 0, width, height, &m_offScreenDC, 0, 0, SRCCOPY);
}


OPENMPT_NAMESPACE_END
