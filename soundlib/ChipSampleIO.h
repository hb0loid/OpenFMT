/*
 * ChipSampleIO.h
 * --------------
 * Purpose: Shared storage of native chip patches inside IT sample headers.
 * Notes  : The sample conversion byte marks which chip a patch belongs to, and the
 *          patch itself takes the place of the sample data. The same layout is used
 *          by the module writer and by standalone instrument and sample files, so a
 *          patch saved from one of them loads back in the others.
 *
 *          A reader that does not know these conversion codes sees an OPL patch,
 *          because that is what the stock format uses for the same field.
 *
 */

#pragma once

#include "openmpt/all/BuildSettings.hpp"

#include "Snd_defs.h"
#include "../common/FileReader.h"

#include <memory>
#include <ostream>

OPENMPT_NAMESPACE_BEGIN

// Коды конверсии сэмпла, которыми помечается патч нативного чипа.
inline constexpr uint8 FmtOPN2Sample = 0x41, FmtOPMSample = 0x42, FmtPSGSample = 0x43;

inline std::shared_ptr<PSGPatch> ReadPSGPatch(FileReader &file)
{
	if(!file.CanRead(5))
		return {};
	auto patch = std::make_shared<PSGPatch>();
	patch->lane = file.ReadUint8();
	patch->chip = file.ReadUint8();
	const auto chipFlags = file.ReadUint8();
	patch->easyPitch = (chipFlags & 1) != 0;
	patch->resetNoise = (chipFlags & 2) != 0;
	patch->furnacePitch = (chipFlags & 4) != 0;
	patch->fastNoise = (chipFlags & 8) != 0;
	patch->mixGain = std::min<uint16>(512, file.ReadUint16LE());
	if(patch->lane >= 4 || patch->chip >= 32)
		return {};
	for(auto &macro : patch->macros)
	{
		if(!file.CanRead(6))
			return {};
		const auto length = file.ReadUint8();
		macro.loop = file.ReadUint8();
		macro.release = file.ReadUint8();
		macro.delay = file.ReadUint8();
		macro.speed = std::max<uint8>(1, file.ReadUint8());
		const auto flags = file.ReadUint8();
		macro.relative = (flags & 1) != 0;
		macro.instantRelease = (flags & 2) != 0;
		if(!file.CanRead(length * 4u))
			return {};
		macro.values.reserve(length);
		for(size_t i = 0; i < length; i++)
			macro.values.push_back(file.ReadInt32LE());
	}
	return patch;
}

#ifndef MODPLUG_NO_FILESAVE
inline size_t WritePSGPatch(std::ostream &file, const PSGPatch &patch)
{
	mpt::IO::WriteIntLE(file, patch.lane);
	mpt::IO::WriteIntLE(file, patch.chip);
	mpt::IO::WriteIntLE(file, static_cast<uint8>((patch.easyPitch ? 1 : 0) | (patch.resetNoise ? 2 : 0) | (patch.furnacePitch ? 4 : 0) | (patch.fastNoise ? 8 : 0)));
	mpt::IO::WriteIntLE(file, patch.mixGain);
	size_t bytes = 5;
	for(const auto &macro : patch.macros)
	{
		const auto length = mpt::saturate_cast<uint8>(macro.values.size());
		const uint8 flags = (macro.relative ? 1 : 0) | (macro.instantRelease ? 2 : 0);
		const uint8 header[] = {length, macro.loop, macro.release, macro.delay, macro.speed, flags};
		mpt::IO::WriteRaw(file, header, sizeof(header));
		for(size_t i = 0; i < length; i++)
			mpt::IO::WriteIntLE(file, macro.values[i]);
		bytes += sizeof(header) + length * sizeof(int32);
	}
	return bytes;
}
#endif

OPENMPT_NAMESPACE_END
