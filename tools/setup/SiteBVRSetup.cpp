// Site B VR Setup
//
// Sets up Site B VR from the player's own copy of Jurassic Park: Trespasser.  The game itself is
// not included: the player points this at their CD, a disc image of it (.iso, .bin/.cue, .img)
// or a folder where the game is already installed.  The game's files are read straight out of
// the image (the 1998 installer is never run), checked, and copied with the mod into one folder.

#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// Messages from the work thread to the window.
static HWND g_hwnd = nullptr;
enum { WM_APP_LOG = WM_APP + 1, WM_APP_PROGRESS, WM_APP_DONE };

static void Log(const std::wstring& str)
{
	PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(str));
}
static void Progress(double f)
{
	PostMessageW(g_hwnd, WM_APP_PROGRESS, (WPARAM)(int)(f * 1000.0), 0);
}

static std::wstring Lower(std::wstring s)
{
	for (auto& c : s) c = (wchar_t)std::towlower(c);
	return s;
}

// ---------------------------------------------------------------------------------------------
// The files the game needs, relative to its data folder.
static std::vector<std::wstring> RequiredFiles()
{
	std::vector<std::wstring> a = {
		L"menu\\tpassintro.smk", L"Effects.tpa", L"Stream.tpa", L"ambient.tpa", L"Menu.tpa"
	};
	for (const wchar_t* l : { L"be", L"jr", L"ij", L"it", L"lab", L"sum", L"as", L"as2" })
	{
		std::wstring s(l);
		a.push_back(s + L".scn");
		a.push_back(s + L"-130.spz");
		a.push_back(s + L"-130.pid");
		a.push_back(s + L".grf");
	}
	return a;
}

// ---------------------------------------------------------------------------------------------
// A CD image: plain 2048-byte sectors (.iso) or raw 2352-byte sectors (.bin, .img), read as an
// ISO 9660 disc with Joliet long names.
class CDiscImage
{
public:
	struct Entry
	{
		std::wstring strName;
		bool bDir = false;
		uint32_t u4Lba = 0;
		uint32_t u4Size = 0;
	};

	bool Open(const fs::path& path, std::wstring& str_err)
	{
		fs::path p_bin = path;
		if (Lower(path.extension().wstring()) == L".cue")
		{
			// The first FILE in the cue sheet.
			std::ifstream is(path);
			std::string line;
			while (std::getline(is, line))
			{
				size_t i = line.find("FILE");
				if (i == std::string::npos)
					continue;
				size_t a = line.find('"', i), b = (a == std::string::npos) ? a : line.find('"', a + 1);
				std::string name = (a != std::string::npos && b != std::string::npos) ? line.substr(a + 1, b - a - 1) : "";
				if (name.empty())
				{
					// FILE name BINARY, unquoted.
					size_t s = line.find_first_not_of(" \t", i + 4);
					size_t e = line.find_first_of(" \t", s);
					name = line.substr(s, e - s);
				}
				p_bin = path.parent_path() / fs::path(name);
				break;
			}
			if (p_bin == path)
			{
				str_err = L"The .cue file doesn't name a disc image file.";
				return false;
			}
		}
		hFile = CreateFileW(p_bin.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			str_err = L"Couldn't open " + p_bin.wstring() + L".";
			return false;
		}
		// Raw sectors start with the 12-byte sync pattern 00 FF*10 00.
		static const unsigned char au1_sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
		unsigned char au1[2352];
		if (bReadAt(16ull * 2352, au1, 2352) && memcmp(au1, au1_sync, 12) == 0)
		{
			iSectorSize = 2352;
			iDataOffset = (au1[15] == 2) ? 24 : 16;
		}
		else
		{
			iSectorSize = 2048;
			iDataOffset = 0;
		}
		// Volume descriptors: prefer Joliet (long names).
		std::vector<unsigned char> sec(2048);
		bool b_found = false;
		for (uint32_t lba = 16; lba < 64; lba++)
		{
			if (!bReadSector(lba, sec.data()))
				break;
			if (memcmp(&sec[1], "CD001", 5) != 0)
				break;
			int type = sec[0];
			if (type == 255)
				break;
			bool b_joliet = type == 2 && sec[88] == '%' && sec[89] == '/' && (sec[90] == '@' || sec[90] == 'C' || sec[90] == 'E');
			if (type == 1 && !b_found)
			{
				Root = RecordAt(&sec[156], false);
				b_found = true;
			}
			if (b_joliet)
			{
				Root = RecordAt(&sec[156], true);
				bJoliet = true;
				b_found = true;
			}
		}
		if (!b_found)
		{
			str_err = L"That file isn't a CD image this setup can read (no ISO 9660 disc in it).";
			return false;
		}
		Root.strName = L"";
		Root.bDir = true;
		return true;
	}

