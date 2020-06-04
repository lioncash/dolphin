// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"

#include "Core/HW/Memmap.h"

#include "VideoCommon/CPMemory.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/XFMemory.h"

static void XFMemWritten(u32 transferSize, u32 baseAddress)
{
  g_vertex_manager->Flush();
  VertexShaderManager::InvalidateXFRange(baseAddress, baseAddress + transferSize);
}

static void XFRegWritten(int transferSize, u32 baseAddress, DataReader src)
{
  u32 address = baseAddress;
  u32 dataIndex = 0;

  while (transferSize > 0 && address < 0x1058)
  {
    u32 newValue = src.Peek<u32>(dataIndex * sizeof(u32));
    u32 nextAddress = address + 1;

    switch (address)
    {
    case XFMEM_ERROR:
    case XFMEM_DIAG:
    case XFMEM_STATE0:  // internal state 0
    case XFMEM_STATE1:  // internal state 1
    case XFMEM_CLOCK:
    case XFMEM_SETGPMETRIC:
      nextAddress = 0x1007;
      break;

    case XFMEM_CLIPDISABLE:
      // if (data & 1) {} // disable clipping detection
      // if (data & 2) {} // disable trivial rejection
      // if (data & 4) {} // disable cpoly clipping acceleration
      break;

    case XFMEM_VTXSPECS:  //__GXXfVtxSpecs, wrote 0004
      break;

    case XFMEM_SETNUMCHAN:
      if (xfmem.numChan.numColorChans != (newValue & 3))
        g_vertex_manager->Flush();
      VertexShaderManager::SetLightingConfigChanged();
      break;

    case XFMEM_SETCHAN0_AMBCOLOR:  // Channel Ambient Color
    case XFMEM_SETCHAN1_AMBCOLOR:
    {
      u8 chan = address - XFMEM_SETCHAN0_AMBCOLOR;
      if (xfmem.ambColor[chan] != newValue)
      {
        g_vertex_manager->Flush();
        VertexShaderManager::SetMaterialColorChanged(chan);
      }
      break;
    }

    case XFMEM_SETCHAN0_MATCOLOR:  // Channel Material Color
    case XFMEM_SETCHAN1_MATCOLOR:
    {
      u8 chan = address - XFMEM_SETCHAN0_MATCOLOR;
      if (xfmem.matColor[chan] != newValue)
      {
        g_vertex_manager->Flush();
        VertexShaderManager::SetMaterialColorChanged(chan + 2);
      }
      break;
    }

    case XFMEM_SETCHAN0_COLOR:  // Channel Color
    case XFMEM_SETCHAN1_COLOR:
    case XFMEM_SETCHAN0_ALPHA:  // Channel Alpha
    case XFMEM_SETCHAN1_ALPHA:
      if (((u32*)&xfmem)[address] != (newValue & 0x7fff))
        g_vertex_manager->Flush();
      VertexShaderManager::SetLightingConfigChanged();
      break;

    case XFMEM_DUALTEX:
      if (xfmem.dualTexTrans.enabled != (newValue & 1))
        g_vertex_manager->Flush();
      VertexShaderManager::SetTexMatrixInfoChanged(-1);
      break;

    case XFMEM_SETMATRIXINDA:
      VertexShaderManager::SetTexMatrixChangedA(newValue);
      break;
    case XFMEM_SETMATRIXINDB:
      VertexShaderManager::SetTexMatrixChangedB(newValue);
      break;

    case XFMEM_SETVIEWPORT:
    case XFMEM_SETVIEWPORT + 1:
    case XFMEM_SETVIEWPORT + 2:
    case XFMEM_SETVIEWPORT + 3:
    case XFMEM_SETVIEWPORT + 4:
    case XFMEM_SETVIEWPORT + 5:
      g_vertex_manager->Flush();
      VertexShaderManager::SetViewportChanged();
      PixelShaderManager::SetViewportChanged();
      GeometryShaderManager::SetViewportChanged();

      nextAddress = XFMEM_SETVIEWPORT + 6;
      break;

    case XFMEM_SETPROJECTION:
    case XFMEM_SETPROJECTION + 1:
    case XFMEM_SETPROJECTION + 2:
    case XFMEM_SETPROJECTION + 3:
    case XFMEM_SETPROJECTION + 4:
    case XFMEM_SETPROJECTION + 5:
    case XFMEM_SETPROJECTION + 6:
      g_vertex_manager->Flush();
      VertexShaderManager::SetProjectionChanged();
      GeometryShaderManager::SetProjectionChanged();

      nextAddress = XFMEM_SETPROJECTION + 7;
      break;

    case XFMEM_SETNUMTEXGENS:  // GXSetNumTexGens
      if (xfmem.numTexGen.numTexGens != (newValue & 15))
        g_vertex_manager->Flush();
      break;

    case XFMEM_SETTEXMTXINFO:
    case XFMEM_SETTEXMTXINFO + 1:
    case XFMEM_SETTEXMTXINFO + 2:
    case XFMEM_SETTEXMTXINFO + 3:
    case XFMEM_SETTEXMTXINFO + 4:
    case XFMEM_SETTEXMTXINFO + 5:
    case XFMEM_SETTEXMTXINFO + 6:
    case XFMEM_SETTEXMTXINFO + 7:
      g_vertex_manager->Flush();
      VertexShaderManager::SetTexMatrixInfoChanged(address - XFMEM_SETTEXMTXINFO);

      nextAddress = XFMEM_SETTEXMTXINFO + 8;
      break;

    case XFMEM_SETPOSTMTXINFO:
    case XFMEM_SETPOSTMTXINFO + 1:
    case XFMEM_SETPOSTMTXINFO + 2:
    case XFMEM_SETPOSTMTXINFO + 3:
    case XFMEM_SETPOSTMTXINFO + 4:
    case XFMEM_SETPOSTMTXINFO + 5:
    case XFMEM_SETPOSTMTXINFO + 6:
    case XFMEM_SETPOSTMTXINFO + 7:
      g_vertex_manager->Flush();
      VertexShaderManager::SetTexMatrixInfoChanged(address - XFMEM_SETPOSTMTXINFO);

      nextAddress = XFMEM_SETPOSTMTXINFO + 8;
      break;

    // --------------
    // Unknown Regs
    // --------------

    // Maybe these are for Normals?
    case 0x1048:  // xfmem.texcoords[0].nrmmtxinfo.hex = data; break; ??
    case 0x1049:
    case 0x104a:
    case 0x104b:
    case 0x104c:
    case 0x104d:
    case 0x104e:
    case 0x104f:
      DEBUG_LOG(VIDEO, "Possible Normal Mtx XF reg?: %x=%x", address, newValue);
      break;

    case 0x1013:
    case 0x1014:
    case 0x1015:
    case 0x1016:
    case 0x1017:

    default:
      if (newValue != 0)  // Ignore writes of zero.
        WARN_LOG(VIDEO, "Unknown XF Reg: %x=%x", address, newValue);
      break;
    }

    int transferred = nextAddress - address;
    address = nextAddress;

    transferSize -= transferred;
    dataIndex += transferred;
  }
}

