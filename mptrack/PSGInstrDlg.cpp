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
#include <random>

OPENMPT_NAMESPACE_BEGIN

BEGIN_MESSAGE_MAP(PSGInstrDlg, DialogBase)
	ON_WM_MOUSEWHEEL()
	ON_WM_SIZE()
	ON_COMMAND(IDC_PSG_RESETNOISE, &PSGInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_PSG_EASYPITCH, &PSGInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_PSG_FURNACEPITCH, &PSGInstrDlg::ParamsChanged)
	ON_COMMAND(IDC_PSG_PITCHREL, &PSGInstrDlg::ParamsChanged)
	ON_COMMAND(2598, &PSGInstrDlg::Randomize)
	ON_CBN_SELCHANGE(IDC_PSG_LANE, &PSGInstrDlg::ParamsChanged)
	ON_CONTROL_RANGE(EN_CHANGE, IDC_PSG_SPEED1, IDC_PSG_SPEED1 + PSGPatch::Count - 1, &PSGInstrDlg::OnFieldChanged)
	ON_CONTROL_RANGE(EN_CHANGE, IDC_PSG_DELAY1, IDC_PSG_DELAY1 + PSGPatch::Count - 1, &PSGInstrDlg::OnFieldChanged)
END_MESSAGE_MAP()


namespace
{
// Пределы значений у макросов разные: громкость это четыре бита, режим шума
// два, а высота и арпеджио ходят в обе стороны.
// Ниже этого строка перестаёт быть редактируемой мышью, но лучше тесная строка,
// чем строка за краем окна.
constexpr int MinRowHeight = 26;
// Выше этого графику уже незачем расти: шестнадцать уровней громкости на сотне
// пикселей это шесть пикселей на шаг, попасть легко, а дальше строки просто
// разъезжаются.
constexpr int MaxRowHeight = 96;

struct MacroRange { int32 low, high; };
constexpr MacroRange MacroRanges[PSGPatch::Count] =
{
	{0, 15},    // громкость
	{-24, 24},  // арпеджио
	{0, 3},     // режим шума
	{-64, 64},  // высота
	{0, 1},     // сброс фазы
};
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

	for(int i = 0; i < PSGPatch::Count; i++)
	{
		m_graphs[i].SubclassDlgItem(IDC_PSG_SEQ1 + i, this);
		m_graphs[i].Init(&m_working.macros[i], MacroRanges[i].low, MacroRanges[i].high,
			[this] { ParamsChanged(); });
	}

	CRect client;
	GetClientRect(client);
	m_designed = client.Size();
	const auto remember = [this](int id)
	{
		CRect rect;
		if(const CWnd *wnd = GetDlgItem(id))
		{
			wnd->GetWindowRect(rect);
			ScreenToClient(rect);
			m_designedRects[id] = rect;
		}
	};
	for(int i = 0; i < PSGPatch::Count; i++)
	{
		remember(IDC_PSG_SEQ1 + i);
		remember(IDC_PSG_NAME1 + i);
		remember(IDC_PSG_SPEEDLABEL1 + i);
		remember(IDC_PSG_SPEED1 + i);
		remember(IDC_PSG_DELAYLABEL1 + i);
		remember(IDC_PSG_DELAY1 + i);
	}
	remember(IDC_PSG_GROUP);
	remember(IDC_PSG_PITCHREL);
	remember(IDC_PSG_HINT);

	// Минимальный размер нарочно меньше нарисованного в ресурсах: вкладке
	// сэмплов часто достаётся полоса ниже диалога, и тогда нижние строки
	// уезжали за край. Со сжатыми строками видны все пять, а лишнюю высоту
	// раскладка всё равно раздаёт.
	const int footerHeight = m_designed.cy - m_designedRects[IDC_PSG_PITCHREL].top;
	m_minimumHeight = m_designedRects[IDC_PSG_SEQ1].top + MinRowHeight * PSGPatch::Count + footerHeight + 8;
	return TRUE;
}


void PSGInstrDlg::OnSize(UINT type, int cx, int cy)
{
	DialogBase::OnSize(type, cx, cy);
	LayoutMacros();
}