	~CDiscImage()
	{
		if (hFile != INVALID_HANDLE_VALUE)
			CloseHandle(hFile);
	}

	std::vector<Entry> List(const Entry& dir)
	{
		std::vector<Entry> a;
		std::vector<unsigned char> sec(2048);
		uint32_t u_sectors = (dir.u4Size + 2047) / 2048;
		for (uint32_t s = 0; s < u_sectors; s++)
		{
			if (!bReadSector(dir.u4Lba + s, sec.data()))
				break;
			uint32_t i = 0;
			while (i < 2048)
			{
				unsigned len = sec[i];
				if (len == 0)
					break;
				if (i + len > 2048)
					break;
				Entry e = RecordAt(&sec[i], bJoliet);
				unsigned name_len = sec[i + 32];
				bool b_self = name_len == 1 && (sec[i + 33] == 0 || sec[i + 33] == 1);
				if (!b_self)
					a.push_back(e);
				i += len;
			}
		}
		return a;
	}

	bool Extract(const Entry& e, const fs::path& dest, std::atomic<uint64_t>& r_done)
	{
		HANDLE h = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		std::vector<unsigned char> buf(2048 * 64);
		uint32_t left = e.u4Size;
		uint32_t lba = e.u4Lba;
		bool b_ok = true;
		while (left > 0 && b_ok)
		{
			uint32_t n_sec = std::min<uint32_t>(64, (left + 2047) / 2048);
			for (uint32_t k = 0; k < n_sec && b_ok; k++)
				b_ok = bReadSector(lba + k, &buf[k * 2048]);
			uint32_t n = std::min<uint32_t>(left, n_sec * 2048);
			DWORD w = 0;
			if (b_ok)
				b_ok = WriteFile(h, buf.data(), n, &w, nullptr) && w == n;
			left -= n;
			lba += n_sec;
			r_done += n;
		}
		CloseHandle(h);
		return b_ok;
	}

	Entry Root;

private:
	HANDLE hFile = INVALID_HANDLE_VALUE;
	int iSectorSize = 2048, iDataOffset = 0;
	bool bJoliet = false;

	bool bReadAt(uint64_t off, void* p, DWORD n)
	{
		LARGE_INTEGER li;
		li.QuadPart = (LONGLONG)off;
		if (!SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN))
			return false;
		DWORD r = 0;
		return ReadFile(hFile, p, n, &r, nullptr) && r == n;
	}
	bool bReadSector(uint32_t lba, unsigned char* p)
	{
		return bReadAt((uint64_t)lba * iSectorSize + iDataOffset, p, 2048);
	}
	static uint32_t U32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
	static Entry RecordAt(const unsigned char* r, bool b_joliet)
	{
		Entry e;
		e.u4Lba = U32(r + 2);
		e.u4Size = U32(r + 10);
		e.bDir = (r[25] & 2) != 0;
		unsigned n = r[32];
		const unsigned char* pn = r + 33;
		if (b_joliet)
		{
			for (unsigned k = 0; k + 1 < n; k += 2)
				e.strName += (wchar_t)((pn[k] << 8) | pn[k + 1]);
		}
		else
		{
			for (unsigned k = 0; k < n; k++)
				e.strName += (wchar_t)pn[k];
		}
		size_t semi = e.strName.find(L';');
		if (semi != std::wstring::npos)
			e.strName.resize(semi);
		if (!e.bDir && !e.strName.empty() && e.strName.back() == L'.')
			e.strName.pop_back();
		return e;
	}
};

