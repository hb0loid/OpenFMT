/*
 * ColorPickerDlg.h
 * ----------------
 * Purpose: OpenFMT's own colour picker. The Windows one cannot be themed (white
 *          frames around its swatches, light panels) and its preset colours do
 *          not suit a dark tracker.
 * Notes  : The dialog has no resource template; all controls are created in code.
 * Authors: OpenFMT
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */


#pragma once

#include "openmpt/all/BuildSettings.hpp"

OPENMPT_NAMESPACE_BEGIN

namespace ColorPicker
{
// usedColors: colours already in use (song channels, colour scheme...), shown as
// a row of their own. Returns the chosen colour, or nothing if cancelled.
std::optional<COLORREF> Pick(CWnd *parent, COLORREF initial, mpt::span<const COLORREF> usedColors);
}

OPENMPT_NAMESPACE_END