// Вкладке сэмплов обычно достаётся куда больше места, чем нарисовано в
// ресурсах, и строки по 36 пикселей на шестнадцать уровней громкости означают
// два пикселя на шаг: попасть мышью невозможно. Поэтому строки растягиваются на
// всю доступную высоту, а графики на всю ширину.
void PSGInstrDlg::LayoutMacros()
{
	if(!::IsWindow(m_hWnd) || m_designedRects.empty())
		return;
	CRect client;
	GetClientRect(client);
	const CRect &firstGraph = m_designedRects[IDC_PSG_SEQ1];
	const CRect &footer = m_designedRects[IDC_PSG_PITCHREL];
	const int footerHeight = m_designed.cy - footer.top;
	const int top = firstGraph.top - 8;
	const int bottom = client.Height() - footerHeight - 6;
	const int rowHeight = std::clamp((bottom - top) / PSGPatch::Count, MinRowHeight, MaxRowHeight);
	if(client.Width() < 200)
		return;

	// Поля скорости и задержки держатся правого края.
	const int blockWidth = m_designed.cx - m_designedRects[IDC_PSG_SPEEDLABEL1].left;
	const int graphRight = std::max<int>(client.Width() - blockWidth, firstGraph.left + 60);

	const auto place = [&](int id, int x, int y, int width, int height)
	{
		if(CWnd *wnd = GetDlgItem(id))
			wnd->SetWindowPos(nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
	};
	for(int i = 0; i < PSGPatch::Count; i++)
	{
		const int y = top + 4 + i * rowHeight;
		const int height = rowHeight - 8;
		place(IDC_PSG_SEQ1 + i, firstGraph.left, y, graphRight - firstGraph.left, height);
		const auto centred = [&](int id, int x)
		{
			const CRect &designed = m_designedRects[id];
			place(id, x, y + (height - designed.Height()) / 2, designed.Width(), designed.Height());
		};
		const int speedLabel = graphRight + 8;
		centred(IDC_PSG_SPEEDLABEL1 + i, speedLabel);
		centred(IDC_PSG_SPEED1 + i, speedLabel + m_designedRects[IDC_PSG_SPEEDLABEL1].Width() + 4);
		const int delayLabel = speedLabel + m_designedRects[IDC_PSG_SPEEDLABEL1].Width()
			+ m_designedRects[IDC_PSG_SPEED1].Width() + 12;
		centred(IDC_PSG_DELAYLABEL1 + i, delayLabel);
		centred(IDC_PSG_DELAY1 + i, delayLabel + m_designedRects[IDC_PSG_DELAYLABEL1].Width() + 4);
		centred(IDC_PSG_NAME1 + i, m_designedRects[IDC_PSG_NAME1].left);
	}

	// Рамка кончается сразу под последней строкой.
	const CRect &group = m_designedRects[IDC_PSG_GROUP];
	place(IDC_PSG_GROUP, group.left, group.top, client.Width() - group.left * 2,
		top + 4 + PSGPatch::Count * rowHeight - group.top + 4);
	// Флажок с подсказкой идут сразу под рамкой, а не по низу окна: иначе на
	// развёрнутом окне они уезжали бы от макросов на пол-экрана.
	const int underGroup = top + 4 + PSGPatch::Count * rowHeight + 10;
	const auto underneath = [&](int id)
	{
		const CRect &designed = m_designedRects[id];
		place(id, designed.left, underGroup, designed.Width(), designed.Height());
	};
	underneath(IDC_PSG_PITCHREL);
	underneath(IDC_PSG_HINT);
	Invalidate(TRUE);
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
	if(m_applying)
	{
		// Это отражение нашей же правки: поля трогать нельзя, иначе рисование
		// мышью сбрасывало бы само себя.
		m_patch = &holder;
		return;
	}
	SetRedraw(FALSE);
	m_filling = true;
	m_patch = nullptr;
	m_working = *holder;

	m_lane.SetCurSel(std::min<int>(m_working.lane, 3));
	CheckDlgButton(IDC_PSG_RESETNOISE, m_working.resetNoise ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_PSG_EASYPITCH, m_working.easyPitch ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_PSG_FURNACEPITCH, m_working.furnacePitch ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_PSG_PITCHREL, m_working.macros[PSGPatch::Pitch].relative ? BST_CHECKED : BST_UNCHECKED);
	for(int i = 0; i < PSGPatch::Count; i++)
	{
		const auto &seq = m_working.macros[i];
		SetDlgItemInt(IDC_PSG_SPEED1 + i, seq.speed, FALSE);
		SetDlgItemInt(IDC_PSG_DELAY1 + i, seq.delay, FALSE);
		m_graphs[i].SetSequence(&m_working.macros[i]);
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
	// Значения макросов правятся прямо в рабочей копии графиками, остальное
	// читается из полей.
	PSGPatch patch = m_working;
	patch.lane = static_cast<uint8>(std::max(0, m_lane.GetCurSel()));
	patch.resetNoise = IsDlgButtonChecked(IDC_PSG_RESETNOISE) != BST_UNCHECKED;
	patch.easyPitch = IsDlgButtonChecked(IDC_PSG_EASYPITCH) != BST_UNCHECKED;
	patch.furnacePitch = IsDlgButtonChecked(IDC_PSG_FURNACEPITCH) != BST_UNCHECKED;
	for(int i = 0; i < PSGPatch::Count; i++)
	{
		auto &seq = patch.macros[i];
		seq.speed = static_cast<uint8>(std::clamp<UINT>(GetDlgItemInt(IDC_PSG_SPEED1 + i, nullptr, FALSE), 1, 255));
		seq.delay = static_cast<uint8>(std::min<UINT>(GetDlgItemInt(IDC_PSG_DELAY1 + i, nullptr, FALSE), 255));
	}
	patch.macros[PSGPatch::Pitch].relative = IsDlgButtonChecked(IDC_PSG_PITCHREL) != BST_UNCHECKED;
	m_working = patch;

	const auto sameSequence = [](const PSGSequence &a, const PSGSequence &b)
	{
		return a.values == b.values && a.loop == b.loop && a.release == b.release
			&& a.speed == b.speed && a.delay == b.delay && a.relative == b.relative
			&& a.instantRelease == b.instantRelease;
	};
	if(patch.lane != current.lane
		|| patch.resetNoise != current.resetNoise
		|| patch.easyPitch != current.easyPitch
		|| patch.furnacePitch != current.furnacePitch
		|| !std::equal(patch.macros.begin(), patch.macros.end(), current.macros.begin(), sameSequence))
	{
		m_applying = true;
		m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_PREPAREUNDO);
		*m_patch = std::make_shared<const PSGPatch>(std::move(patch));
		m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_SETMODIFIED, SampleHint().Data().AsLPARAM());
		m_applying = false;
	}
}