// ---------------------------------------------------------------------------------------------
// Settings from the window.
struct SJob
{
	fs::path pSource;		// Empty: only update the mod in an existing install.
	fs::path pDest;
	fs::path pMod;			// The folder this setup runs from (the mod's files).
};

static uint64_t g_u8Total = 1;
static std::atomic<uint64_t> g_u8Done{ 0 };

static void ReportProgress()
{
	Progress(std::min(1.0, (double)g_u8Done / (double)std::max<uint64_t>(g_u8Total, 1)));
}

// The data folder in a disc image: the folder holding menu\tpassintro.smk.
static bool bFindDataInImage(CDiscImage& img, const CDiscImage::Entry& dir, int depth, CDiscImage::Entry& r_found, std::wstring& r_path)
{
	if (depth > 5)
		return false;
	auto a = img.List(dir);
	for (auto& e : a)
		if (e.bDir && Lower(e.strName) == L"menu")
			for (auto& f : img.List(e))
				if (!f.bDir && Lower(f.strName) == L"tpassintro.smk")
				{
					r_found = dir;
					return true;
				}
	for (auto& e : a)
		if (e.bDir && bFindDataInImage(img, e, depth + 1, r_found, r_path))
		{
			r_path = e.strName + L"\\" + r_path;
			return true;
		}
	return false;
}

static uint64_t u8ImageSize(CDiscImage& img, const CDiscImage::Entry& dir)
{
	uint64_t n = 0;
	for (auto& e : img.List(dir))
		n += e.bDir ? u8ImageSize(img, e) : e.u4Size;
	return n;
}

static bool bExtractTree(CDiscImage& img, const CDiscImage::Entry& dir, const fs::path& dest)
{
	std::error_code ec;
	fs::create_directories(dest, ec);
	for (auto& e : img.List(dir))
	{
		if (e.bDir)
		{
			if (!bExtractTree(img, e, dest / e.strName))
				return false;
		}
		else
		{
			if (!img.Extract(e, dest / e.strName, g_u8Done))
			{
				Log(L"Couldn't copy " + e.strName + L" out of the disc image.");
				return false;
			}
			ReportProgress();
		}
	}
	return true;
}

static bool bMissingInImage(CDiscImage& img, const CDiscImage::Entry& data, std::vector<std::wstring>& r_missing)
{
	auto top = img.List(data);
	std::vector<CDiscImage::Entry> menu;
	for (auto& e : top)
		if (e.bDir && Lower(e.strName) == L"menu")
			menu = img.List(e);
	for (auto& f : RequiredFiles())
	{
		std::wstring lf = Lower(f);
		bool b_found = false;
		if (lf.rfind(L"menu\\", 0) == 0)
		{
			for (auto& e : menu)
				b_found |= Lower(e.strName) == lf.substr(5);
		}
		else
			for (auto& e : top)
				b_found |= !e.bDir && Lower(e.strName) == lf;
		if (!b_found)
			r_missing.push_back(f);
	}
	return !r_missing.empty();
}

