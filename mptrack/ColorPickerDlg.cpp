/*
 * ColorPickerDlg.cpp
 * ------------------
 * Purpose: OpenFMT's own colour picker (see ColorPickerDlg.h).
 * Notes  : Layout is in 96 DPI pixels and scaled to the window's DPI.
 * Authors: OpenFMT
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */


#include "stdafx.h"
#include "ColorPickerDlg.h"
#include "DarkUI.h"
#include "HighDPISupport.h"

#include <cmath>
#include <functional>

OPENMPT_NAMESPACE_BEGIN

namespace ColorPicker
{
namespace
{

struct HSV
{
	double h = 0.0;  // 0...360
	double s = 0.0;  // 0...1
	double v = 0.0;  // 0...1
};

HSV ToHSV(COLORREF color)
{
	const double r = GetRValue(color) / 255.0, g = GetGValue(color) / 255.0, b = GetBValue(color) / 255.0;
	const double high = std::max({r, g, b}), low = std::min({r, g, b}), delta = high - low;
	HSV hsv;
	if(delta > 0.0)
	{
		if(high == r)
			hsv.h = 60.0 * std::fmod((g - b) / delta, 6.0);
		else if(high == g)
			hsv.h = 60.0 * ((b - r) / delta + 2.0);
		else
			hsv.h = 60.0 * ((r - g) / delta + 4.0);
		if(hsv.h < 0.0)
			hsv.h += 360.0;
	}
	hsv.s = high > 0.0 ? delta / high : 0.0;
	hsv.v = high;
	return hsv;
}

void FromHSV(double h, double s, double v, double &r, double &g, double &b)
{
	const double chroma = v * s;
	const double x = chroma * (1.0 - std::abs(std::fmod(h / 60.0, 2.0) - 1.0));
	const double m = v - chroma;
	r = g = b = 0.0;
	switch(static_cast<int>(h / 60.0) % 6)
	{
	case 0: r = chroma; g = x; break;
	case 1: r = x; g = chroma; break;
	case 2: g = chroma; b = x; break;
	case 3: g = x; b = chroma; break;
	case 4: r = x; b = chroma; break;
	default: r = chroma; b = x; break;
	}
	r += m;
	g += m;
	b += m;
}

COLORREF FromHSV(const HSV &hsv)
{
	double r, g, b;
	FromHSV(hsv.h, hsv.s, hsv.v, r, g, b);
	return RGB(static_cast<int>(std::lround(r * 255.0)), static_cast<int>(std::lround(g * 255.0)), static_cast<int>(std::lround(b * 255.0)));
}

// Подобраны под тёмный фон: различимы между собой и не кислотные.
// Верхний ряд светлее (для заметных каналов), нижний — глубже.
constexpr COLORREF Palette[] =
{
	RGB(224, 108, 117), RGB(229, 147, 90), RGB(229, 192, 123), RGB(152, 195, 121), RGB(86, 182, 194), RGB(97, 175, 239),
	RGB(140, 122, 230), RGB(198, 120, 221), RGB(224, 123, 181), RGB(181, 162, 127), RGB(127, 181, 160), RGB(160, 168, 184),
	RGB(168, 67, 76), RGB(181, 101, 46), RGB(176, 143, 62), RGB(94, 138, 69), RGB(46, 127, 138), RGB(47, 111, 168),
	RGB(90, 75, 175), RGB(138, 69, 163), RGB(168, 74, 130), RGB(122, 106, 78), RGB(78, 122, 104), RGB(106, 114, 130),
};
constexpr int PaletteColumns = 12;
constexpr int MaxUsed = 15;
constexpr int IdcHex = 1001;

class Dialog : public CDialog
{
public:
	Dialog(COLORREF initial, mpt::span<const COLORREF> used)
		: m_color(initial)
		, m_original(initial)
		, m_hsv(ToHSV(initial))
	{
		for(const COLORREF color : used)
		{
			if(color > 0xFFFFFF || std::find(m_used.begin(), m_used.end(), color) != m_used.end())
				continue;
			m_used.push_back(color);
			if(m_used.size() >= MaxUsed)
				break;
		}
	}

