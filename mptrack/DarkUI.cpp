/*
 * DarkUI.cpp
 * Dark colors for OpenMPT's Windows controls and custom drawing.
 */
#include "stdafx.h"
#include "DarkUI.h"

#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")

OPENMPT_NAMESPACE_BEGIN
namespace DarkUI
{
namespace
{
bool active = false;
HHOOK windowHook = nullptr;
using AllowWindow = BOOL (WINAPI *)(HWND, BOOL);
AllowWindow allowWindow = nullptr;

constexpr COLORREF Surface = RGB(32, 34, 38);
constexpr COLORREF Canvas = RGB(21, 23, 26);
constexpr COLORREF Raised = RGB(47, 50, 56);
constexpr COLORREF Border = RGB(68, 72, 79);
constexpr COLORREF Text = RGB(200, 204, 210);
constexpr COLORREF Muted = RGB(118, 124, 134);
constexpr COLORREF Selection = RGB(46, 76, 105);
// Ползунки и кнопки: у темы Windows для них нет тёмного варианта — белая
// дорожка, ярко-синий бегунок, светлые рамки. Рисуем сами, спокойнее.
constexpr COLORREF Track = RGB(50, 54, 60);
constexpr COLORREF Thumb = RGB(80, 98, 120);
constexpr COLORREF ThumbHot = RGB(98, 120, 146);
constexpr COLORREF Tick = RGB(66, 70, 78);
constexpr COLORREF ButtonFace = RGB(35, 37, 42);
constexpr COLORREF ButtonHot = RGB(45, 48, 54);
constexpr COLORREF ButtonBorder = RGB(50, 53, 60);

using GetSysColorFn = DWORD (WINAPI *)(int);
using GetSysColorBrushFn = HBRUSH (WINAPI *)(int);
GetSysColorFn realGetSysColor = ::GetSysColor;
GetSysColorBrushFn realGetSysColorBrush = ::GetSysColorBrush;

DWORD WINAPI HookGetSysColor(int index) { return Color(index); }
HBRUSH WINAPI HookGetSysColorBrush(int index) { return Brush(index); }

// Системные цвета запрашивают сотни мест программы и MFC, а не только наши
// ~20 мест с DarkUI::Color. Подменяем вызовы в таблице импорта своего же
// exe (MFC в нём статически): чужие DLL — comctl32, uxtheme — не задеты,
// темы Windows рисуют свои контролы сами.
template<typename T>
bool PatchImport(const char *dll, const char *function, void *hook, T &original)
{
	auto base = reinterpret_cast<BYTE *>(GetModuleHandleW(nullptr));
	const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if(!dir.VirtualAddress)
		return false;
	for(auto imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name; imp++)
	{
		if(lstrcmpiA(reinterpret_cast<const char *>(base + imp->Name), dll))
			continue;
		auto names = reinterpret_cast<const IMAGE_THUNK_DATA *>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
		auto thunks = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
		for(; names->u1.AddressOfData; names++, thunks++)
		{
			if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;
			const auto byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
			if(strcmp(reinterpret_cast<const char *>(byName->Name), function))
				continue;
			DWORD protect = 0;
			if(!VirtualProtect(&thunks->u1.Function, sizeof(thunks->u1.Function), PAGE_READWRITE, &protect))
				return false;
			original = reinterpret_cast<T>(thunks->u1.Function);
			thunks->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
			VirtualProtect(&thunks->u1.Function, sizeof(thunks->u1.Function), protect, &protect);
			return true;
		}
	}
	return false;
}

bool ClassIs(HWND window, const wchar_t *name)
{
	wchar_t actual[64] = {};
	return GetClassNameW(window, actual, static_cast<int>(std::size(actual))) && !lstrcmpiW(actual, name);
}

bool IsAfxClass(HWND window)
{
	wchar_t actual[64] = {};
	return GetClassNameW(window, actual, static_cast<int>(std::size(actual))) && !wcsncmp(actual, L"Afx", 3);
}

void Fill(HDC dc, const RECT &rect, int color)
{
	FillRect(dc, &rect, Brush(color));
}

void ExcludeWindow(HDC dc, HWND owner, HWND other)
{
	RECT rect{};
	if(!GetWindowRect(other, &rect))
		return;
	MapWindowPoints(HWND_DESKTOP, owner, reinterpret_cast<POINT *>(&rect), 2);
	ExcludeClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
}

void DrawTabs(HWND window, HDC dc)
{
	RECT client{};
	GetClientRect(window, &client);
	const int clipping = SaveDC(dc);
	const HWND parent = GetParent(window);
	for(HWND sibling = GetWindow(parent, GW_CHILD); sibling; sibling = GetWindow(sibling, GW_HWNDNEXT))
		if(sibling != window && (GetWindowLongPtrW(sibling, GWL_STYLE) & WS_VISIBLE))
			ExcludeWindow(dc, window, sibling);
	{
		RECT strip = client;
		strip.bottom = client.top;
		for(int i = 0, count = TabCtrl_GetItemCount(window); i < count; i++)
		{
			RECT item{};
			if(TabCtrl_GetItemRect(window, i, &item))
				strip.bottom = std::max(strip.bottom, item.bottom + 2);
		}
		Fill(dc, strip, COLOR_BTNFACE);
	}
	const int selected = TabCtrl_GetCurSel(window);
	const HFONT font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
	const int saved = SaveDC(dc);
	if(font)
		SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	const HIMAGELIST images = TabCtrl_GetImageList(window);
	POINT mouse{};
	GetCursorPos(&mouse);
	ScreenToClient(window, &mouse);
	for(int i = 0, count = TabCtrl_GetItemCount(window); i < count; i++)
	{
		RECT item{};
		if(!TabCtrl_GetItemRect(window, i, &item))
			continue;
		Fill(dc, item, i == selected ? COLOR_HIGHLIGHT : (PtInRect(&item, mouse) ? COLOR_3DLIGHT : COLOR_BTNFACE));
		FrameRect(dc, &item, Brush(COLOR_BTNSHADOW));
		wchar_t label[256]{};
		TCITEMW info{};
		info.mask = TCIF_TEXT | TCIF_IMAGE;
		info.pszText = label;
		info.cchTextMax = static_cast<int>(std::size(label));
		SendMessageW(window, TCM_GETITEMW, i, reinterpret_cast<LPARAM>(&info));
		SIZE textSize{};
		GetTextExtentPoint32W(dc, label, lstrlenW(label), &textSize);
		int x = item.left + std::max<LONG>(4, (item.right - item.left - textSize.cx) / 2);
		if(images && info.iImage >= 0)
		{
			int width = 0, height = 0;
			ImageList_GetIconSize(images, &width, &height);
			x = std::max<int>(item.left + 4, x - (width + 4) / 2);
			ImageList_Draw(images, info.iImage, dc, x, (item.top + item.bottom - height) / 2, ILD_TRANSPARENT);
			x += width + 4;
		}
		item.left = x;
		SetTextColor(dc, IsWindowEnabled(window) ? Text : Muted);
		DrawTextW(dc, label, -1, &item, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
		if(i == selected && GetFocus() == window && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
			DrawFocusRect(dc, &item);
	}
	RestoreDC(dc, saved);
	RestoreDC(dc, clipping);
}

void DrawGroup(HWND window, HDC dc)
{
	// Dialog group boxes are sibling controls. Paint only the outline and
	// caption, otherwise the group box can cover the controls it surrounds.
	RECT rect{};
	GetClientRect(window, &rect);
	const int saved = SaveDC(dc);
	const HFONT font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
	if(font)
		SelectObject(dc, font);
	wchar_t label[256]{};
	const int length = GetWindowTextW(window, label, static_cast<int>(std::size(label)));
	TEXTMETRICW metrics{};
	GetTextMetricsW(dc, &metrics);
	RECT border = rect;
	border.top += metrics.tmHeight / 2;
	FrameRect(dc, &border, Brush(COLOR_BTNSHADOW));
	if(length)
	{
		RECT caption = rect;
		caption.left += 8;
		SIZE extent{};
		GetTextExtentPoint32W(dc, label, length, &extent);
		caption.right = std::min(caption.right, caption.left + extent.cx + 6);
		caption.bottom = std::min(caption.bottom, caption.top + metrics.tmHeight);
		Fill(dc, caption, COLOR_BTNFACE);
		caption.left += 3;
		SetTextColor(dc, IsWindowEnabled(window) ? Text : Muted);
		SetBkMode(dc, TRANSPARENT);
		DrawTextW(dc, label, length, &caption, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
	}
	RestoreDC(dc, saved);
}

// Prefix of the Win32 themed non-client menu messages. Normal menu handling
// continues to own item layout, icons, mouse navigation and keyboard shortcuts.
constexpr UINT DrawMenuBar = 0x0091, DrawMenuBarItem = 0x0092;
struct MenuDrawData { HMENU menu; HDC dc; DWORD flags; };
struct MenuItemDrawData { DRAWITEMSTRUCT item; MenuDrawData menu; int position; };

void DrawHeader(HWND window, HDC dc)
{
	RECT client{};
	GetClientRect(window, &client);
	Fill(dc, client, COLOR_BTNFACE);
	const int saved = SaveDC(dc);
	if(const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)))
		SelectObject(dc, font);
	SetTextColor(dc, Text);
	SetBkMode(dc, TRANSPARENT);
	for(int i = 0, count = Header_GetItemCount(window); i < count; ++i)
	{
		RECT item{};
		if(!Header_GetItemRect(window, i, &item))
			continue;
		wchar_t text[256]{};
		HDITEMW header{};
		header.mask = HDI_TEXT | HDI_FORMAT;
		header.pszText = text;
		header.cchTextMax = static_cast<int>(std::size(text));
		SendMessageW(window, HDM_GETITEMW, i, reinterpret_cast<LPARAM>(&header));
		FrameRect(dc, &item, Brush(COLOR_BTNSHADOW));
		item.left += 6;
		item.right -= 5;
		if(header.fmt & (HDF_SORTUP | HDF_SORTDOWN))
		{
			const wchar_t *marker = (header.fmt & HDF_SORTUP) ? L"\u25B4" : L"\u25BE";
			DrawTextW(dc, marker, -1, &item, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
			item.right -= 14;
		}
		const UINT align = (header.fmt & HDF_CENTER) ? DT_CENTER : (header.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT;
		DrawTextW(dc, text, -1, &item, align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
	}
	RestoreDC(dc, saved);
}

void DrawStatus(HWND window, HDC dc)
{
	RECT client{};
	GetClientRect(window, &client);
	Fill(dc, client, COLOR_BTNFACE);
	const int saved = SaveDC(dc);
	const HFONT font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
	if(font)
		SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, Text);
	const int count = static_cast<int>(SendMessageW(window, SB_GETPARTS, 0, 0));
	for(int i = 0; i < count; i++)
	{
		RECT rect{};
		if(!SendMessageW(window, SB_GETRECT, i, reinterpret_cast<LPARAM>(&rect)))
			continue;
		const auto length = SendMessageW(window, SB_GETTEXTLENGTHW, i, 0);
		if(HIWORD(length) & SBT_OWNERDRAW)
			continue;
		std::wstring value(LOWORD(length) + 1, L'\0');
		SendMessageW(window, SB_GETTEXTW, i, reinterpret_cast<LPARAM>(value.data()));
		rect.left += 5;
		DrawTextW(dc, value.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
	}
	RestoreDC(dc, saved);
}

// Рамки рисует сама Windows системным цветом (белым у тёмной темы):
// у главного окна это полоса между меню и клиентской частью, у полей и
// вложенных диалогов — их рамка. Перекрашиваем поверх уже нарисованного.
void PaintFrame(HWND window)
{
	RECT windowRect{}, client{};
	if(!GetWindowRect(window, &windowRect) || !GetClientRect(window, &client))
		return;
	MapWindowPoints(window, HWND_DESKTOP, reinterpret_cast<POINT *>(&client), 2);
	OffsetRect(&client, -windowRect.left, -windowRect.top);
	OffsetRect(&windowRect, -windowRect.left, -windowRect.top);
	const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
	const HDC dc = GetWindowDC(window);
	if(!dc)
		return;
	if(!(style & WS_CHILD) && (style & WS_CAPTION) == WS_CAPTION)
	{
		if(GetMenu(window))
		{
			RECT line = client;
			line.bottom = line.top;
			line.top--;
			Fill(dc, line, COLOR_MENU);
		}
	} else if(ClassIs(window, L"ToolbarWindow32"))
	{
		const int saved = SaveDC(dc);
		ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
		Fill(dc, windowRect, COLOR_BTNFACE);
		RestoreDC(dc, saved);
	} else
	{
		const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
		const int thickness = std::min(client.left - windowRect.left, client.top - windowRect.top);
		if(thickness > 0 && thickness <= 3 && ((style & (WS_BORDER | WS_DLGFRAME)) || (exStyle & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE))))
		{
			const bool field = ClassIs(window, L"Edit") || ClassIs(window, L"ListBox") || ClassIs(window, L"SysListView32") || ClassIs(window, L"SysTreeView32");
			RECT frame = windowRect;
			FrameRect(dc, &frame, Brush(COLOR_BTNSHADOW));
			for(int i = 1; i < thickness; i++)
			{
				InflateRect(&frame, -1, -1);
				FrameRect(dc, &frame, Brush(field ? COLOR_WINDOW : COLOR_BTNFACE));
			}
		}
	}
	ReleaseDC(window, dc);
}

void FillColor(HDC dc, const RECT &rect, COLORREF color)
{
	SetDCBrushColor(dc, color);
	FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void FrameColor(HDC dc, const RECT &rect, COLORREF color)
{
	SetDCBrushColor(dc, color);
	FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

LRESULT DrawTrackbar(HWND trackbar, NMCUSTOMDRAW &draw)
{
	if(draw.dwDrawStage == CDDS_PREPAINT)
		return CDRF_NOTIFYITEMDRAW;
	if(draw.dwDrawStage != CDDS_ITEMPREPAINT)
		return CDRF_DODEFAULT;
	const LONG_PTR style = GetWindowLongPtrW(trackbar, GWL_STYLE);
	const bool vertical = (style & TBS_VERT) != 0;
	const HDC dc = draw.hdc;
	switch(draw.dwItemSpec)
	{
	case TBCD_CHANNEL:
	{
		// Дорожка тоньше системной: 3 пикселя по центру её прямоугольника.
		RECT rect = draw.rc;
		if(vertical)
		{
			const LONG mid = (rect.left + rect.right) / 2;
			rect.left = mid - 1;
			rect.right = mid + 2;
		} else
		{
			const LONG mid = (rect.top + rect.bottom) / 2;
			rect.top = mid - 1;
			rect.bottom = mid + 2;
		}
		FillColor(dc, rect, IsWindowEnabled(trackbar) ? Track : Border);
		return CDRF_SKIPDEFAULT;
	}
	case TBCD_THUMB:
	{
		const bool hot = (draw.uItemState & (CDIS_HOT | CDIS_SELECTED)) != 0;
		FillColor(dc, draw.rc, !IsWindowEnabled(trackbar) ? Border : hot ? ThumbHot : Thumb);
		return CDRF_SKIPDEFAULT;
	}
	case TBCD_TICS:
	{
		if(style & TBS_NOTICKS)
			return CDRF_SKIPDEFAULT;
		RECT channel{}, thumb{};
		SendMessageW(trackbar, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
		SendMessageW(trackbar, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
		if(vertical)
		{
			// У вертикального ползунка прямоугольник дорожки приходит повёрнутым.
			std::swap(channel.left, channel.top);
			std::swap(channel.right, channel.bottom);
		}
		const int half = vertical ? (thumb.bottom - thumb.top) / 2 : (thumb.right - thumb.left) / 2;
		// Деления считаем сами из значений: TBM_GETTICPOS у автоделений
		// отдаёт не те координаты, и они сбивались в кучку у края.
		const int start = vertical ? channel.top + half : channel.left + half;
		const int end = vertical ? channel.bottom - half : channel.right - half;
		const auto minimum = static_cast<int>(SendMessageW(trackbar, TBM_GETRANGEMIN, 0, 0));
		const auto maximum = static_cast<int>(SendMessageW(trackbar, TBM_GETRANGEMAX, 0, 0));
		std::vector<int> positions{start, end};
		const auto count = static_cast<int>(SendMessageW(trackbar, TBM_GETNUMTICS, 0, 0));
		if((style & TBS_AUTOTICKS) && count > 2)
		{
			// Автоделения всегда равномерные, а их значения Windows не отдаёт.
			for(int i = 1; i + 1 < count; i++)
				positions.push_back(start + MulDiv(i, end - start, count - 1));
		} else if(maximum > minimum)
		{
			for(int i = 0; i + 2 < count; i++)
			{
				const auto value = static_cast<int>(SendMessageW(trackbar, TBM_GETTIC, i, 0));
				if(value < minimum || value > maximum)
					continue;
				int pos = start + MulDiv(value - minimum, end - start, maximum - minimum);
				if(style & TBS_REVERSED)
					pos = start + end - pos;
				positions.push_back(pos);
			}
		}
		const bool before = (style & (TBS_BOTH | TBS_TOP)) != 0;
		const bool after = (style & TBS_BOTH) || !(style & TBS_TOP);
		for(const int pos : positions)
		{
			if(vertical)
			{
				if(before)
					FillColor(dc, RECT{thumb.left - 4, pos, thumb.left - 1, pos + 1}, Tick);
				if(after)
					FillColor(dc, RECT{thumb.right + 1, pos, thumb.right + 4, pos + 1}, Tick);
			} else
			{
				if(before)
					FillColor(dc, RECT{pos, thumb.top - 4, pos + 1, thumb.top - 1}, Tick);
				if(after)
					FillColor(dc, RECT{pos, thumb.bottom + 1, pos + 1, thumb.bottom + 4}, Tick);
			}
		}
		return CDRF_SKIPDEFAULT;
	}
	}
	return CDRF_DODEFAULT;
}

// Обычные кнопки и кнопки-переключатели (Mute, Surround). Кнопки с картинкой
// и самодельные (BS_OWNERDRAW) не трогаем.
int ScaleForWindow(HWND window, int value)
{
	const UINT dpi = GetDpiForWindow(window);
	return MulDiv(value, dpi ? static_cast<int>(dpi) : 96, 96);
}

LRESULT DrawCheckBox(HWND button, NMCUSTOMDRAW &draw, bool radio)
{
	const LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
	const HDC dc = draw.hdc;
	const bool enabled = IsWindowEnabled(button) != FALSE;
	const auto check = SendMessageW(button, BM_GETCHECK, 0, 0);
	const bool hot = (draw.uItemState & CDIS_HOT) != 0;
	RECT client = draw.rc;
	FillColor(dc, client, Surface);
	const int box = ScaleForWindow(button, 13);
	const bool rightAligned = (style & BS_LEFTTEXT) != 0;
	RECT mark{};
	mark.top = (client.top + client.bottom - box) / 2;
	mark.bottom = mark.top + box;
	mark.left = rightAligned ? client.right - box : client.left;
	mark.right = mark.left + box;
	const COLORREF frame = !enabled ? Border : hot ? ThumbHot : Thumb;
	const int saved = SaveDC(dc);
	SelectObject(dc, GetStockObject(DC_PEN));
	SelectObject(dc, GetStockObject(DC_BRUSH));
	if(radio)
	{
		SetDCPenColor(dc, frame);
		SetDCBrushColor(dc, Canvas);
		Ellipse(dc, mark.left, mark.top, mark.right, mark.bottom);
		if(check == BST_CHECKED)
		{
			const int inset = box / 4 + 1;
			SetDCPenColor(dc, enabled ? Text : Muted);
			SetDCBrushColor(dc, enabled ? Text : Muted);
			Ellipse(dc, mark.left + inset, mark.top + inset, mark.right - inset, mark.bottom - inset);
		}
	} else
	{
		FillColor(dc, mark, check == BST_CHECKED ? (enabled ? Thumb : Border) : Canvas);
		FrameColor(dc, mark, frame);
		if(check == BST_CHECKED)
		{
			// Галочка из двух отрезков, толщиной в два пикселя при 100%.
			SetDCPenColor(dc, enabled ? Text : Muted);
			const int w = box;
			for(int t = 0; t < std::max(2, ScaleForWindow(button, 2)); t++)
			{
				MoveToEx(dc, mark.left + w * 3 / 13, mark.top + w * 6 / 13 + t, nullptr);
				LineTo(dc, mark.left + w * 5 / 13, mark.top + w * 9 / 13 + t);
				LineTo(dc, mark.left + w * 10 / 13, mark.top + w * 3 / 13 + t);
			}
		} else if(check == BST_INDETERMINATE)
		{
			RECT inner = mark;
			InflateRect(&inner, -box / 4, -box / 4);
			FillColor(dc, inner, enabled ? Muted : Border);
		}
	}
	wchar_t label[256]{};
	GetWindowTextW(button, label, static_cast<int>(std::size(label)));
	if(const auto font = reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)))
		SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, enabled ? Text : Muted);
	RECT textRect = client;
	const int gap = ScaleForWindow(button, 4);
	if(rightAligned)
		textRect.right = mark.left - gap;
	else
		textRect.left = mark.right + gap;
	UINT format = DT_VCENTER | DT_END_ELLIPSIS | ((style & BS_MULTILINE) ? DT_WORDBREAK : DT_SINGLELINE);
	if(rightAligned)
		format |= DT_RIGHT;
	if(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL)
		format |= DT_HIDEPREFIX;
	if(style & BS_MULTILINE)
	{
		// DT_VCENTER работает только с одной строкой: центрируем сами.
		RECT measure = textRect;
		DrawTextW(dc, label, -1, &measure, format | DT_CALCRECT);
		const int height = measure.bottom - measure.top;
		textRect.top = std::max(textRect.top, (client.top + client.bottom - height) / 2);
	}
	DrawTextW(dc, label, -1, &textRect, format);
	if((draw.uItemState & CDIS_FOCUS) && !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
	{
		RECT focus = textRect;
		DrawTextW(dc, label, -1, &focus, format | DT_CALCRECT);
		InflateRect(&focus, 1, 0);
		FrameColor(dc, focus, Border);
	}
	RestoreDC(dc, saved);
	return CDRF_SKIPDEFAULT;
}

LRESULT DrawButton(HWND button, NMCUSTOMDRAW &draw)
{
	if(draw.dwDrawStage != CDDS_PREPAINT)
		return CDRF_DODEFAULT;
	const LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
	const LONG_PTR type = style & BS_TYPEMASK;
	if(!(style & (BS_PUSHLIKE | BS_BITMAP | BS_ICON)) && !SendMessageW(button, BM_GETIMAGE, IMAGE_BITMAP, 0) && !SendMessageW(button, BM_GETIMAGE, IMAGE_ICON, 0))
	{
		if(type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_3STATE || type == BS_AUTO3STATE)
			return DrawCheckBox(button, draw, false);
		if(type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON)
			return DrawCheckBox(button, draw, true);
	}
	const bool push = type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON;
	const bool pushLike = (style & BS_PUSHLIKE) && (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON || type == BS_3STATE || type == BS_AUTO3STATE);
	if((!push && !pushLike) || (style & (BS_BITMAP | BS_ICON)) || SendMessageW(button, BM_GETIMAGE, IMAGE_BITMAP, 0) || SendMessageW(button, BM_GETIMAGE, IMAGE_ICON, 0))
		return CDRF_DODEFAULT;
	const HDC dc = draw.hdc;
	const bool enabled = IsWindowEnabled(button) != FALSE;
	const bool pressed = (draw.uItemState & CDIS_SELECTED) || (pushLike && SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED);
	const bool hot = (draw.uItemState & CDIS_HOT) != 0;
	RECT rect = draw.rc;
	FillColor(dc, rect, pressed ? Selection : hot ? ButtonHot : ButtonFace);
	FrameColor(dc, rect, type == BS_DEFPUSHBUTTON ? Thumb : ButtonBorder);
	wchar_t label[256]{};
	GetWindowTextW(button, label, static_cast<int>(std::size(label)));
	const int saved = SaveDC(dc);
	if(const auto font = reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)))
		SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, enabled ? Text : Muted);
	UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
	format |= (style & BS_LEFT) == BS_LEFT && !(style & BS_CENTER) ? DT_LEFT : (style & BS_RIGHT) == BS_RIGHT && !(style & BS_CENTER) ? DT_RIGHT : DT_CENTER;
	if(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL)
		format |= DT_HIDEPREFIX;
	InflateRect(&rect, -3, 0);
	DrawTextW(dc, label, -1, &rect, format);
	if((draw.uItemState & CDIS_FOCUS) && !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
	{
		RECT focus = draw.rc;
		InflateRect(&focus, -2, -2);
		FrameColor(dc, focus, Thumb);
	}
	RestoreDC(dc, saved);
	return CDRF_SKIPDEFAULT;
}

// Окно сообщения (MessageBox): полосу с кнопками внизу Windows рисует
// светлой изнутри себя, мимо GetSysColor. Узнаём его по тексту с номером
// 0xFFFF и закрашиваем полосу после обычной отрисовки.
bool IsMessageBox(HWND window)
{
	if(GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD)
		return false;
	const HWND text = GetDlgItem(window, 0xFFFF);
	return text && ClassIs(text, L"Static");
}

void PaintMessageBoxFooter(HWND window)
{
	RECT client{};
	GetClientRect(window, &client);
	LONG top = client.bottom, bottom = client.top;
	for(HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
	{
		if(!ClassIs(child, L"Button") || !(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE))
			continue;
		RECT rect{};
		GetWindowRect(child, &rect);
		MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT *>(&rect), 2);
		top = std::min(top, rect.top);
		bottom = std::max(bottom, rect.bottom);
	}
	if(top >= bottom)
		return;
	RECT footer = client;
	footer.top = std::max(client.top, top - (client.bottom - bottom));
	const HDC dc = GetDC(window);
	const int saved = SaveDC(dc);
	for(HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
		if(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)
			ExcludeWindow(dc, window, child);
	Fill(dc, footer, COLOR_BTNFACE);
	RestoreDC(dc, saved);
	ReleaseDC(window, dc);
}

void DrawUpDown(HWND window, HDC dc)
{
	RECT client{};
	GetClientRect(window, &client);
	const bool horizontal = (GetWindowLongPtrW(window, GWL_STYLE) & UDS_HORZ) != 0;
	const bool enabled = IsWindowEnabled(window) != FALSE;
	POINT mouse{};
	GetCursorPos(&mouse);
	ScreenToClient(window, &mouse);
	const bool pressed = GetCapture() == window && GetKeyState(VK_LBUTTON) < 0;
	RECT halves[2] = {client, client};
	if(horizontal)
		halves[0].right = halves[1].left = (client.left + client.right) / 2;
	else
		halves[0].bottom = halves[1].top = (client.top + client.bottom) / 2;
	const int saved = SaveDC(dc);
	SelectObject(dc, GetStockObject(DC_PEN));
	SelectObject(dc, GetStockObject(DC_BRUSH));
	for(int i = 0; i < 2; i++)
	{
		const RECT &half = halves[i];
		const bool over = PtInRect(&half, mouse) != FALSE;
		FillColor(dc, half, (over && pressed) ? Selection : over ? ButtonHot : ButtonFace);
		FrameColor(dc, half, ButtonBorder);
		const int cx = (half.left + half.right) / 2, cy = (half.top + half.bottom) / 2;
		const int size = std::max(2, std::min<int>(half.right - half.left, half.bottom - half.top) / 4);
		POINT arrow[3];
		if(horizontal)
		{
			const int dir = i == 0 ? -1 : 1;
			arrow[0] = {cx + dir * size, cy};
			arrow[1] = {cx - dir * size, cy - size};
			arrow[2] = {cx - dir * size, cy + size};
		} else
		{
			const int dir = i == 0 ? -1 : 1;
			arrow[0] = {cx, cy + dir * size};
			arrow[1] = {cx - size, cy - dir * size};
			arrow[2] = {cx + size, cy - dir * size};
		}
		const COLORREF color = enabled ? Muted : Border;
		SetDCPenColor(dc, color);
		SetDCBrushColor(dc, color);
		Polygon(dc, arrow, 3);
	}
	RestoreDC(dc, saved);
}

// Выпадающая часть списка (ComboLBox): выделенную строку Windows рисует
// системным цветом выделения прямо при движении мыши, мимо WM_PAINT и
// любой темы, — у многих он белый. Перерисовываем её поверх своими цветами.
void RepaintListSelection(HWND list)
{
	const LONG_PTR style = GetWindowLongPtrW(list, GWL_STYLE);
	if((style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) && !(style & LBS_HASSTRINGS))
		return;
	const auto selected = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
	if(selected < 0)
		return;
	RECT rect{};
	if(SendMessageW(list, LB_GETITEMRECT, selected, reinterpret_cast<LPARAM>(&rect)) == LB_ERR)
		return;
	RECT client{};
	GetClientRect(list, &client);
	if(!IntersectRect(&rect, &rect, &client))
		return;
	const auto length = SendMessageW(list, LB_GETTEXTLEN, selected, 0);
	if(length == LB_ERR || length > 1024)
		return;
	std::wstring text(static_cast<size_t>(length) + 1, L'\0');
	SendMessageW(list, LB_GETTEXT, selected, reinterpret_cast<LPARAM>(text.data()));
	const HDC dc = GetDC(list);
	const int saved = SaveDC(dc);
	if(const auto font = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0)))
		SelectObject(dc, font);
	Fill(dc, rect, COLOR_HIGHLIGHT);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, Text);
	RECT textRect = rect;
	textRect.left += 2;
	DrawTextW(dc, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_EXPANDTABS);
	RestoreDC(dc, saved);
	ReleaseDC(list, dc);
}

void DrawChevron(HDC dc, const RECT &area, COLORREF color)
{
	const int cx = (area.left + area.right) / 2, cy = (area.top + area.bottom) / 2;
	const int saved = SaveDC(dc);
	SelectObject(dc, GetStockObject(DC_PEN));
	SetDCPenColor(dc, color);
	for(int i = 0; i < 2; i++)
	{
		MoveToEx(dc, cx - 4, cy - 2 + i, nullptr);
		LineTo(dc, cx, cy + 2 + i);
		LineTo(dc, cx + 5, cy - 3 + i);
	}
	RestoreDC(dc, saved);
}

bool ComboIsHot(HWND combo)
{
	POINT mouse{};
	GetCursorPos(&mouse);
	RECT rect{};
	GetWindowRect(combo, &rect);
	return PtInRect(&rect, mouse) != FALSE;
}

// Список без своей строки ввода и без самодельной отрисовки — целиком сами.
bool ComboFullyOurs(HWND combo)
{
	const LONG_PTR style = GetWindowLongPtrW(combo, GWL_STYLE);
	return (style & 3) == CBS_DROPDOWNLIST && !(style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE));
}

void DrawCombo(HWND combo, HDC dc)
{
	RECT client{};
	GetClientRect(combo, &client);
	const bool enabled = IsWindowEnabled(combo) != FALSE;
	const bool dropped = SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0;
	const bool hot = enabled && (dropped || ComboIsHot(combo));
	FillColor(dc, client, hot ? ButtonHot : ButtonFace);
	FrameColor(dc, client, ButtonBorder);
	const int arrowWidth = GetSystemMetrics(SM_CXVSCROLL);
	RECT arrow = client;
	arrow.left = arrow.right - arrowWidth;
	DrawChevron(dc, arrow, enabled ? Muted : Border);
	RECT textRect = client;
	textRect.right = arrow.left;
	InflateRect(&textRect, -2, -2);
	if(GetFocus() == combo && !dropped)
		FillColor(dc, textRect, Selection);
	wchar_t text[512]{};
	GetWindowTextW(combo, text, static_cast<int>(std::size(text)));
	const int saved = SaveDC(dc);
	if(const auto font = reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0)))
		SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, enabled ? Text : Muted);
	textRect.left += 2;
	DrawTextW(dc, text, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
	RestoreDC(dc, saved);
}

