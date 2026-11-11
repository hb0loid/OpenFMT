/*
 * YM2612Preset.h
 * Standalone FM preset import for the sample editor.
 */

#pragma once

#include "Snd_defs.h"
#include "../common/FileReader.h"

OPENMPT_NAMESPACE_BEGIN

enum class YM2612PresetFormat { None, Furnace, DMP, TFI, OPNI };

struct YM2612Preset
{
	OPN2Patch patch;
	std::string name;
};

YM2612PresetFormat GetYM2612PresetFormat(std::string_view extension);
std::optional<YM2612Preset> ReadYM2612Preset(FileReader file, YM2612PresetFormat format);

OPENMPT_NAMESPACE_END

