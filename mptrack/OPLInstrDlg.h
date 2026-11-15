/*
 * OPLInstrDlg.h
 * -------------
 * Purpose: Editor for OPL-based synth instruments
 * Notes  : (currently none)
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#pragma once

#include "openmpt/all/BuildSettings.hpp"
#include "DialogBase.h"
#include <vector>
#include "../soundlib/Snd_defs.h"

OPENMPT_NAMESPACE_BEGIN

class CSoundFile;

class OPLInstrDlg : public DialogBase
{
	CButton m_additive, m_sustain[2], m_scaleEnv[2], m_vibrato[2], m_tremolo[2];
	CSliderCtrl m_feedback, m_attackRate[2], m_decayRate[2], m_sustainLevel[2], m_releaseRate[2], m_volume[2], m_levelScaling[2], m_freqMultiplier[2];
	CComboBox m_waveform[2];
	CSize m_windowSize;
	CWnd &m_parent;
	HWND m_lastFocusItem = nullptr;
	const CSoundFile &m_sndFile;
	OPLPatch *m_patch = nullptr;
	OPN2Patch *m_opn2Patch = nullptr;
	const bool m_isOPN2;
	// Размеры из ресурсов: по ним растягивается диалог YM2612, когда вкладке
	// достаётся больше места. Запоминаются при создании, потому что после первой
	// же раскладки считать было бы не от чего.
	CSize m_designed;
	std::map<int, CRect> m_designedRects;
	int m_minimumHeight = 0;

	void OPN2ParamsChanged();
	void LayoutOPN2();
	void LayoutOPL();
	// Десятый ряд оператора: SSG-EG у YM2612, Detune 2 у YM2151. Нарисованных
	// в ресурсах элементов для него нет, они создаются в OnInitDialog.
	CStatic m_extraLabel[4];
	CSliderCtrl m_extraSlider[4];
	CButton m_randomButton;
	// LFO: флажок AM у каждого оператора и строка над операторами —
	// чувствительность канала, а также общий LFO чипа, если инструмент его задаёт.
	CButton m_amCheck[4];
	CStatic m_lfoLabel[7];
	CSliderCtrl m_lfoSlider[7];
	CButton m_lfoSetChip;
	FMLfo *m_fmLfo = nullptr;
	bool m_isOPM = false;
	void UpdateLfoEnable();
	afx_msg void OnExtraClicked(UINT id);
	void CreateExtraControls();
	void Randomize();
	// Нарисованные в ресурсах положения всех элементов редактора OPL.
	std::vector<std::pair<HWND, CRect>> m_oplRects;

public:
	OPLInstrDlg(CWnd &parent, const CSoundFile &sndFile, bool opn2);
	~OPLInstrDlg();
	void SetEnabled(bool enabled);
	void SetPatch(OPLPatch &patch);
	void SetPatch(OPN2Patch &patch, bool opm, FMLfo &lfo);
	bool IsOPN2() const { return m_isOPN2; }
	CSize GetMinimumSize() const
	{
		return m_minimumHeight > 0 ? CSize(m_windowSize.cx, m_minimumHeight) : m_windowSize;
	}
	// Размер при масштабе 100%: компактный, как нарисован редактор OPL.
	CSize GetPreferredSize() const;

protected:
	void ParamsChanged();

	void DoDataExchange(CDataExchange *pDX) override;
	BOOL OnInitDialog() override;
	BOOL PreTranslateMessage(MSG *pMsg) override;
	CString GetToolTipText(UINT id, HWND hwnd) const override;
	void OnOK() override { }
	void OnCancel() override { }
	void OnHScroll(UINT, UINT, CScrollBar *) { ParamsChanged(); }
	afx_msg void OnSize(UINT type, int cx, int cy);
	LRESULT OnDragonDropping(WPARAM wParam, LPARAM lParam);
	DECLARE_MESSAGE_MAP()
};

OPENMPT_NAMESPACE_END