// Список со строкой ввода или самодельной отрисовкой: содержимое рисует он
// сам, а светлые рамку и кнопку со стрелкой перекрываем.
void DrawComboFrame(HWND combo)
{
	RECT client{};
	GetClientRect(combo, &client);
	const HDC dc = GetDC(combo);
	const int saved = SaveDC(dc);
	if(COMBOBOXINFO info{sizeof(info)}; GetComboBoxInfo(combo, &info) && info.hwndItem && info.hwndItem != combo)
		ExcludeWindow(dc, combo, info.hwndItem);
	const bool enabled = IsWindowEnabled(combo) != FALSE;
	FrameColor(dc, client, ButtonBorder);
	RECT arrow = client;
	InflateRect(&arrow, -1, -1);
	arrow.left = arrow.right - GetSystemMetrics(SM_CXVSCROLL);
	FillColor(dc, arrow, (enabled && ComboIsHot(combo)) ? ButtonHot : ButtonFace);
	DrawChevron(dc, arrow, enabled ? Muted : Border);
	RestoreDC(dc, saved);
	ReleaseDC(combo, dc);
}

LRESULT CALLBACK DarkProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR kind)
{
	switch(msg)
	{
	case DrawMenuBar:
	{
		const auto &menu = *reinterpret_cast<const MenuDrawData *>(lParam);
		MENUBARINFO info{sizeof(info)};
		RECT windowRect{};
		if(menu.menu == GetMenu(window) && GetMenuBarInfo(window, OBJID_MENU, 0, &info) && GetWindowRect(window, &windowRect))
		{
			OffsetRect(&info.rcBar, -windowRect.left, -windowRect.top);
			Fill(menu.dc, info.rcBar, COLOR_MENU);
			return 0;
		}
		break;
	}
	case DrawMenuBarItem:
	{
		const auto &data = *reinterpret_cast<const MenuItemDrawData *>(lParam);
		wchar_t label[256]{};
		if(data.menu.menu != GetMenu(window) || !GetMenuStringW(data.menu.menu, data.position, label, static_cast<int>(std::size(label)), MF_BYPOSITION))
			break;
		const auto &item = data.item;
		RECT rect = item.rcItem;
		const bool disabled = (item.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
		Fill(item.hDC, rect, item.itemState & (ODS_SELECTED | ODS_HOTLIGHT) ? COLOR_HIGHLIGHT : COLOR_MENU);
		const int saved = SaveDC(item.hDC);
		SetBkMode(item.hDC, TRANSPARENT);
		SetTextColor(item.hDC, disabled ? Muted : Text);
		DrawTextW(item.hDC, label, -1, &rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | ((item.itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
		RestoreDC(item.hDC, saved);
		return 0;
	}
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORDLG:
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
	case WM_CTLCOLORBTN:
	{
		const HDC dc = reinterpret_cast<HDC>(wParam);
		const HWND control = reinterpret_cast<HWND>(lParam);
		const bool field = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX
			|| (msg == WM_CTLCOLORSTATIC && ClassIs(control, L"Edit"));
		const int color = field ? COLOR_WINDOW : COLOR_BTNFACE;
		SetTextColor(dc, IsWindowEnabled(control) ? Text : Muted);
		SetBkColor(dc, Color(color));
		if(msg == WM_CTLCOLORSTATIC)
			SetBkMode(dc, OPAQUE);
		return reinterpret_cast<LRESULT>(Brush(color));
	}
	case WM_ERASEBKGND:
		if(kind == 2 || kind == 3 || kind == 5 || kind == 6 || kind == 7 || (kind == 9 && ComboFullyOurs(window)))
			return TRUE;
		if(kind == 1 || kind == 4)
		{
			RECT rect{};
			GetClientRect(window, &rect);
			const HDC dc = reinterpret_cast<HDC>(wParam);
			const int saved = SaveDC(dc);
			// Галочка с темой просит фон у родителя своим DC
			// (DrawThemeParentBackground). Вырезать её тогда нельзя: под ней
			// остался бы старый текст.
			for(HWND child = (WindowFromDC(dc) == window) ? GetWindow(window, GW_CHILD) : nullptr; child; child = GetWindow(child, GW_HWNDNEXT))
			{
				const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
				if((style & WS_VISIBLE) && !(ClassIs(child, L"Button") && (style & BS_TYPEMASK) == BS_GROUPBOX) && !ClassIs(child, L"SysTabControl32"))
					ExcludeWindow(dc, window, child);
			}
			Fill(dc, rect, kind == 1 ? COLOR_APPWORKSPACE : COLOR_BTNFACE);
			RestoreDC(dc, saved);
			return TRUE;
		}
		break;
	case WM_PRINTCLIENT:
	case WM_PAINT:
		if(msg == WM_PAINT && kind == 4 && IsMessageBox(window))
		{
			const LRESULT result = DefSubclassProc(window, msg, wParam, lParam);
			PaintMessageBoxFooter(window);
			return result;
		}
		if(kind == 9)
		{
			if(ComboFullyOurs(window))
			{
				PAINTSTRUCT paint{};
				const HDC dc = msg == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wParam);
				DrawCombo(window, dc);
				if(msg == WM_PAINT)
					EndPaint(window, &paint);
				return 0;
			}
			const LRESULT result = DefSubclassProc(window, msg, wParam, lParam);
			if(msg == WM_PAINT)
				DrawComboFrame(window);
			return result;
		}
		if(kind == 7)
		{
			PAINTSTRUCT paint{};
			const HDC dc = msg == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wParam);
			DrawUpDown(window, dc);
			if(msg == WM_PAINT)
				EndPaint(window, &paint);
			return 0;
		}
		if(kind == 2 || kind == 3 || kind == 5 || kind == 6)
		{
			PAINTSTRUCT paint{};
			const HDC dc = msg == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wParam);
			if(kind == 2)
				DrawTabs(window, dc);
			else if(kind == 3)
				DrawStatus(window, dc);
			else if(kind == 5)
				DrawGroup(window, dc);
			else
				DrawHeader(window, dc);
			if(msg == WM_PAINT)
				EndPaint(window, &paint);
			return 0;
		}
		break;
	case WM_NOTIFY:
	{
		const auto &notification = *reinterpret_cast<const NMHDR *>(lParam);
		if(notification.code == NM_CUSTOMDRAW && ClassIs(notification.hwndFrom, L"SysListView32"))
		{
			auto &draw = *reinterpret_cast<NMLVCUSTOMDRAW *>(lParam);
			if(draw.dwItemType == LVCDI_GROUP && draw.nmcd.dwDrawStage == CDDS_PREPAINT)
			{
				wchar_t text[256]{};
				LVGROUP group{};
				group.cbSize = sizeof(group);
				group.mask = LVGF_HEADER;
				group.pszHeader = text;
				group.cchHeader = static_cast<int>(std::size(text));
				ListView_GetGroupInfo(notification.hwndFrom, static_cast<int>(draw.nmcd.dwItemSpec), &group);
				const HDC dc = draw.nmcd.hdc;
				RECT rect = draw.rcText;
				Fill(dc, rect, COLOR_WINDOW);
				const int saved = SaveDC(dc);
				if(const auto font = reinterpret_cast<HFONT>(SendMessageW(notification.hwndFrom, WM_GETFONT, 0, 0)))
					SelectObject(dc, font);
				SetBkMode(dc, TRANSPARENT);
				SetTextColor(dc, Text);
				rect.left += 10;
				DrawTextW(dc, text, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
				RestoreDC(dc, saved);
				return CDRF_SKIPDEFAULT;
			}
			// Заголовки групп приходят, только если список попросили сообщать о каждом элементе.
			if(draw.nmcd.dwDrawStage == CDDS_PREPAINT && draw.dwItemType != LVCDI_GROUP)
				return DefSubclassProc(window, msg, wParam, lParam) | CDRF_NOTIFYITEMDRAW;
		}
		if(notification.code == NM_CUSTOMDRAW && ClassIs(notification.hwndFrom, TRACKBAR_CLASSW))
			return DrawTrackbar(notification.hwndFrom, *reinterpret_cast<NMCUSTOMDRAW *>(lParam));
		if(notification.code == NM_CUSTOMDRAW && ClassIs(notification.hwndFrom, L"Button"))
		{
			const LRESULT drawn = DrawButton(notification.hwndFrom, *reinterpret_cast<NMCUSTOMDRAW *>(lParam));
			if(drawn != CDRF_DODEFAULT)
				return drawn;
		}
		if(notification.code == NM_CUSTOMDRAW && ClassIs(notification.hwndFrom, L"ToolbarWindow32"))
		{
			auto &draw = *reinterpret_cast<NMTBCUSTOMDRAW *>(lParam);
			if(draw.nmcd.dwDrawStage == CDDS_PREPAINT)
			{
				const HWND toolbar = notification.hwndFrom;
				const HDC dc = draw.nmcd.hdc;
				const int saved = SaveDC(dc);
				for(HWND child = GetWindow(toolbar, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
					if(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)
						ExcludeWindow(dc, toolbar, child);
				RECT client{};
				GetClientRect(toolbar, &client);
				Fill(dc, client, COLOR_BTNFACE);
				RestoreDC(dc, saved);
				return CDRF_NOTIFYITEMDRAW;
			}
			if(draw.nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
			{
				// Тема для нажатых (включённых) кнопок светлая, и они горели
				// голубыми квадратами. Подложку рисуем сами, значок — тулбар.
				const UINT state = draw.nmcd.uItemState;
				RECT rect = draw.nmcd.rc;
				if(state & (CDIS_CHECKED | CDIS_SELECTED))
				{
					Fill(draw.nmcd.hdc, rect, COLOR_HIGHLIGHT);
					FrameRect(draw.nmcd.hdc, &rect, Brush(COLOR_BTNSHADOW));
				} else if(state & CDIS_HOT)
				{
					Fill(draw.nmcd.hdc, rect, COLOR_3DLIGHT);
				} else
				{
					Fill(draw.nmcd.hdc, rect, COLOR_BTNFACE);
				}
				draw.clrText = (state & CDIS_DISABLED) ? Muted : Text;
				return TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES | TBCDRF_NOOFFSET | CDRF_NOTIFYPOSTPAINT;
			}
			if(draw.nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT)
			{
				// Стрелку раскрывающегося меню тема рисует тёмной на тёмном —
				// её почти не видно. Закрашиваем и рисуем свою.
				const HWND toolbar = notification.hwndFrom;
				TBBUTTONINFOW info{sizeof(info)};
				info.dwMask = TBIF_STYLE;
				if(SendMessageW(toolbar, TB_GETBUTTONINFOW, draw.nmcd.dwItemSpec, reinterpret_cast<LPARAM>(&info)) >= 0
					&& (info.fsStyle & BTNS_DROPDOWN) && !(info.fsStyle & BTNS_WHOLEDROPDOWN)
					&& (SendMessageW(toolbar, TB_GETEXTENDEDSTYLE, 0, 0) & TBSTYLE_EX_DRAWDDARROWS))
				{
					RECT arrow = draw.nmcd.rc;
					arrow.left = arrow.right - ScaleForWindow(toolbar, 12);
					const UINT state = draw.nmcd.uItemState;
					Fill(draw.nmcd.hdc, arrow, (state & (CDIS_CHECKED | CDIS_SELECTED)) ? COLOR_HIGHLIGHT : (state & CDIS_HOT) ? COLOR_3DLIGHT : COLOR_BTNFACE);
					const int cx = (arrow.left + arrow.right) / 2, cy = (arrow.top + arrow.bottom) / 2;
					const int size = std::max(2, ScaleForWindow(toolbar, 3));
					POINT points[3] = {{cx - size, cy - size / 2}, {cx + size + 1, cy - size / 2}, {cx, cy + size / 2 + 1}};
					const int saved = SaveDC(draw.nmcd.hdc);
					SelectObject(draw.nmcd.hdc, GetStockObject(DC_PEN));
					SelectObject(draw.nmcd.hdc, GetStockObject(DC_BRUSH));
					const COLORREF color = (state & CDIS_DISABLED) ? Border : Text;
					SetDCPenColor(draw.nmcd.hdc, color);
					SetDCBrushColor(draw.nmcd.hdc, color);
					Polygon(draw.nmcd.hdc, points, 3);
					RestoreDC(draw.nmcd.hdc, saved);
				}
				return CDRF_DODEFAULT;
			}
		}
		break;
	}
	case WM_NCPAINT:
	case WM_NCACTIVATE:
	{
		const LRESULT result = DefSubclassProc(window, msg, wParam, lParam);
		PaintFrame(window);
		return result;
	}
	case WM_NCDESTROY:
		RemovePropW(window, L"OpenMPT.DarkUI");
		RemoveWindowSubclass(window, DarkProc, id);
		break;
	}
	const LRESULT result = DefSubclassProc(window, msg, wParam, lParam);
	if((kind == 2 && (msg == TCM_SETCURSEL || msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_MOUSELEAVE || msg == WM_MOUSEMOVE))
		|| (kind == 3 && (msg == SB_SETTEXTW || msg == SB_SETTEXTA)))
		InvalidateRect(window, nullptr, FALSE);
	if(kind == 8)
	{
		switch(msg)
		{
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_KEYDOWN:
		case WM_MOUSEWHEEL:
		case WM_VSCROLL:
		case WM_TIMER:
		case LB_SETCURSEL:
		case LB_SETTOPINDEX:
		{
			const auto selected = SendMessageW(window, LB_GETCURSEL, 0, 0);
			const auto top = SendMessageW(window, LB_GETTOPINDEX, 0, 0);
			SendMessageW(window, WM_SETREDRAW, FALSE, 0);
			const LRESULT quiet = DefSubclassProc(window, msg, wParam, lParam);
			SendMessageW(window, WM_SETREDRAW, TRUE, 0);
			if(selected != SendMessageW(window, LB_GETCURSEL, 0, 0) || top != SendMessageW(window, LB_GETTOPINDEX, 0, 0))
			{
				// Полоса прокрутки — часть рамки окна, её тоже обновить.
				RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
			}
			return quiet;
		}
		}
	}
	if(kind == 8)
	{
		switch(msg)
		{
		case WM_PAINT:
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_KEYDOWN:
		case WM_MOUSEWHEEL:
		case WM_VSCROLL:
		case WM_TIMER:
		case LB_SETCURSEL:
		case WM_SHOWWINDOW:
			RepaintListSelection(window);
			break;
		}
	}
	if((kind == 7 || kind == 9) && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE || msg == WM_ENABLE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS))
		InvalidateRect(window, nullptr, FALSE);
	if((kind == 2 || kind == 7 || kind == 9) && msg == WM_MOUSEMOVE)
	{
		TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
		TrackMouseEvent(&tracking);
	}
	return result;
}

void Apply(HWND window)
{
	// Use a window property to keep the create and dialog-init paths idempotent.
	if(GetPropW(window, L"OpenMPT.DarkUI") || !SetPropW(window, L"OpenMPT.DarkUI", reinterpret_cast<HANDLE>(1)))
		return;

	if(allowWindow)
		allowWindow(window, TRUE);
	const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
	if(!(style & WS_CHILD))
	{
		const BOOL dark = TRUE;
		DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
		// Explicit caption colors are supported by Windows 11.
		DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &Surface, sizeof(Surface));
		DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &Text, sizeof(Text));
	}
	DWORD_PTR kind = 0;
	if(ClassIs(window, L"MDIClient"))
		kind = 1;
	else if(ClassIs(window, L"SysTabControl32"))
	{
		SetWindowLongPtrW(window, GWL_STYLE, style | WS_CLIPSIBLINGS);
		kind = 2;
	}
	else if(ClassIs(window, L"msctls_statusbar32"))
		kind = 3;
	else if(ClassIs(window, L"Button") && (style & BS_TYPEMASK) == BS_GROUPBOX)
		kind = 5;
	else if(ClassIs(window, L"SysHeader32") && !(style & HDS_FILTERBAR))
		kind = 6;
	else if(ClassIs(window, L"msctls_updown32"))
		kind = 7;
	else if(ClassIs(window, L"ComboLBox"))
		kind = 8;
	else if(ClassIs(window, L"ComboBox"))
		kind = 9;
	else if(ClassIs(window, L"#32770") || ClassIs(window, L"ReBarWindow32")
		|| (GetDlgCtrlID(window) >= AFX_IDW_DOCKBAR_TOP && GetDlgCtrlID(window) <= AFX_IDW_DOCKBAR_RIGHT))
		kind = 4;

	// Фон из системного цвета класса Windows заливает сама, мимо GetSysColor.
	// Подставляем свою кисть (классы этого процесса, системе не мешает).
	const auto classBrush = static_cast<UINT_PTR>(GetClassLongPtrW(window, GCLP_HBRBACKGROUND));
	if(classBrush > 0 && classBrush <= COLOR_MENUBAR + 1
		&& !ClassIs(window, L"Button") && !ClassIs(window, L"Static") && !ClassIs(window, L"Edit")
		&& !ClassIs(window, L"ComboBox") && !ClassIs(window, L"ListBox") && !ClassIs(window, L"ScrollBar"))
	{
		SetClassLongPtrW(window, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(Brush(static_cast<int>(classBrush - 1))));
	}
	if((style & WS_CHILD) && ClassIs(window, L"#32770"))
	{
		const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
		if((style & WS_DLGFRAME) || (exStyle & WS_EX_DLGMODALFRAME))
		{
			SetWindowLongPtrW(window, GWL_EXSTYLE, exStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE));
			SetWindowLongPtrW(window, GWL_STYLE, (style & ~WS_DLGFRAME) | WS_BORDER);
			SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}
	SetWindowSubclass(window, DarkProc, 1, kind);
	if(ClassIs(window, L"SysTreeView32"))
	{
		SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
		TreeView_SetBkColor(window, Canvas);
		TreeView_SetTextColor(window, Text);
		TreeView_SetLineColor(window, Border);
	} else if(ClassIs(window, L"SysListView32"))
	{
		SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
		if(const HWND header = ListView_GetHeader(window))
			SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
		ListView_SetBkColor(window, Canvas);
		ListView_SetTextBkColor(window, Canvas);
		ListView_SetTextColor(window, Text);
	} else if(ClassIs(window, L"Edit") && (style & ES_MULTILINE))
	{
		SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
	} else if(ClassIs(window, L"ComboLBox"))
	{
		SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
	} else if(ClassIs(window, L"Edit") || ClassIs(window, L"ComboBox") || ClassIs(window, L"ListBox"))
	{
		SetWindowTheme(window, L"DarkMode_CFD", nullptr);
	} else if(ClassIs(window, L"Button") || ClassIs(window, L"ToolbarWindow32")
		|| ClassIs(window, L"msctls_trackbar32") || ClassIs(window, L"msctls_updown32")
		|| ClassIs(window, L"SysHeader32") || ClassIs(window, L"ScrollBar"))
	{
		SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
	} else if((style & WS_CHILD) && ((kind == 0 && IsAfxClass(window)) || ClassIs(window, L"#32770")))
	{
		// Полосы прокрутки окна берут его тему: самодельные окна (список
		// паттернов, редакторы) и вложенные диалоги (вкладка General).
		SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
	} else if(ClassIs(window, L"ReBarWindow32"))
	{
		SetWindowTheme(window, L"", L"");
		SendMessageW(window, RB_SETBKCOLOR, 0, Surface);
		SendMessageW(window, RB_SETTEXTCOLOR, 0, Text);
	}
}

LRESULT CALLBACK OnCreated(int code, WPARAM wParam, LPARAM lParam)
{
	if(code >= 0)
	{
		const auto &message = *reinterpret_cast<const CWPRETSTRUCT *>(lParam);
		if(message.message == WM_CREATE && message.lResult != -1)
			Apply(message.hwnd);
		else if(message.message == WM_INITDIALOG)
		{
			Apply(message.hwnd);
			EnumChildWindows(message.hwnd, [](HWND child, LPARAM) -> BOOL { Apply(child); return TRUE; }, 0);
			RedrawWindow(message.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
		}
	}
	return CallNextHookEx(windowHook, code, wParam, lParam);
}
}

void Initialize(bool enabled)
{
	HIGHCONTRASTW contrast{sizeof(contrast)};
	if(!enabled || (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON)))
		return;

	// UxTheme's app and per-window switches vary across older Windows releases.
	// On builds with the modern API, allow native menus and common controls to use dark visuals.
	OSVERSIONINFOW version{sizeof(version)};
	const auto rtlGetVersion = reinterpret_cast<LONG (WINAPI *)(OSVERSIONINFOW *)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
	if(rtlGetVersion && rtlGetVersion(&version) == 0 && version.dwMajorVersion >= 10 && version.dwBuildNumber >= 18362)
	{
		const HMODULE theme = GetModuleHandleW(L"uxtheme.dll");
		using SetPreferredAppMode = int (WINAPI *)(int);
		const auto preferredMode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(theme, MAKEINTRESOURCEA(135)));
		allowWindow = reinterpret_cast<AllowWindow>(GetProcAddress(theme, MAKEINTRESOURCEA(133)));
		if(preferredMode)
			preferredMode(2); // ForceDark, local to this process.
	}
	windowHook = SetWindowsHookExW(WH_CALLWNDPROCRET, OnCreated, nullptr, GetCurrentThreadId());
	active = windowHook != nullptr;
	if(active)
	{
		PatchImport("user32.dll", "GetSysColor", reinterpret_cast<void *>(HookGetSysColor), realGetSysColor);
		PatchImport("user32.dll", "GetSysColorBrush", reinterpret_cast<void *>(HookGetSysColorBrush), realGetSysColorBrush);
	}
}

