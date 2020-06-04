// Copyright 2011 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>

#include "Core/FifoPlayer/FifoDataFile.h"

struct ClearInfo
{
  u32 address;
  u32 value;
};

struct AnalyzedFrameInfo
{
  std::vector<u32> objectStarts;
  std::vector<u32> objectEnds;
  std::vector<MemoryUpdate> memoryUpdates;
  std::vector<ClearInfo> clears;
};

namespace FifoPlaybackAnalyzer
{
void AnalyzeFrames(FifoDataFile* file, std::vector<AnalyzedFrameInfo>& frameInfo);
}  // namespace FifoPlaybackAnalyzer