	COLORREF Color() const { return m_color; }

	INT_PTR Run(CWnd *parent)
	{
		// Пустой шаблон диалога в памяти: все элементы создаются в OnInitDialog.
		static constexpr wchar_t title[] = L"Colour";
		static constexpr wchar_t font[] = L"MS Shell Dlg";
		m_template.assign(sizeof(DLGTEMPLATE) / sizeof(WORD), 0);
		auto &dlg = *reinterpret_cast<DLGTEMPLATE *>(m_template.data());
		dlg.style = DS_MODALFRAME | DS_SHELLFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
		dlg.cx = 200;
		dlg.cy = 150;
		m_template.push_back(0);  // меню
		m_template.push_back(0);  // класс
		m_template.insert(m_template.end(), std::begin(title), std::end(title));
		m_template.push_back(8);  // кегль
		m_template.insert(m_template.end(), std::begin(font), std::end(font));
		InitModalIndirect(reinterpret_cast<LPCDLGTEMPLATE>(m_template.data()), parent);
		return DoModal();
	}

protected:
	BOOL OnInitDialog() override
	{
		CDialog::OnInitDialog();
		const auto px = [this](int value) { return HighDPISupport::ScalePixels(value, m_hWnd); };
		const int margin = px(12), cell = px(20), step = px(24);
		m_square.SetRect(margin, margin, margin + px(200), margin + px(200));
		m_hue.SetRect(m_square.right + px(8), margin, m_square.right + px(26), m_square.bottom);
		const int column = m_hue.right + px(14);
		m_newSwatch.SetRect(column, margin, column + px(96), margin + px(44));
		m_oldSwatch.SetRect(column, m_newSwatch.bottom, column + px(96), m_newSwatch.bottom + px(26));
		m_hexLabel.SetRect(column, m_oldSwatch.bottom + px(14), column + px(96), m_oldSwatch.bottom + px(30));
		const CRect hexRect(column, m_hexLabel.bottom + px(2), column + px(96), m_hexLabel.bottom + px(26));

		int y = m_square.bottom + px(14);
		m_paletteRects.clear();
		for(int i = 0; i < static_cast<int>(std::size(Palette)); i++)
		{
			const int x = margin + (i % PaletteColumns) * step;
			const int row = y + (i / PaletteColumns) * step;
			m_paletteRects.emplace_back(x, row, x + cell, row + cell);
		}
		y += ((static_cast<int>(std::size(Palette)) + PaletteColumns - 1) / PaletteColumns) * step;
		m_usedRects.clear();
		if(!m_used.empty())
		{
			m_usedLabel.SetRect(margin, y + px(4), margin + px(200), y + px(20));
			y = m_usedLabel.bottom + px(2);
			for(size_t i = 0; i < m_used.size(); i++)
			{
				const int x = margin + static_cast<int>(i) * step;
				m_usedRects.emplace_back(x, y, x + cell, y + cell);
			}
			y += step;
		}
		const int width = std::max<int>(m_newSwatch.right, margin + PaletteColumns * step - (step - cell)) + margin;
		y += px(8);
		const CRect cancelRect(width - margin - px(80), y, width - margin, y + px(26));
		const CRect okRect(cancelRect.left - px(88), y, cancelRect.left - px(8), y + px(26));
		const int height = y + px(26) + margin;

		CRect window(0, 0, width, height);
		AdjustWindowRectEx(window, GetStyle(), FALSE, GetExStyle());
		SetWindowPos(nullptr, 0, 0, window.Width(), window.Height(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		CenterWindow(GetParent());

		CFont *dlgFont = GetFont();
		m_hexEdit.CreateEx(WS_EX_CLIENTEDGE, _T("Edit"), nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, hexRect, this, IdcHex);
		m_hexEdit.SetFont(dlgFont);
		m_hexEdit.SetLimitText(7);
		m_ok.Create(_T("OK"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, okRect, this, IDOK);
		m_ok.SetFont(dlgFont);
		m_cancel.Create(_T("Cancel"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, cancelRect, this, IDCANCEL);
		m_cancel.SetFont(dlgFont);

		BuildHueStrip();
		BuildSquare();
		UpdateHex();
		m_ok.SetFocus();
		return FALSE;
	}

	void BuildBitmap(CBitmap &bitmap, int width, int height, const std::function<COLORREF(int x, int y)> &pixel)
	{
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(info.bmiHeader);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		void *bits = nullptr;
		HBITMAP dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if(!dib)
			return;
		auto *out = static_cast<uint32 *>(bits);
		for(int y = 0; y < height; y++)
		{
			for(int x = 0; x < width; x++)
			{
				const COLORREF c = pixel(x, y);
				out[y * width + x] = (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
			}
		}
		bitmap.DeleteObject();
		bitmap.Attach(dib);
	}

	void BuildSquare()
	{
		const int w = m_square.Width(), h = m_square.Height();
		BuildBitmap(m_squareBitmap, w, h, [&](int x, int y)
		{
			return FromHSV(HSV{m_hsv.h, x / double(std::max(w - 1, 1)), 1.0 - y / double(std::max(h - 1, 1))});
		});
		m_squareHue = m_hsv.h;
	}

	void BuildHueStrip()
	{
		const int w = m_hue.Width(), h = m_hue.Height();
		BuildBitmap(m_hueBitmap, w, h, [&](int, int y)
		{
			return FromHSV(HSV{360.0 * y / double(std::max(h, 1)), 1.0, 1.0});
		});
	}

	void UpdateHex()
	{
		CString text;
		text.Format(_T("#%02X%02X%02X"), GetRValue(m_color), GetGValue(m_color), GetBValue(m_color));
		m_updatingHex = true;
		m_hexEdit.SetWindowText(text);
		m_updatingHex = false;
	}

	// Цвет пришёл целиком (образец, поле #RRGGBB): пересчитать тон и
	// насыщенность, но у серого тон оставить прежним — его не видно.
	void SetColor(COLORREF color, bool updateHex = true)
	{
		m_color = color;
		const HSV hsv = ToHSV(color);
		m_hsv.s = hsv.s;
		m_hsv.v = hsv.v;
		if(hsv.s > 0.0)
			m_hsv.h = hsv.h;
		if(m_hsv.h != m_squareHue)
			BuildSquare();
		if(updateHex)
			UpdateHex();
		Invalidate(FALSE);
	}

	void SetFromHSV()
	{
		m_color = FromHSV(m_hsv);
		if(m_hsv.h != m_squareHue)
			BuildSquare();
		UpdateHex();
		Invalidate(FALSE);
	}

	void Drag(CPoint point)
	{
		if(m_drag == DragSquare)
		{
			m_hsv.s = std::clamp((point.x - m_square.left) / double(std::max(m_square.Width() - 1, 1)), 0.0, 1.0);
			m_hsv.v = std::clamp(1.0 - (point.y - m_square.top) / double(std::max(m_square.Height() - 1, 1)), 0.0, 1.0);
			SetFromHSV();
		} else if(m_drag == DragHue)
		{
			m_hsv.h = std::clamp(360.0 * (point.y - m_hue.top) / double(std::max(m_hue.Height(), 1)), 0.0, 359.9);
			SetFromHSV();
		}
	}

	std::optional<COLORREF> SwatchAt(CPoint point) const
	{
		for(size_t i = 0; i < m_paletteRects.size(); i++)
			if(m_paletteRects[i].PtInRect(point))
				return Palette[i];
		for(size_t i = 0; i < m_usedRects.size(); i++)
			if(m_usedRects[i].PtInRect(point))
				return m_used[i];
		if(m_oldSwatch.PtInRect(point))
			return m_original;
		return std::nullopt;
	}

	void DrawSwatch(CDC &dc, const CRect &rect, COLORREF color)
	{
		dc.FillSolidRect(rect, color);
		const bool current = color == m_color;
		CRect frame = rect;
		dc.Draw3dRect(frame, current ? DarkUI::Color(COLOR_BTNTEXT) : DarkUI::Color(COLOR_BTNSHADOW), current ? DarkUI::Color(COLOR_BTNTEXT) : DarkUI::Color(COLOR_BTNSHADOW));
		if(current)
		{
			frame.DeflateRect(1, 1);
			dc.Draw3dRect(frame, DarkUI::Color(COLOR_BTNFACE), DarkUI::Color(COLOR_BTNFACE));
		}
	}

	void DrawLabel(CDC &dc, const CRect &rect, const TCHAR *text)
	{
		dc.SetTextColor(DarkUI::Color(COLOR_BTNTEXT));
		CRect r = rect;
		dc.DrawText(text, r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
	}

	afx_msg void OnPaint()
	{
		CPaintDC paint(this);
		CRect client;
		GetClientRect(client);
		CDC dc;
		dc.CreateCompatibleDC(&paint);
		CBitmap buffer;
		buffer.CreateCompatibleBitmap(&paint, client.Width(), client.Height());
		CBitmap *oldBuffer = dc.SelectObject(&buffer);
		dc.FillSolidRect(client, DarkUI::Color(COLOR_BTNFACE));
		CFont *oldFont = dc.SelectObject(GetFont());
		dc.SetBkMode(TRANSPARENT);

		CDC source;
		source.CreateCompatibleDC(&paint);
		CBitmap *oldSource = source.SelectObject(&m_squareBitmap);
		dc.BitBlt(m_square.left, m_square.top, m_square.Width(), m_square.Height(), &source, 0, 0, SRCCOPY);
		source.SelectObject(&m_hueBitmap);
		dc.BitBlt(m_hue.left, m_hue.top, m_hue.Width(), m_hue.Height(), &source, 0, 0, SRCCOPY);
		source.SelectObject(oldSource);
		const COLORREF border = DarkUI::Color(COLOR_BTNSHADOW);
		CRect frame = m_square;
		frame.InflateRect(1, 1);
		dc.Draw3dRect(frame, border, border);
		frame = m_hue;
		frame.InflateRect(1, 1);
		dc.Draw3dRect(frame, border, border);

		// Метки: кольцо в квадрате, полоска на тоне — чёрная с белой, видна на любом цвете.
		const int radius = HighDPISupport::ScalePixels(5, m_hWnd);
		const CPoint mark(m_square.left + static_cast<int>(std::lround(m_hsv.s * (m_square.Width() - 1))), m_square.top + static_cast<int>(std::lround((1.0 - m_hsv.v) * (m_square.Height() - 1))));
		CRgn clip;
		clip.CreateRectRgnIndirect(m_square);
		dc.SelectClipRgn(&clip);
		dc.SelectStockObject(NULL_BRUSH);
		CPen white(PS_SOLID, 1, RGB(255, 255, 255)), black(PS_SOLID, 1, RGB(0, 0, 0));
		CPen *oldPen = dc.SelectObject(&black);
		dc.Ellipse(mark.x - radius - 1, mark.y - radius - 1, mark.x + radius + 2, mark.y + radius + 2);
		dc.SelectObject(&white);
		dc.Ellipse(mark.x - radius, mark.y - radius, mark.x + radius + 1, mark.y + radius + 1);
		dc.SelectClipRgn(nullptr);
		const int hueY = m_hue.top + static_cast<int>(std::lround(m_hsv.h / 360.0 * m_hue.Height()));
		dc.FillSolidRect(m_hue.left - 2, hueY - 2, m_hue.Width() + 4, 5, RGB(0, 0, 0));
		dc.FillSolidRect(m_hue.left - 1, hueY - 1, m_hue.Width() + 2, 3, RGB(255, 255, 255));
		dc.SelectObject(oldPen);

		dc.FillSolidRect(m_newSwatch, m_color);
		dc.FillSolidRect(m_oldSwatch, m_original);
		frame = CRect(m_newSwatch.left, m_newSwatch.top, m_oldSwatch.right, m_oldSwatch.bottom);
		dc.Draw3dRect(frame, border, border);
		DrawLabel(dc, m_hexLabel, _T("Hex:"));

		for(size_t i = 0; i < m_paletteRects.size(); i++)
			DrawSwatch(dc, m_paletteRects[i], Palette[i]);
		if(!m_usedRects.empty())
		{
			DrawLabel(dc, m_usedLabel, _T("In use:"));
			for(size_t i = 0; i < m_usedRects.size(); i++)
				DrawSwatch(dc, m_usedRects[i], m_used[i]);
		}

		paint.BitBlt(0, 0, client.Width(), client.Height(), &dc, 0, 0, SRCCOPY);
		dc.SelectObject(oldFont);
		dc.SelectObject(oldBuffer);
	}

	afx_msg BOOL OnEraseBkgnd(CDC *) { return TRUE; }

	afx_msg void OnLButtonDown(UINT, CPoint point)
	{
		CRect hueHit = m_hue;
		hueHit.InflateRect(4, 2);
		if(m_square.PtInRect(point))
			m_drag = DragSquare;
		else if(hueHit.PtInRect(point))
			m_drag = DragHue;
		else if(const auto swatch = SwatchAt(point))
			SetColor(*swatch);
		if(m_drag != DragNone)
		{
			SetCapture();
			Drag(point);
		}
	}

	afx_msg void OnMouseMove(UINT, CPoint point)
	{
		if(m_drag != DragNone)
			Drag(point);
	}

	afx_msg void OnLButtonUp(UINT, CPoint)
	{
		if(m_drag != DragNone)
		{
			m_drag = DragNone;
			ReleaseCapture();
		}
	}

	afx_msg void OnLButtonDblClk(UINT, CPoint point)
	{
		// Двойной щелчок по образцу — сразу выбрать его и закрыть.
		if(const auto swatch = SwatchAt(point))
		{
			SetColor(*swatch);
			EndDialog(IDOK);
		}
	}

	afx_msg void OnHexChanged()
	{
		if(m_updatingHex)
			return;
		CString text;
		m_hexEdit.GetWindowText(text);
		text.Trim();
		text.TrimLeft(_T('#'));
		if(text.GetLength() != 6)
			return;
		TCHAR *end = nullptr;
		const unsigned long value = _tcstoul(text, &end, 16);
		if(!end || *end)
			return;
		SetColor(RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF), false);
	}

	DECLARE_MESSAGE_MAP()

	enum DragMode { DragNone, DragSquare, DragHue };

	std::vector<WORD> m_template;
	COLORREF m_color, m_original;
	HSV m_hsv;
	double m_squareHue = -1.0;
	std::vector<COLORREF> m_used;
	CRect m_square, m_hue, m_newSwatch, m_oldSwatch, m_hexLabel, m_usedLabel;
	std::vector<CRect> m_paletteRects, m_usedRects;
	CBitmap m_squareBitmap, m_hueBitmap;
	CEdit m_hexEdit;
	CButton m_ok, m_cancel;
	DragMode m_drag = DragNone;
	bool m_updatingHex = false;
};

BEGIN_MESSAGE_MAP(Dialog, CDialog)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_LBUTTONDOWN()
	ON_WM_MOUSEMOVE()
	ON_WM_LBUTTONUP()
	ON_WM_LBUTTONDBLCLK()
	ON_EN_CHANGE(IdcHex, &Dialog::OnHexChanged)
END_MESSAGE_MAP()

}  // namespace


std::optional<COLORREF> Pick(CWnd *parent, COLORREF initial, mpt::span<const COLORREF> usedColors)
{
	Dialog dlg(initial & 0xFFFFFF, usedColors);
	if(dlg.Run(parent) == IDOK)
		return dlg.Color();
	return std::nullopt;
}

}  // namespace ColorPicker

OPENMPT_NAMESPACE_END
