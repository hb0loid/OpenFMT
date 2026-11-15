/*
 * OPLInstrDlg.cpp
 * ---------------
 * Purpose: Editor for OPL-based synth instruments
 * Notes  : (currently none)
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */

#include "stdafx.h"
#include <random>
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
	ON_COMMAND(2720, &OPLInstrDlg::Randomize)
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
constexpr int OPN2Rows = 9;   // параметров в операторе, нарисованных в ресурсах
constexpr int OPN2LayoutRows = OPN2Rows + 1;  // плюс SSG-EG / Detune 2
constexpr int ExtraLabelId = 2700, ExtraSliderId = 2710, RandomButtonId = 2720;
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
		CreateExtraControls();
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
		m_minimumHeight = m_designedRects[IDC_OPN2_GROUP1].top + OPN2LayoutRows * OPN2MinRow + 20;
		return TRUE;
	}
	CreateExtraControls();
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
	else
		LayoutOPL();
}


CSize OPLInstrDlg::GetPreferredSize() const
{
	if(!m_isOPN2 || m_designedRects.empty())
		return m_windowSize;
	// YM2612 при 100%: ползунок ~110 диалоговых единиц, ряд ~12 — рядом с
	// редактором OPL он выглядит соразмерно, а Total Level ещё различим.
	CRect unit(0, 0, 110, 12);
	const_cast<OPLInstrDlg *>(this)->MapDialogRect(unit);
	const CRect &group = m_designedRects.at(IDC_OPN2_GROUP1);
	const CRect &label = m_designedRects.at(IDC_OPN2_LABEL1);
	const int column = 5 + label.Width() + unit.Width() + 9;
	const int rowTop = label.top - group.top;
	return CSize(group.left * 2 + column * 4, group.top + rowTop + OPN2LayoutRows * std::max<int>(unit.Height(), OPN2MinRow) + 20);
}