// The data folder under a folder (the CD, or an installed game).
static bool bFindDataInFolder(const fs::path& dir, int depth, fs::path& r_found)
{
	std::error_code ec;
	if (fs::exists(dir / L"menu" / L"tpassintro.smk", ec))
	{
		r_found = dir;
		return true;
	}
	if (depth > 4)
		return false;
	for (auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
		if (e.is_directory(ec) && bFindDataInFolder(e.path(), depth + 1, r_found))
			return true;
	return false;
}

// A disc image inside a folder (a downloaded, unzipped image).
static fs::path FindImageInFolder(const fs::path& dir)
{
	std::error_code ec;
	fs::path best;
	for (auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
	{
		std::wstring ext = Lower(e.path().extension().wstring());
		if (ext == L".cue")
			return e.path();
		if ((ext == L".iso" || ext == L".bin" || ext == L".img") && best.empty())
			best = e.path();
	}
	return best;
}

static bool bCopyTree(const fs::path& from, const fs::path& to)
{
	std::error_code ec;
	fs::create_directories(to, ec);
	for (auto& e : fs::directory_iterator(from, ec))
	{
		fs::path t = to / e.path().filename();
		if (e.is_directory())
		{
			if (!bCopyTree(e.path(), t))
				return false;
		}
		else
		{
			if (!CopyFileW(e.path().c_str(), t.c_str(), FALSE))
			{
				Log(L"Couldn't copy " + e.path().wstring() + L".");
				return false;
			}
			SetFileAttributesW(t.c_str(), FILE_ATTRIBUTE_NORMAL);	// (Off a CD they are read-only.)
			g_u8Done += fs::file_size(t, ec);
			ReportProgress();
		}
	}
	return true;
}

static uint64_t u8FolderSize(const fs::path& dir)
{
	uint64_t n = 0;
	std::error_code ec;
	for (auto& e : fs::recursive_directory_iterator(dir, ec))
		if (e.is_regular_file(ec))
			n += e.file_size(ec);
	return n;
}

static bool bRun(const std::wstring& cmd)
{
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	std::wstring c = cmd;
	if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return false;
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return code == 0;
}

static void MakeShortcut(const fs::path& exe, const fs::path& dir)
{
	PWSTR pz = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &pz)))
		return;
	fs::path lnk = fs::path(pz) / L"Site B VR.lnk";
	CoTaskMemFree(pz);
	IShellLinkW* psl = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl))))
	{
		psl->SetPath(exe.c_str());
		psl->SetWorkingDirectory(dir.c_str());
		psl->SetDescription(L"Site B VR");
		IPersistFile* ppf = nullptr;
		if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARGS(&ppf))))
		{
			ppf->Save(lnk.c_str(), TRUE);
			ppf->Release();
		}
		psl->Release();
	}
}

static bool bSteamVRInstalled()
{
	PWSTR pz = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pz)))
		return false;
	fs::path p = fs::path(pz) / L"openvr" / L"openvrpaths.vrpath";
	CoTaskMemFree(pz);
	std::error_code ec;
	return fs::exists(p, ec);
}

