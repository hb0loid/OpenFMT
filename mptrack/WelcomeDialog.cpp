/*
 * WelcomeDialog.cpp
 * -----------------
 * Purpose: "First run" OpenMPT welcome dialog
 * Notes  : (currently none)
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */


#include "stdafx.h"
#include "OpenFMTVersion.h"
#include "WelcomeDialog.h"
#include "CommandSet.h"
#include "InputHandler.h"
#include "Mainfrm.h"
#include "Mptrack.h"
#include "resource.h"
#include "SelectPluginDialog.h"
#include "TrackerSettings.h"
#include "UpdateCheck.h"
#include "WindowMessages.h"
#include "../common/mptStringBuffer.h"
#include "mpt/fs/fs.hpp"
#include "mpt/string/utility.hpp"
#include "ColorSchemes.h"
#include "Reporting.h"


OPENMPT_NAMESPACE_BEGIN

BEGIN_MESSAGE_MAP(WelcomeDlg, DialogBase)
	ON_COMMAND(IDC_BUTTON1, &WelcomeDlg::OnOptions)
	ON_COMMAND(IDC_BUTTON2, &WelcomeDlg::OnScanPlugins)
END_MESSAGE_MAP()


WelcomeDlg::WelcomeDlg(CWnd *parent)
{
	Create(IDD_WECLOME, parent);
	CenterWindow(parent);
}


static mpt::PathString GetFullKeyPath(const char *keyFile)
{
	return theApp.GetInstallPkgPath() + P_("extraKeymaps\\") + mpt::PathString::FromUTF8(keyFile) + P_(".mkb");
}


BOOL WelcomeDlg::OnInitDialog()
{
	DialogBase::OnInitDialog();

#ifdef MPT_WITH_VST
	HKEY hkEnum = NULL;
	TCHAR str[MAX_PATH];
	DWORD datasize = sizeof(str);
	DWORD datatype = REG_SZ;
	if(RegOpenKey(HKEY_LOCAL_MACHINE, _T("Software\\VST"), &hkEnum) == ERROR_SUCCESS
		&& RegQueryValueEx(hkEnum, _T("VSTPluginsPath"), 0, &datatype, mpt::void_cast<LPBYTE>(static_cast<void*>(str)), &datasize) == ERROR_SUCCESS)
	{
		m_vstPath = mpt::PathString::FromNative(ParseMaybeNullTerminatedStringFromBufferWithSizeInBytes<mpt::winstring>(str, datasize));
	} else if(SHGetSpecialFolderPath(0, str, CSIDL_PROGRAM_FILES, FALSE))
	{
		m_vstPath = mpt::PathString::FromNative(ParseMaybeNullTerminatedStringFromBufferWithSizeInBytes<mpt::winstring>(str, datasize)) + P_("\\Steinberg\\VstPlugins\\");
		if(!mpt::native_fs{}.is_directory(m_vstPath))
		{
			m_vstPath = mpt::PathString();
		}
	}
	SetDlgItemText(IDC_EDIT2, mpt::ToCString(TrackerSettings::Instance().defaultArtist.Get()));
	if(!m_vstPath.empty())
	{
		SetDlgItemText(IDC_EDIT1, m_vstPath.AsNative().c_str());
		if(TrackerSettings::Instance().PathPlugins.GetDefaultDir().empty())
		{
			TrackerSettings::Instance().PathPlugins.SetDefaultDir(m_vstPath);
		}
	} else
#endif // MPT_WITH_VST
	{
		SetDlgItemText(IDC_EDIT1, _T("No plugin path found!"));
		GetDlgItem(IDC_BUTTON2)->EnableWindow(FALSE);
	}

	const char *keyFile = nullptr;
	const TCHAR *keyFileName = nullptr;
	const uint16 language = LOWORD(GetKeyboardLayout(0)), primaryLang = language & 0x3FF;
	CComboBox *combo = (CComboBox *)GetDlgItem(IDC_COMBO1);
	combo->AddString(_T("OpenMPT / Chromatic (Default)"));
	combo->SetCurSel(0);
	switch(primaryLang)
	{
		case LANG_GERMAN:
			keyFile = "DE_jojo";
			keyFileName = _T("German");
			break;
		case LANG_SPANISH:
			// Spanish latin-american keymap, so we ignore Spain.
			if(language != SUBLANG_SPANISH_MODERN && language != SUBLANG_SPANISH)
			{
				keyFile = "es-LA_mpt_(jmkz)";
				keyFileName = _T("Spanish");
			}
			break;
		case LANG_FRENCH:
			keyFile = "FR_mpt_(legovitch)";
			keyFileName = _T("French");
			break;
		case LANG_NORWEGIAN:
			keyFile = "NO_mpt_classic_(rakib)";
			keyFileName = _T("Norwegian");
			break;
	}
	if(keyFile != nullptr)
	{
		if(mpt::native_fs{}.is_file(GetFullKeyPath(keyFile)))
		{
			int i = combo->AddString(_T("OpenMPT / Chromatic (") + CString(keyFileName) + _T(")"));
			combo->SetItemDataPtr(i, (void *)keyFile);
			combo->SetCurSel(i);

			// As this is presented as the default, load it right now, even if the user closes the dialog through the close button
			auto cmdSet = std::make_unique<CCommandSet>();
			cmdSet->LoadFile(GetFullKeyPath(keyFile));
			CMainFrame::GetInputHandler()->SetNewCommandSet(*cmdSet);
		}
	}
	combo->SetItemDataPtr(combo->AddString(_T("Impulse Tracker")), (void*)("US_mpt-it2_classic"));
	combo->SetItemDataPtr(combo->AddString(_T("FastTracker 2")), (void*)("US_mpt-ft2_classic"));

	CheckDlgButton(IDC_CHECK1, BST_CHECKED);
	CheckDlgButton(IDC_CHECK3, BST_CHECKED);
	// OpenFMT: в этом окне нет ни галочки проверки обновлений, ни текста о
	// статистике OpenMPT — их убрали в 0.1.0. Обращение к несуществующему
	// элементу роняло окно при первом запуске.
	CheckDlgButton(IDC_CHECK2, (TrackerSettings::Instance().patternFont.Get().name == PATTERNFONT_LARGE) ? BST_CHECKED : BST_UNCHECKED);

	SetWindowText(_T("Welcome to ") _T(OPENFMT_NAME) _T("!"));
	// Тема: тёмная — тёмный интерфейс и тёмные цвета паттерна, светлая —
	// обычный вид Windows и стандартные цвета.
	if(CComboBox *theme = static_cast<CComboBox *>(GetDlgItem(IDC_COMBO_THEME)); theme != nullptr)
	{
		theme->AddString(_T("Dark"));
		theme->AddString(_T("Light (Windows)"));
		theme->SetCurSel(TrackerSettings::Instance().darkInterface ? 0 : 1);
	}

	ShowWindow(SW_SHOW);

	return TRUE;
}