bool Enabled() { return active; }

COLORREF Color(int index)
{
	if(!active)
		return realGetSysColor(index);
	switch(index)
	{
	case COLOR_WINDOW:
	case COLOR_APPWORKSPACE: return Canvas;
	case COLOR_BTNFACE:
	case COLOR_MENU: return Surface;
	case COLOR_3DLIGHT:
	case COLOR_BTNHIGHLIGHT: return Raised;
	case COLOR_BTNSHADOW:
	case COLOR_3DDKSHADOW:
	case COLOR_WINDOWFRAME: return Border;
	case COLOR_WINDOWTEXT:
	case COLOR_BTNTEXT:
	case COLOR_MENUTEXT:
	case COLOR_HIGHLIGHTTEXT: return Text;
	case COLOR_GRAYTEXT: return Muted;
	case COLOR_HIGHLIGHT: return Selection;
	case COLOR_INFOBK: return Raised;
	case COLOR_INFOTEXT: return Text;
	case COLOR_SCROLLBAR: return Surface;
	case COLOR_HOTLIGHT: return RGB(110, 170, 230);
	default: return realGetSysColor(index);
	}
}

HBRUSH Brush(int index)
{
	if(!active || index < 0 || index > COLOR_MENUBAR)
		return realGetSysColorBrush(index);
	static std::array<HBRUSH, COLOR_MENUBAR + 1> brushes{};
	if(!brushes[index])
		brushes[index] = CreateSolidBrush(Color(index));
	return brushes[index];
}
}
OPENMPT_NAMESPACE_END

