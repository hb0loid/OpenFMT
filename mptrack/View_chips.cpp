/*
 * View_chips.cpp
 * --------------
 * Purpose: Chips tab, lower panel: a mixer with one strip per chip.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#include "stdafx.h"
#include "View_chips.h"
#include "HighDPISupport.h"
#include "Mainfrm.h"
#include "Moddoc.h"
#include "TrackerSettings.h"
#include "../common/mptStringBuffer.h"
#include <cmath>

OPENMPT_NAMESPACE_BEGIN

// Номера нужны только внутри этого окна: какой ползунок двигают, видно по его
// собственному окну, а не по номеру, и командами они не управляют.
static constexpr UINT FirstSliderID = 0x4000;

IMPLEMENT_SERIAL(CViewChips, CModScrollView, 0)

BEGIN_MESSAGE_MAP(CViewChips, CModScrollView)
	ON_WM_SIZE()
	ON_WM_VSCROLL()
	ON_WM_ERASEBKGND()
END_MESSAGE_MAP()


const TCHAR *CViewChips::ChipName(ChipKind chip)
{
	switch(chip)
	{
	case ChipOPL: return _T("OPL");
	case ChipOPN2: return _T("YM2612");
	case ChipOPM: return _T("YM2151");
	case ChipPSG: return _T("SN76489");
	default: return _T("?");
	}
}


void CViewChips::OnInitialUpdate()
{
	CModScrollView::OnInitialUpdate();
	CreateStrips();
	LayoutStrips();
	ReadSong();
}


void CViewChips::CreateStrips()
{
	if(m_created)
		return;
	m_created = true;
	HFONT font = CMainFrame::GetGUIFont();
	for(uint8 i = 0; i < ChipKinds; i++)
	{
		Strip &strip = m_strips[i];
		const CRect empty(0, 0, 0, 0);
		strip.name.Create(ChipName(static_cast<ChipKind>(i)), WS_CHILD | WS_VISIBLE | SS_CENTER, empty, this);
		strip.name.SendMessage(WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
		strip.slider.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_VERT | TBS_BOTH | TBS_NOTICKS | TBS_TOOLTIPS,
			empty, this, FirstSliderID + i);
		strip.slider.SetRange(0, MaxVolume);
		strip.slider.SetPageSize(ChipVolumeUnity / 8);
		strip.slider.SetLineSize(1);
		strip.value.Create(_T(""), WS_CHILD | WS_VISIBLE | SS_CENTER, empty, this);
		strip.value.SendMessage(WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
	}
}


void CViewChips::LayoutStrips()
{
	if(!m_created || !m_hWnd)
		return;
	CRect client;
	GetClientRect(&client);
	const int margin = HighDPISupport::ScalePixels(8, m_hWnd);
	const int lineHeight = HighDPISupport::ScalePixels(14, m_hWnd);
	const int stripWidth = HighDPISupport::ScalePixels(72, m_hWnd);
	const int sliderWidth = HighDPISupport::ScalePixels(28, m_hWnd);
	// Под цифрой две строки: проценты и децибелы. Ползунок занимает всё, что
	// осталось: на низкой панели он становится коротким, но не исчезает и не
	// вылезает за край.
	const int valueHeight = lineHeight * 2;
	const int sliderHeight = std::max(lineHeight, client.Height() - margin * 2 - lineHeight - valueHeight);
	for(uint8 i = 0; i < ChipKinds; i++)
	{
		Strip &strip = m_strips[i];
		const int left = margin + i * stripWidth;
		int top = margin;
		strip.name.SetWindowPos(nullptr, left, top, stripWidth - margin, lineHeight, SWP_NOZORDER | SWP_NOACTIVATE);
		top += lineHeight;
		strip.slider.SetWindowPos(nullptr, left + (stripWidth - margin - sliderWidth) / 2, top, sliderWidth, sliderHeight,
			SWP_NOZORDER | SWP_NOACTIVATE);
		top += sliderHeight;
		strip.value.SetWindowPos(nullptr, left, top, stripWidth - margin, valueHeight, SWP_NOZORDER | SWP_NOACTIVATE);
	}
}


void CViewChips::ShowValue(ChipKind chip, int volume)
{
	// Проценты, а рядом децибелы: процентами удобно ставить ровную единицу,
	// а на слух разница измеряется всё-таки в децибелах.
	CString text = mpt::cfmt::val(volume * 100 / ChipVolumeUnity) + _T("%");
	if(volume <= 0)
		text += _T("\n-inf dB");
	else
		text += _T("\n") + mpt::cfmt::fix(20.0 * std::log10(static_cast<double>(volume) / ChipVolumeUnity), 1) + _T(" dB");
	m_strips[chip].value.SetWindowText(text);
}


void CViewChips::ReadSong()
{
	const CModDoc *modDoc = GetDocument();
	if(!modDoc || !m_created)
		return;
	const CSoundFile &sndFile = modDoc->GetSoundFile();
	for(uint8 i = 0; i < ChipKinds; i++)
	{
		const int volume = std::clamp<int>(sndFile.m_chipVolume[i], 0, MaxVolume);
		m_strips[i].slider.SetPos(MaxVolume - volume);
		ShowValue(static_cast<ChipKind>(i), volume);
	}
}


void CViewChips::UpdateView(UpdateHint hint, CObject *)
{
	// Пока ползунок под пальцем, переставлять его по песне нельзя: он сам её и
	// меняет, и подхват собственной правки сбивал бы ход.
	if(!m_updating && (hint.GetType()[HINT_MODGENERAL | HINT_MODTYPE]))
		ReadSong();
}


void CViewChips::OnSize(UINT type, int cx, int cy)
{
	CModScrollView::OnSize(type, cx, cy);
	LayoutStrips();
}


void CViewChips::OnVScroll(UINT code, UINT pos, CScrollBar *bar)
{
	CModDoc *modDoc = GetDocument();
	if(!modDoc || !bar)
	{
		CModScrollView::OnVScroll(code, pos, bar);
		return;
	}
	CSoundFile &sndFile = modDoc->GetSoundFile();
	bool changed = false;
	for(uint8 i = 0; i < ChipKinds; i++)
	{
		if(bar->m_hWnd != m_strips[i].slider.m_hWnd)
			continue;
		// Ход ползунка идёт сверху вниз, а громкость наоборот, поэтому
		// значение берётся от конца хода.
		const int volume = std::clamp(MaxVolume - m_strips[i].slider.GetPos(), 0, MaxVolume);
		if(sndFile.m_chipVolume[i] != static_cast<uint16>(volume))
		{
			sndFile.m_chipVolume[i] = static_cast<uint16>(volume);
			changed = true;
		}
		ShowValue(static_cast<ChipKind>(i), volume);
	}
	if(changed)
	{
		m_updating = true;
		modDoc->SetModified();
		m_updating = false;
	}
	CModScrollView::OnVScroll(code, pos, bar);
}


BOOL CViewChips::OnEraseBkgnd(CDC *dc)
{
	CRect client;
	GetClientRect(&client);
	dc->FillSolidRect(&client, GetSysColor(COLOR_BTNFACE));
	return TRUE;
}

OPENMPT_NAMESPACE_END
