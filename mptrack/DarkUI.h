#pragma once

OPENMPT_NAMESPACE_BEGIN
namespace DarkUI
{
void Initialize(bool enabled);
bool Enabled();
COLORREF Color(int index);
HBRUSH Brush(int index);
}
OPENMPT_NAMESPACE_END