void PSGInstrDlg::Randomize()
{
	if(m_patch == nullptr || !*m_patch)
		return;
	static std::mt19937 rng{std::random_device{}()};
	const auto rnd = [](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
	const bool noise = m_lane.GetCurSel() == 3;

	PSGPatch patch = m_working;
	for(auto &seq : patch.macros)
	{
		seq.values.clear();
		seq.loop = seq.release = 255;
		seq.speed = 1;
		seq.delay = 0;
		seq.relative = false;
	}

	// Громкость: короткая атака (не всегда), пик, спад к удержанию. У шума —
	// удар без удержания, как у ударных.
	auto &vol = patch.macros[PSGPatch::Volume];
	const int peak = rnd(12, 15);
	const int attack = rnd(0, 3) == 0 ? rnd(1, 3) : 0;
	for(int i = 1; i <= attack; i++)
		vol.values.push_back(peak * i / (attack + 1));
	const int sustain = noise ? 0 : (rnd(0, 2) == 0 ? 0 : rnd(5, 11));
	const int decay = noise ? rnd(3, 10) : rnd(4, 24);
	for(int i = 0; i <= decay; i++)
		vol.values.push_back(peak + (sustain - peak) * i / decay);
	if(sustain > 0)
		vol.loop = static_cast<uint8>(vol.values.size() - 1);
	vol.speed = static_cast<uint8>(rnd(1, 2));

	// Арпеджио: у тона иногда аккорд по кругу, иногда «щелчок» октавой в начале.
	auto &arp = patch.macros[PSGPatch::Arpeggio];
	static constexpr std::array<std::array<int, 3>, 4> chords = {{{0, 4, 7}, {0, 3, 7}, {0, 7, 12}, {0, 5, 7}}};
	const int arpChoice = rnd(0, 5);
	if(!noise && arpChoice <= 1)
	{
		const auto &chord = chords[rnd(0, 3)];
		arp.values.assign(chord.begin(), chord.end());
		arp.loop = 0;
	} else if(arpChoice == 2)
	{
		arp.values = {12, 0};
	}

	// Режим шума: у шумового канала — случайный, изредка смена после удара.
	if(noise)
	{
		auto &mode = patch.macros[PSGPatch::Duty];
		mode.values.push_back(rnd(0, 3));
		if(rnd(0, 2) == 0)
			mode.values.push_back(rnd(0, 3));
	}

	// Высота: иногда вибрато, иногда спад вниз (относительный макрос).
	auto &pitch = patch.macros[PSGPatch::Pitch];
	const int pitchChoice = rnd(0, 5);
	if(pitchChoice == 0)
	{
		const int depth = rnd(1, 3);
		pitch.values = {0, depth, depth * 2, depth, 0, -depth, -depth * 2, -depth};
		pitch.loop = 0;
		pitch.delay = static_cast<uint8>(rnd(4, 12));
		pitch.relative = true;
	} else if(pitchChoice == 1)
	{
		const int step = rnd(1, 4);
		const int steps = rnd(4, 10);
		for(int i = 0; i < steps; i++)
			pitch.values.push_back(-step);
		pitch.relative = true;
	}

	m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_PREPAREUNDO);
	*m_patch = std::make_shared<const PSGPatch>(std::move(patch));
	m_parent.SendMessage(WM_MOD_VIEWMSG, VIEWMSG_SETMODIFIED, SampleHint().Data().AsLPARAM());
	// Показать новый патч в полях и графиках.
	SetPatch(*m_patch);
}