void LoadXFReg(u32 transferSize, u32 baseAddress, DataReader src)
{
  // do not allow writes past registers
  if (baseAddress + transferSize > 0x1058)
  {
    WARN_LOG(VIDEO, "XF load exceeds address space: %x %d bytes", baseAddress, transferSize);

    if (baseAddress >= 0x1058)
      transferSize = 0;
    else
      transferSize = 0x1058 - baseAddress;
  }

  // write to XF mem
  if (baseAddress < 0x1000 && transferSize > 0)
  {
    u32 end = baseAddress + transferSize;

    u32 xfMemBase = baseAddress;
    u32 xfMemTransferSize = transferSize;

    if (end >= 0x1000)
    {
      xfMemTransferSize = 0x1000 - baseAddress;

      baseAddress = 0x1000;
      transferSize = end - 0x1000;
    }
    else
    {
      transferSize = 0;
    }

    XFMemWritten(xfMemTransferSize, xfMemBase);
    for (u32 i = 0; i < xfMemTransferSize; i++)
    {
      ((u32*)&xfmem)[xfMemBase + i] = src.Read<u32>();
    }
  }

  // write to XF regs
  if (transferSize > 0)
  {
    XFRegWritten(transferSize, baseAddress, src);
    for (u32 i = 0; i < transferSize; i++)
    {
      ((u32*)&xfmem)[baseAddress + i] = src.Read<u32>();
    }
  }
}

