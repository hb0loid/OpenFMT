/*
 * ChannelScopeGrid.cpp
 * --------------------
 * Назначение: осциллографы по каналам — сетка маленьких окон волны.
 * Авторы   : OpenMPT Devs
 * Исходники OpenMPT распространяются по лицензии BSD. Подробности в LICENSE.
 */

#include "stdafx.h"
#include "ChannelScopeGrid.h"
#include "HighDPISupport.h"
#include "Mainfrm.h"
#include "Moddoc.h"
#include "TrackerSettings.h"
#include "../soundlib/Sndfile.h"
#include <cmath>

OPENMPT_NAMESPACE_BEGIN

// Свой номер сообщения: такт кадра приходит из отдельного потока.
static constexpr UINT WM_GRID_FRAME = WM_APP + 2;
static constexpr UINT WM_GRID_CLOSE = WM_APP + 3;
static constexpr UINT ID_GRID_FULLSCREEN = 1;
static constexpr UINT ID_GRID_HIDE = 2;

BEGIN_MESSAGE_MAP(ChannelScopeGrid, CWnd)
	ON_MESSAGE(WM_GRID_FRAME, &ChannelScopeGrid::OnFrame)
	ON_MESSAGE(WM_GRID_CLOSE, &ChannelScopeGrid::OnClosePopup)
	ON_WM_PAINT()
	ON_WM_DESTROY()
	ON_WM_RBUTTONUP()
	ON_WM_LBUTTONDBLCLK()
	ON_WM_KEYDOWN()
	ON_WM_ERASEBKGND()
END_MESSAGE_MAP()


bool ChannelScopeGrid::Create(CWnd *parent, UINT id)
{
	m_gain.fill(1.0f);
	m_parent = parent;
	const CRect empty(0, 0, 0, 0);
	return CWnd::Create(nullptr, _T(""), WS_CHILD, empty, parent, id) != FALSE;
}


// Кадры шлёт отдельный поток: таймер окна упирается в шаг системного таймера,
// и шестидесяти кадров от него не добиться, сколько ни проси.
void ChannelScopeGrid::StartTicker()
{
	if(m_ticking || !m_hWnd)
		return;
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
		// Больше шестидесяти кадров волне не нужно, а на мониторе со
		// стодвадцатью герцами сетка рисовала вдвое чаще и нагружала машину
		// во время воспроизведения. Явно заданная частота остаётся как есть.
		refresh = std::min(refresh, 60);
	}
	m_interval = std::max(1, 1000 / refresh);
	// Без мелкого шага системного таймера сон округлится до пятнадцати
	// миллисекунд, и смысл отдельного потока пропадёт.
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
				PostMessage(WM_GRID_FRAME);
		}
	});
}


void ChannelScopeGrid::StopTicker()
{
	if(!m_ticking)
		return;
	m_ticking = false;
	if(m_ticker.joinable())
		m_ticker.join();
	m_clock.reset();
}


LRESULT ChannelScopeGrid::OnFrame(WPARAM, LPARAM)
{
	m_pending = false;
	if(IsWindowVisible())
		Invalidate(FALSE);
	return 0;
}


ChannelScopeGrid::~ChannelScopeGrid()
{
	if(m_popup != nullptr && ::IsWindow(m_popup->m_hWnd))
		m_popup->DestroyWindow();  // копия удаляет себя сама
	m_popup = nullptr;
	StopTicker();
	if(m_running && CMainFrame::GetMainFrame() != nullptr)
		CMainFrame::GetMainFrame()->GetChannelScopes().RemoveUser();
}