BOOL PSGInstrDlg::OnMouseWheel(UINT flags, short delta, CPoint point)
{
	if(WheelStep(delta, point))
		return TRUE;
	return DialogBase::OnMouseWheel(flags, delta, point);
}


bool PSGInstrDlg::WheelStep(short delta, CPoint screenPoint)
{
	if(m_patch == nullptr || !delta || !::IsWindow(m_hWnd))
		return false;
	const int step = delta > 0 ? 1 : -1;
	CPoint client = screenPoint;
	ScreenToClient(&client);
	CWnd *child = ChildWindowFromPoint(client, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
	if(child == nullptr || child->m_hWnd == m_hWnd)
		return false;
	const int id = child->GetDlgCtrlID();

	// Над графиком колесо обрабатывает он сам: ему известно, над каким шагом
	// стоит курсор.
	if(id >= IDC_PSG_SEQ1 && id < IDC_PSG_SEQ1 + PSGPatch::Count)
		return m_graphs[id - IDC_PSG_SEQ1].WheelStep(delta, screenPoint);
	if(id >= IDC_PSG_SPEED1 && id < IDC_PSG_SPEED1 + PSGPatch::Count)
	{
		SetDlgItemInt(id, std::clamp(static_cast<int>(GetDlgItemInt(id, nullptr, FALSE)) + step, 1, 255), FALSE);
		return true;
	}
	if(id >= IDC_PSG_DELAY1 && id < IDC_PSG_DELAY1 + PSGPatch::Count)
	{
		SetDlgItemInt(id, std::clamp(static_cast<int>(GetDlgItemInt(id, nullptr, FALSE)) + step, 0, 255), FALSE);
		return true;
	}
	return false;
}

OPENMPT_NAMESPACE_END
