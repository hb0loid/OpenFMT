/*
 * OPLInstrDlg.cpp
 * ---------------
 * Purpose: Editor for OPL-based synth instruments
 * Notes  : (currently none)
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#include "stdafx.h"
#include "OPLInstrDlg.h"
#include "resource.h"
#include "UpdateHints.h"
#include "WindowMessages.h"
#include "../soundlib/OPL.h"
#include "../soundlib/Sndfile.h"

OPENMPT_NAMESPACE_BEGIN

BEGIN_MESSAGE_MAP(OPLInstrDlg, DialogBase)
	ON_WM_HSCROLL()
	ON_WM_SIZE()
	ON_MESSAGE(WM_MOD_DRAGONDROPPING, &OPLInstrDlg::OnDragonDropping)
	ON_COMMAND(IDC_CHECK1, &OPLInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_CHECK2, &OPLInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_CHECK3, &OPLInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_CHECK4, &OPLInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_CHECK5, &OPLInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_CHECK6, &OPLInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_CHECK7, &OPLInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_CHECK8, &OPLInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_CHECK9, &OPLInstrDlg::ParamsChanged)
	ON_CBN_SELCHANGE(IDC_COMBO1, &OPLInstrDlg::ParamsChanged)
	ON_CBN_SELCHANGE(IDC_COMBO2, &OPLInstrDlg::ParamsChanged)
END_MESSAGE_MAP()


void OPLInstrDlg::DoDataExchange(CDataExchange *pDX)
{
	DialogBase::DoDataExchange(pDX);
	if(m_isOPN2)
		return;
	DDX_Control(pDX, IDC_CHECK1, m_additive);
	DDX_Control(pDX, IDC_SLIDER1, m_feedback);

	for(int op = 0; op < 2; op++)
	{
		const int slider = op * 7;
		const int check = op * 4;
		DDX_Control(pDX, IDC_SLIDER2 + slider, m_attackRate[op]);
		DDX_Control(pDX, IDC_SLIDER3 + slider, m_decayRate[op]);
		DDX_Control(pDX, IDC_SLIDER4 + slider, m_sustainLevel[op]);
		DDX_Control(pDX, IDC_SLIDER5 + slider, m_releaseRate[op]);
		DDX_Control(pDX, IDC_CHECK2  + check,  m_sustain[op]);
		DDX_Control(pDX, IDC_SLIDER6 + slider, m_volume[op]);
		DDX_Control(pDX, IDC_CHECK3  + check,  m_scaleEnv[op]);
		DDX_Control(pDX, IDC_SLIDER7 + slider, m_levelScaling[op]);
		DDX_Control(pDX, IDC_SLIDER8 + slider, m_freqMultiplier[op]);
		DDX_Control(pDX, IDC_COMBO1  + op,     m_waveform[op]);
		DDX_Control(pDX, IDC_CHECK4  + check,  m_vibrato[op]);
		DDX_Control(pDX, IDC_CHECK5  + check,  m_tremolo[op]);
	}
}


OPLInstrDlg::OPLInstrDlg(CWnd &parent, const CSoundFile &sndFile, bool opn2)
	: m_parent(parent)
	, m_sndFile(sndFile)
	, m_isOPN2(opn2)
{
	Create(opn2 ? IDD_OPN2_PARAMS : IDD_OPL_PARAMS, &parent);
	CRect rect;
	GetClientRect(rect);
	m_windowSize = rect.BottomRight();
}


OPLInstrDlg::~OPLInstrDlg()
{
	DestroyWindow();
}


namespace
{
struct OPN2Param
{
	uint8 reg, shift, mask;
};

constexpr OPN2Param Algorithm{0, 0, 7}, Feedback{0, 3, 7};
constexpr int OPN2Rows = 9;   // параметров в операторе
constexpr int OPN2MinRow = 16;
// Выше этого ряды разъезжаются впустую: на развёрнутом окне между слайдерами
// получались пустые полосы в полсотни пикселей. Лишнюю высоту лучше оставить
// пустой под рамками.
constexpr int OPN2MaxRow = 36;
constexpr OPN2Param OperatorParams[] = {
	{0, 0, 15}, {0, 4, 7}, {1, 0, 127}, {2, 6, 3}, {2, 0, 31},
	{3, 0, 31}, {4, 0, 31}, {5, 4, 15}, {5, 0, 15},
};

template<typename Callback>
void ForEachOPN2Param(Callback callback)
{
	int id = IDC_OPN2_FIRST;
	callback(id++, Algorithm);
	callback(id++, Feedback);
	for(uint8 op = 0; op < 4; op++)
	{
		for(auto param : OperatorParams)
		{
			param.reg += 1 + op * 7;
			callback(id++, param);
		}
	}
}
} // namespace


BOOL OPLInstrDlg::OnInitDialog()
{
	DialogBase::OnInitDialog();
	if(m_isOPN2)
	{
		ForEachOPN2Param([&](int id, OPN2Param param)
		{
			GetDlgItem(id)->SendMessage(TBM_SETRANGEMAX, TRUE, param.mask);
		});

		CRect client;
		GetClientRect(client);
		m_designed = client.Size();
		const auto remember = [this](int id)
		{
			if(const CWnd *wnd = GetDlgItem(id))
			{
				CRect rect;
				wnd->GetWindowRect(rect);
				ScreenToClient(rect);
				m_designedRects[id] = rect;
			}
		};
		remember(IDC_OPN2_ALGLABEL);
		remember(IDC_OPN2_FBLABEL);
		remember(IDC_OPN2_FIRST);
		remember(IDC_OPN2_FIRST + 1);
		for(int op = 0; op < 4; op++)
		{
			remember(IDC_OPN2_GROUP1 + op);
			for(int row = 0; row < OPN2Rows; row++)
			{
				remember(IDC_OPN2_LABEL1 + op * OPN2Rows + row);
				remember(IDC_OPN2_FIRST + 2 + op * OPN2Rows + row);
			}
		}
		// Минимум нарочно меньше нарисованного: вкладке сэмплов часто достаётся
		// полоса ниже диалога, и тогда нижние ряды уезжали бы за край.
		m_minimumHeight = m_designedRects[IDC_OPN2_GROUP1].top + OPN2Rows * OPN2MinRow + 20;
		return TRUE;
	}
	m_feedback.SetRange(0, 7);
	for(int op = 0; op < 2; op++)
	{
		m_attackRate[op].SetRange(0, 15);
		m_decayRate[op].SetRange(0, 15);
		m_sustainLevel[op].SetRange(0, 15);
		m_releaseRate[op].SetRange(0, 15);
		m_volume[op].SetRange(0, 63);
		m_volume[op].SetTicFreq(4);
		m_levelScaling[op].SetRange(0, 3);
		m_freqMultiplier[op].SetRange(0, 15);
	}

	return TRUE;
}


void OPLInstrDlg::OnSize(UINT type, int cx, int cy)
{
	DialogBase::OnSize(type, cx, cy);
	if(m_isOPN2)
		LayoutOPN2();
}


// Крутилки нарисованы под фиксированный размер и не пользовались свободным
// местом: на широкой вкладке слайдер в 55 единиц это полсотни пикселей на
// 127 шагов Total Level. Теперь колонки операторов делят всю ширину, а ряды
// всю высоту.
void OPLInstrDlg::LayoutOPN2()
{
	if(!::IsWindow(m_hWnd) || m_designedRects.empty())
		return;
	CRect client;
	GetClientRect(client);
	if(client.Width() < 240 || client.Height() < 80)
		return;

	const auto place = [&](int id, int x, int y, int width, int height)
	{
		if(CWnd *wnd = GetDlgItem(id))
			wnd->SetWindowPos(nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
	};
	const CRect &group = m_designedRects[IDC_OPN2_GROUP1];
	const CRect &label = m_designedRects[IDC_OPN2_LABEL1];
	const int margin = group.left;
	const int columnWidth = (client.Width() - margin * 2) / 4;
	const int groupTop = group.top;
	const int labelWidth = label.Width();
	const int rowTop = label.top - groupTop;
	const int available = client.Height() - groupTop - 6 - rowTop - 8;
	const int rowHeight = std::clamp(available / OPN2Rows, OPN2MinRow, OPN2MaxRow);
	// Рамка кончается сразу под последним рядом, а не тянется до низа окна.
	const int groupHeight = rowTop + OPN2Rows * rowHeight + 8;

	// Верхний ряд: алгоритм и обратная связь делят ширину пополам.
	const int topSlider = (client.Width() - margin * 2) / 2 - m_designedRects[IDC_OPN2_ALGLABEL].Width() - 12;
	const CRect &algSlider = m_designedRects[IDC_OPN2_FIRST];
	place(IDC_OPN2_FIRST, m_designedRects[IDC_OPN2_ALGLABEL].right + 6, algSlider.top,
		std::max(topSlider, 40), algSlider.Height());
	const int fbLabel = margin + (client.Width() - margin * 2) / 2 + 6;
	place(IDC_OPN2_FBLABEL, fbLabel, m_designedRects[IDC_OPN2_FBLABEL].top,
		m_designedRects[IDC_OPN2_FBLABEL].Width(), m_designedRects[IDC_OPN2_FBLABEL].Height());
	place(IDC_OPN2_FIRST + 1, fbLabel + m_designedRects[IDC_OPN2_FBLABEL].Width() + 6, algSlider.top,
		std::max(topSlider, 40), algSlider.Height());

	for(int op = 0; op < 4; op++)
	{
		const int left = margin + op * columnWidth;
		place(IDC_OPN2_GROUP1 + op, left, groupTop, columnWidth - 3, groupHeight);
		for(int row = 0; row < OPN2Rows; row++)
		{
			const int y = groupTop + rowTop + row * rowHeight;
			const int sliderLeft = left + 5 + labelWidth;
			place(IDC_OPN2_LABEL1 + op * OPN2Rows + row, left + 5, y, labelWidth, label.Height());
			place(IDC_OPN2_FIRST + 2 + op * OPN2Rows + row, sliderLeft, y - 2,
				std::max(left + columnWidth - 9 - sliderLeft, 30), rowHeight - 4);
		}
	}
	Invalidate(TRUE);
}


BOOL OPLInstrDlg::PreTranslateMessage(MSG *pMsg)
{
	if(pMsg)
	{
		// Forward key presses and drag&drop support to parent editor
		if(pMsg->message == WM_SYSKEYUP || pMsg->message == WM_KEYUP ||
			pMsg->message == WM_SYSKEYDOWN || pMsg->message == WM_KEYDOWN ||
			pMsg->message == WM_DROPFILES || pMsg->message == WM_XBUTTONUP)
		{
			if(pMsg->hwnd == m_hWnd)
			{
				pMsg->hwnd = m_parent.m_hWnd;
			}
			if(m_parent.PreTranslateMessage(pMsg))
			{
				return TRUE;
			}
		} else if(pMsg->message == WM_CHAR)
		{
			// Avoid Windows sounds on note key repeats
			if(HIWORD(pMsg->lParam) & 0x4000)
				return TRUE;
		}
	}
	return DialogBase::PreTranslateMessage(pMsg);
}


LRESULT OPLInstrDlg::OnDragonDropping(WPARAM wParam, LPARAM lParam)
{
	return m_parent.SendMessage(WM_MOD_DRAGONDROPPING, wParam, lParam);
}


// Swap OPL Key Scale Level bits for a "human-readable" value.
static uint8 KeyScaleLevel(uint8 kslVolume)
{
	static constexpr uint8 KSLFix[4] = { 0x00, 0x80, 0x40, 0xC0 };
	return KSLFix[kslVolume >> 6];
}


void OPLInstrDlg::SetEnabled(bool enabled)
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


void OPLInstrDlg::SetPatch(OPLPatch &patch)
{
	SetRedraw(FALSE);

	m_additive.SetCheck((patch[10] & OPL::CONNECTION_BIT) ? BST_CHECKED : BST_UNCHECKED);
	m_feedback.SetPos((patch[10] & OPL::FEEDBACK_MASK) >> 1);
	for(int op = 0; op < 2; op++)
	{
		m_attackRate[op].SetPos(15 - (patch[4 + op] >> 4));
		m_decayRate[op].SetPos(15 - (patch[4 + op] & 0x0F));
		m_sustainLevel[op].SetPos(15 - (patch[6 + op] >> 4));
		m_releaseRate[op].SetPos(15 - (patch[6 + op] & 0x0F));
		m_volume[op].SetPos(63 - (patch[2 + op] & OPL::TOTAL_LEVEL_MASK));
		m_levelScaling[op].SetPos(KeyScaleLevel(patch[2 + op]) >> 6);
		m_freqMultiplier[op].SetPos(patch[0 + op] & OPL::MULTIPLE_MASK);

		m_sustain[op].SetCheck((patch[0 + op] & OPL::SUSTAIN_ON) ? BST_CHECKED : BST_UNCHECKED);
		m_scaleEnv[op].SetCheck((patch[0 + op] & OPL::KSR) ? BST_CHECKED : BST_UNCHECKED);
		m_vibrato[op].SetCheck((patch[0 + op] & OPL::VIBRATO_ON) ? BST_CHECKED : BST_UNCHECKED);
		m_tremolo[op].SetCheck((patch[0 + op] & OPL::TREMOLO_ON) ? BST_CHECKED : BST_UNCHECKED);

		const auto waveform = patch[8 + op];
		const int numWaveforms = (m_sndFile.GetType() == MOD_TYPE_S3M && waveform < 4) ? 4 : 8;
		if(numWaveforms != m_waveform[op].GetCount())
		{
			m_waveform[op].ResetContent();
			static constexpr const TCHAR *waveformNames[] =
			{
				_T("Sine"), _T("Half Sine"), _T("Absolute Sine"), _T("Pulse Sine"),
				_T("Sine (Even Periods)"), _T("Absolute Sine (Even Periods)"), _T("Square"), _T("Derived Square")
			};
			for(int i = 0; i < numWaveforms; i++)
			{
				m_waveform[op].AddString(waveformNames[i]);
			}
		}
		m_waveform[op].SetCurSel(waveform);
	}
	SetRedraw(TRUE);
	m_patch = &patch;
}


void OPLInstrDlg::SetPatch(OPN2Patch &patch, bool opm)
{
	SetDlgItemText(2558, opm ? _T("YM2151 / OPM") : _T("YM2612 / OPN2"));
	ForEachOPN2Param([&](int id, OPN2Param param)
	{
		GetDlgItem(id)->SendMessage(TBM_SETPOS, TRUE, (patch[param.reg] >> param.shift) & param.mask);
	});
	m_opn2Patch = &patch;
}


void OPLInstrDlg::OPN2ParamsChanged()
{
	if(!m_opn2Patch)
		return;
	OPN2Patch patch = *m_opn2Patch;
	ForEachOPN2Param([&](int id, OPN2Param param)
	{
		const auto value = GetDlgItem(id)->SendMessage(TBM_GETPOS);
		patch[param.reg] = static_cast<uint8>((patch[param.reg] & ~(param.mask << param.shift)) | (value << param.shift));
	});
	if(patch != *m_opn2Patch)
	{
		m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_PREPAREUNDO);
		*m_opn2Patch = patch;
		m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_SETMODIFIED, SampleHint().Data().AsLPARAM());
	}
}


void OPLInstrDlg::ParamsChanged()
{
	if(m_isOPN2)
	{
		OPN2ParamsChanged();
		return;
	}
	OPLPatch patch{{}};
	if(m_additive.GetCheck() != BST_UNCHECKED) patch[10] |= OPL::CONNECTION_BIT;
	patch[10] |= static_cast<uint8>(m_feedback.GetPos() << 1);
	for(int op = 0; op < 2; op++)
	{
		patch[op] = static_cast<uint8>(m_freqMultiplier[op].GetPos());
		if(m_sustain[op].GetCheck() != BST_UNCHECKED) patch[op] |= OPL::SUSTAIN_ON;
		if(m_scaleEnv[op].GetCheck() != BST_UNCHECKED) patch[op] |= OPL::KSR;
		if(m_vibrato[op].GetCheck() != BST_UNCHECKED) patch[op] |= OPL::VIBRATO_ON;
		if(m_tremolo[op].GetCheck() != BST_UNCHECKED) patch[op] |= OPL::TREMOLO_ON;
		patch[2 + op] = static_cast<uint8>((63 - m_volume[op].GetPos()) | KeyScaleLevel(static_cast<uint8>(m_levelScaling[op].GetPos() << 6)));
		patch[4 + op] = static_cast<uint8>(((15 - m_attackRate[op].GetPos()) << 4) | (15 - m_decayRate[op].GetPos()));
		patch[6 + op] = static_cast<uint8>(((15 - m_sustainLevel[op].GetPos()) << 4) | (15 - m_releaseRate[op].GetPos()));
		patch[8 + op] = static_cast<uint8>(m_waveform[op].GetCurSel());
	}

	if(*m_patch != patch)
	{
		m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_PREPAREUNDO);
		*m_patch = patch;
		m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_SETMODIFIED, SampleHint().Data().AsLPARAM());
	}
}


CString OPLInstrDlg::GetToolTipText(UINT id, HWND) const
{
	static constexpr const char *feedback[] = {"disabled", "\xCF\x80/16", "\xCF\x80/8", "\xCF\x80/4", "\xCF\x80/2", "\xCF\x80", "2\xCF\x80", "4\xCF\x80"};
	static constexpr const TCHAR *ksl[] = {_T("disabled"), _T("1.5 dB / octave"), _T("3 dB / octave"), _T("6 dB / octave")};

	CString text;
	const CWnd *wnd = GetDlgItem(static_cast<int>(id));
	const CSliderCtrl *slider = static_cast<const CSliderCtrl *>(wnd);
	switch(id)
	{
	case IDC_SLIDER1:
		// Feedback
		text = mpt::ToCString(mpt::Charset::UTF8, feedback[slider->GetPos() & 7]);
		break;

	case IDC_SLIDER2:
	case IDC_SLIDER3:
	case IDC_SLIDER5:
	case IDC_SLIDER9:
	case IDC_SLIDER10:
	case IDC_SLIDER12:
		// Attack / Decay / Release
		text = _T("faster < ") + mpt::cfmt::val(slider->GetPos()) + _T(" > slower");
		break;
	case IDC_SLIDER4:
	case IDC_SLIDER11:
		// Sustain Level
		{
			const int pos = slider->GetPos();
			text = mpt::cfmt::val((pos == 0) ? -93 : ((-15 + pos) * 3)) + _T(" dB");
		}
		break;
	case IDC_SLIDER6:
	case IDC_SLIDER13:
		// Volume Level
		text = mpt::cfmt::fix((-63 + slider->GetPos()) * 0.75, 2) + _T(" dB");
		break;
	case IDC_SLIDER7:
	case IDC_SLIDER14:
		// Key Scale Level
		text = ksl[slider->GetPos() & 3];
		break;
	case IDC_SLIDER8:
	case IDC_SLIDER15:
		// Frequency Multiplier
		if(slider->GetPos() == 0)
			text = _T("0.5");
		else
			text = mpt::cfmt::val(slider->GetPos());
		break;
	}

	return text;
}

OPENMPT_NAMESPACE_END