// ---------------------------------------------------------------------------------------------
static void Work(SJob job)
{
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	bool b_ok = false;
	std::error_code ec;
	fs::path p_temp;
	do
	{
		fs::path p_data_dest = job.pDest / L"data";
		bool b_have_game = fs::exists(p_data_dest / L"menu" / L"tpassintro.smk", ec);

		// ---- The game's files ----
		if (!job.pSource.empty())
		{
			fs::path src = job.pSource;
			std::wstring ext = Lower(src.extension().wstring());

			// A zip: unpacked first with Windows' own tar.
			if (fs::is_regular_file(src, ec) && (ext == L".zip" || ext == L".7z"))
			{
				Log(L"Unpacking " + src.filename().wstring() + L"...");
				wchar_t tmp[MAX_PATH];
				GetTempPathW(MAX_PATH, tmp);
				p_temp = fs::path(tmp) / L"SiteBVR-source";
				fs::remove_all(p_temp, ec);
				fs::create_directories(p_temp, ec);
				wchar_t sys[MAX_PATH];
				GetSystemDirectoryW(sys, MAX_PATH);
				if (!bRun(L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + src.wstring() + L"\" -C \"" + p_temp.wstring() + L"\""))
				{
					Log(L"Couldn't unpack it.  Please unzip it yourself and choose the unzipped folder or disc image.");
					break;
				}
				src = p_temp;
			}

			fs::path p_folder_data;
			if (fs::is_directory(src, ec) && bFindDataInFolder(src, 0, p_folder_data))
			{
				// A CD, an unpacked disc, or an installed game.
				std::vector<std::wstring> missing;
				for (auto& f : RequiredFiles())
					if (!fs::exists(p_folder_data / f, ec))
						missing.push_back(f);
				if (!missing.empty())
				{
					Log(L"The game in that folder is missing files, for example " + missing[0] + L".  Choose the full game (the disc or its image).");
					break;
				}
				if (fs::equivalent(p_folder_data, p_data_dest, ec))
					Log(L"The game is already in the setup folder.");
				else
				{
					Log(L"Copying the game from " + p_folder_data.wstring() + L"...");
					g_u8Total = u8FolderSize(p_folder_data);
					g_u8Done = 0;
					if (!bCopyTree(p_folder_data, p_data_dest))
						break;
				}
			}
			else
			{
				fs::path p_image = fs::is_directory(src, ec) ? FindImageInFolder(src) : src;
				if (p_image.empty())
				{
					Log(L"There's no Trespasser game or disc image in " + job.pSource.wstring() + L".");
					break;
				}
				CDiscImage img;
				std::wstring str_err;
				Log(L"Reading the disc image " + p_image.filename().wstring() + L"...");
				if (!img.Open(p_image, str_err))
				{
					Log(str_err);
					break;
				}
				CDiscImage::Entry data;
				std::wstring str_path;
				if (!bFindDataInImage(img, img.Root, 0, data, str_path))
				{
					Log(L"That disc image doesn't hold Trespasser's game files (no data\\menu\\tpassintro.smk).  Make sure it's the Trespasser CD.");
					break;
				}
				std::vector<std::wstring> missing;
				if (bMissingInImage(img, data, missing))
				{
					Log(L"The disc image is missing game files, for example " + missing[0] + L".  It may be incomplete or a demo.");
					break;
				}
				Log(L"Copying the game out of the disc image...");
				g_u8Total = u8ImageSize(img, data);
				g_u8Done = 0;
				if (!bExtractTree(img, data, p_data_dest))
					break;
			}
			b_have_game = true;
		}
		if (!b_have_game)
		{
			Log(L"Choose your Trespasser CD, disc image or installed game first.");
			break;
		}

		// ---- The mod ----
		Log(L"Adding Site B VR...");
		fs::create_directories(job.pDest / L"menu", ec);
		fs::path self;
		{
			wchar_t sz[MAX_PATH];
			GetModuleFileNameW(nullptr, sz, MAX_PATH);
			self = sz;
		}
		bool b_mod_ok = true;
		for (auto& e : fs::recursive_directory_iterator(job.pMod, ec))
		{
			if (!e.is_regular_file())
				continue;
			if (fs::equivalent(e.path(), self, ec))
				continue;
			fs::path rel = fs::relative(e.path(), job.pMod, ec);
			if (rel.empty() || Lower(rel.begin()->wstring()) == L"data")
				continue;
			fs::path t = job.pDest / rel;
			if (fs::equivalent(e.path().parent_path(), t.parent_path(), ec))
				continue;		// Setup runs from the install folder itself.
			fs::create_directories(t.parent_path(), ec);
			if (!CopyFileW(e.path().c_str(), t.c_str(), FALSE))
			{
				Log(L"Couldn't copy " + rel.wstring() + L".  Is the game running?");
				b_mod_ok = false;
				break;
			}
		}
		if (!b_mod_ok)
			break;
		if (!fs::exists(job.pDest / L"OpenTrespasser.exe", ec))
		{
			Log(L"The mod's files weren't found next to this setup.  Unzip the whole SiteB-VR.zip and run setup from there.");
			break;
		}

		// The pause menu with its VR Settings entry: the game's own menu, plus the mod's addition.
		{
			fs::path p_orig = p_data_dest / L"menu" / L"ingameopt.ddf";
			fs::path p_add = job.pDest / L"menu" / L"ingameopt_vr_addition.ddf";
			std::ifstream a(p_orig, std::ios::binary), b(p_add, std::ios::binary);
			if (a && b)
			{
				std::string s((std::istreambuf_iterator<char>(a)), {});
				std::string t((std::istreambuf_iterator<char>(b)), {});
				std::ofstream o(job.pDest / L"menu" / L"ingameopt.ddf", std::ios::binary | std::ios::trunc);
				o << s << "\r\n" << t;
			}
		}
		// The menu sounds live next to the exe.
		if (!fs::exists(job.pDest / L"menu.tpa", ec))
			CopyFileW((p_data_dest / L"Menu.tpa").c_str(), (job.pDest / L"menu.tpa").c_str(), TRUE);

		MakeShortcut(job.pDest / L"OpenTrespasser.exe", job.pDest);
		Log(L"Made a desktop shortcut, \"Site B VR\".");
		if (!bSteamVRInstalled())
			Log(L"SteamVR doesn't seem to be installed.  Install it from Steam (it's free) before you play.");
		Progress(1.0);
		Log(L"Done!  Start SteamVR, then play from the \"Site B VR\" shortcut.");
		b_ok = true;
	} while (false);

	if (!p_temp.empty())
		fs::remove_all(p_temp, ec);
	PostMessageW(g_hwnd, WM_APP_DONE, b_ok ? 1 : 0, 0);
	CoUninitialize();
}

