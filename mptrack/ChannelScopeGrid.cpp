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
#include "../soundlib/Sndfile.h"
#include <cmath>

OPENMPT_NAMESPACE_BEGIN

static constexpr UINT_PTR FrameTimer = 1;

BEGIN_MESSAGE_MAP(ChannelScopeGrid, CWnd)
	ON_WM_PAINT()
	ON_WM_TIMER()
	ON_WM_DESTROY()
	ON_WM_ERASEBKGND()
END_MESSAGE_MAP()


bool ChannelScopeGrid::Create(CWnd *parent, UINT id)
{
	m_gain.fill(1.0f);
	const CRect empty(0, 0, 0, 0);
	return CWnd::Create(nullptr, _T(""), WS_CHILD, empty, parent, id) != FALSE;
}


ChannelScopeGrid::~ChannelScopeGrid()
{
	if(m_running && CMainFrame::GetMainFrame() != nullptr)
		CMainFrame::GetMainFrame()->GetChannelScopes().RemoveUser();
}


void ChannelScopeGrid::Place(const CRect &rect, bool visible)
{
	if(!m_hWnd)
		return;
	const bool show = visible && rect.Height() > HighDPISupport::ScalePixels(24, m_hWnd);
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
		SetTimer(FrameTimer, 33, nullptr);
	else
		KillTimer(FrameTimer);
}


void ChannelScopeGrid::OnDestroy()
{
	if(m_running)
	{
		KillTimer(FrameTimer);
		if(CMainFrame *main = CMainFrame::GetMainFrame(); main != nullptr)
			main->GetChannelScopes().RemoveUser();
		m_running = false;
	}
	CWnd::OnDestroy();
}


void ChannelScopeGrid::OnTimer(UINT_PTR id)
{
	if(id == FrameTimer && m_running)
		Invalidate(FALSE);
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

	// Сетка: столько столбцов, чтобы окошко не стало уже своей подписи.
	const int minCell = HighDPISupport::ScalePixels(96, m_hWnd);
	int columns = std::max(1, client.Width() / minCell);
	columns = std::min<int>(columns, numChannels);
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
		const float wanted = (peak > 0.0001f) ? 0.45f / peak : gain;
		gain = (wanted < gain) ? wanted : gain + (wanted - gain) * 0.05f;
		gain = std::clamp(gain, 0.2f, 400.0f);

		const int middle = cell.top + cell.Height() / 2;
		HGDIOBJ oldPen = ::SelectObject(dc.m_hDC, (peak > 0.0001f) ? wavePen : quietPen);
		if(!haveData || peak <= 0.0f)
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
