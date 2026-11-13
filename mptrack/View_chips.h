/*
 * View_chips.h
 * ------------
 * Purpose: Chips tab, lower panel: a mixer with one strip per chip.
 * Notes  : The strips are laid out by code rather than by a dialog resource,
 *          so the panel does not run out of room as more chips are added, and
 *          a new gadget only needs a place in the layout.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */


#pragma once

#include "openmpt/all/BuildSettings.hpp"
#include "Globals.h"
#include "../soundlib/Snd_defs.h"
#include <array>

OPENMPT_NAMESPACE_BEGIN

class CViewChips : public CModScrollView
{
public:
	CViewChips() = default;
	DECLARE_SERIAL(CViewChips)

protected:
	// Ползунок берёт целые единицы 8.8, то есть ход от тишины до вдвое громче
	// исходного. Единица стоит ровно посередине хода.
	static constexpr int MaxVolume = ChipVolumeUnity * 2;

	struct Strip
	{
		CStatic name;
		CSliderCtrl slider;
		CStatic value;
	};
	std::array<Strip, ChipKinds> m_strips;
	bool m_created = false;
	// Пока ползунок двигают, подпись обновляется от него, а не от песни:
	// иначе округление единиц дёргало бы цифру под пальцем.
	bool m_updating = false;

	static const TCHAR *ChipName(ChipKind chip);
	void CreateStrips();
	void LayoutStrips();
	void ShowValue(ChipKind chip, int volume);
	void ReadSong();

public:
	void OnInitialUpdate() override;
	void UpdateView(UpdateHint hint, CObject *pObject = nullptr) override;

protected:
	afx_msg void OnSize(UINT type, int cx, int cy);
	afx_msg void OnVScroll(UINT code, UINT pos, CScrollBar *bar);
	afx_msg BOOL OnEraseBkgnd(CDC *dc);
	DECLARE_MESSAGE_MAP()
};

OPENMPT_NAMESPACE_END
