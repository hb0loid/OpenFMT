/*
 * PSGInstrDlg.h
 * -------------
 * Purpose: Editor for SN76489 (Sega PSG) synth instruments
 * Notes  : The chip is driven by macros rather than by an envelope, so the editor
 *          shows each macro as a plain sequence of values.
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#pragma once

#include "openmpt/all/BuildSettings.hpp"
#include "DialogBase.h"
#include "../soundlib/Snd_defs.h"

OPENMPT_NAMESPACE_BEGIN

class CSoundFile;

class PSGInstrDlg : public DialogBase
{
	CComboBox m_lane;
	CSize m_windowSize;
	CWnd &m_parent;
	HWND m_lastFocusItem = nullptr;
	const CSoundFile &m_sndFile;
	// Патч лежит в сэмпле как указатель на неизменяемый объект: его могут
	// делить несколько сэмплов, поэтому правка подменяет указатель целиком.
	std::shared_ptr<const PSGPatch> *m_patch = nullptr;
	// Пока поля заполняются из патча, их изменения не считаются правками
	// пользователя, иначе каждое открытие инструмента писало бы в undo.
	bool m_filling = false;

public:
	PSGInstrDlg(CWnd &parent, const CSoundFile &sndFile);
	~PSGInstrDlg();
	void SetEnabled(bool enabled);
	void SetPatch(std::shared_ptr<const PSGPatch> &patch);
	CSize GetMinimumSize() const { return m_windowSize; }

protected:
	void ParamsChanged();
	// Обработчики диапазона получают номер элемента, а правка всегда читает
	// все поля разом, поэтому номер здесь не нужен.
	void OnFieldChanged(UINT) { ParamsChanged(); }

	void DoDataExchange(CDataExchange *pDX) override;
	BOOL OnInitDialog() override;
	void OnOK() override { }
	void OnCancel() override { }
	DECLARE_MESSAGE_MAP()
};

OPENMPT_NAMESPACE_END
