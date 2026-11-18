/*
 * UpdateCheck.cpp
 * ---------------
 * Purpose: Class for easy software update check.
 * Notes  : (currently none)
 * Authors: OpenMPT Devs
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */


#include "stdafx.h"
#include "UpdateCheck.h"
#include "DialogBase.h"
#include "dlg_misc.h"
#include "HTTP.h"
#include "Mainfrm.h"
#include "Moddoc.h"
#include "Mptrack.h"
#include "ProgressDialog.h"
#include "Reporting.h"
#include "resource.h"
#include "TrackerSettings.h"
#include "WindowMessages.h"
#include "OpenFMTVersion.h"
#include "../common/misc_util.h"
#include "../common/mptStringBuffer.h"
#include "../common/version.h"
#include "mpt/binary/hex.hpp"
#include "mpt/crypto/hash.hpp"
#include "mpt/crypto/jwk.hpp"
#include "mpt/exception/runtime_error.hpp"
#include "mpt/format/join.hpp"
#include "mpt/fs/common_directories.hpp"
#include "mpt/fs/fs.hpp"
#include "mpt/io/io.hpp"
#include "mpt/io/io_stdstream.hpp"
#include "mpt/io_file/fstream.hpp"
#include "mpt/io_file/inputfile.hpp"
#include "mpt/io_file/outputfile.hpp"
#include "mpt/io_file_read/inputfile_filecursor.hpp"
#include "mpt/json/json.hpp"
#include "mpt/path/os_path_long.hpp"
#include "mpt/string/utility.hpp"
#include "mpt/system_error/system_error.hpp"
#include "openmpt/sounddevice/SoundDeviceManager.hpp"


OPENMPT_NAMESPACE_BEGIN


#if defined(MPT_ENABLE_UPDATE)


namespace Update {

	struct windowsversion {
		uint64 version_major = 0;
		uint64 version_minor = 0;
		uint64 servicepack_major = 0;
		uint64 servicepack_minor = 0;
		uint64 build = 0;
		uint64 wine_major = 0;
		uint64 wine_minor = 0;
		uint64 wine_update = 0;
	};
	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(windowsversion
		,version_major
		,version_minor
		,servicepack_major
		,servicepack_minor
		,build
		,wine_major
		,wine_minor
		,wine_update
	)

	struct autoupdate_installer {
		std::vector<mpt::ustring> arguments = {};
	};
	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(autoupdate_installer
		,arguments
	)

	struct autoupdate_archive {
		mpt::ustring subfolder = U_("");
		mpt::ustring restartbinary = U_("");
	};
	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(autoupdate_archive
		,subfolder
		,restartbinary
	)

	struct downloadinfo {
		mpt::ustring url = U_("");
		std::map<mpt::ustring, mpt::ustring> checksums = {};
		mpt::ustring filename = U_("");
		std::optional<autoupdate_installer> autoupdate_installer;
		std::optional<autoupdate_archive> autoupdate_archive;
	};
	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(downloadinfo
		,url
		,checksums
		,filename
		,autoupdate_installer
		,autoupdate_archive
	)

	struct download {
		mpt::ustring url = U_("");
		mpt::ustring download_url = U_("");
		mpt::ustring type = U_("");
		bool can_autoupdate = false;
		mpt::ustring autoupdate_minversion = U_("");
		mpt::ustring os = U_("");
		std::optional<windowsversion> required_windows_version;
		std::map<mpt::ustring, bool> required_architectures = {};
		std::map<mpt::ustring, bool> supported_architectures = {};
		std::map<mpt::ustring, std::map<mpt::ustring, bool>> required_processor_features = {};
	};
	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(download
		,url
		,download_url
		,type
		,can_autoupdate
		,autoupdate_minversion
		,os
		,required_windows_version
		,required_architectures
		,supported_architectures
		,required_processor_features
	)

	struct versioninfo {
		mpt::ustring version = U_("");
		mpt::ustring date = U_("");
		mpt::ustring announcement_url = U_("");
		mpt::ustring changelog_url = U_("");
		std::map<mpt::ustring, download> downloads = {};
	};
	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(versioninfo
		,version
		,date
		,announcement_url
		,changelog_url
		,downloads
	)

	using versions = std::map<mpt::ustring, versioninfo>;

} // namespace Update


struct UpdateInfo {
	mpt::ustring version;
	mpt::ustring download;
	bool IsAvailable() const
	{
		return !version.empty();
	}
};

static bool IsCurrentArchitecture(const mpt::ustring &architecture)
{
	return mpt::OS::Windows::Name(mpt::OS::Windows::GetProcessArchitecture()) == architecture;
}

static bool IsArchitectureSupported(const mpt::ustring &architecture)
{
	const auto & architectures = mpt::OS::Windows::GetSupportedProcessArchitectures(mpt::OS::Windows::GetHostArchitecture());
	for(const auto & arch : architectures)
	{
		if(mpt::OS::Windows::Name(arch) == architecture)
		{
			return true;
		}
	}
	return false;
}

static bool IsArchitectureFeatureSupported(const mpt::ustring &architecture, const mpt::ustring &feature)
{
	MPT_UNUSED_VARIABLE(architecture);
	#ifdef MPT_ENABLE_ARCH_INTRINSICS
		#if MPT_ARCH_X86 || MPT_ARCH_AMD64
			const mpt::arch::current::cpu_info CPUInfo = mpt::arch::get_cpu_info();
			if(feature == U_("")) return true;
			else if(feature == U_("lm")) return CPUInfo.can_long_mode();
			else if(feature == U_("mmx")) return CPUInfo[mpt::arch::current::feature::mmx];
			else if(feature == U_("sse")) return CPUInfo[mpt::arch::current::feature::sse] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			else if(feature == U_("sse2")) return CPUInfo[mpt::arch::current::feature::sse2] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			else if(feature == U_("sse3")) return CPUInfo[mpt::arch::current::feature::sse3] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			else if(feature == U_("ssse3")) return CPUInfo[mpt::arch::current::feature::ssse3] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			else if(feature == U_("sse4.1")) return CPUInfo[mpt::arch::current::feature::sse4_1] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			else if(feature == U_("sse4.2")) return CPUInfo[mpt::arch::current::feature::sse4_2] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			else if(feature == U_("avx")) return CPUInfo[mpt::arch::current::feature::avx] && CPUInfo[mpt::arch::current::mode::ymm256avx];
			else if(feature == U_("avx2")) return CPUInfo[mpt::arch::current::feature::avx2] && CPUInfo[mpt::arch::current::mode::ymm256avx];
			else return false;
		#else // MPT_ARCH
			MPT_UNUSED_VARIABLE(feature);
			return true;
		#endif // MPT_ARCH
	#else // !MPT_ENABLE_ARCH_INTRINSICS
		MPT_UNUSED_VARIABLE(feature);
		return true;
	#endif // MPT_ENABLE_ARCH_INTRINSICS
}


static mpt::ustring GetChannelName(UpdateChannel channel)
{
	mpt::ustring channelName = U_("release");
	switch(channel)
	{
	case UpdateChannelDevelopment:
		channelName = U_("development");
		break;
	case UpdateChannelNext:
		channelName = U_("next");
		break;
	case UpdateChannelRelease:
		channelName = U_("release");
		break;
	default:
		channelName = U_("release");
		break;
	}
	return channelName;
}


static UpdateInfo GetBestDownload(const Update::versions &versions)
{
	
	UpdateInfo result;
	VersionWithRevision bestVersion = VersionWithRevision::Current();
	mpt::osinfo::windows::Version bestRequiredWindowsVersion = mpt::osinfo::windows::Version::AnyWindows();

	for(const auto & [versionname, versioninfo] : versions)
	{

		if(!VersionWithRevision::Parse(versioninfo.version).IsNewerThan(bestVersion))
		{
			continue;
		}

		mpt::ustring bestDownloadName;

		// check if version supports the current system
		bool is_supported = false;
		for(auto & [downloadname, download] : versioninfo.downloads)
		{

			// is it for windows?
			if(download.os != U_("windows") || !download.required_windows_version)
			{
				continue;
			}

			// can the installer run on the current system?
			bool download_supported = true;
			for(const auto & [architecture, required] : download.required_architectures)
			{
				if(!(required && IsArchitectureSupported(architecture)))
				{
					download_supported = false;
				}
			}

			// does the download run on current architecture?
			bool architecture_supported = false;
			for(const auto & [architecture, supported] : download.supported_architectures)
			{
				if(supported && IsCurrentArchitecture(architecture))
				{
					architecture_supported = true;
				}
			}
			if(!architecture_supported)
			{
				download_supported = false;
			}

			// does the current system have all required features?
			for(const auto & [architecture, features] : download.required_processor_features)
			{
				if(IsCurrentArchitecture(architecture))
				{
					for(const auto & [feature, required] : features)
					{
						if(!(required && IsArchitectureFeatureSupported(architecture, feature)))
						{
							download_supported = false;
						}
					}
				}
			}

			const mpt::osinfo::windows::Version download_required_windows_version = mpt::osinfo::windows::Version(
					mpt::osinfo::windows::Epoch::WinNT,
					mpt::osinfo::windows::System(mpt::saturate_cast<uint32>(download.required_windows_version->version_major), mpt::saturate_cast<uint32>(download.required_windows_version->version_minor)),
					mpt::osinfo::windows::ServicePack(mpt::saturate_cast<uint16>(download.required_windows_version->servicepack_major), mpt::saturate_cast<uint16>(download.required_windows_version->servicepack_minor)),
					mpt::osinfo::windows::InternetExplorer(0, 0),
					mpt::osinfo::windows::Build(mpt::saturate_cast<uint32>(download.required_windows_version->build))
				);

			if(mpt::osinfo::windows::Version::Current().IsBefore(download_required_windows_version))
			{
				download_supported = false;
			}

			if(mpt::OS::Windows::IsWine() && !theApp.GetWineVersion()->IsValid())
			{
				download_supported = false;
			}
			if(mpt::OS::Windows::IsWine() && theApp.GetWineVersion()->IsValid())
			{
				if(theApp.GetWineVersion()->IsBefore(mpt::osinfo::windows::wine::version{mpt::saturate_cast<uint8>(download.required_windows_version->wine_major), mpt::saturate_cast<uint8>(download.required_windows_version->wine_minor), mpt::saturate_cast<uint8>(download.required_windows_version->wine_update)}))
				{
					download_supported = false;
				}
			}

			if(download_supported)
			{
				is_supported = true;
				bool downloadtype_supported = false;
				if(theApp.IsInstallerMode() && download.type == U_("installer"))
				{
					downloadtype_supported = true;
				} else if(theApp.IsPortableMode() && download.type == U_("archive"))
				{
					downloadtype_supported = true;
				}
				if(downloadtype_supported)
				{
					if(download_required_windows_version.IsAtLeast(bestRequiredWindowsVersion))
					{
						bestRequiredWindowsVersion = download_required_windows_version;
						bestDownloadName = downloadname;
					}
				}
			}

		}

		if(is_supported)
		{
			bestVersion = VersionWithRevision::Parse(versioninfo.version);
			result.version = versionname;
			result.download = bestDownloadName;
		}

	}

	return result;

}


// Update notification dialog
class UpdateDialog : public DialogBase
{
protected:
	const CString m_releaseVersion;
	const CString m_releaseDate;
	const CString m_releaseURL;
	const CString m_buttonText;
	CFont m_boldFont;

public:
	UpdateDialog(const CString &releaseVersion, const CString &releaseDate, const CString &releaseURL, const CString &buttonText = _T("&Update"))
		: DialogBase(IDD_UPDATE)
		, m_releaseVersion(releaseVersion)
		, m_releaseDate(releaseDate)
		, m_releaseURL(releaseURL)
		, m_buttonText(buttonText)
	{ }

