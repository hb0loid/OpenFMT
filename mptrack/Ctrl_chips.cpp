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
END_MESSAGE_MAP()


CCtrlChips::CCtrlChips(CModControlView &parent, CModDoc &document)
	: CModControlDlg(parent, document)
{
}


// Верхняя панель этой вкладки пустая: держим её минимальной, чтобы громкости
// и осциллографы начинались сразу под ярлыками.
Setting<LONG> &CCtrlChips::GetSplitPosRef()
{
	auto &height = TrackerSettings::Instance().glChipsWindowHeight;
	height = 26;  // ровно под ярлыки вкладок (в пикселях при 100%)
	return height;
}


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

OPENMPT_NAMESPACE_END