// TODO - verify that it is correct. Seems to work, though.
void LoadIndexedXF(u32 val, int refarray)
{
  int index = val >> 16;
  int address = val & 0xFFF;  // check mask
  int size = ((val >> 12) & 0xF) + 1;
  // load stuff from array to address in xf mem

  u32* currData = (u32*)(&xfmem) + address;
  u32* newData;
  if (Fifo::UseDeterministicGPUThread())
  {
    newData = (u32*)Fifo::PopFifoAuxBuffer(size * sizeof(u32));
  }
  else
  {
    newData = (u32*)Memory::GetPointer(g_main_cp_state.array_bases[refarray] +
                                       g_main_cp_state.array_strides[refarray] * index);
  }
  bool changed = false;
  for (int i = 0; i < size; ++i)
  {
    if (currData[i] != Common::swap32(newData[i]))
    {
      changed = true;
      XFMemWritten(size, address);
      break;
    }
  }
  if (changed)
  {
    for (int i = 0; i < size; ++i)
      currData[i] = Common::swap32(newData[i]);
  }
}

void PreprocessIndexedXF(u32 val, int refarray)
{
  const u32 index = val >> 16;
  const u32 size = ((val >> 12) & 0xF) + 1;

  const u8* new_data = Memory::GetPointer(g_preprocess_cp_state.array_bases[refarray] +
                                          g_preprocess_cp_state.array_strides[refarray] * index);

  const size_t buf_size = size * sizeof(u32);
  Fifo::PushFifoAuxBuffer(new_data, buf_size);
}

