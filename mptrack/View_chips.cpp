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
static constexpr UINT ScopeGridID = 0x4100;
// Ход ползунка микросхемы: от тишины до вдвое громче, единица посередине.
static constexpr int MaxChipVolume = ChipVolumeUnity * 2;
// У сэмплов шкала своя, 0..255, и середина у неё 128 — столько же, сколько
// разрешает поле громкости в IT. Так полоска ведёт себя как остальные: внизу
// тишина, посередине единица, наверху вдвое громче. Децибелы на вкладке
// General считаются от «обычного» уровня своего режима микшера, он бывает и
// 192, и 256, поэтому цифры там и здесь могут не совпасть.
static constexpr int MaxSampleVolume = 255;


BEGIN_MESSAGE_MAP(ChipSlider, CSliderCtrl)
	ON_WM_RBUTTONUP()
END_MESSAGE_MAP()


void ChipSlider::OnRButtonUp(UINT, CPoint)
{
	if(owner)
		owner->Reset(index);
}


IMPLEMENT_SERIAL(CViewChips, CModScrollView, 0)

BEGIN_MESSAGE_MAP(CViewChips, CModScrollView)
	ON_WM_SIZE()
	ON_WM_VSCROLL()
	ON_WM_ERASEBKGND()
	ON_WM_RBUTTONUP()
END_MESSAGE_MAP()


const TCHAR *CViewChips::StripName(uint8 strip)
{
	switch(strip)
	{
	case ChipOPL: return _T("OPL");
	case ChipOPN2: return _T("YM2612");
	case ChipOPM: return _T("YM2151");
	case ChipPSG: return _T("SN76489");
	case SampleStrip: return _T("Samples");
	default: return _T("?");
	}
}


int CViewChips::Value(uint8 strip) const
{
	const CModDoc *modDoc = GetDocument();
	if(!modDoc)
		return 0;
	const CSoundFile &sndFile = modDoc->GetSoundFile();
	if(strip == SampleStrip)
		return static_cast<int>(sndFile.m_nSamplePreAmp);
	return static_cast<int>(sndFile.m_chipVolume[strip]);
}


void CViewChips::SetValue(uint8 strip, int value)
{
	CModDoc *modDoc = GetDocument();
	if(!modDoc)
		return;
	CSoundFile &sndFile = modDoc->GetSoundFile();
	if(strip == SampleStrip)
	{
		if(sndFile.m_nSamplePreAmp == static_cast<uint32>(value))
			return;
		sndFile.m_nSamplePreAmp = static_cast<uint32>(value);
	} else
	{
		if(sndFile.m_chipVolume[strip] == static_cast<uint16>(value))
			return;
		sndFile.m_chipVolume[strip] = static_cast<uint16>(value);
	}
	// Громкость сэмплов показана и в General, поэтому о правке сообщается всем:
	// две копии одного числа не должны расходиться.
	m_updating = true;
	modDoc->SetModified();
	modDoc->UpdateAllViews(nullptr, GeneralHint().General());
	m_updating = false;
}


