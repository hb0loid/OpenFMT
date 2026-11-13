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

class CViewChips;

// Ползунок, который по правой кнопке возвращается к исходному: щелчок мимо
// него всё равно попадает в него же, поэтому сбрасывать удобнее всего так.
class ChipSlider final : public CSliderCtrl
{
public:
	CViewChips *owner = nullptr;
	uint8 index = 0;

protected:
	afx_msg void OnRButtonUp(UINT flags, CPoint point);
	DECLARE_MESSAGE_MAP()
};


class CViewChips : public CModScrollView
{
public:
	CViewChips() = default;
	DECLARE_SERIAL(CViewChips)

	// Полоски: по одной на микросхему и одна на обычные сэмплы, чтобы весь
	// баланс трека стоял рядом, а не половина здесь, половина в General.
	static constexpr uint8 SampleStrip = ChipKinds;
	static constexpr uint8 NumStrips = ChipKinds + 1;

	void Reset(uint8 strip);

protected:
	struct Strip
	{
		CStatic name;
		ChipSlider slider;
		CStatic value;
		int max = 0;
		int def = 0;
	};
	std::array<Strip, NumStrips> m_strips;
	bool m_created = false;
	// Пока ползунок под пальцем, подпись ведётся от него, а не от песни: иначе
	// подхват собственной правки дёргал бы цифру.
	bool m_updating = false;

	static const TCHAR *StripName(uint8 strip);
	int Value(uint8 strip) const;
	void SetValue(uint8 strip, int value);
	void CreateStrips();
	void LayoutStrips();
	void ShowValue(uint8 strip, int value);
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
