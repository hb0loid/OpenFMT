/*
 * Ctrl_chips.h
 * ------------
 * Purpose: Chips tab, upper panel.
 * Notes  : The tab is where the per-chip mixer lives, and where further
 *          chip-related gadgets are meant to go: unlike the other control
 *          bars, its lower view is drawn by code and has no layout ceiling.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */


#pragma once

#include "openmpt/all/BuildSettings.hpp"
#include "Globals.h"

OPENMPT_NAMESPACE_BEGIN

class CCtrlChips final : public CModControlDlg
{
public:
	CCtrlChips(CModControlView &parent, CModDoc &document);

	Setting<LONG> &GetSplitPosRef() override;
	BOOL OnInitDialog() override;
	CRuntimeClass *GetAssociatedViewClass() override;
	void RecalcLayout() override;
	void UpdateView(UpdateHint hint, CObject *pObj = nullptr) override;
	void OnActivatePage(LPARAM) override;
	void OnDeactivatePage() override;

protected:
	afx_msg void OnResetVolumes();
	DECLARE_MESSAGE_MAP()
};

OPENMPT_NAMESPACE_END
