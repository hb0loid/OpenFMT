/*
 * PSGInstrDlg.cpp
 * ---------------
 * Purpose: Editor for SN76489 (Sega PSG) synth instruments
 * Notes  : (currently none)
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#include "stdafx.h"
#include "PSGInstrDlg.h"
#include "resource.h"
#include "UpdateHints.h"
#include "WindowMessages.h"
#include "../soundlib/Sndfile.h"

OPENMPT_NAMESPACE_BEGIN

BEGIN_MESSAGE_MAP(PSGInstrDlg, DialogBase)
	ON_COMMAND(IDC_PSG_RESETNOISE, &PSGInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_PSG_EASYPITCH, &PSGInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_PSG_FURNACEPITCH, &PSGInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_PSG_PITCHREL, &PSGInstrDlg::ParamsChanged)
	ON_CBN_SELCHANGE(IDC_PSG_LANE, &PSGInstrDlg::ParamsChanged)
	ON_CONTROL_RANGE(EN_CHANGE, IDC_PSG_SEQ1, IDC_PSG_SEQ1 + PSGPatch::Count - 1, &PSGInstrDlg::OnFieldChanged)
	ON_CONTROL_RANGE(EN_CHANGE, IDC_PSG_SPEED1, IDC_PSG_SPEED1 + PSGPatch::Count - 1, &PSGInstrDlg::OnFieldChanged)
	ON_CONTROL_RANGE(EN_CHANGE, IDC_PSG_DELAY1, IDC_PSG_DELAY1 + PSGPatch::Count - 1, &PSGInstrDlg::OnFieldChanged)
END_MESSAGE_MAP()


namespace
{
// Макрос показывается строкой значений через пробел. `|` перед значением
// отмечает начало петли, `/` — точку release: так обе метки видны прямо в
// последовательности и их не нужно считать по номеру шага.
CString FormatSequence(const PSGSequence &seq)
{
	CString text;
	for(size_t i = 0; i < seq.values.size(); i++)
	{
		if(!text.IsEmpty())
			text += _T(' ');
		if(i == seq.loop)
			text += _T("| ");
		if(i == seq.release)
			text += _T("/ ");
		text += mpt::cfmt::val(seq.values[i]);
	}
	return text;
}


void ParseSequence(const CString &text, PSGSequence &seq)
{
	seq.values.clear();
	seq.loop = seq.release = 255;
	int pos = 0;
	bool loopNext = false, releaseNext = false;
	while(pos >= 0 && pos < text.GetLength())
	{
		const CString token = text.Tokenize(_T(" \t,;"), pos);
		if(token.IsEmpty())
			continue;
		if(token == _T("|"))
		{
			loopNext = true;
			continue;
		}
		if(token == _T("/"))
		{
			releaseNext = true;
			continue;
		}
		if(seq.values.size() >= 255)
			break;
		if(loopNext)
		{
			seq.loop = static_cast<uint8>(seq.values.size());
			loopNext = false;
		}
		if(releaseNext)
		{
			seq.release = static_cast<uint8>(seq.values.size());
			releaseNext = false;
		}
		seq.values.push_back(static_cast<int32>(_ttoi(token.GetString())));
	}
}
} // namespace


void PSGInstrDlg::DoDataExchange(CDataExchange *pDX)
{
	DialogBase::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_PSG_LANE, m_lane);
}


PSGInstrDlg::PSGInstrDlg(CWnd &parent, const CSoundFile &sndFile)
	: m_parent(parent)
	, m_sndFile(sndFile)
{
	Create(IDD_PSG_PARAMS, &parent);
	CRect rect;
	GetClientRect(rect);
	m_windowSize = rect.BottomRight();
}


PSGInstrDlg::~PSGInstrDlg()
{
	DestroyWindow();
}


BOOL PSGInstrDlg::OnInitDialog()
{
	DialogBase::OnInitDialog();
	static constexpr const TCHAR *lanes[] = {_T("Tone 1"), _T("Tone 2"), _T("Tone 3"), _T("Noise")};
	for(const auto *name : lanes)
		m_lane.AddString(name);
	m_lane.SetCurSel(0);
	return TRUE;
}


void PSGInstrDlg::SetEnabled(bool enabled)
{
	if(!enabled)
		m_lastFocusItem = ::GetFocus();

	const auto EnableProc = [](HWND hwnd, LPARAM lParam) -> BOOL
	{
		::EnableWindow(hwnd, lParam ? TRUE : FALSE);
		return TRUE;
	};
	EnumChildWindows(m_hWnd, EnableProc, enabled);
	ShowWindow(enabled ? SW_SHOW : SW_HIDE);

	if(enabled && !IsChild(GetFocus()))
	{
		if(::IsChild(m_hWnd, m_lastFocusItem))
			::SetFocus(m_lastFocusItem);
		else
			NextDlgCtrl();
	}
}


void PSGInstrDlg::SetPatch(std::shared_ptr<const PSGPatch> &holder)
{
	if(!holder)
		return;
	SetRedraw(FALSE);
	m_filling = true;
	m_patch = nullptr;
	const PSGPatch &patch = *holder;

	m_lane.SetCurSel(std::min<int>(patch.lane, 3));
	CheckDlgButton(IDC_PSG_RESETNOISE, patch.resetNoise ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_PSG_EASYPITCH, patch.easyPitch ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_PSG_FURNACEPITCH, patch.furnacePitch ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_PSG_PITCHREL, patch.macros[PSGPatch::Pitch].relative ? BST_CHECKED : BST_UNCHECKED);
	for(int i = 0; i < PSGPatch::Count; i++)
	{
		const auto &seq = patch.macros[i];
		SetDlgItemText(IDC_PSG_SEQ1 + i, FormatSequence(seq));
		SetDlgItemInt(IDC_PSG_SPEED1 + i, seq.speed, FALSE);
		SetDlgItemInt(IDC_PSG_DELAY1 + i, seq.delay, FALSE);
	}

	m_filling = false;
	m_patch = &holder;
	SetRedraw(TRUE);
	Invalidate(FALSE);
}


void PSGInstrDlg::ParamsChanged()
{
	if(m_filling || m_patch == nullptr || !*m_patch)
		return;

	const PSGPatch &current = **m_patch;
	PSGPatch patch = current;
	patch.lane = static_cast<uint8>(std::max(0, m_lane.GetCurSel()));
	patch.resetNoise = IsDlgButtonChecked(IDC_PSG_RESETNOISE) != BST_UNCHECKED;
	patch.easyPitch = IsDlgButtonChecked(IDC_PSG_EASYPITCH) != BST_UNCHECKED;
	patch.furnacePitch = IsDlgButtonChecked(IDC_PSG_FURNACEPITCH) != BST_UNCHECKED;
	for(int i = 0; i < PSGPatch::Count; i++)
	{
		CString text;
		GetDlgItemText(IDC_PSG_SEQ1 + i, text);
		auto &seq = patch.macros[i];
		const bool relative = seq.relative, instantRelease = seq.instantRelease;
		ParseSequence(text, seq);
		seq.relative = relative;
		seq.instantRelease = instantRelease;
		seq.speed = static_cast<uint8>(std::clamp<UINT>(GetDlgItemInt(IDC_PSG_SPEED1 + i, nullptr, FALSE), 1, 255));
		seq.delay = static_cast<uint8>(std::min<UINT>(GetDlgItemInt(IDC_PSG_DELAY1 + i, nullptr, FALSE), 255));
	}
	patch.macros[PSGPatch::Pitch].relative = IsDlgButtonChecked(IDC_PSG_PITCHREL) != BST_UNCHECKED;

	if(patch.lane != current.lane
		|| patch.resetNoise != current.resetNoise
		|| patch.easyPitch != current.easyPitch
		|| patch.furnacePitch != current.furnacePitch
		|| !std::equal(patch.macros.begin(), patch.macros.end(), current.macros.begin(),
			[](const PSGSequence &a, const PSGSequence &b)
			{
				return a.values == b.values && a.loop == b.loop && a.release == b.release
					&& a.speed == b.speed && a.delay == b.delay && a.relative == b.relative
					&& a.instantRelease == b.instantRelease;
			}))
	{
		m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_PREPAREUNDO);
		*m_patch = std::make_shared<const PSGPatch>(std::move(patch));
		m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_SETMODIFIED, SampleHint().Data().AsLPARAM());
	}
}

OPENMPT_NAMESPACE_END