void ChannelScopeGrid::Place(const CRect &rect, bool visible)
{
	if(!m_hWnd)
		return;
	const bool show = visible && rect.Height() > HighDPISupport::ScalePixels(24, m_hWnd);
	m_placed = rect;
	SetWindowPos(nullptr, rect.left, rect.top, rect.Width(), rect.Height(),
		SWP_NOZORDER | SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
	if(show == m_running)
		return;
	m_running = show;
	// Писать звук по каналам микшер начинает только когда сетку видно: за
	// невидимую картинку платить нечем.
	if(CMainFrame *main = CMainFrame::GetMainFrame(); main != nullptr)
	{
		if(show)
			main->GetChannelScopes().AddUser();
		else
			main->GetChannelScopes().RemoveUser();
	}
	if(show)
		StartTicker();
	else
		StopTicker();
}


// Во весь экран: поверх всего открывается копия сетки, сама сетка остаётся в
// панели. Назад — тем же пунктом меню, двойным щелчком или клавишей Esc.
void ChannelScopeGrid::ToggleFullScreen()
{
	if(m_owner != nullptr)
	{
		// Копия просит выйти свою сетку: закрывать себя из своего же
		// обработчика нельзя.
		m_owner->ToggleFullScreen();
		return;
	}
	if(!m_hWnd)
		return;
	if(!m_fullScreen)
	{
		MONITORINFO monitor{};
		monitor.cbSize = sizeof(monitor);
		if(!::GetMonitorInfo(::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &monitor))
			return;
		auto popup = new ChannelScopeGrid;
		if(!popup->CreatePopup(*this, CRect(monitor.rcMonitor)))
		{
			delete popup;
			return;
		}
		m_popup = popup;
		m_fullScreen = true;
	} else
	{
		m_fullScreen = false;
		if(m_popup != nullptr)
		{
			m_popup->m_owner = nullptr;
			m_popup->PostMessage(WM_GRID_CLOSE);
			m_popup = nullptr;
		}
		if(m_parent != nullptr)
			m_parent->SetFocus();
	}
}


bool ChannelScopeGrid::CreatePopup(ChannelScopeGrid &owner, const CRect &screen)
{
	m_gain.fill(1.0f);
	m_owner = &owner;
	const LPCTSTR windowClass = AfxRegisterWndClass(CS_DBLCLKS, ::LoadCursor(nullptr, IDC_ARROW));
	if(!CreateEx(WS_EX_TOPMOST, windowClass, _T("OpenFMT"), WS_POPUP, screen, nullptr, 0))
		return false;
	Place(screen, true);
	SetForegroundWindow();
	SetFocus();
	return true;
}


LRESULT ChannelScopeGrid::OnClosePopup(WPARAM, LPARAM)
{
	DestroyWindow();
	return 0;
}


void ChannelScopeGrid::PostNcDestroy()
{
	CWnd::PostNcDestroy();
	// Копию на весь экран создают через new, и после закрытия окна она уходит сама.
	if(m_owner != nullptr || m_parent == nullptr)
	{
		if(m_owner != nullptr && m_owner->m_popup == this)
		{
			m_owner->m_popup = nullptr;
			m_owner->m_fullScreen = false;
		}
		delete this;
	}
}


void ChannelScopeGrid::HideScopes()
{
	ChannelScopeGrid &grid = m_owner != nullptr ? *m_owner : *this;
	if(grid.m_fullScreen)
		grid.ToggleFullScreen();
	TrackerSettings::Instance().chipsShowScopes = false;
	if(grid.m_parent != nullptr)
		grid.m_parent->SendMessage(WM_SIZE, SIZE_RESTORED, 0);
}


void ChannelScopeGrid::OnRButtonUp(UINT, CPoint point)
{
	CMenu menu;
	menu.CreatePopupMenu();
	menu.AppendMenu(MF_STRING | (IsFullScreen() ? MF_CHECKED : 0), ID_GRID_FULLSCREEN, _T("&Full Screen	Esc"));
	menu.AppendMenu(MF_STRING, ID_GRID_HIDE, _T("&Hide Oscilloscopes"));
	CPoint screen = point;
	ClientToScreen(&screen);
	const UINT choice = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
		screen.x, screen.y, this);
	menu.DestroyMenu();
	if(choice == ID_GRID_FULLSCREEN)
	{
		ToggleFullScreen();
	} else if(choice == ID_GRID_HIDE)
	{
		HideScopes();
	}
}