// ---------------------------------------------------------------------------------------------
// The window.
enum { ID_SRC = 100, ID_SRC_IMAGE, ID_SRC_FOLDER, ID_DEST, ID_DEST_BROWSE, ID_INSTALL, ID_PLAY, ID_LOG, ID_PROGRESS };
static HWND hSrc, hDest, hLog, hProgress, hInstall, hPlay;
static HFONT hFont, hFontBig;
static bool g_bBusy = false;

static std::wstring GetText(HWND h)
{
	int n = GetWindowTextLengthW(h);
	std::wstring s(n, L'\0');
	GetWindowTextW(h, s.data(), n + 1);
	return s;
}

static std::wstring PickFolder(HWND owner)
{
	std::wstring r;
	IFileOpenDialog* pfd = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
	{
		DWORD o;
		pfd->GetOptions(&o);
		pfd->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
		if (SUCCEEDED(pfd->Show(owner)))
		{
			IShellItem* psi;
			if (SUCCEEDED(pfd->GetResult(&psi)))
			{
				PWSTR pz;
				if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pz)))
				{
					r = pz;
					CoTaskMemFree(pz);
				}
				psi->Release();
			}
		}
		pfd->Release();
	}
	return r;
}

static std::wstring PickImage(HWND owner)
{
	wchar_t sz[MAX_PATH] = L"";
	OPENFILENAMEW ofn = { sizeof(ofn) };
	ofn.hwndOwner = owner;
	ofn.lpstrFilter = L"Disc images and archives (*.iso;*.cue;*.bin;*.img;*.zip)\0*.iso;*.cue;*.bin;*.img;*.zip\0All files\0*.*\0";
	ofn.lpstrFile = sz;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	return GetOpenFileNameW(&ofn) ? sz : L"";
}

