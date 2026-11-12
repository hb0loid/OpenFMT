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
#include "PSGMacroCtrl.h"
#include "../soundlib/Snd_defs.h"

OPENMPT_NAMESPACE_BEGIN

class CSoundFile;

class PSGInstrDlg : public DialogBase
{
	CComboBox m_lane;
	// Каждый макрос рисуется своим графиком: строка чисел в поле не читается
	// уже на десятке шагов, а макросы бывают и длиннее.
	std::array<PSGMacroCtrl, PSGPatch::Count> m_graphs;
	// Правки графиков идут в эту копию, из неё собирается патч.
	PSGPatch m_working;
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
	// Пока правка расходится по документу, вид успевает обновиться и позвать
	// SetPatch обратно. Без этого флага поля переписывались прямо во время
	// набора, курсор прыгал в начало, и цифры перемешивались.
	bool m_applying = false;

public:
	PSGInstrDlg(CWnd &parent, const CSoundFile &sndFile);
	~PSGInstrDlg();
	void SetEnabled(bool enabled);
	void SetPatch(std::shared_ptr<const PSGPatch> &patch);
	// Колесо мыши перебирает то число, над которым стоит курсор: в полях
	// скорости и задержки само значение, а в строке макроса — то, на которое
	// указывает курсор. Вызывается и своим обработчиком, и видом сэмпла, когда
	// курсор над диалогом; пересылать сообщение туда нельзя — необработанное
	// колесо диалог вернёт родителю, и получится петля.
	bool WheelStep(short delta, CPoint screenPoint);
	CSize GetMinimumSize() const { return m_windowSize; }

protected:
	void ParamsChanged();
	// Обработчики диапазона получают номер элемента, а правка всегда читает
	// все поля разом, поэтому номер здесь не нужен.
	void OnFieldChanged(UINT) { ParamsChanged(); }

	BOOL OnMouseWheel(UINT flags, short delta, CPoint point);
	bool WheelOnSequence(int id, int macro, int step, CPoint screenPoint);

	void DoDataExchange(CDataExchange *pDX) override;
	BOOL OnInitDialog() override;
	void OnOK() override { }
	void OnCancel() override { }
	DECLARE_MESSAGE_MAP()
};

OPENMPT_NAMESPACE_END
