/*
 * Ctrl_chips.cpp
 * --------------
 * Purpose: Chips tab, upper panel.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#include "stdafx.h"
#include "Ctrl_chips.h"
#include "Mainfrm.h"
#include "Moddoc.h"
#include "resource.h"
#include "TrackerSettings.h"
#include "View_chips.h"

OPENMPT_NAMESPACE_BEGIN

BEGIN_MESSAGE_MAP(CCtrlChips, CModControlDlg)
	ON_COMMAND(IDC_BUTTON_CHIPRESET, &CCtrlChips::OnResetVolumes)
END_MESSAGE_MAP()


CCtrlChips::CCtrlChips(CModControlView &parent, CModDoc &document)
	: CModControlDlg(parent, document)
{
}


Setting<LONG> &CCtrlChips::GetSplitPosRef() { return TrackerSettings::Instance().glChipsWindowHeight; }


CRuntimeClass *CCtrlChips::GetAssociatedViewClass() { return RUNTIME_CLASS(CViewChips); }


BOOL CCtrlChips::OnInitDialog()
{
	CModControlDlg::OnInitDialog();
	return FALSE;
}


// Полоска у этой вкладки одна на все размеры: кнопка и подпись стоят на месте,
// а всё, что растягивается, живёт во вьюшке под ней.
void CCtrlChips::RecalcLayout() { }


void CCtrlChips::UpdateView(UpdateHint, CObject *) { }


void CCtrlChips::OnActivatePage(LPARAM)
{
	// Индикатор уровня продолжает работать: вкладка про громкость, и смотреть
	// на неё без уровня бессмысленно.
	m_modDoc.SetNotifications(Notification::Default);
	m_modDoc.SetFollowWnd(m_hWnd);
	CMainFrame::GetMainFrame()->SetInfoText(_T(""));
}


void CCtrlChips::OnDeactivatePage()
{
	m_modDoc.SetFollowWnd(nullptr);
}


void CCtrlChips::OnResetVolumes()
{
	CSoundFile &sndFile = m_modDoc.GetSoundFile();
	bool changed = false;
	for(auto &volume : sndFile.m_chipVolume)
	{
		if(volume != ChipVolumeUnity)
		{
			volume = ChipVolumeUnity;
			changed = true;
		}
	}
	if(changed)
	{
		m_modDoc.SetModified();
		m_modDoc.UpdateAllViews(nullptr, GeneralHint().General());
	}
}

OPENMPT_NAMESPACE_END