void WelcomeDlg::OnOptions()
{
	OnOK();
	CMainFrame::GetMainFrame()->PostMessage(WM_COMMAND, ID_VIEW_OPTIONS);
}


void WelcomeDlg::OnScanPlugins()
{
#ifdef MPT_WITH_VST
	CSelectPluginDlg::ScanPlugins(m_vstPath, this);
#endif // MPT_WITH_VST
}


void WelcomeDlg::OnOK()
{
	// Читаем до DialogBase::OnOK: после него окно уже закрывается.
	bool dark = true;
	if(CComboBox *theme = static_cast<CComboBox *>(GetDlgItem(IDC_COMBO_THEME)); theme != nullptr)
		dark = theme->GetCurSel() != 1;
	CString artistName;
	GetDlgItemText(IDC_EDIT2, artistName);

	DialogBase::OnOK();

	// Имя автора у OpenMPT сохранялось только вместе с настройками обновлений,
	// а они у OpenFMT выключены.
	TrackerSettings::Instance().defaultArtist = mpt::ToUnicode(artistName);
	const bool wasDark = TrackerSettings::Instance().darkInterface;
	TrackerSettings::Instance().darkInterface = dark;
	if(dark)
	{
		if(const ColorScheme *scheme = FindColorScheme(_T("OpenFMT (Dark)")); scheme != nullptr)
		{
			auto colors = TrackerSettings::Instance().rgbCustomColors;
			TrackerSettings::GetDefaultColourScheme(colors);
			for(const auto &c : scheme->colors)
			{
				if(c.color != 0 || c.id != 0)
					colors[c.id] = c.color;
			}
			TrackerSettings::Instance().rgbCustomColors = colors;
			CMainFrame::UpdateColors();
		}
	}

#if defined(MPT_ENABLE_UPDATE)
	// Проверка выпусков OpenFMT включена по умолчанию (раз в день); выключается
	// в настройках Update. Статистику OpenFMT не отправляет.
	const bool runUpdates = TrackerSettings::Instance().UpdateIntervalDays >= 0;
	TrackerSettings::Instance().UpdateStatistics = false;
	TrackerSettings::Instance().UpdateShowUpdateHint = false;
	TrackerSettings::Instance().UpdateStatisticsConsentAsked = true;

#endif // MPT_ENABLE_UPDATE
	if(IsDlgButtonChecked(IDC_CHECK2) != BST_UNCHECKED)
	{
		FontSetting font = TrackerSettings::Instance().patternFont;
		font.name = PATTERNFONT_LARGE;
		TrackerSettings::Instance().patternFont = font;
	}

	CComboBox *combo = (CComboBox *)GetDlgItem(IDC_COMBO1);
	const char *keyFile = static_cast<char *>(combo->GetItemDataPtr(combo->GetCurSel()));
	auto cmdSet = std::make_unique<CCommandSet>();
	if(keyFile != nullptr)
		cmdSet->LoadFile(GetFullKeyPath(keyFile));
	else
		cmdSet->LoadDefaultKeymap();
	CMainFrame::GetInputHandler()->SetNewCommandSet(*cmdSet);

#if defined(MPT_ENABLE_UPDATE)
	if(runUpdates)
	{
		CUpdateCheck::DoAutoUpdateCheck();
	}
#endif // MPT_ENABLE_UPDATE

	theApp.GetSettings().Flush();

	CMainFrame::GetMainFrame()->PostMessage(WM_MOD_INVALIDATEPATTERNS, HINT_MPTOPTIONS);

	if(dark != wasDark)
		Reporting::Information(_T("The new theme will be applied after restarting OpenFMT."), _T("OpenFMT"));

	DestroyWindow();
}

void WelcomeDlg::OnCancel()
{
	DialogBase::OnCancel();
	DestroyWindow();
}

OPENMPT_NAMESPACE_END