	BOOL OnInitDialog() override
	{
		DialogBase::OnInitDialog();

		SetDlgItemText(IDOK, m_buttonText);

		CFont *font = GetDlgItem(IDC_VERSION2)->GetFont();
		LOGFONT lf;
		font->GetLogFont(&lf);
		lf.lfWeight = FW_BOLD;
		m_boldFont.CreateFontIndirect(&lf);
		GetDlgItem(IDC_VERSION2)->SetFont(&m_boldFont);

		SetDlgItemText(IDC_VERSION1, _T(OPENFMT_VERSION_A));
		SetDlgItemText(IDC_VERSION2, m_releaseVersion);
		SetDlgItemText(IDC_DATE, m_releaseDate);
		SetDlgItemText(IDC_SYSLINK1, _T("More information about this build:\n<a href=\"") + m_releaseURL + _T("\">") + m_releaseURL + _T("</a>"));
		CheckDlgButton(IDC_CHECK1, (TrackerSettings::Instance().UpdateIgnoreVersion == m_releaseVersion) ? BST_CHECKED : BST_UNCHECKED);
		return FALSE;
	}

	void OnDestroy()
	{
		TrackerSettings::Instance().UpdateIgnoreVersion = IsDlgButtonChecked(IDC_CHECK1) != BST_UNCHECKED ? m_releaseVersion : CString();
		m_boldFont.DeleteObject();
		DialogBase::OnDestroy();
	}

	void OnClickURL(NMHDR * /*pNMHDR*/, LRESULT * /*pResult*/)
	{
		CTrackApp::OpenURL(m_releaseURL);
	}

	DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(UpdateDialog, DialogBase)
	ON_NOTIFY(NM_CLICK, IDC_SYSLINK1, &UpdateDialog::OnClickURL)
	ON_WM_DESTROY()
END_MESSAGE_MAP()


mpt::ustring CUpdateCheck::GetStatisticsUserInformation(bool shortText)
{
	if(shortText)
	{
		return U_("A randomized user ID is sent together with basic system information."
			" This ID cannot be linked to you personally in any way."
			"\nOpenMPT will use this information to gather usage statistics and to plan system support for future OpenMPT versions.");
	} else
	{
		return U_(
			"When checking for updates, OpenMPT can additionally collect basic statistical information."
			" A randomized user ID is sent alongside the update check. This ID and the transmitted statistics cannot be linked to you personally in any way."
			" OpenMPT will use this information to gather usage statistics and to plan system support for future OpenMPT versions."
			"\nOpenMPT would collect the following statistical data points: OpenMPT version, Windows version, type of CPU, amount of RAM, sound device settings, configured update check frequency of OpenMPT.");
	}
}


std::vector<mpt::ustring> CUpdateCheck::GetDefaultUpdateSigningKeysRootAnchors()
{
	// IMPORTANT:
	// Signing keys are *NOT* stored on the same server as openmpt.org or the updates themselves,
	// because otherwise, a single compromised server could allow for rogue updates.
	return {
		U_("https://update.openmpt.de/update/"),
		U_("https://demo-scene.de/openmpt/update/")
	};
}


mpt::ustring CUpdateCheck::GetDefaultAPIURL()
{
	return U_("https://update.openmpt.org/api/v3/");
}


// OpenFMT проверяет обновления у себя на GitHub, а не у OpenMPT: тот сервер
// знает только версии OpenMPT и предлагал бы людям не ту программу.
static constexpr char OpenFMTReleasesURL[] = "https://api.github.com/repos/hb0loid/OpenFMT/releases?per_page=20";
static constexpr char OpenFMTUpdateCache[] = "openfmt-releases.json";

// Номер версии «0.1.2» (или «v0.1.2») в числа для сравнения.
static std::vector<int> ParseOpenFMTVersion(std::string text)
{
	if(!text.empty() && (text[0] == 'v' || text[0] == 'V'))
		text.erase(0, 1);
	std::vector<int> parts;
	int value = 0;
	bool digits = false;
	for(char c : text)
	{
		if(c >= '0' && c <= '9')
		{
			value = value * 10 + (c - '0');
			digits = true;
		} else if(c == '.')
		{
			parts.push_back(value);
			value = 0;
			digits = false;
		} else
		{
			break;
		}
	}
	if(digits)
		parts.push_back(value);
	return parts;
}

static bool OpenFMTVersionNewer(const std::vector<int> &a, const std::vector<int> &b)
{
	for(size_t i = 0; i < std::max(a.size(), b.size()); i++)
	{
		const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
		if(x != y)
			return x > y;
	}
	return false;
}

// Самый новый выпуск на GitHub новее нашей версии (черновики пропускаем,
// пробные — нет: все наши выпуски пока помечены как pre-release).
struct OpenFMTRelease
{
	std::string version, date, url;
	// Пакет выпуска и его контрольная сумма (файлы, которые кладёт
	// make_release.py). Нет обоих — ставить нечего, только страница.
	std::string zipName, zipURL, sha256URL;
};
static std::optional<OpenFMTRelease> FindNewerOpenFMTRelease(const std::vector<std::byte> &json)
{
	const auto releases = nlohmann::json::parse(mpt::buffer_cast<std::string>(json));
	if(!releases.is_array())
		throw std::runtime_error("not a release list");
	const auto current = ParseOpenFMTVersion(OPENFMT_VERSION_A);
	std::optional<OpenFMTRelease> best;
	std::vector<int> bestVersion = current;
	for(const auto &release : releases)
	{
		if(!release.is_object() || release.value("draft", false))
			continue;
		const std::string tag = release.value("tag_name", std::string{});
		const auto version = ParseOpenFMTVersion(tag);
		if(version.empty() || !OpenFMTVersionNewer(version, bestVersion))
			continue;
		bestVersion = version;
		OpenFMTRelease info;
		info.version = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
		info.date = release.value("published_at", std::string{}).substr(0, 10);
		info.url = release.value("html_url", std::string{"https://github.com/hb0loid/OpenFMT/releases"});
		if(const auto assets = release.find("assets"); assets != release.end() && assets->is_array())
		{
			const auto endsWith = [](const std::string &text, const std::string &suffix) { return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0; };
			for(const auto &asset : *assets)
			{
				if(!asset.is_object())
					continue;
				const std::string name = asset.value("name", std::string{}), link = asset.value("browser_download_url", std::string{});
				if(endsWith(name, "-win64.zip"))
				{
					info.zipName = name;
					info.zipURL = link;
				} else if(endsWith(name, "-win64.zip.sha256"))
				{
					info.sha256URL = link;
				}
			}
			if(info.zipURL.empty() || info.sha256URL.empty())
				info.zipName.clear(), info.zipURL.clear(), info.sha256URL.clear();
		}
		best = info;
	}
	return best;
}


std::atomic<int32> CUpdateCheck::s_InstanceCount(0);


int32 CUpdateCheck::GetNumCurrentRunningInstances()
{
	return s_InstanceCount.load();
}



bool CUpdateCheck::IsSuitableUpdateMoment()
{
	const auto documents = theApp.GetOpenDocuments();
	return std::all_of(documents.begin(), documents.end(), [](auto doc) { return !doc->IsModified(); });
}


void CUpdateCheck::WaitForUpdateCheckFinished()
{
	while(GetNumCurrentRunningInstances() > 0)
	{
		MSG msg;
		while(::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
		if(GetNumCurrentRunningInstances() > 0)
		{
			Sleep(1);
		}
	}
}


// Start update check
void CUpdateCheck::StartUpdateCheckAsync(bool isAutoUpdate)
{
	bool loadPersisted = false;
	if(isAutoUpdate)
	{
		if(!TrackerSettings::Instance().UpdateEnabled)
		{
			return;
		}
		if(!IsSuitableUpdateMoment())
		{
			return;
		}
		int updateCheckPeriod = TrackerSettings::Instance().UpdateIntervalDays;
		if(updateCheckPeriod < 0)
		{
			return;
		}
		// Do we actually need to run the update check right now?
		const mpt::chrono::default_system_clock::time_point now = mpt::chrono::default_system_clock::now();
		const mpt::chrono::default_system_clock::time_point lastCheck = TrackerSettings::Instance().UpdateLastUpdateCheck.Get();
		// Check update interval. Note that we always check for updates when the system time had gone backwards (i.e. when the last update check supposedly happened in the future).
		const int64 secsSinceLastCheck = mpt::chrono::default_system_clock::to_unix_seconds(now) - mpt::chrono::default_system_clock::to_unix_seconds(lastCheck);
		if(secsSinceLastCheck > 0 && secsSinceLastCheck < updateCheckPeriod * 86400)
		{
			loadPersisted = true;
		}

		// Never ran update checks before, so we notify the user of automatic update checks.
		if(TrackerSettings::Instance().UpdateShowUpdateHint)
		{
			TrackerSettings::Instance().UpdateShowUpdateHint = false;
			const auto checkIntervalDays = TrackerSettings::Instance().UpdateIntervalDays.Get();
			CString msg = MPT_CFORMAT("OpenMPT would like to check for updates now, proceed?\n\nNote: In the future, OpenMPT will check for updates {}. If you do not want this, you can disable update checks in the setup.")
				(
					checkIntervalDays == 0 ? CString(_T("on every program start")) :
					checkIntervalDays == 1 ? CString(_T("every day")) :
					MPT_CFORMAT("every {} days")(checkIntervalDays)
				);
			if(Reporting::Confirm(msg, _T("OpenMPT Update")) == cnfNo)
			{
				TrackerSettings::Instance().UpdateLastUpdateCheck = now;
				return;
			}
		}
	} else
	{
		if(!IsSuitableUpdateMoment())
		{
			Reporting::Notification(_T("Please save all modified modules before updating OpenMPT."), _T("OpenMPT Update"));
			return;
		}
		if(!TrackerSettings::Instance().UpdateEnabled)
		{
			if(Reporting::Confirm(_T("Update Check is disabled. Do you want to check anyway?"), _T("OpenMPT Update")) != cnfYes)
			{
				return;
			}
		}
	}
	TrackerSettings::Instance().UpdateShowUpdateHint = false;

	// OpenFMT статистику не собирает, поэтому и не спрашивает.
	TrackerSettings::Instance().UpdateStatistics = false;
	if(false)
	{
		const auto enableStatistics = Reporting::Confirm(
			U_("Do you want to contribute to OpenMPT by providing system statistics?\r\n\r\n") +
			mpt::replace(CUpdateCheck::GetStatisticsUserInformation(false), U_("\n"), U_("\r\n")) + U_("\r\n\r\n") +
			MPT_UFORMAT("This option was previously {} on your system.\r\n")(TrackerSettings::Instance().UpdateStatistics ? U_("enabled") : U_("disabled")),
			false, !TrackerSettings::Instance().UpdateStatistics.Get());
		TrackerSettings::Instance().UpdateStatistics = (enableStatistics == ConfirmAnswer::cnfYes);
		TrackerSettings::Instance().UpdateStatisticsConsentAsked = true;
	}

	int32 expected = 0;
	if(!s_InstanceCount.compare_exchange_strong(expected, 1))
	{
		return;
	}

	CUpdateCheck::Context context;
	context.window = CMainFrame::GetMainFrame();
	context.msgStart = MPT_WM_APP_UPDATECHECK_START;
	context.msgProgress = MPT_WM_APP_UPDATECHECK_PROGRESS;
	context.msgCanceled = MPT_WM_APP_UPDATECHECK_CANCELED;
	context.msgFailure = MPT_WM_APP_UPDATECHECK_FAILURE;
	context.msgSuccess = MPT_WM_APP_UPDATECHECK_SUCCESS;
	context.autoUpdate = isAutoUpdate;
	context.loadPersisted = loadPersisted;
	context.statistics = GetStatisticsDataV3(CUpdateCheck::Settings());
	std::thread(CUpdateCheck::ThreadFunc(CUpdateCheck::Settings(), context)).detach();
}


CUpdateCheck::Settings::Settings()
	: previousVersion(TrackerSettings::Instance().PreviousSettingsVersion)
	, periodDays(TrackerSettings::Instance().UpdateIntervalDays)
	, channel(static_cast<UpdateChannel>(TrackerSettings::Instance().UpdateChannel.Get()))
	, persistencePath(theApp.GetConfigPath())
	, apiURL(TrackerSettings::Instance().UpdateAPIURL)
	, sendStatistics(TrackerSettings::Instance().UpdateStatistics)
	, statisticsUUID(TrackerSettings::Instance().VersionInstallGUID)
{
}


CUpdateCheck::ThreadFunc::ThreadFunc(const CUpdateCheck::Settings &settings, const CUpdateCheck::Context &context)
	: settings(settings)
	, context(context)
{
	return;
}


void CUpdateCheck::ThreadFunc::operator () ()
{
	SetThreadPriority(GetCurrentThread(), context.autoUpdate ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
	CheckForUpdate(settings, context);
}


std::string CUpdateCheck::GetStatisticsDataV3(const Settings &settings)
{
	nlohmann::json j;
	j["OpenMPT"]["Version"] = mpt::ufmt::val(Version::Current());
	j["OpenMPT"]["InstallationMode"] = theApp.GetInstallationMode();
	j["OpenMPT"]["BuildVariant"] = mpt::ToUnicode(mpt::Charset::ASCII, OPENMPT_BUILD_VARIANT);
	j["OpenMPT"]["Architecture"] = mpt::OS::Windows::Name(mpt::OS::Windows::GetProcessArchitecture());
	j["Update"]["PeriodDays"] = settings.periodDays;
	j["Update"]["Channel"] = ((settings.channel == UpdateChannelRelease) ? U_("Release") : (settings.channel == UpdateChannelNext) ? U_("Next") : (settings.channel == UpdateChannelDevelopment) ? U_("Development") : U_(""));
	j["System"]["Windows"]["Version"]["Name"] = mpt::OS::Windows::Version::GetCurrentName();
	j["System"]["Windows"]["Version"]["Major"] = mpt::osinfo::windows::Version::Current().GetSystem().Major;
	j["System"]["Windows"]["Version"]["Minor"] = mpt::osinfo::windows::Version::Current().GetSystem().Minor;
	j["System"]["Windows"]["ServicePack"]["Major"] = mpt::osinfo::windows::Version::Current().GetServicePack().Major;
	j["System"]["Windows"]["ServicePack"]["Minor"] = mpt::osinfo::windows::Version::Current().GetServicePack().Minor;
	j["System"]["Windows"]["Build"] = mpt::osinfo::windows::Version::Current().GetBuild();
	j["System"]["Windows"]["Architecture"] = mpt::OS::Windows::Name(mpt::OS::Windows::GetHostArchitecture());
	j["System"]["Windows"]["IsWine"] = mpt::OS::Windows::IsWine();
	j["System"]["Windows"]["TypeRaw"] = MPT_UFORMAT("0x{}")(mpt::ufmt::HEX0<8>(mpt::osinfo::windows::Version::Current().GetProduct()));
	std::vector<mpt::OS::Windows::Architecture> architectures = mpt::OS::Windows::GetSupportedProcessArchitectures(mpt::OS::Windows::GetHostArchitecture());
	for(const auto & arch : architectures)
	{
		j["System"]["Windows"]["ProcessArchitectures"][mpt::ToCharset(mpt::Charset::UTF8, mpt::OS::Windows::Name(arch))] = true;
	}
	j["System"]["Memory"] = mpt::OS::Windows::GetSystemMemorySize() / 1024 / 1024;  // MB
	j["System"]["Threads"] = std::thread::hardware_concurrency();
	if(mpt::OS::Windows::IsWine() && theApp.GetWineVersion())
	{
		const mpt::OS::Wine::VersionContext & v = *theApp.GetWineVersion().get();
		j["System"]["Windows"]["Wine"]["Version"]["Raw"] = mpt::ToUnicode(mpt::Charset::UTF8, v.RawVersion());
		if(v.IsValid())
		{
			j["System"]["Windows"]["Wine"]["Version"]["Major"] = v.Version().major;
			j["System"]["Windows"]["Wine"]["Version"]["Minor"] = v.Version().minor;
			j["System"]["Windows"]["Wine"]["Version"]["Update"] = v.Version().update;
		}
		j["System"]["Windows"]["Wine"]["HostSysName"] = mpt::ToUnicode(mpt::Charset::UTF8, v.RawHostSysName());
	}
	const SoundDevice::Identifier deviceIdentifier = TrackerSettings::Instance().GetSoundDeviceIdentifier();
	const SoundDevice::Info deviceInfo = theApp.GetSoundDevicesManager()->FindDeviceInfo(deviceIdentifier);
	const SoundDevice::Settings deviceSettings = TrackerSettings::Instance().GetSoundDeviceSettings(deviceIdentifier);
	j["OpenMPT"]["SoundDevice"]["Type"] = deviceInfo.type;
	j["OpenMPT"]["SoundDevice"]["Name"] = deviceInfo.name;
	j["OpenMPT"]["SoundDevice"]["Settings"]["Samplerate"] = deviceSettings.Samplerate;
	j["OpenMPT"]["SoundDevice"]["Settings"]["Latency"] = deviceSettings.Latency;
	j["OpenMPT"]["SoundDevice"]["Settings"]["UpdateInterval"] = deviceSettings.UpdateInterval;
	j["OpenMPT"]["SoundDevice"]["Settings"]["Channels"] = deviceSettings.Channels.GetNumHostChannels();
	j["OpenMPT"]["SoundDevice"]["Settings"]["BoostThreadPriority"] = deviceSettings.BoostThreadPriority;
	j["OpenMPT"]["SoundDevice"]["Settings"]["ExclusiveMode"] = deviceSettings.ExclusiveMode;
	j["OpenMPT"]["SoundDevice"]["Settings"]["UseHardwareTiming"] = deviceSettings.UseHardwareTiming;
	j["OpenMPT"]["SoundDevice"]["Settings"]["KeepDeviceRunning"] = deviceSettings.KeepDeviceRunning;
	#ifdef MPT_ENABLE_ARCH_INTRINSICS
		#if MPT_ARCH_X86 || MPT_ARCH_AMD64
			const mpt::arch::current::cpu_info CPUInfo = mpt::arch::get_cpu_info();
			j["System"]["Processor"]["Vendor"] = mpt::ToUnicode(mpt::Charset::ASCII, CPUInfo.get_vendor_string());
			j["System"]["Processor"]["Brand"] = mpt::ToUnicode(mpt::Charset::ASCII, CPUInfo.get_brand_string());
			j["System"]["Processor"]["CpuidRaw"] = mpt::ufmt::hex0<8>(CPUInfo.get_cpuid());
			j["System"]["Processor"]["Id"]["Family"] = CPUInfo.get_family();
			j["System"]["Processor"]["Id"]["Model"] = CPUInfo.get_model();
			j["System"]["Processor"]["Id"]["Stepping"] = CPUInfo.get_stepping();
			j["System"]["Processor"]["Features"]["lm"] = CPUInfo.can_long_mode();
			j["System"]["Processor"]["Features"]["mmx"] = CPUInfo[mpt::arch::current::feature::mmx] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			j["System"]["Processor"]["Features"]["sse"] = CPUInfo[mpt::arch::current::feature::sse] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			j["System"]["Processor"]["Features"]["sse2"] = CPUInfo[mpt::arch::current::feature::sse2] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			j["System"]["Processor"]["Features"]["sse3"] = CPUInfo[mpt::arch::current::feature::sse3] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			j["System"]["Processor"]["Features"]["ssse3"] = CPUInfo[mpt::arch::current::feature::ssse3] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			j["System"]["Processor"]["Features"]["sse4.1"] = CPUInfo[mpt::arch::current::feature::sse4_1] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			j["System"]["Processor"]["Features"]["sse4.2"] = CPUInfo[mpt::arch::current::feature::sse4_2] && CPUInfo[mpt::arch::current::mode::xmm128sse];
			j["System"]["Processor"]["Features"]["avx"] = CPUInfo[mpt::arch::current::feature::avx] && CPUInfo[mpt::arch::current::mode::ymm256avx];
			j["System"]["Processor"]["Features"]["avx2"] = CPUInfo[mpt::arch::current::feature::avx2] && CPUInfo[mpt::arch::current::mode::ymm256avx];
			j["System"]["Processor"]["Virtual"] = CPUInfo.is_virtual();
		#endif // MPT_ARCH_X86 || MPT_ARCH_AMD64
	#endif // MPT_ENABLE_ARCH_INTRINSICS
	return j.dump(1, '\t');
}


// Run update check (independent thread)
UpdateCheckResult CUpdateCheck::SearchUpdate(const CUpdateCheck::Context &context, const CUpdateCheck::Settings &settings, const std::string &statistics)
{
	UpdateCheckResult result;
	{
		if(!context.window->SendMessage(context.msgProgress, context.autoUpdate ? 1 : 0, 0))
		{
			throw CUpdateCheck::Cancel();
		}
		if(!context.window->SendMessage(context.msgProgress, context.autoUpdate ? 1 : 0, 10))
		{
			throw CUpdateCheck::Cancel();
		}
		bool loaded = false;
		// try to load cached results before establishing any connection
		if(context.loadPersisted)
		{
			try
			{
				mpt::IO::InputFile f(settings.persistencePath + mpt::PathString::FromUTF8(OpenFMTUpdateCache));
				if(f.IsValid())
				{
					std::vector<std::byte> data = GetFileReader(f).ReadRawDataAsByteVector();
					FindNewerOpenFMTRelease(data);
					result.CheckTime = mpt::chrono::default_system_clock::time_point{};
					result.json = data;
					loaded = true;
				}
			} catch(mpt::out_of_memory e)
			{
				mpt::delete_out_of_memory(e);
			} catch(const std::exception &)
			{
				// ignore
			}
		}
		if(!context.window->SendMessage(context.msgProgress, context.autoUpdate ? 1 : 0, 20))
		{
			throw CUpdateCheck::Cancel();
		}
		if(!loaded)
		{
			HTTP::InternetSession internet(U_("OpenFMT/") + mpt::ToUnicode(mpt::Charset::ASCII, OPENFMT_VERSION_A));
			if(!context.window->SendMessage(context.msgProgress, context.autoUpdate ? 1 : 0, 30))
			{
				throw CUpdateCheck::Cancel();
			}
			result = SearchUpdateModern(internet, settings);
			try
			{
				mpt::IO::SafeOutputFile f(settings.persistencePath + mpt::PathString::FromUTF8(OpenFMTUpdateCache), std::ios::binary);
				f.stream().imbue(std::locale::classic());
				mpt::IO::WriteRaw(f.stream(), mpt::as_span(result.json));
				f.stream().flush();
			} catch(mpt::out_of_memory e)
			{
				mpt::delete_out_of_memory(e);
			} catch(const std::exception &)
			{
				// ignore
			}
			if(!context.window->SendMessage(context.msgProgress, context.autoUpdate ? 1 : 0, 50))
			{
				throw CUpdateCheck::Cancel();
			}
			// Статистику о системе OpenFMT никуда не отправляет.
			MPT_UNREFERENCED_PARAMETER(statistics);
			if(!context.window->SendMessage(context.msgProgress, context.autoUpdate ? 1 : 0, 70))
			{
				throw CUpdateCheck::Cancel();
			}
		}
		if(!context.window->SendMessage(context.msgProgress, context.autoUpdate ? 1 : 0, 90))
		{
			throw CUpdateCheck::Cancel();
		}
		CleanOldUpdates(settings, context);
		if(!context.window->SendMessage(context.msgProgress, context.autoUpdate ? 1 : 0, 100))
		{
			throw CUpdateCheck::Cancel();
		}
	}
	return result;
}


mpt::PathString CUpdateCheck::GetUpdateTempDirectory(bool portable)
{
	if(portable)
	{
		return theApp.GetInstallPath().WithTrailingSlash() + P_("Temp") + mpt::PathString::FromNative(mpt::RawPathString(1, mpt::PathString::GetDefaultPathSeparator()));
	} else
	{
		return mpt::common_directories::get_temp_directory();
	}
}


void CUpdateCheck::CleanOldUpdates(const CUpdateCheck::Settings & settings, const CUpdateCheck::Context & /* context */ )
{
	if(theApp.IsPortableMode())
	{
		CleanOldUpdates(GetUpdateTempDirectory(true));
		if(settings.previousVersion < MPT_V("1.31.02.02"))
		{
			CleanOldUpdates(GetUpdateTempDirectory(false));
		}
	} else
	{
		CleanOldUpdates(GetUpdateTempDirectory(false));
	}
}


void CUpdateCheck::CleanOldUpdates(mpt::PathString dirTemp)
{
	if(dirTemp.empty())
	{
		return;
	}
	if(PathIsRelative(dirTemp.AsNative().c_str()))
	{
		return;
	}
	if(!mpt::native_fs{}.is_directory(dirTemp))
	{
		return;
	}
	mpt::PathString dirTempOpenMPT = dirTemp + P_("OpenMPT") + mpt::PathString::FromNative(mpt::RawPathString(1, mpt::PathString::GetDefaultPathSeparator()));
	mpt::PathString dirTempOpenMPTUpdates = dirTempOpenMPT + P_("Updates") + mpt::PathString::FromNative(mpt::RawPathString(1, mpt::PathString::GetDefaultPathSeparator()));
	mpt::native_fs{}.delete_tree(dirTempOpenMPTUpdates);
}


void CUpdateCheck::SendStatistics(HTTP::InternetSession &internet, const CUpdateCheck::Settings &settings, const std::string &statistics)
{
	if(settings.sendStatistics)
	{
		HTTP::Request requestStatistics;
		if(settings.statisticsUUID.IsValid())
		{
			requestStatistics.SetURI(ParseURI(settings.apiURL + MPT_UFORMAT("statistics/{}")(settings.statisticsUUID)));
			requestStatistics.method = HTTP::Method::Put;
		} else
		{
			requestStatistics.SetURI(ParseURI(settings.apiURL + U_("statistics/")));
			requestStatistics.method = HTTP::Method::Post;
		}
		requestStatistics.dataMimeType = HTTP::MimeType::JSON();
		requestStatistics.acceptMimeTypes = HTTP::MimeTypes::JSON();
		std::string jsondata = statistics;
		MPT_LOG_GLOBAL(LogInformation, "Update", mpt::ToUnicode(mpt::Charset::UTF8, jsondata));
		requestStatistics.data = mpt::byte_cast<mpt::const_byte_span>(mpt::as_span(jsondata));
#if MPT_WIN_BEFORE(MPT_WIN_VISTA)
		requestStatistics.InsecureTLSDowngradeWindowsXP();
#endif // < MPT_WIN_VISTA
		internet(requestStatistics);
	}
}


UpdateCheckResult CUpdateCheck::SearchUpdateModern(HTTP::InternetSession &internet, const CUpdateCheck::Settings &settings)
{

	HTTP::Request request;
	request.SetURI(ParseURI(mpt::ToUnicode(mpt::Charset::ASCII, OpenFMTReleasesURL)));
	request.method = HTTP::Method::Get;
	request.acceptMimeTypes = HTTP::MimeTypes::JSON();
	request.flags = HTTP::NoCache;

#if MPT_WIN_BEFORE(MPT_WIN_VISTA)
	request.InsecureTLSDowngradeWindowsXP();
#endif // < MPT_WIN_VISTA
	HTTP::Result resultHTTP = internet(request);

	// Retrieve HTTP status code.
	if(resultHTTP.Status >= 400)
	{
		throw CUpdateCheck::Error(MPT_CFORMAT("Release information could not be loaded from GitHub (HTTP status code {}).")(resultHTTP.Status));
	}

	// Now, evaluate the downloaded data.
	UpdateCheckResult result;
	result.CheckTime = mpt::chrono::default_system_clock::now();
	try
	{
		FindNewerOpenFMTRelease(resultHTTP.Data);
		result.json = resultHTTP.Data;
	} catch(mpt::out_of_memory e)
	{
		mpt::rethrow_out_of_memory(e);
	}	catch(const nlohmann::json::exception &e)
	{
		throw CUpdateCheck::Error(MPT_CFORMAT("Could not understand the GitHub response ({}).")(mpt::get_exception_text<mpt::ustring>(e)));
	} catch(const std::exception &e)
	{
		throw CUpdateCheck::Error(MPT_CFORMAT("Could not understand the GitHub response ({}).")(mpt::get_exception_text<mpt::ustring>(e)));
	}

	return result;

}


void CUpdateCheck::CheckForUpdate(const CUpdateCheck::Settings &settings, const CUpdateCheck::Context &context)
{
	// incremented before starting the thread
	MPT_ASSERT(s_InstanceCount.load() >= 1);
	UpdateCheckResult result;
	try
	{
		context.window->SendMessage(context.msgStart, context.autoUpdate ? 1 : 0, 0);
		try
		{
			result = SearchUpdate(context, settings, context.statistics);
		} catch(const bad_uri &e)
		{
			throw CUpdateCheck::Error(MPT_CFORMAT("Error parsing update URL: {}")(mpt::get_exception_text<mpt::ustring>(e)));
		} catch(const HTTP::exception &e)
		{
			throw CUpdateCheck::Error(CString(_T("HTTP error: ")) + mpt::ToCString(e.GetMessage()));
		}
	} catch(const CUpdateCheck::Cancel &)
	{
		context.window->SendMessage(context.msgCanceled, context.autoUpdate ? 1 : 0, 0);
		s_InstanceCount.fetch_sub(1);
		MPT_ASSERT(s_InstanceCount.load() >= 0);
		return;
	} catch(const CUpdateCheck::Error &e)
	{
		context.window->SendMessage(context.msgFailure, context.autoUpdate ? 1 : 0, reinterpret_cast<LPARAM>(&e));
		s_InstanceCount.fetch_sub(1);
		MPT_ASSERT(s_InstanceCount.load() >= 0);
		return;
	}
	context.window->SendMessage(context.msgSuccess, context.autoUpdate ? 1 : 0, reinterpret_cast<LPARAM>(&result));
	s_InstanceCount.fetch_sub(1);
	MPT_ASSERT(s_InstanceCount.load() >= 0);
}


bool CUpdateCheck::IsAutoUpdateFromMessage(WPARAM wparam, LPARAM /* lparam */ )
{
	return wparam ? true : false;
}


const UpdateCheckResult &CUpdateCheck::MessageAsResult(WPARAM /* wparam */ , LPARAM lparam)
{
	return *reinterpret_cast<UpdateCheckResult *>(lparam);
}


const CUpdateCheck::Error &CUpdateCheck::MessageAsError(WPARAM /* wparam */ , LPARAM lparam)
{
	return *reinterpret_cast<CUpdateCheck::Error*>(lparam);
}



static const char updateScript_vbs[] = R"vbs(

Wscript.Echo
Wscript.Echo "OpenMPT portable Update"
Wscript.Echo "======================="

Wscript.Echo "[  0%] Waiting for OpenMPT to close..."
WScript.Sleep 2000

Wscript.Echo "[ 10%] Loading update settings..."
zip = WScript.Arguments.Item(0)
subfolder = WScript.Arguments.Item(1)
dst = WScript.Arguments.Item(2)
restartbinary = WScript.Arguments.Item(3)

Wscript.Echo "[ 20%] Preparing update..."
Set fso = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("Wscript.Shell")
Set application = CreateObject("Shell.Application")

Sub CreateFolder(pathname)
	If Not fso.FolderExists(pathname) Then
		fso.CreateFolder pathname
	End If
End Sub

Sub DeleteFolder(pathname)
	If fso.FolderExists(pathname) Then
		fso.DeleteFolder pathname
	End If
End Sub

Sub UnZIP(zipfilename, destinationfolder)
	If Not fso.FolderExists(destinationfolder) Then
		fso.CreateFolder(destinationfolder)
	End If
	application.NameSpace(destinationfolder).Copyhere application.NameSpace(zipfilename).Items, 16+256
End Sub

Wscript.Echo "[ 30%] Changing to temporary directory..."
shell.CurrentDirectory = fso.GetParentFolderName(WScript.ScriptFullName)

Wscript.Echo "[ 40%] Decompressing update..."
UnZIP zip, fso.BuildPath(fso.GetAbsolutePathName("."), "tmp")

Wscript.Echo "[ 60%] Installing update..."
If subfolder = "" Or subfolder = "." Then
	fso.CopyFolder fso.BuildPath(fso.GetAbsolutePathName("."), "tmp"), dst, True
Else
	fso.CopyFolder fso.BuildPath(fso.BuildPath(fso.GetAbsolutePathName("."), "tmp"), subfolder), dst, True
End If

Wscript.Echo "[ 80%] Deleting temporary directory..."
DeleteFolder fso.BuildPath(fso.GetAbsolutePathName("."), "tmp")

Wscript.Echo "[ 90%] Restarting OpenMPT..."
application.ShellExecute fso.BuildPath(dst, restartbinary), , dst, , 10

Wscript.Echo "[100%] Update successful!"
Wscript.Echo
WScript.Sleep 1000

Wscript.Echo "Closing update window in 5 seconds..."
WScript.Sleep 1000
Wscript.Echo "Closing update window in 4 seconds..."
WScript.Sleep 1000
Wscript.Echo "Closing update window in 3 seconds..."
WScript.Sleep 1000
Wscript.Echo "Closing update window in 2 seconds..."
WScript.Sleep 1000
Wscript.Echo "Closing update window in 1 seconds..."
WScript.Sleep 1000
Wscript.Echo "Closing update window..."

WScript.Quit

)vbs";


static const char updateScript_ps1[] = R"ps1(

param(
	[String]$zip="",
	[String]$subfolder="",
	[String]$dst="",
	[String]$restartbinary="")

Write-Output ""
Write-Output "OpenMPT portable Update"
Write-Output "======================="

Write-Output "[  0%] Waiting for OpenMPT to close..."
Start-Sleep -Seconds 2

Write-Output "[ 10%] Changing to temporary directory..."
Set-Location -Path (Split-Path -Parent $MyInvocation.MyCommand.Definition)

Write-Output "[ 20%] Decompressing update..."
Expand-Archive -Path $zip -DestinationPath (Join-Path -Path (Resolve-Path -Path ".") -ChildPath "tmp") -Force

Write-Output "[ 40%] Installing update..."
if (($subfolder -eq "") -or ($subfolder -eq ".")) {
	Copy-Item -Path (Join-Path -Path (Join-Path -Path (Resolve-Path -Path ".") -ChildPath "tmp") -ChildPath "*") -Destination $dst -Recurse -Force
} else {
	Copy-Item -Path (Join-Path -Path (Join-Path -Path (Join-Path -Path (Resolve-Path -Path ".") -ChildPath "tmp") -ChildPath $subfolder) -ChildPath "*") -Destination $dst -Recurse -Force
}

Write-Output "[ 60%] Deleting temporary directory..."
Remove-Item -Path (Join-Path -Path (Resolve-Path -Path ".") -ChildPath "tmp") -Recurse -Force

Write-Output "[ 80%] Restarting OpenMPT..."
Start-Process -FilePath (Join-Path -Path (Resolve-Path -Path $dst) -ChildPath $restartbinary)  -WorkingDirectory $dst

Write-Output "[100%] Update successful!"
Write-Output ""
Start-Sleep -Seconds 1

Write-Output "Closing update window in 5 seconds..."
Start-Sleep -Seconds 1
Write-Output "Closing update window in 4 seconds..."
Start-Sleep -Seconds 1
Write-Output "Closing update window in 3 seconds..."
Start-Sleep -Seconds 1
Write-Output "Closing update window in 2 seconds..."
Start-Sleep -Seconds 1
Write-Output "Closing update window in 1 seconds..."
Start-Sleep -Seconds 1
Write-Output "Closing update window..."

)ps1";


class CDoUpdate: public CProgressDialog
{
private:
	Update::download download;
	class Aborted : public std::exception {};
	class Warning : public mpt::runtime_error
	{
	public:
		Warning(const mpt::ustring &msg)
			: mpt::runtime_error(msg)
		{
			return;
		}
	};
	class Error : public mpt::runtime_error
	{
	public:
		Error(const mpt::ustring &msg)
			: mpt::runtime_error(msg)
		{
			return;
		}
	};
public:
	CDoUpdate(Update::download download, CWnd *parent = nullptr)
		: CProgressDialog(parent)
		, download(download)
	{
		return;
	}
	void UpdateProgress(const CString &text, double percent)
	{
		SetText(text);
		SetProgress(static_cast<uint64>(percent * 100.0));
		ProcessMessages();
		if(m_abort)
		{
			throw Aborted();
		}
	}
	void Run() override
	{
		try
		{
			SetTitle(_T("OpenMPT Update"));
			SetAbortText(_T("Cancel"));
			SetText(_T("OpenMPT Update"));
			SetRange(0, 10000);
			ProcessMessages();

			Update::downloadinfo downloadinfo;
			mpt::PathString dirTempOpenMPTUpdates;
			mpt::PathString updateFilename;
			{

				UpdateProgress(_T("Connecting..."), 0.0);
				HTTP::InternetSession internet(U_("OpenFMT/") + mpt::ToUnicode(mpt::Charset::ASCII, OPENFMT_VERSION_A));

				UpdateProgress(_T("Downloading update information..."), 1.0);
				std::vector<std::byte> rawDownloadInfo;
				{
					HTTP::Request request;
					request.SetURI(ParseURI(download.url));
					request.method = HTTP::Method::Get;
					request.acceptMimeTypes = HTTP::MimeTypes::JSON();
#if MPT_WIN_BEFORE(MPT_WIN_VISTA)
					request.InsecureTLSDowngradeWindowsXP();
#endif // < MPT_WIN_VISTA
					HTTP::Result resultHTTP = internet(request);
					if(resultHTTP.Status != 200)
					{
						throw Error(MPT_UFORMAT("Error downloading update information: HTTP status {}.")(resultHTTP.Status));
					}
					rawDownloadInfo = std::move(resultHTTP.Data);
				}

				if(!TrackerSettings::Instance().UpdateSkipSignatureVerificationUNSECURE)
				{
					std::vector<std::byte> rawSignature;
					UpdateProgress(_T("Retrieving update signature..."), 2.0);
					{
						HTTP::Request request;
						request.SetURI(ParseURI(download.url + U_(".jws.json")));
						request.method = HTTP::Method::Get;
						request.acceptMimeTypes = HTTP::MimeTypes::JSON();
#if MPT_WIN_BEFORE(MPT_WIN_VISTA)
						request.InsecureTLSDowngradeWindowsXP();
#endif // < MPT_WIN_VISTA
						HTTP::Result resultHTTP = internet(request);
						if(resultHTTP.Status != 200)
						{
							throw Error(MPT_UFORMAT("Error downloading update signature: HTTP status {}.")(resultHTTP.Status));
						}
						rawSignature = std::move(resultHTTP.Data);
					}
					UpdateProgress(_T("Retrieving update signing public keys..."), 3.0);
					std::vector<mpt::crypto::asymmetric::rsassa_pss<>::public_key> keys;
					{
						std::vector<mpt::ustring> keyAnchors = TrackerSettings::Instance().UpdateSigningKeysRootAnchors;
						if(keyAnchors.empty())
						{
							Reporting::Warning(U_("Warning: No update signing public key root anchors configured. Update cannot be verified."), U_("OpenMPT Update"));
						}
						for(const auto & keyAnchor : keyAnchors)
						{
							HTTP::Request request;
							request.SetURI(ParseURI(keyAnchor + U_("signingkeys.jwkset.json")));
							request.method = HTTP::Method::Get;
							request.flags = HTTP::NoCache;
							request.acceptMimeTypes = HTTP::MimeTypes::JSON();
							try
							{
#if MPT_WIN_BEFORE(MPT_WIN_VISTA)
								request.InsecureTLSDowngradeWindowsXP();
#endif // < MPT_WIN_VISTA
								HTTP::Result resultHTTP = internet(request);
								resultHTTP.CheckStatus(200);
								mpt::append(keys, mpt::crypto::asymmetric::rsassa_pss<>::parse_jwk_set(mpt::ToUnicode(mpt::Charset::UTF8, mpt::buffer_cast<std::string>(resultHTTP.Data))));
							} catch(mpt::out_of_memory e)
							{
								mpt::rethrow_out_of_memory(e);
							} catch(const std::exception &e)
							{
								Reporting::Warning(MPT_UFORMAT("Warning: Retrieving update signing public keys from {} failed: {}")(keyAnchor, mpt::get_exception_text<mpt::ustring>(e)), U_("OpenMPT Update"));
							} catch(...)
							{
								Reporting::Warning(MPT_UFORMAT("Warning: Retrieving update signing public keys from {} failed.")(keyAnchor), U_("OpenMPT Update"));
							}
						}
					}
					if(keys.empty())
					{
						throw Error(U_("Error retrieving update signing public keys."));
					}
					UpdateProgress(_T("Verifying signature..."), 4.0);
					std::vector<std::byte> expectedPayload = mpt::buffer_cast<std::vector<std::byte>>(rawDownloadInfo);
					mpt::ustring signature = mpt::ToUnicode(mpt::Charset::UTF8, mpt::buffer_cast<std::string>(rawSignature));

					mpt::crypto::asymmetric::rsassa_pss<>::jws_verify_at_least_one(keys, expectedPayload, signature);
			
				}

				UpdateProgress(_T("Parsing update information..."), 5.0);
				try
				{
					downloadinfo = nlohmann::json::parse(mpt::buffer_cast<std::string>(rawDownloadInfo)).get<Update::downloadinfo>();
				}	catch(const nlohmann::json::exception &e)
				{
					throw Error(MPT_UFORMAT("Error parsing update information: {}.")(mpt::get_exception_text<mpt::ustring>(e)));
				}

				UpdateProgress(_T("Preparing download..."), 6.0);
				mpt::PathString dirTemp = CUpdateCheck::GetUpdateTempDirectory(theApp.IsPortableMode());
				if(theApp.IsPortableMode())
				{
					::CreateDirectory(mpt::support_long_path(dirTemp.AsNative()).c_str(), NULL);
				}
				mpt::PathString dirTempOpenMPT = dirTemp + P_("OpenMPT") + mpt::PathString::FromNative(mpt::RawPathString(1, mpt::PathString::GetDefaultPathSeparator()));
				dirTempOpenMPTUpdates = dirTempOpenMPT + P_("Updates") + mpt::PathString::FromNative(mpt::RawPathString(1, mpt::PathString::GetDefaultPathSeparator()));
				updateFilename = dirTempOpenMPTUpdates + mpt::PathString::FromUnicode(downloadinfo.filename);
				::CreateDirectory(mpt::support_long_path(dirTempOpenMPT.AsNative()).c_str(), NULL);
				::CreateDirectory(mpt::support_long_path(dirTempOpenMPTUpdates.AsNative()).c_str(), NULL);

				{
			
					UpdateProgress(_T("Creating file..."), 7.0);
					mpt::IO::SafeOutputFile file(updateFilename, std::ios::binary);
					file.stream().imbue(std::locale::classic());
					file.stream().exceptions(std::ios::failbit | std::ios::badbit);
				
					UpdateProgress(_T("Downloading update..."), 8.0);
					HTTP::Request request;
					request.SetURI(ParseURI(downloadinfo.url));
					request.method = HTTP::Method::Get;
					request.acceptMimeTypes = HTTP::MimeTypes::Binary();
					request.outputStream = &file.stream();
					request.progressCallback = [&](HTTP::Progress progress, uint64 transferred, std::optional<uint64> expectedSize) {
						switch(progress)
						{
						case HTTP::Progress::Start:
							SetProgress(900);
							break;
						case HTTP::Progress::ConnectionEstablished:
							SetProgress(1000);
							break;
						case HTTP::Progress::RequestOpened:
							SetProgress(1100);
							break;
						case HTTP::Progress::RequestSent:
							SetProgress(1200);
							break;
						case HTTP::Progress::ResponseReceived:
							SetProgress(1300);
							break;
						case HTTP::Progress::TransferBegin:
							SetProgress(1400);
							break;
						case HTTP::Progress::TransferRunning:
							if(expectedSize && ((*expectedSize) != 0))
							{
								SetProgress(static_cast<int64>((static_cast<double>(transferred) / static_cast<double>(*expectedSize)) * (10000.0-1500.0-400.0) + 1500.0));
							} else
							{
								SetProgress((1500 + 9600) / 2);
							}
							break;
						case HTTP::Progress::TransferDone:
							SetProgress(9600);
							break;
						}
						ProcessMessages();
						if(m_abort)
						{
							throw HTTP::Abort();
						}
					};
#if MPT_WIN_BEFORE(MPT_WIN_VISTA)
					request.InsecureTLSDowngradeWindowsXP();
#endif // < MPT_WIN_VISTA
					HTTP::Result resultHTTP = internet(request);
					if(resultHTTP.Status != 200)
					{
						throw Error(MPT_UFORMAT("Error downloading update: HTTP status {}.")(resultHTTP.Status));
					}
				}

				UpdateProgress(_T("Disconnecting..."), 97.0);
			}

			UpdateProgress(_T("Verifying download..."), 98.0);
			bool verified = false;
			for(const auto & [algorithm, value] : downloadinfo.checksums)
			{
				if(algorithm == U_("SHA-512"))
				{
					std::vector<std::byte> binhash = mpt::decode_hex(value);
					if(binhash.size() != 512/8)
					{
						throw Error(U_("Download verification failed."));
					}
					std::array<std::byte, 512/8> expected;
					std::copy(binhash.begin(), binhash.end(), expected.begin());
					mpt::crypto::hash::SHA512 hash;
					mpt::IO::ifstream f(updateFilename, std::ios::binary);
					f.imbue(std::locale::classic());
					f.exceptions(std::ios::badbit);
					while(!mpt::IO::IsEof(f))
					{
						std::array<std::byte, mpt::IO::BUFFERSIZE_TINY> buf;
						hash.process(mpt::IO::ReadRaw(f, mpt::as_span(buf)));
					}
					std::array<std::byte, 512/8> gotten = hash.result();
					if(gotten != expected)
					{
						throw Error(U_("Download verification failed."));
					}
					verified = true;
				}
			}
			if(!verified)
			{
				throw Error(U_("Error verifying update: No suitable checksum found."));
			}

			UpdateProgress(_T("Installing update..."), 99.0);
			bool wantClose = false;
			if(download.can_autoupdate && (Version::Current() >= Version::Parse(download.autoupdate_minversion)))
			{
				if(download.type == U_("installer") && downloadinfo.autoupdate_installer)
				{
					if(theApp.IsSourceTreeMode())
					{
						throw Warning(MPT_UFORMAT("Refusing to launch update '{} {}' when running from source tree.")(updateFilename, mpt::join_format(downloadinfo.autoupdate_installer->arguments, U_(" "))));
					}
					if(mpt::OS::Windows::IsOriginal() && mpt::osinfo::windows::Version::Current().IsBefore(mpt::osinfo::windows::Version::WinVista))
					{
						wantClose = true;
						Reporting::Information(U_("OpenMPT cannot automatically restart itself after the update is finished on Windows XP. You will need to restart it manually."), U_("OpenMPT Update"));
					}
					if(reinterpret_cast<INT_PTR>(ShellExecute(NULL, NULL,
						updateFilename.AsNative().c_str(),
						mpt::ToWin(mpt::join_format(downloadinfo.autoupdate_installer->arguments, U_(" "))).c_str(),
						dirTempOpenMPTUpdates.AsNative().c_str(),
						SW_SHOWDEFAULT)) < 32)
					{
						throw Error(U_("Error launching update."));
					}
				} else if(download.type == U_("archive") && downloadinfo.autoupdate_archive)
				{
					bool usePowerShell = mpt::osinfo::windows::Version::Current().IsAtLeast(mpt::osinfo::windows::Version::Win10, 22000);
					switch(TrackerSettings::Instance().UpdatePortableBackend)
					{
						case 1:
							usePowerShell = false;
							break;
						case 2:
							usePowerShell = true;
							break;
						default:
							// nothing
							break;
					}
					if(usePowerShell)
					{
						try
						{
							mpt::IO::SafeOutputFile file(dirTempOpenMPTUpdates + P_("update.ps1"), std::ios::binary);
							file.stream().imbue(std::locale::classic());
							file.stream().exceptions(std::ios::failbit | std::ios::badbit);
							mpt::IO::WriteRaw(file.stream(), mpt::as_span(std::string(updateScript_ps1)));
						} catch(...)
						{
							throw Error(U_("Error creating update script."));
						}
						std::vector<mpt::ustring> arguments = {
							U_("-NoLogo"),
							U_("-ExecutionPolicy"), U_("Unrestricted"),
							U_("\"") + (dirTempOpenMPTUpdates + P_("update.ps1")).ToUnicode() + U_("\""),
							U_("-zip"), U_("\"") + updateFilename.ToUnicode() + U_("\""),
							U_("-subfolder"), U_("\"") + (downloadinfo.autoupdate_archive->subfolder.empty() ? U_(".") : downloadinfo.autoupdate_archive->subfolder) + U_("\""),
							U_("-dst"), U_("\"") + theApp.GetInstallPath().WithoutTrailingSlash().ToUnicode() + U_("\""),
							U_("-restartbinary"), U_("\"") + downloadinfo.autoupdate_archive->restartbinary + U_("\"")
						};
						if(theApp.IsSourceTreeMode())
						{
							throw Warning(MPT_UFORMAT("Refusing to launch update '{} {}' when running from source tree.")(P_("cscript.exe"), mpt::join_format(arguments, U_(" "))));
						}
						if(reinterpret_cast<INT_PTR>(ShellExecute(NULL, NULL,
							P_("PowerShell.exe").AsNative().c_str(),
							mpt::ToWin(mpt::join_format(arguments, U_(" "))).c_str(),
							dirTempOpenMPTUpdates.AsNative().c_str(),
							SW_SHOWDEFAULT)) < 32)
						{
							throw Error(U_("Error launching update."));
						}
					} else
					{
						try
						{
							mpt::IO::SafeOutputFile file(dirTempOpenMPTUpdates + P_("update.vbs"), std::ios::binary);
							file.stream().imbue(std::locale::classic());
							file.stream().exceptions(std::ios::failbit | std::ios::badbit);
							mpt::IO::WriteRaw(file.stream(), mpt::as_span(std::string(updateScript_vbs)));
						} catch(...)
						{
							throw Error(U_("Error creating update script."));
						}
						std::vector<mpt::ustring> arguments;
						arguments.push_back(U_("\"") + (dirTempOpenMPTUpdates + P_("update.vbs")).ToUnicode() + U_("\""));
						arguments.push_back(U_("\"") + updateFilename.ToUnicode() + U_("\""));
						arguments.push_back(U_("\"") + (downloadinfo.autoupdate_archive->subfolder.empty() ? U_(".") : downloadinfo.autoupdate_archive->subfolder) + U_("\""));
						arguments.push_back(U_("\"") + theApp.GetInstallPath().WithoutTrailingSlash().ToUnicode() + U_("\""));
						arguments.push_back(U_("\"") + downloadinfo.autoupdate_archive->restartbinary + U_("\""));
						if(theApp.IsSourceTreeMode())
						{
							throw Warning(MPT_UFORMAT("Refusing to launch update '{} {}' when running from source tree.")(P_("cscript.exe"), mpt::join_format(arguments, U_(" "))));
						}
						if(reinterpret_cast<INT_PTR>(ShellExecute(NULL, NULL,
							P_("cscript.exe").AsNative().c_str(),
							mpt::ToWin(mpt::join_format(arguments, U_(" "))).c_str(),
							dirTempOpenMPTUpdates.AsNative().c_str(),
							SW_SHOWDEFAULT)) < 32)
						{
							throw Error(U_("Error launching update."));
						}
					}
					wantClose = true;
				} else
				{
					CTrackApp::OpenDirectory(dirTempOpenMPTUpdates);
					wantClose = true;
				}
			} else
			{
				CTrackApp::OpenDirectory(dirTempOpenMPTUpdates);
				wantClose = true;
			}
			UpdateProgress(_T("Waiting for installer..."), 100.0);
			if(wantClose)
			{
				CMainFrame::GetMainFrame()->PostMessage(WM_QUIT, 0, 0);
			}
			EndDialog(IDOK);
		} catch(mpt::out_of_memory e)
		{
			mpt::delete_out_of_memory(e);
			Reporting::Error(U_("Not enough memory to install update."), U_("OpenMPT Update Error"));
			EndDialog(IDCANCEL);
			return;
		} catch(const HTTP::Abort &)
		{
			EndDialog(IDCANCEL);
			return;
		} catch(const Aborted &)
		{
			EndDialog(IDCANCEL);
			return;
		} catch(const Warning &e)
		{
			Reporting::Warning(mpt::get_exception_text<mpt::ustring>(e), U_("OpenMPT Update"));
			EndDialog(IDCANCEL);
			return;
		} catch(const Error &e)
		{
			Reporting::Error(mpt::get_exception_text<mpt::ustring>(e), U_("OpenMPT Update Error"));
			EndDialog(IDCANCEL);
			return;
		} catch(const std::exception &e)
		{
			Reporting::Error(MPT_UFORMAT("Error installing update: {}")(mpt::get_exception_text<mpt::ustring>(e)), U_("OpenMPT Update Error"));
			EndDialog(IDCANCEL);
			return;
		} catch(...)
		{
			Reporting::Error(U_("Error installing update."), U_("OpenMPT Update Error"));
			EndDialog(IDCANCEL);
			return;
		}
	}
};


void CUpdateCheck::AcknowledgeSuccess(const UpdateCheckResult &result)
{
	if(!result.IsFromCache())
	{
		TrackerSettings::Instance().UpdateLastUpdateCheck = result.CheckTime;
	}
}


// Установка обновления OpenFMT. Скачиваем пакет выпуска с GitHub, сверяем
// SHA-256 с файлом суммы того же выпуска и отдаём сценарию PowerShell: он
// ждёт, пока OpenFMT закроется, сохраняет заменяемые файлы, распаковывает
// новые поверх папки программы, при ошибке возвращает старые и запускает
// OpenFMT снова. Настроек (mptrack.ini и прочего) в пакете нет — их не трогаем.
static const char OpenFMTUpdateScript[] = R"ps1(
param(
	[String]$zip = "",
	[String]$subfolder = "",
	[String]$dst = "",
	[String]$restartbinary = "OpenFMT.exe",
	[Int32]$waitpid = 0)

$ErrorActionPreference = "Stop"
Write-Output "OpenFMT update"
Write-Output "=============="
Write-Output ""
Write-Output "Waiting for OpenFMT to close..."
if ($waitpid -ne 0) {
	$deadline = (Get-Date).AddMinutes(10)
	while (Get-Process -Id $waitpid -ErrorAction SilentlyContinue) {
		if ((Get-Date) -gt $deadline) {
			Write-Output "OpenFMT did not close. The update was not installed."
			Start-Sleep -Seconds 5
			exit 1
		}
		Start-Sleep -Milliseconds 500
	}
}
Start-Sleep -Seconds 1

$work = Split-Path -Parent $MyInvocation.MyCommand.Definition
$tmp = Join-Path $work "tmp"
$backup = Join-Path $work "backup"
foreach ($dir in @($tmp, $backup)) { if (Test-Path $dir) { Remove-Item $dir -Recurse -Force } }

Write-Output "Unpacking..."
Expand-Archive -Path $zip -DestinationPath $tmp -Force
$src = $tmp
if ($subfolder -ne "" -and $subfolder -ne ".") { $src = Join-Path $tmp $subfolder }
$src = (Resolve-Path $src).Path
$dst = (Resolve-Path $dst).Path

Write-Output "Backing up the files that will be replaced..."
$files = Get-ChildItem -Path $src -Recurse -File
foreach ($file in $files) {
	$rel = $file.FullName.Substring($src.Length).TrimStart('\')
	$old = Join-Path $dst $rel
	if (Test-Path $old) {
		$keep = Join-Path $backup $rel
		New-Item -ItemType Directory -Force -Path (Split-Path -Parent $keep) | Out-Null
		Copy-Item -Path $old -Destination $keep -Force
	}
}

Write-Output "Installing..."
try {
	foreach ($file in $files) {
		$rel = $file.FullName.Substring($src.Length).TrimStart('\')
		$new = Join-Path $dst $rel
		New-Item -ItemType Directory -Force -Path (Split-Path -Parent $new) | Out-Null
		Copy-Item -Path $file.FullName -Destination $new -Force
	}
} catch {
	Write-Output "Installing failed: $_"
	Write-Output "Restoring the previous version..."
	if (Test-Path $backup) { Copy-Item -Path (Join-Path $backup "*") -Destination $dst -Recurse -Force }
	Write-Output "The previous version was restored."
	Start-Sleep -Seconds 10
	exit 1
}

Write-Output "Cleaning up..."
foreach ($dir in @($tmp, $backup)) { if (Test-Path $dir) { Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue } }
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Remove-Item $MyInvocation.MyCommand.Path -Force -ErrorAction SilentlyContinue

Write-Output "Starting OpenFMT..."
Start-Process -FilePath (Join-Path $dst $restartbinary) -WorkingDirectory $dst
Write-Output "Done."
Start-Sleep -Seconds 2
)ps1";


class COpenFMTUpdate : public CProgressDialog
{
	OpenFMTRelease m_release;
	class Failure : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};
	class Aborted : public std::exception {};

	void Step(const TCHAR *text, int permille)
	{
		SetText(text);
		SetProgress(permille);
		ProcessMessages();
		if(m_abort)
			throw Aborted();
	}

	std::vector<std::byte> Fetch(HTTP::InternetSession &internet, const std::string &url)
	{
		HTTP::Request request;
		request.SetURI(ParseURI(mpt::ToUnicode(mpt::Charset::UTF8, url)));
		request.method = HTTP::Method::Get;
		request.acceptMimeTypes = HTTP::MimeTypes::Binary();
		HTTP::Result result = internet(request);
		if(result.Status != 200)
			throw Failure("HTTP status " + std::to_string(result.Status) + " for " + url);
		return std::move(result.Data);
	}

public:
	COpenFMTUpdate(OpenFMTRelease release, CWnd *parent)
		: CProgressDialog(parent), m_release(std::move(release)) {}

	void Run() override
	{
		try
		{
			SetTitle(_T("OpenFMT Update"));
			SetAbortText(_T("Cancel"));
			SetRange(0, 1000);
			if(theApp.IsSourceTreeMode())
				throw Failure("OpenFMT is running from its source tree; it will not update itself there.");

			// Писать в папку программы должно быть можно — иначе (Program
			// Files и т.п.) ставить некуда, остаётся страница загрузки.
			const mpt::PathString installDir = theApp.GetInstallPath().WithTrailingSlash();
			{
				const mpt::PathString probe = installDir + P_("openfmt-update-write-test.tmp");
				HANDLE f = ::CreateFile(probe.AsNative().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
				if(f == INVALID_HANDLE_VALUE)
					throw Failure("OpenFMT cannot write to its own folder, so it cannot update itself there. Please download the new version from the release page.");
				::CloseHandle(f);
			}

			Step(_T("Connecting..."), 10);
			HTTP::InternetSession internet(U_("OpenFMT/") + mpt::ToUnicode(mpt::Charset::ASCII, OPENFMT_VERSION_A));

			Step(_T("Downloading checksum..."), 20);
			const auto checksumText = mpt::buffer_cast<std::string>(Fetch(internet, m_release.sha256URL));
			if(checksumText.size() < 64)
				throw Failure("The checksum file of the release is not valid.");
			const std::vector<std::byte> expected = mpt::decode_hex(mpt::ToUnicode(mpt::Charset::ASCII, checksumText.substr(0, 64)));
			if(expected.size() != 32)
				throw Failure("The checksum file of the release is not valid.");

			const mpt::PathString tempDir = CUpdateCheck::GetUpdateTempDirectory(theApp.IsPortableMode());
			::CreateDirectory(mpt::support_long_path(tempDir.AsNative()).c_str(), nullptr);
			const mpt::PathString workDir = tempDir + P_("OpenFMT-Update") + mpt::PathString::FromNative(mpt::RawPathString(1, mpt::PathString::GetDefaultPathSeparator()));
			::CreateDirectory(mpt::support_long_path(workDir.AsNative()).c_str(), nullptr);
			const mpt::PathString zipPath = workDir + mpt::PathString::FromUTF8(m_release.zipName);

			Step(_T("Downloading OpenFMT..."), 30);
			{
				mpt::IO::SafeOutputFile file(zipPath, std::ios::binary);
				file.stream().imbue(std::locale::classic());
				file.stream().exceptions(std::ios::failbit | std::ios::badbit);
				HTTP::Request request;
				request.SetURI(ParseURI(mpt::ToUnicode(mpt::Charset::UTF8, m_release.zipURL)));
				request.method = HTTP::Method::Get;
				request.acceptMimeTypes = HTTP::MimeTypes::Binary();
				request.outputStream = &file.stream();
				request.progressCallback = [&](HTTP::Progress progress, uint64 transferred, std::optional<uint64> expectedSize)
				{
					if(progress == HTTP::Progress::TransferRunning && expectedSize && *expectedSize)
						SetProgress(30 + static_cast<int64>(900.0 * transferred / *expectedSize));
					ProcessMessages();
					if(m_abort)
						throw HTTP::Abort();
				};
				HTTP::Result result = internet(request);
				if(result.Status != 200)
					throw Failure("HTTP status " + std::to_string(result.Status) + " while downloading the update.");
			}

			Step(_T("Verifying download..."), 940);
			{
				mpt::crypto::hash::SHA256 hash;
				mpt::IO::ifstream f(zipPath, std::ios::binary);
				f.imbue(std::locale::classic());
				f.exceptions(std::ios::badbit);
				while(!mpt::IO::IsEof(f))
				{
					std::array<std::byte, mpt::IO::BUFFERSIZE_TINY> buf;
					hash.process(mpt::IO::ReadRaw(f, mpt::as_span(buf)));
				}
				const auto gotten = hash.result();
				if(!std::equal(gotten.begin(), gotten.end(), expected.begin(), expected.end()))
				{
					::DeleteFile(zipPath.AsNative().c_str());
					throw Failure("The downloaded file does not match the release checksum. Nothing was changed.");
				}
			}

			Step(_T("Starting the installer..."), 970);
			const mpt::PathString scriptPath = workDir + P_("update.ps1");
			{
				mpt::IO::SafeOutputFile file(scriptPath, std::ios::binary);
				file.stream().imbue(std::locale::classic());
				file.stream().exceptions(std::ios::failbit | std::ios::badbit);
				mpt::IO::WriteRaw(file.stream(), mpt::as_span(std::string(OpenFMTUpdateScript)));
			}
			std::string subfolder = m_release.zipName;
			if(subfolder.size() > 4)
				subfolder.erase(subfolder.size() - 4);
			const mpt::ustring arguments = U_("-NoLogo -ExecutionPolicy Bypass -File \"") + scriptPath.ToUnicode()
				+ U_("\" -zip \"") + zipPath.ToUnicode()
				+ U_("\" -subfolder \"") + mpt::ToUnicode(mpt::Charset::UTF8, subfolder)
				+ U_("\" -dst \"") + theApp.GetInstallPath().WithoutTrailingSlash().ToUnicode()
				+ U_("\" -restartbinary \"OpenFMT.exe\" -waitpid ") + mpt::ufmt::val(::GetCurrentProcessId());
			if(reinterpret_cast<INT_PTR>(::ShellExecute(nullptr, nullptr, P_("PowerShell.exe").AsNative().c_str(), mpt::ToWin(arguments).c_str(), workDir.AsNative().c_str(), SW_SHOWDEFAULT)) < 32)
				throw Failure("Could not start the installer (PowerShell).");

			Step(_T("Closing OpenFMT..."), 1000);
			// Закрываемся по-обычному, с вопросом о несохранённых песнях. Если
			// закрытие отменить, сценарий подождёт и выйдет, ничего не меняя.
			CMainFrame::GetMainFrame()->PostMessage(WM_CLOSE);
			EndDialog(IDOK);
		} catch(const HTTP::Abort &)
		{
			EndDialog(IDCANCEL);
		} catch(const Aborted &)
		{
			EndDialog(IDCANCEL);
		} catch(const std::exception &e)
		{
			Reporting::Error(mpt::ToUnicode(mpt::Charset::UTF8, e.what()), U_("OpenFMT Update"));
			EndDialog(IDCANCEL);
		}
	}
};


void CUpdateCheck::ShowSuccessGUI(const bool autoUpdate, const UpdateCheckResult &result)
{
	// OpenFMT: выпуски с GitHub. Сами ничего не скачиваем и не ставим —
	// показываем, что вышло, и открываем страницу выпуска.
	bool modal = !autoUpdate;
	std::optional<OpenFMTRelease> release;
	try
	{
		release = FindNewerOpenFMTRelease(result.json);
	} catch(const std::exception &)
	{
		if(modal)
			Reporting::Error(_T("Could not understand the release information from GitHub."), _T("OpenFMT Update"));
		return;
	}
	if(!release)
	{
		if(modal)
			Reporting::Information(U_("You already have the latest version of OpenFMT."), U_("OpenFMT Update"));
		return;
	}
	const CString version = mpt::ToCString(mpt::Charset::UTF8, release->version);
	const CString url = mpt::ToCString(mpt::Charset::UTF8, release->url);
	if(autoUpdate && version == TrackerSettings::Instance().UpdateIgnoreVersion.Get())
		return;
	// Значок на панели — всегда; всплывающую подсказку не показываем, если
	// сейчас откроется окно или результат взят из кэша.
	if(!CMainFrame::GetMainFrame()->ShowUpdateIndicator(result, version, url, !modal && !result.IsFromCache()))
		modal = true;
	if(!modal)
		return;
	const bool canInstall = !release->zipURL.empty();
	UpdateDialog dlg(version, mpt::ToCString(mpt::Charset::UTF8, release->date), url, canInstall ? _T("&Install Update") : _T("&Open Download Page"));
	if(dlg.DoModal() != IDOK)
		return;
	if(!canInstall)
	{
		CTrackApp::OpenURL(url);
		return;
	}
	COpenFMTUpdate installer(*release, theApp.GetMainWnd());
	installer.DoModal();
}


void CUpdateCheck::ShowFailureGUI(const bool autoUpdate, const CUpdateCheck::Error &error)
{
	if(!autoUpdate)
	{
		Reporting::Error(error.GetMessage(), _T("OpenFMT Update Error"));
	}
}


CUpdateCheck::Error::Error(CString errorMessage)
	: std::runtime_error(mpt::transcode<std::string>(mpt::exception_encoding, errorMessage))
	, m_Message(errorMessage)
{
	return;
}


CUpdateCheck::Error::Error(CString errorMessage, DWORD errorCode)
	: std::runtime_error(mpt::transcode<std::string>(mpt::exception_encoding, FormatErrorCode(errorMessage, errorCode)))
	, m_Message(errorMessage)
{
	return;
}


CString CUpdateCheck::Error::GetMessage() const
{
	return m_Message;
}


CString CUpdateCheck::Error::FormatErrorCode(CString errorMessage, DWORD errorCode)
{
	errorMessage += mpt::ToCString(mpt::windows::GetErrorMessage(errorCode, GetModuleHandle(TEXT("wininet.dll"))));
	return errorMessage;
}



CUpdateCheck::Cancel::Cancel()
{
	return;
}


/////////////////////////////////////////////////////////////
// CUpdateSetupDlg

BEGIN_MESSAGE_MAP(CUpdateSetupDlg, CPropertyPage)
	ON_COMMAND(IDC_CHECK_UPDATEENABLED,         &CUpdateSetupDlg::OnSettingsChanged)
	ON_COMMAND(IDC_RADIO1,                      &CUpdateSetupDlg::OnSettingsChanged)
	ON_COMMAND(IDC_RADIO2,                      &CUpdateSetupDlg::OnSettingsChanged)
	ON_COMMAND(IDC_RADIO3,                      &CUpdateSetupDlg::OnSettingsChanged)
	ON_COMMAND(IDC_BUTTON1,                     &CUpdateSetupDlg::OnCheckNow)
	ON_CBN_SELCHANGE(IDC_COMBO_UPDATEFREQUENCY, &CUpdateSetupDlg::OnSettingsChanged)
	ON_COMMAND(IDC_CHECK_UPDATEINSTALLAUTOMATICALLY, &CUpdateSetupDlg::OnSettingsChanged)
	ON_COMMAND(IDC_CHECK1,                      &CUpdateSetupDlg::OnSettingsChanged)
	ON_NOTIFY(NM_CLICK, IDC_SYSLINK1,           &CUpdateSetupDlg::OnShowStatisticsData)
END_MESSAGE_MAP()


CUpdateSetupDlg::CUpdateSetupDlg()
	: CPropertyPage(IDD_OPTIONS_UPDATE)
	, m_SettingChangedNotifyGuard(theApp.GetSettings(), TrackerSettings::Instance().UpdateLastUpdateCheck.GetPath())
{
	return;
}


void CUpdateSetupDlg::DoDataExchange(CDataExchange *pDX)
{
	CPropertyPage::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_COMBO_UPDATEFREQUENCY, m_CbnUpdateFrequency);
}


BOOL CUpdateSetupDlg::OnInitDialog()
{
	CPropertyPage::OnInitDialog();

	CheckDlgButton(IDC_CHECK_UPDATEENABLED, TrackerSettings::Instance().UpdateEnabled ? BST_CHECKED : BST_UNCHECKED);

	int radioID = 0;
	uint32 updateChannel = TrackerSettings::Instance().UpdateChannel;
	if(updateChannel == UpdateChannelRelease)
	{
		radioID = IDC_RADIO1;
	} else if(updateChannel == UpdateChannelNext)
	{
		radioID = IDC_RADIO2;
	} else if(updateChannel == UpdateChannelDevelopment)
	{
		radioID = IDC_RADIO3;
	} else
	{
		radioID = IDC_RADIO1;
	}
	CheckRadioButton(IDC_RADIO1, IDC_RADIO3, radioID);

	int32 periodDays = TrackerSettings::Instance().UpdateIntervalDays;
	int ndx;

	ndx = m_CbnUpdateFrequency.AddString(_T("always"));
	m_CbnUpdateFrequency.SetItemData(ndx, 0);
	if(periodDays >= 0)
	{
		m_CbnUpdateFrequency.SetCurSel(ndx);
	}

	ndx = m_CbnUpdateFrequency.AddString(_T("daily"));
	m_CbnUpdateFrequency.SetItemData(ndx, 1);
	if(periodDays >= 1)
	{
		m_CbnUpdateFrequency.SetCurSel(ndx);
	}

	ndx = m_CbnUpdateFrequency.AddString(_T("weekly"));
	m_CbnUpdateFrequency.SetItemData(ndx, 7);
	if(periodDays >= 7)
	{
		m_CbnUpdateFrequency.SetCurSel(ndx);
	}

	ndx = m_CbnUpdateFrequency.AddString(_T("monthly"));
	m_CbnUpdateFrequency.SetItemData(ndx, 30);
	if(periodDays >= 30)
	{
		m_CbnUpdateFrequency.SetCurSel(ndx);
	}

	ndx = m_CbnUpdateFrequency.AddString(_T("never"));
	m_CbnUpdateFrequency.SetItemData(ndx, ~(DWORD_PTR)0);
	if(periodDays < 0)
	{		
		m_CbnUpdateFrequency.SetCurSel(ndx);
	}

	CheckDlgButton(IDC_CHECK_UPDATEINSTALLAUTOMATICALLY, TrackerSettings::Instance().UpdateInstallAutomatically ? BST_CHECKED : BST_UNCHECKED);

	CheckDlgButton(IDC_CHECK1, TrackerSettings::Instance().UpdateStatistics ? BST_CHECKED : BST_UNCHECKED);

	GetDlgItem(IDC_STATIC_UPDATEPRIVACYTEXT)->SetWindowText(mpt::ToCString(CUpdateCheck::GetStatisticsUserInformation(true)));

	EnableDisableDialog();

	m_SettingChangedNotifyGuard.Register(this);
	SettingChanged(TrackerSettings::Instance().UpdateLastUpdateCheck.GetPath());

	return TRUE;
}


void CUpdateSetupDlg::OnShowStatisticsData(NMHDR * /*pNMHDR*/, LRESULT * /*pResult*/)
{
	CUpdateCheck::Settings settings;

	uint32 updateChannel = TrackerSettings::Instance().UpdateChannel;
	const int channelRadio = GetCheckedRadioButton(IDC_RADIO1, IDC_RADIO3);
	if(channelRadio == IDC_RADIO1) updateChannel = UpdateChannelRelease;
	if(channelRadio == IDC_RADIO2) updateChannel = UpdateChannelNext;
	if(channelRadio == IDC_RADIO3) updateChannel = UpdateChannelDevelopment;

	int updateCheckPeriod = (m_CbnUpdateFrequency.GetItemData(m_CbnUpdateFrequency.GetCurSel()) == ~(DWORD_PTR)0) ? -1 : static_cast<int>(m_CbnUpdateFrequency.GetItemData(m_CbnUpdateFrequency.GetCurSel()));

	settings.periodDays = updateCheckPeriod;
	settings.channel = static_cast<UpdateChannel>(updateChannel);
	settings.sendStatistics = (IsDlgButtonChecked(IDC_CHECK1) != BST_UNCHECKED);

	mpt::ustring statistics;

	statistics += U_("Update:") + UL_("\n");
	statistics += UL_("\n");

	{
		statistics += U_("User-Agent: ") + Version::Current().GetOpenMPTVersionString() + UL_("\n");
		statistics += UL_("\n");
		statistics += U_("GET ") + settings.apiURL + MPT_UFORMAT("update/{}")(GetChannelName(static_cast<UpdateChannel>(settings.channel))) + UL_("\n");
		statistics += UL_("\n");
		std::vector<mpt::ustring> keyAnchors = TrackerSettings::Instance().UpdateSigningKeysRootAnchors;
		for(const auto & keyAnchor : keyAnchors)
		{
			statistics += U_("GET ") + keyAnchor + U_("signingkeys.jwkset.json") + UL_("\n");
			statistics += UL_("\n");
		}
	}

	if(settings.sendStatistics)
	{
		statistics += U_("Statistics:") + UL_("\n");
		statistics += UL_("\n");
		statistics += U_("User-Agent: ") + Version::Current().GetOpenMPTVersionString() + UL_("\n");
		statistics += UL_("\n");
		if(settings.statisticsUUID.IsValid())
		{
			statistics += U_("PUT ") + settings.apiURL + MPT_UFORMAT("statistics/{}")(settings.statisticsUUID) + UL_("\n");
		} else
		{
			statistics += U_("POST ") + settings.apiURL + U_("statistics/") + UL_("\n");
		}
		statistics += mpt::replace(mpt::ToUnicode(mpt::Charset::UTF8, CUpdateCheck::GetStatisticsDataV3(settings)), U_("\t"), U_("    "));
	}

	InfoDialog dlg(this);
	dlg.SetCaption(_T("Update Statistics Data"));
	dlg.SetContent(mpt::ToWin(mpt::replace(statistics, U_("\n"), U_("\r\n"))));
	dlg.DoModal();
}


void CUpdateSetupDlg::SettingChanged(const SettingPath &changedPath)
{
	if(changedPath == TrackerSettings::Instance().UpdateLastUpdateCheck.GetPath())
	{
		CString updateText;
		const mpt::chrono::default_system_clock::time_point t = TrackerSettings::Instance().UpdateLastUpdateCheck.Get();
		if(t != mpt::chrono::default_system_clock::time_point{})
		{
			updateText += CTime(mpt::chrono::default_system_clock::to_unix_seconds(t)).Format(_T("The last successful update check was run on %F, %R."));
		}
		updateText += _T("\r\n");
		SetDlgItemText(IDC_LASTUPDATE, updateText);
	}
}


void CUpdateSetupDlg::EnableDisableDialog()
{

	BOOL status = ((IsDlgButtonChecked(IDC_CHECK_UPDATEENABLED) != BST_UNCHECKED) ? TRUE : FALSE);

	GetDlgItem(IDC_STATIC_UDATECHANNEL)->EnableWindow(status);
	GetDlgItem(IDC_RADIO1)->EnableWindow(status);
	GetDlgItem(IDC_RADIO2)->EnableWindow(status);
	GetDlgItem(IDC_RADIO3)->EnableWindow(status);

	GetDlgItem(IDC_STATIC_UPDATECHECK)->EnableWindow(status);
	GetDlgItem(IDC_STATIC_UPDATEFREQUENCY)->EnableWindow(status);
	GetDlgItem(IDC_COMBO_UPDATEFREQUENCY)->EnableWindow(status);
	GetDlgItem(IDC_BUTTON1)->EnableWindow(status);
	GetDlgItem(IDC_LASTUPDATE)->EnableWindow(status);
	GetDlgItem(IDC_CHECK_UPDATEINSTALLAUTOMATICALLY)->EnableWindow(status);

	GetDlgItem(IDC_STATIC_UPDATEPRIVACY)->EnableWindow(status);
	GetDlgItem(IDC_CHECK1)->EnableWindow(status);
	GetDlgItem(IDC_STATIC_UPDATEPRIVACYTEXT)->EnableWindow(status);
	GetDlgItem(IDC_SYSLINK1)->EnableWindow(status);

	// OpenFMT: каналы обновлений, статистика и автоустановка — это всё
	// сервер OpenMPT. Мы только смотрим выпуски на GitHub, поэтому прячем.
	for(const int id : {IDC_STATIC_UDATECHANNEL, IDC_RADIO1, IDC_RADIO2, IDC_RADIO3, IDC_CHECK_UPDATEINSTALLAUTOMATICALLY,
		IDC_STATIC_UPDATEPRIVACY, IDC_CHECK1, IDC_STATIC_UPDATEPRIVACYTEXT, IDC_SYSLINK1})
	{
		if(CWnd *item = GetDlgItem(id))
			item->ShowWindow(SW_HIDE);
	}
}


void CUpdateSetupDlg::OnSettingsChanged()
{
	EnableDisableDialog();
	SetModified(TRUE);
}


void CUpdateSetupDlg::OnOK()
{
	uint32 updateChannel = TrackerSettings::Instance().UpdateChannel;
	const int channelRadio = GetCheckedRadioButton(IDC_RADIO1, IDC_RADIO3);
	if(channelRadio == IDC_RADIO1) updateChannel = UpdateChannelRelease;
	if(channelRadio == IDC_RADIO2) updateChannel = UpdateChannelNext;
	if(channelRadio == IDC_RADIO3) updateChannel = UpdateChannelDevelopment;

	int updateCheckPeriod = (m_CbnUpdateFrequency.GetItemData(m_CbnUpdateFrequency.GetCurSel()) == ~(DWORD_PTR)0) ? -1 : static_cast<int>(m_CbnUpdateFrequency.GetItemData(m_CbnUpdateFrequency.GetCurSel()));
	
	TrackerSettings::Instance().UpdateEnabled = (IsDlgButtonChecked(IDC_CHECK_UPDATEENABLED) != BST_UNCHECKED);

	TrackerSettings::Instance().UpdateIntervalDays = updateCheckPeriod;
	TrackerSettings::Instance().UpdateInstallAutomatically = (IsDlgButtonChecked(IDC_CHECK_UPDATEINSTALLAUTOMATICALLY) != BST_UNCHECKED);
	TrackerSettings::Instance().UpdateChannel = updateChannel;
	TrackerSettings::Instance().UpdateStatistics = (IsDlgButtonChecked(IDC_CHECK1) != BST_UNCHECKED);
	
	CPropertyPage::OnOK();
}


BOOL CUpdateSetupDlg::OnSetActive()
{
	CMainFrame::m_nLastOptionsPage = OPTIONS_PAGE_UPDATE;
	return CPropertyPage::OnSetActive();
}


void CUpdateSetupDlg::OnCheckNow()
{
	CUpdateCheck::DoManualUpdateCheck();
}


#endif // MPT_ENABLE_UPDATE


OPENMPT_NAMESPACE_END