void ChannelScopeGrid::OnLButtonDblClk(UINT, CPoint)
{
	ToggleFullScreen();
}


void ChannelScopeGrid::OnKeyDown(UINT key, UINT repeat, UINT flags)
{
	if(key == VK_ESCAPE && IsFullScreen())
		ToggleFullScreen();
	else
		CWnd::OnKeyDown(key, repeat, flags);
}


void ChannelScopeGrid::OnDestroy()
{
	StopTicker();
	if(m_running)
	{
		if(CMainFrame *main = CMainFrame::GetMainFrame(); main != nullptr)
			main->GetChannelScopes().RemoveUser();
		m_running = false;
	}
	CWnd::OnDestroy();
}


uint32 ChannelScopeGrid::Trigger(const float *samples, uint32 count, float peak)
{
	if(peak <= 0.0f)
		return 0;
	// Нуль снизу вверх: с него волна выглядит стоящей на месте.
	const uint32 search = count / 2;
	for(uint32 i = 1; i < search; i++)
	{
		if(samples[i - 1] <= 0.0f && samples[i] > 0.0f)
			return i;
	}
	return 0;
}


void ChannelScopeGrid::OnPaint()
{
	CPaintDC dc(this);
	CRect client;
	GetClientRect(&client);
	if(client.IsRectEmpty())
		return;
	if(m_offScreenSize != client.Size() || m_offScreenDC.m_hDC == nullptr)
	{
		if(m_offScreenDC.m_hDC != nullptr)
		{
			m_offScreenBitmap.DeleteObject();
			m_offScreenDC.DeleteDC();
		}
		m_offScreenDC.CreateCompatibleDC(&dc);
		m_offScreenBitmap.CreateCompatibleBitmap(&dc, client.Width(), client.Height());
		m_offScreenDC.SelectObject(&m_offScreenBitmap);
		m_offScreenSize = client.Size();
	}
	Draw(m_offScreenDC, client);
	dc.BitBlt(0, 0, client.Width(), client.Height(), &m_offScreenDC, 0, 0, SRCCOPY);
}