static HWND Ctl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f)
{
	HWND c = CreateWindowExW(cls == WC_EDITW && !(style & ES_READONLY) ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
		x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
	SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
	return c;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_CREATE:
	{
		g_hwnd = h;
		hFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
		hFontBig = CreateFontW(-26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
		Ctl(h, L"STATIC", L"Site B VR Setup", 0, 20, 14, 600, 34, 0, hFontBig);
		Ctl(h, L"STATIC", L"Site B VR needs your own copy of Jurassic Park: Trespasser (it isn't included).", 0, 20, 52, 640, 20, 0, hFont);
		Ctl(h, L"STATIC", L"1.  Your Trespasser CD, disc image (.iso, .cue/.bin, .img, .zip) or installed game:", 0, 20, 86, 640, 20, 0, hFont);
		hSrc = Ctl(h, WC_EDITW, L"", ES_AUTOHSCROLL, 20, 110, 420, 26, ID_SRC, hFont);
		Ctl(h, L"BUTTON", L"Disc image...", BS_PUSHBUTTON, 450, 109, 100, 28, ID_SRC_IMAGE, hFont);
		Ctl(h, L"BUTTON", L"Folder / CD...", BS_PUSHBUTTON, 556, 109, 104, 28, ID_SRC_FOLDER, hFont);
		Ctl(h, L"STATIC", L"2.  Install Site B VR in:", 0, 20, 150, 640, 20, 0, hFont);
		PWSTR pz = nullptr;
		std::wstring str_def = L"C:\\Games\\Site B VR";
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &pz)))
		{
			str_def = (fs::path(pz) / L"Games" / L"Site B VR").wstring();
			CoTaskMemFree(pz);
		}
		hDest = Ctl(h, WC_EDITW, str_def.c_str(), ES_AUTOHSCROLL, 20, 174, 530, 26, ID_DEST, hFont);
		Ctl(h, L"BUTTON", L"Change...", BS_PUSHBUTTON, 556, 173, 104, 28, ID_DEST_BROWSE, hFont);
		hInstall = Ctl(h, L"BUTTON", L"Install", BS_DEFPUSHBUTTON, 20, 216, 120, 32, ID_INSTALL, hFont);
		hPlay = Ctl(h, L"BUTTON", L"Play", BS_PUSHBUTTON, 150, 216, 120, 32, ID_PLAY, hFont);
		EnableWindow(hPlay, FALSE);
		hProgress = Ctl(h, PROGRESS_CLASSW, L"", PBS_SMOOTH, 286, 222, 374, 20, ID_PROGRESS, hFont);
		SendMessageW(hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
		hLog = Ctl(h, WC_EDITW, L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL | WS_BORDER, 20, 262, 640, 170, ID_LOG, hFont);
		Log(L"If you already have Site B VR installed, leave step 1 empty and press Install to update the mod.");
		return 0;
	}
	case WM_COMMAND:
		switch (LOWORD(wp))
		{
		case ID_SRC_IMAGE: { auto s = PickImage(h); if (!s.empty()) SetWindowTextW(hSrc, s.c_str()); break; }
		case ID_SRC_FOLDER: { auto s = PickFolder(h); if (!s.empty()) SetWindowTextW(hSrc, s.c_str()); break; }
		case ID_DEST_BROWSE: { auto s = PickFolder(h); if (!s.empty()) SetWindowTextW(hDest, (fs::path(s) / L"Site B VR").c_str()); break; }
		case ID_INSTALL:
		{
			if (g_bBusy)
				break;
			SJob job;
			std::wstring s = GetText(hSrc);
			s.erase(std::remove(s.begin(), s.end(), L'"'), s.end());
			job.pSource = s;
			job.pDest = GetText(hDest);
			wchar_t sz[MAX_PATH];
			GetModuleFileNameW(nullptr, sz, MAX_PATH);
			job.pMod = fs::path(sz).parent_path();
			if (job.pDest.empty())
				break;
			std::error_code ec;
			fs::create_directories(job.pDest, ec);
			if (!fs::is_directory(job.pDest, ec))
			{
				Log(L"Couldn't make the folder " + job.pDest.wstring() + L".");
				break;
			}
			g_bBusy = true;
			EnableWindow(hInstall, FALSE);
			EnableWindow(hPlay, FALSE);
			SendMessageW(hProgress, PBM_SETPOS, 0, 0);
			std::thread(Work, job).detach();
			break;
		}
		case ID_PLAY:
		{
			fs::path d = GetText(hDest);
			ShellExecuteW(h, L"open", (d / L"OpenTrespasser.exe").c_str(), nullptr, d.c_str(), SW_SHOWNORMAL);
			PostQuitMessage(0);
			break;
		}
		}
		return 0;
	case WM_APP_LOG:
	{
		std::wstring* p = (std::wstring*)lp;
		std::wstring s = GetText(hLog);
		if (!s.empty())
			s += L"\r\n";
		s += *p;
		delete p;
		SetWindowTextW(hLog, s.c_str());
		SendMessageW(hLog, EM_LINESCROLL, 0, 10000);
		return 0;
	}
	case WM_APP_PROGRESS:
		SendMessageW(hProgress, PBM_SETPOS, wp, 0);
		return 0;
	case WM_APP_DONE:
		g_bBusy = false;
		EnableWindow(hInstall, TRUE);
		EnableWindow(hPlay, wp ? TRUE : FALSE);
		return 0;
	case WM_CTLCOLORSTATIC:
		SetBkMode((HDC)wp, TRANSPARENT);
		return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show)
{
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
	InitCommonControlsEx(&icc);
	WNDCLASSW wc = {};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hi;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
	wc.lpszClassName = L"SiteBVRSetup";
	wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
	RegisterClassW(&wc);
	RECT r = { 0, 0, 680, 452 };
	AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
	g_hwnd = CreateWindowW(L"SiteBVRSetup", L"Site B VR Setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);
	ShowWindow(g_hwnd, show);
	MSG m;
	while (GetMessageW(&m, nullptr, 0, 0))
	{
		if (!IsDialogMessageW(g_hwnd, &m))
		{
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	}
	CoUninitialize();
	return 0;
}