void CViewChips::Reset(uint8 strip)
{
	if(strip >= NumStrips)
		return;
	SetValue(strip, m_strips[strip].def);
	m_strips[strip].slider.SetPos(m_strips[strip].max - m_strips[strip].def);
	ShowValue(strip, m_strips[strip].def);
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
	for(uint8 i = 0; i < NumStrips; i++)
	{
		Strip &strip = m_strips[i];
		strip.max = (i == SampleStrip) ? MaxSampleVolume : MaxChipVolume;
		strip.def = strip.max / 2;
		const CRect empty(0, 0, 0, 0);
		strip.name.Create(StripName(i), WS_CHILD | WS_VISIBLE | SS_CENTER, empty, this);
		strip.name.SendMessage(WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
		strip.slider.owner = this;
		strip.slider.index = i;
		strip.slider.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_VERT | TBS_BOTH | TBS_NOTICKS,
			empty, this, FirstSliderID + i);
		strip.slider.SetRange(0, strip.max);
		strip.slider.SetPageSize(std::max(1, strip.max / 16));
		strip.slider.SetLineSize(1);
		strip.value.Create(_T(""), WS_CHILD | WS_VISIBLE | SS_CENTER, empty, this);
		strip.value.SendMessage(WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
	}
	m_scopes.Create(this, ScopeGridID);
}


void CViewChips::LayoutStrips()
{
	if(!m_created || !m_hWnd)
		return;
	// Размеры взяты с группы громкостей на вкладке General: там ползунок
	// высотой в пятьдесят единиц диалога, и этого хватает. Растягивать их на
	// всю панель незачем — место нужно под то, что появится рядом.
	const int margin = HighDPISupport::ScalePixels(6, m_hWnd);
	// Высота строки берётся у самого шрифта: на глаз она не угадывается, и
	// вторая строка подписи обрезалась снизу.
	int lineHeight = HighDPISupport::ScalePixels(14, m_hWnd);
	if(CDC *dc = GetDC(); dc != nullptr)
	{
		HGDIOBJ oldFont = ::SelectObject(dc->m_hDC, CMainFrame::GetGUIFont());
		TEXTMETRIC metrics{};
		if(dc->GetTextMetrics(&metrics))
			lineHeight = metrics.tmHeight + metrics.tmExternalLeading + HighDPISupport::ScalePixels(1, m_hWnd);
		::SelectObject(dc->m_hDC, oldFont);
		ReleaseDC(dc);
	}
	const int stripWidth = HighDPISupport::ScalePixels(58, m_hWnd);
	const int sliderWidth = HighDPISupport::ScalePixels(24, m_hWnd);
	const int sliderHeight = HighDPISupport::ScalePixels(84, m_hWnd);
	const int valueHeight = lineHeight * 2;
	for(uint8 i = 0; i < NumStrips; i++)
	{
		Strip &strip = m_strips[i];
		const int left = margin + i * stripWidth;
		int top = margin;
		strip.name.SetWindowPos(nullptr, left, top, stripWidth, lineHeight, SWP_NOZORDER | SWP_NOACTIVATE);
		top += lineHeight;
		strip.slider.SetWindowPos(nullptr, left + (stripWidth - sliderWidth) / 2, top, sliderWidth, sliderHeight,
			SWP_NOZORDER | SWP_NOACTIVATE);
		top += sliderHeight;
		strip.value.SetWindowPos(nullptr, left, top, stripWidth, valueHeight, SWP_NOZORDER | SWP_NOACTIVATE);
	}

	// Осциллографы занимают всё, что осталось справа от полосок.
	CRect client;
	GetClientRect(&client);
	const int stripsRight = margin + NumStrips * stripWidth + margin;
	CRect scopes(stripsRight, margin, client.right - margin, client.bottom - margin);
	m_scopes.Place(scopes, TrackerSettings::Instance().chipsShowScopes
		&& scopes.Width() > HighDPISupport::ScalePixels(120, m_hWnd));
}


void CViewChips::ShowValue(uint8 strip, int value)
{
	// Проценты, а рядом децибелы: процентами удобно ставить ровную единицу,
	// а на слух разница измеряется всё-таки в децибелах.
	const int def = std::max(1, m_strips[strip].def);
	CString text = mpt::cfmt::val(value * 100 / def) + _T("%\n");
	if(value <= 0)
		text += _T("-inf dB");
	else
		text += mpt::cfmt::fix(20.0 * std::log10(static_cast<double>(value) / def), 1) + _T(" dB");
	m_strips[strip].value.SetWindowText(text);
}


void CViewChips::ReadSong()
{
	if(!m_created || !GetDocument())
		return;
	for(uint8 i = 0; i < NumStrips; i++)
	{
		const int value = std::clamp(Value(i), 0, m_strips[i].max);
		m_strips[i].slider.SetPos(m_strips[i].max - value);
		ShowValue(i, value);
	}
}


void CViewChips::UpdateView(UpdateHint hint, CObject *)
{
	if(!m_updating && hint.GetType()[HINT_MODGENERAL | HINT_MODTYPE])
		ReadSong();
}


// Когда сетку спрятали, вернуть её можно отсюда: на самой сетке щёлкать уже
// негде, а панель под ней остаётся.
void CViewChips::OnRButtonUp(UINT, CPoint point)
{
	CMenu menu;
	menu.CreatePopupMenu();
	const bool shown = TrackerSettings::Instance().chipsShowScopes;
	menu.AppendMenu(MF_STRING | (shown ? MF_CHECKED : 0), 1, _T("&Oscilloscopes"));
	CPoint screen = point;
	ClientToScreen(&screen);
	const UINT choice = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
		screen.x, screen.y, this);
	menu.DestroyMenu();
	if(choice == 1)
	{
		TrackerSettings::Instance().chipsShowScopes = !shown;
		LayoutStrips();
		Invalidate(FALSE);
	}
}


void CViewChips::OnSize(UINT type, int cx, int cy)
{
	CModScrollView::OnSize(type, cx, cy);
	LayoutStrips();
}


void CViewChips::OnVScroll(UINT code, UINT pos, CScrollBar *bar)
{
	if(bar && GetDocument())
	{
		for(uint8 i = 0; i < NumStrips; i++)
		{
			if(bar->m_hWnd != m_strips[i].slider.m_hWnd)
				continue;
			// Ход ползунка идёт сверху вниз, а громкость наоборот, поэтому
			// значение берётся от конца хода.
			const int value = std::clamp(m_strips[i].max - m_strips[i].slider.GetPos(), 0, m_strips[i].max);
			SetValue(i, value);
			ShowValue(i, value);
		}
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