// Редактор OPL нарисован в ресурсах фиксированным. Чтобы его можно было
// увеличить лупами, все элементы растягиваются пропорционально окну:
// положения и ширины — по горизонтали, положения — по вертикали (по центру
// элемента, высота та же), рамки групп — целиком.
void OPLInstrDlg::LayoutOPL()
{
	if(!::IsWindow(m_hWnd) || m_windowSize.cx <= 0 || m_windowSize.cy <= 0)
		return;
	if(m_oplRects.empty())
	{
		for(CWnd *child = GetWindow(GW_CHILD); child != nullptr; child = child->GetWindow(GW_HWNDNEXT))
		{
			CRect rect;
			child->GetWindowRect(rect);
			ScreenToClient(rect);
			m_oplRects.emplace_back(child->m_hWnd, rect);
		}
	}
	CRect client;
	GetClientRect(client);
	const double sx = std::max(1.0, static_cast<double>(client.Width()) / m_windowSize.cx);
	const double sy = std::max(1.0, static_cast<double>(client.Height()) / m_windowSize.cy);
	for(const auto &[hwnd, rect] : m_oplRects)
	{
		if(!::IsWindow(hwnd))
			continue;
		const bool groupBox = (::GetWindowLong(hwnd, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX && [&]
		{
			TCHAR cls[16]{};
			::GetClassName(hwnd, cls, 16);
			return !lstrcmpi(cls, _T("Button"));
		}();
		const int left = mpt::saturate_round<int>(rect.left * sx);
		const int width = mpt::saturate_round<int>(rect.Width() * sx);
		int top, height;
		if(groupBox)
		{
			top = mpt::saturate_round<int>(rect.top * sy);
			height = mpt::saturate_round<int>(rect.Height() * sy);
		} else
		{
			height = rect.Height();
			top = mpt::saturate_round<int>((rect.top + rect.Height() / 2.0) * sy - height / 2.0);
		}
		::SetWindowPos(hwnd, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
	}
	Invalidate(TRUE);
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
	const int labelWidth = label.Width();
	// Редактор заполняет своё окно, а размер окна задаёт вид сэмпла по
	// масштабу (лупы на панели): так и на полном экране размер выбирается.
	const int columnWidth = (client.Width() - margin * 2) / 4;
	const int usedWidth = columnWidth * 4;
	const int groupTop = group.top;
	const int rowTop = label.top - groupTop;
	const int available = client.Height() - groupTop - 6 - rowTop - 8;
	const int rowHeight = std::max(available / OPN2LayoutRows, OPN2MinRow);
	// Рамка кончается сразу под последним рядом, а не тянется до низа окна.
	const int groupHeight = rowTop + OPN2LayoutRows * rowHeight + 8;

	// Верхний ряд: алгоритм и обратная связь делят ширину пополам.
	// Ползунок алгоритма начинается после своей подписи и кончается перед
	// подписью обратной связи. Раньше его ширина считалась от края окна, и он
	// наезжал на подпись Feedback, так что её не было видно.
	const CRect &algSlider = m_designedRects[IDC_OPN2_FIRST];
	CRect buttonSize(0, 0, 44, 13);
	MapDialogRect(buttonSize);
	const int buttonLeft = margin + usedWidth - buttonSize.Width();
	const int algStart = m_designedRects[IDC_OPN2_ALGLABEL].right + 6;
	const int fbLabel = std::max(margin + usedWidth / 2 + 6, algStart + 46);
	place(IDC_OPN2_FIRST, algStart, algSlider.top, std::max(fbLabel - 12 - algStart, 40), algSlider.Height());
	place(IDC_OPN2_FBLABEL, fbLabel, m_designedRects[IDC_OPN2_FBLABEL].top,
		m_designedRects[IDC_OPN2_FBLABEL].Width(), m_designedRects[IDC_OPN2_FBLABEL].Height());
	const int fbStart = fbLabel + m_designedRects[IDC_OPN2_FBLABEL].Width() + 6;
	place(IDC_OPN2_FIRST + 1, fbStart, algSlider.top, std::max(buttonLeft - 10 - fbStart, 40), algSlider.Height());
	place(RandomButtonId, buttonLeft, algSlider.top - 1, buttonSize.Width(), buttonSize.Height());

	for(int op = 0; op < 4; op++)
	{
		const int left = margin + op * columnWidth;
		place(IDC_OPN2_GROUP1 + op, left, groupTop, columnWidth - 3, groupHeight);
		for(int row = 0; row < OPN2LayoutRows; row++)
		{
			const int y = groupTop + rowTop + row * rowHeight;
			const int sliderLeft = left + 5 + labelWidth;
			const bool extra = row == OPN2Rows;
			place(extra ? ExtraLabelId + op : IDC_OPN2_LABEL1 + op * OPN2Rows + row, left + 5, y, labelWidth, label.Height());
			place(extra ? ExtraSliderId + op : IDC_OPN2_FIRST + 2 + op * OPN2Rows + row, sliderLeft, y - 2,
				std::max(left + columnWidth - 9 - sliderLeft, 30), rowHeight - 4);
		}
	}
	Invalidate(TRUE);
}


void OPLInstrDlg::CreateExtraControls()
{
	CFont *font = GetFont();
	const CRect placeholder(0, 0, 10, 10);
	if(m_isOPN2)
	{
		for(int op = 0; op < 4; op++)
		{
			m_extraLabel[op].Create(_T("SSG-EG"), WS_CHILD | WS_VISIBLE | SS_LEFT, placeholder, this, ExtraLabelId + op);
			m_extraLabel[op].SetFont(font);
			m_extraSlider[op].Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_TOOLTIPS, placeholder, this, ExtraSliderId + op);
			m_extraSlider[op].SetRange(0, 8);
		}
	}
	// Кнопка случайного патча. У YM2612 её ставит раскладка, у OPL — сюда:
	// в просвет между флажком и обратной связью в верхней строке.
	CRect button(182, 5, 182 + 32, 5 + 13);
	MapDialogRect(button);
	m_randomButton.Create(_T("Random"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, button, this, RandomButtonId);
	m_randomButton.SetFont(font);
}


// Случайный, но почти всегда звучащий патч: несущие операторы громкие,
// модуляторы тише, атака быстрая, множители чаще маленькие.
void OPLInstrDlg::Randomize()
{
	static std::mt19937 rng{std::random_device{}()};
	const auto rnd = [](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
	const auto multiplier = [&]() { return rnd(0, 3) < 3 ? rnd(0, 4) : rnd(0, 15); };

	if(m_isOPN2 && m_opn2Patch)
	{
		// Операторы, которые идут на выход при каждом из восьми алгоритмов (бит 0 — оператор 1).
		static constexpr uint8 carriers[] = {0x08, 0x08, 0x08, 0x08, 0x0A, 0x0E, 0x0E, 0x0F};
		OPN2Patch patch = *m_opn2Patch;
		const int algorithm = rnd(0, 7);
		patch[0] = static_cast<uint8>(algorithm | (rnd(0, 6) << 3));
		for(int op = 0; op < 4; op++)
		{
			const int base = 1 + op * 7;
			const bool carrier = (carriers[algorithm] >> op) & 1;
			patch[base + 0] = static_cast<uint8>((rnd(0, 7) << 4) | multiplier());
			patch[base + 1] = static_cast<uint8>(carrier ? rnd(0, 18) : rnd(12, 60));
			patch[base + 2] = static_cast<uint8>((rnd(0, 3) << 6) | rnd(20, 31));
			patch[base + 3] = static_cast<uint8>(rnd(0, 20));
			patch[base + 4] = static_cast<uint8>(rnd(0, 12) | (m_isOPM && rnd(0, 3) == 0 ? rnd(1, 3) << 6 : 0));
			patch[base + 5] = static_cast<uint8>((rnd(0, 15) << 4) | rnd(3, 15));
			patch[base + 6] = static_cast<uint8>(!m_isOPM && rnd(0, 7) == 0 ? (8 | rnd(0, 7)) : 0);
		}
		m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_PREPAREUNDO);
		*m_opn2Patch = patch;
		SetPatch(*m_opn2Patch, m_isOPM);
		m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_SETMODIFIED, SampleHint().Data().AsLPARAM());
	} else if(!m_isOPN2 && m_patch)
	{
		OPLPatch patch{{}};
		const bool additive = rnd(0, 3) == 0;
		patch[10] = static_cast<uint8>((rnd(0, 6) << 1) | (additive ? OPL::CONNECTION_BIT : 0));
		for(int op = 0; op < 2; op++)
		{
			// Байт 0 — модулятор, 1 — несущий; в аддитивном режиме звучат оба.
			const bool audible = additive || op == 1;
			uint8 flags = static_cast<uint8>(multiplier());
			if(rnd(0, 1)) flags |= OPL::SUSTAIN_ON;
			if(rnd(0, 3) == 0) flags |= OPL::KSR;
			if(rnd(0, 5) == 0) flags |= OPL::VIBRATO_ON;
			if(rnd(0, 7) == 0) flags |= OPL::TREMOLO_ON;
			patch[op] = flags;
			patch[2 + op] = static_cast<uint8>((audible ? rnd(0, 10) : rnd(5, 45)) | (rnd(0, 3) << 6));
			patch[4 + op] = static_cast<uint8>((rnd(9, 15) << 4) | rnd(1, 15));
			patch[6 + op] = static_cast<uint8>((rnd(0, 15) << 4) | rnd(2, 12));
			patch[8 + op] = static_cast<uint8>(rnd(0, 3) == 0 ? rnd(1, 7) : 0);
		}
		m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_PREPAREUNDO);
		*m_patch = patch;
		SetPatch(*m_patch);
		m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_SETMODIFIED, SampleHint().Data().AsLPARAM());
	}
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
	m_isOPM = opm;
	ForEachOPN2Param([&](int id, OPN2Param param)
	{
		GetDlgItem(id)->SendMessage(TBM_SETPOS, TRUE, (patch[param.reg] >> param.shift) & param.mask);
	});
	for(int op = 0; op < 4; op++)
	{
		if(!m_extraSlider[op].m_hWnd)
			continue;
		if(opm)
		{
			// YM2151: Detune 2, два старших бита регистра D2R.
			m_extraLabel[op].SetWindowText(_T("Detune 2"));
			m_extraSlider[op].SetRange(0, 3, TRUE);
			m_extraSlider[op].SetPos(patch[1 + op * 7 + 4] >> 6);
		} else
		{
			// YM2612: SSG-EG. 0 — выключен, 1–8 — режимы 0–7 (с битом включения).
			const uint8 ssg = patch[1 + op * 7 + 6];
			m_extraLabel[op].SetWindowText(_T("SSG-EG"));
			m_extraSlider[op].SetRange(0, 8, TRUE);
			m_extraSlider[op].SetPos((ssg & 8) ? (ssg & 7) + 1 : 0);
		}
	}
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
	for(int op = 0; op < 4; op++)
	{
		if(!m_extraSlider[op].m_hWnd)
			continue;
		const int value = m_extraSlider[op].GetPos();
		if(m_isOPM)
			patch[1 + op * 7 + 4] = static_cast<uint8>((patch[1 + op * 7 + 4] & 0x3F) | ((value & 3) << 6));
		else
			patch[1 + op * 7 + 6] = static_cast<uint8>(value > 0 ? (8 | ((value - 1) & 7)) : 0);
	}
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
	if(id >= ExtraSliderId && id < ExtraSliderId + 4 && slider != nullptr)
	{
		const int pos = slider->GetPos();
		if(m_isOPM)
			return _T("Detune 2: ") + mpt::cfmt::val(pos);
		// Форма огибающей SSG-EG по режимам 0-7.
		static constexpr const TCHAR *shapes[] = {_T("\\\\ repeat"), _T("\\___ once"), _T("\\/\\/ alternate"), _T("\\--- hold"),
			_T("//// repeat"), _T("/___ once, then silent"), _T("/\\/\\ alternate"), _T("/--- hold")};
		if(pos == 0)
			return _T("SSG-EG off");
		return _T("SSG-EG mode ") + mpt::cfmt::val(pos - 1) + _T(": ") + CString(shapes[(pos - 1) & 7]);
	}
	if(id == RandomButtonId)
		return _T("Random patch (undo with Ctrl+Z)");
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