void ChannelScopeGrid::Draw(CDC &dc, const CRect &client)
{
	CMainFrame *main = CMainFrame::GetMainFrame();
	const COLORREF back = RGB(0x10, 0x12, 0x18);
	dc.FillSolidRect(&client, back);
	if(main == nullptr)
		return;
	const CSoundFile *sndFile = main->GetSoundFilePlaying();
	const ChannelScopes &scopes = main->GetChannelScopes();
	const CHANNELINDEX numChannels = sndFile
		? std::min<CHANNELINDEX>(sndFile->GetNumChannels(), ChannelScopes::Channels)
		: 0;
	if(!numChannels)
		return;

	// Раскладка. Делить окно только по ширине нельзя: на широком экране все
	// каналы вставали в одну строку, и каждая клетка выходила высотой во всю
	// панель. Поэтому перебираем число столбцов и берём то, при котором
	// клетка ближе всего к привычным пропорциям осциллографа — вдвое шире,
	// чем выше, — не забывая, что уже своей подписи она быть не должна.
	const int minWidth = HighDPISupport::ScalePixels(90, m_hWnd);
	const int minHeight = HighDPISupport::ScalePixels(28, m_hWnd);
	constexpr double TargetRatio = 2.2;
	int columns = 1;
	double bestScore = -1.0;
	for(int candidate = 1; candidate <= numChannels; candidate++)
	{
		const int rowCount = (numChannels + candidate - 1) / candidate;
		const double w = static_cast<double>(client.Width()) / candidate;
		const double h = static_cast<double>(client.Height()) / rowCount;
		if(w < minWidth && candidate > 1)
			continue;
		// Чем ближе отношение сторон к желаемому, тем меньше оценка.
		double score = std::fabs(std::log((w / std::max(h, 1.0)) / TargetRatio));
		if(h < minHeight)
			score += 4.0;  // Слишком плоско: читать нечего
		if(bestScore < 0.0 || score < bestScore)
		{
			bestScore = score;
			columns = candidate;
		}
	}
	const int rows = (numChannels + columns - 1) / columns;
	const int cellWidth = client.Width() / columns;
	const int cellHeight = std::max(HighDPISupport::ScalePixels(20, m_hWnd), client.Height() / std::max(rows, 1));
	const int margin = HighDPISupport::ScalePixels(2, m_hWnd);

	HFONT font = CMainFrame::GetGUIFont();
	HGDIOBJ oldFont = ::SelectObject(dc.m_hDC, font);
	dc.SetBkMode(TRANSPARENT);
	CPen wavePen(PS_SOLID, HighDPISupport::ScalePixels(1, m_hWnd), RGB(0x60, 0xE8, 0x90));
	CPen quietPen(PS_SOLID, HighDPISupport::ScalePixels(1, m_hWnd), RGB(0x3A, 0x50, 0x44));

	m_samples.resize(Span);
	for(CHANNELINDEX chn = 0; chn < numChannels; chn++)
	{
		const int column = chn % columns, row = chn / columns;
		CRect cell(column * cellWidth, row * cellHeight, (column + 1) * cellWidth, (row + 1) * cellHeight);
		cell.DeflateRect(margin, margin);
		if(cell.Height() < HighDPISupport::ScalePixels(12, m_hWnd))
			break;
		dc.FillSolidRect(&cell, RGB(0x16, 0x18, 0x22));

		const bool haveData = scopes.ReadLatest(chn, m_samples.data(), Span);
		float peak = 0.0f;
		if(haveData)
		{
			for(uint32 i = 0; i < Span; i++)
				peak = std::max(peak, std::fabs(m_samples[i]));
		}
		// Усиление: быстро вниз на громком, медленно вверх на тихом.
		float &gain = m_gain[chn];
		const bool audible = peak > ChannelScopes::SilencePeak;
		const float wanted = audible ? 0.45f / peak : gain;
		gain = (wanted < gain) ? wanted : gain + (wanted - gain) * 0.05f;
		gain = std::clamp(gain, 0.2f, 0.45f / ChannelScopes::SilencePeak);

		const int middle = cell.top + cell.Height() / 2;
		HGDIOBJ oldPen = ::SelectObject(dc.m_hDC, audible ? wavePen : quietPen);
		if(!haveData || !audible)
		{
			dc.MoveTo(cell.left, middle);
			dc.LineTo(cell.right, middle);
		} else
		{
			const uint32 start = Trigger(m_samples.data(), Span, peak);
			const uint32 visible = Span - start;
			m_points.resize(cell.Width());
			for(int x = 0; x < cell.Width(); x++)
			{
				const uint32 at = start + static_cast<uint32>(static_cast<uint64>(x) * visible / std::max(cell.Width(), 1));
				const float value = m_samples[std::min(at, Span - 1)] * gain;
				const int y = middle - static_cast<int>(value * cell.Height() * 0.5f);
				m_points[x] = CPoint(cell.left + x, std::clamp<int>(y, cell.top, cell.bottom - 1));
			}
			dc.Polyline(m_points.data(), static_cast<int>(m_points.size()));
		}
		::SelectObject(dc.m_hDC, oldPen);

		// Подпись: имя канала, а если его нет — номер.
		CString label;
		if(sndFile != nullptr && !sndFile->ChnSettings[chn].szName.empty())
			label = mpt::ToCString(sndFile->GetCharsetInternal(), sndFile->ChnSettings[chn].szName);
		else
			label.Format(_T("%u"), static_cast<unsigned>(chn + 1));
		dc.SetTextColor(RGB(0x90, 0x98, 0xA8));
		CRect text(cell.left + margin, cell.top, cell.right, cell.top + HighDPISupport::ScalePixels(13, m_hWnd));
		dc.DrawText(label, &text, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
	}
	::SelectObject(dc.m_hDC, oldFont);
}

OPENMPT_NAMESPACE_END