int GetXFRegInfo(u32 newValue, u32 address, std::string* name, std::string* desc)
{
  const char* no_yes[2] = {"No", "Yes"};
// Macro to set the register name and make sure it was written correctly via compile time assertion
#define SetRegName(reg)                                                                            \
  *name = fmt::format("{} = {:x}", #reg, newValue);                                                \
  (void)(reg);

  int color = 0;
  switch (address)
  {
  case XFMEM_ERROR:
    SetRegName(XFMEM_ERROR);
    break;
  case XFMEM_DIAG:
    SetRegName(XFMEM_DIAG);
    break;
  case XFMEM_STATE0:  // internal state 0
    SetRegName(XFMEM_STATE0);
    *desc = "internal state 0";
    break;
  case XFMEM_STATE1:  // internal state 1
    SetRegName(XFMEM_STATE1);
    *desc = "internal state 1";
    break;
  case XFMEM_CLOCK:
    SetRegName(XFMEM_CLOCK);
    break;
  case XFMEM_SETGPMETRIC:
    SetRegName(XFMEM_SETGPMETRIC);
    break;

  case XFMEM_CLIPDISABLE:
    SetRegName(XFMEM_CLIPDISABLE);
    *desc = std::string("");
    if (newValue & 1)
    {
      *desc += "disable clipping detection";
    }
    if (newValue & 2)
    {
      *desc += "disable trivial rejection";
    }
    if (newValue & 4)
    {
      *desc += "disable cpoly clipping acceleration";
    }
    break;

  case XFMEM_VTXSPECS:  //__GXXfVtxSpecs, wrote 0004
    SetRegName(XFMEM_VTXSPECS);
    *desc = "__GXXfVtxSpecs, wrote 0004";
    break;

  case XFMEM_SETNUMCHAN:
    SetRegName(XFMEM_SETNUMCHAN);
    *desc = fmt::format("Number of color channels = {}", newValue & 3);
    break;

  case XFMEM_SETCHAN0_AMBCOLOR:  // Channel Ambient Color
  {
    SetRegName(XFMEM_SETCHAN0_AMBCOLOR);
    *desc = fmt::format("Channel 0 Ambient Color = {:x}", newValue);
    break;
  }
  case XFMEM_SETCHAN1_AMBCOLOR:
  {
    SetRegName(XFMEM_SETCHAN1_AMBCOLOR);
    *desc = fmt::format("Channel 1 Ambient Color = {:x}", newValue);
    break;
  }

  case XFMEM_SETCHAN0_MATCOLOR:  // Channel Material Color
    SetRegName(XFMEM_SETCHAN0_MATCOLOR);
    *desc = fmt::format("Channel 0 Material Color = {:x}", newValue);
    break;
  case XFMEM_SETCHAN1_MATCOLOR:
    SetRegName(XFMEM_SETCHAN1_MATCOLOR);
    *desc = fmt::format("Channel 1 Material Color = {:x}", newValue);
    break;

  case XFMEM_SETCHAN0_COLOR:  // Channel Color
    SetRegName(XFMEM_SETCHAN0_COLOR);
    *desc = fmt::format("Channel 0 Color = {:x}", newValue);
    break;
  case XFMEM_SETCHAN1_COLOR:
    SetRegName(XFMEM_SETCHAN1_COLOR);
    *desc = fmt::format("Channel 1 Color = {:x}", newValue);
    break;
  case XFMEM_SETCHAN0_ALPHA:  // Channel Alpha
    SetRegName(XFMEM_SETCHAN0_ALPHA);
    *desc = fmt::format("Channel 0 Alpha = {:x}", newValue & 0x7fff);
    break;
  case XFMEM_SETCHAN1_ALPHA:
    SetRegName(XFMEM_SETCHAN1_ALPHA);
    *desc = fmt::format("Channel 1 Alpha = {:x}", newValue & 0x7fff);
    break;

  case XFMEM_DUALTEX:
    SetRegName(XFMEM_DUALTEX);
    *desc = fmt::format("Dual Tex Trans enabled = {}", no_yes[newValue & 1]);
    break;

  case XFMEM_SETMATRIXINDA:
    SetRegName(XFMEM_SETMATRIXINDA);
    break;
  case XFMEM_SETMATRIXINDB:
    SetRegName(XFMEM_SETMATRIXINDB);
    break;

  case XFMEM_SETVIEWPORT:
  case XFMEM_SETVIEWPORT + 1:
  case XFMEM_SETVIEWPORT + 2:
  case XFMEM_SETVIEWPORT + 3:
  case XFMEM_SETVIEWPORT + 4:
  case XFMEM_SETVIEWPORT + 5:
    *name = fmt::format("XFMEM_SETVIEWPORT{}", address - XFMEM_SETVIEWPORT);
    *desc = "Set Viewport";
    color = 3;
    break;

  case XFMEM_SETPROJECTION:
  case XFMEM_SETPROJECTION + 1:
  case XFMEM_SETPROJECTION + 2:
  case XFMEM_SETPROJECTION + 3:
  case XFMEM_SETPROJECTION + 4:
  case XFMEM_SETPROJECTION + 5:
  case XFMEM_SETPROJECTION + 6:
    *name = fmt::format("XFMEM_SETPROJECTION{}", address - XFMEM_SETPROJECTION);
    *desc = "Set Projection";
    color = 3;
    break;

  case XFMEM_SETNUMTEXGENS:  // GXSetNumTexGens
    SetRegName(XFMEM_SETNUMTEXGENS);
    *desc = fmt::format("GXSetNumTexGens {}", newValue & 15);
    break;

  case XFMEM_SETTEXMTXINFO:
  case XFMEM_SETTEXMTXINFO + 1:
  case XFMEM_SETTEXMTXINFO + 2:
  case XFMEM_SETTEXMTXINFO + 3:
  case XFMEM_SETTEXMTXINFO + 4:
  case XFMEM_SETTEXMTXINFO + 5:
  case XFMEM_SETTEXMTXINFO + 6:
  case XFMEM_SETTEXMTXINFO + 7:
    *name = fmt::format("XFMEM_SETTEXMTXINFO{}", address - XFMEM_SETTEXMTXINFO);
    break;

  case XFMEM_SETPOSTMTXINFO:
  case XFMEM_SETPOSTMTXINFO + 1:
  case XFMEM_SETPOSTMTXINFO + 2:
  case XFMEM_SETPOSTMTXINFO + 3:
  case XFMEM_SETPOSTMTXINFO + 4:
  case XFMEM_SETPOSTMTXINFO + 5:
  case XFMEM_SETPOSTMTXINFO + 6:
  case XFMEM_SETPOSTMTXINFO + 7:
    *name = fmt::format("XFMEM_SETPOSTMTXINFO{}", address - XFMEM_SETPOSTMTXINFO);
    break;

  // --------------
  // Unknown Regs
  // --------------

  // Maybe these are for Normals?
  case 0x1048:  // xfmem.texcoords[0].nrmmtxinfo.hex = data; break; ??
  case 0x1049:
  case 0x104a:
  case 0x104b:
  case 0x104c:
  case 0x104d:
  case 0x104e:
  case 0x104f:
    *name = fmt::format("Possible Normal Mtx XF reg?: {:x}={:x}", address, newValue);
    *desc = "Maybe these are for Normals? xfmem.texcoords[0].nrmmtxinfo.hex = data; break; ??";
    break;

  case 0x1013:
  case 0x1014:
  case 0x1015:
  case 0x1016:
  case 0x1017:

  default:
    *name = fmt::format("Unknown XF Reg: {:x}={:x}", address, newValue);
    break;
  }
  return color;
#undef SetRegName
}

int GetXFTransferInfo(const u8* data, std::string* name, std::string* desc)
{
  int color = 0;
  const u32 cmd2 = Common::swap32(data);
  data += 4;
  u32 transferSize = ((cmd2 >> 16) & 15) + 1;
  u32 baseAddress = cmd2 & 0xFFFF;
  *desc = std::string("");
  *name = std::string("");
  // do not allow writes past registers
  if (baseAddress + transferSize > 0x1058)
  {
    *name += "Invalid XF Transfer";
    *desc +=
        fmt::format("XF load exceeds address space: {:x} {} bytes\n", baseAddress, transferSize);
    if (baseAddress >= 0x1058)
      transferSize = 0;
    else
      transferSize = 0x1058 - baseAddress;
  }
  // write to XF mem
  if (baseAddress < 0x1000 && transferSize > 0)
  {
    u32 end = baseAddress + transferSize;

    u32 xfMemBase = baseAddress;
    u32 xfMemTransferSize = transferSize;

    if (end >= 0x1000)
    {
      xfMemTransferSize = 0x1000 - baseAddress;
      data += 4 * xfMemTransferSize;
      baseAddress = 0x1000;
      transferSize = end - 0x1000;
    }
    else
    {
      transferSize = 0;
    }
    *name += fmt::format("Write XF mem {:x} {} ", xfMemBase, xfMemTransferSize);
  }

  // write to XF regs
  if (transferSize <= 0)
    return 0;
  if (transferSize == 1)
  {
    return GetXFRegInfo(Common::swap32(data), baseAddress, name, desc);
  }
  else
  {
    *name += fmt::format("Write XF regs {:x} {} ", baseAddress, transferSize);
    u32 address = baseAddress;
    bool colors[4]{};
    while (transferSize > 0 && address < 0x1058)
    {
      u32 newValue = Common::swap32(data);
      std::string name2("");
      std::string desc2("");
      color = GetXFRegInfo(newValue, address, &name2, &desc2);
      colors[color] = true;
      *desc += "\n";
      *desc += name2;
      if (!desc2.empty())
      {
        *desc += "\n";
        *desc += desc2;
      }
      address++;
      transferSize--;
      data += 4;
    }
    if (colors[1])
      return 1;
    if (colors[2])
      return 2;
    if (colors[3])
      return 3;
    return 0;
  }
}

void SimulateXFTransfer(const u8* data, XFMemory* xf, bool* projection_set, bool* viewport_set)
{
  const u32 cmd2 = Common::swap32(data);
  data += 4;
  u32 transferSize = ((cmd2 >> 16) & 15) + 1;
  u32 baseAddress = cmd2 & 0xFFFF;
  // do not allow writes past registers
  if (baseAddress + transferSize > 0x1058)
  {
    if (baseAddress >= 0x1058)
      transferSize = 0;
    else
      transferSize = 0x1058 - baseAddress;
  }
  u32 address = baseAddress;
  u32* xfm = (u32*)xf;
  while (transferSize > 0 && address < 0x1058)
  {
    u32 newValue = Common::swap32(data);
    xfm[address] = newValue;
    if (XFMEM_SETPROJECTION <= address && address <= XFMEM_SETPROJECTION + 6)
      *projection_set = true;
    if (XFMEM_SETVIEWPORT <= address && address <= XFMEM_SETVIEWPORT + 5)
      *viewport_set = true;

    address++;
    transferSize--;
    data += 4;
  }
}
