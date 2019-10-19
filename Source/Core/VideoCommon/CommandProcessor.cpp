// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/CommandProcessor.h"

#include "Common/Assert.h"
#include "Common/Atomic.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "VideoCommon/Fifo.h"

namespace CommandProcessor
{
static CoreTiming::EventType* s_update_interrupts;

// TODO(ector): Warn on bbox read/write

// STATE_TO_SAVE
SCPFifoStruct fifo;
static UCPStatusReg s_cp_status_reg;
static UCPCtrlReg s_cp_ctrl_reg;
static UCPClearReg s_cp_clear_reg;

static MathUtil::Rectangle<u16> s_bbox;
static u16 s_token_reg;

static Common::Flag s_interrupt_set;
static Common::Flag s_interrupt_waiting;

static bool IsOnThread()
{
  return SConfig::GetInstance().bCPUThread;
}

static void UpdateInterrupts_Wrapper(u64 userdata, s64 cyclesLate)
{
  UpdateInterrupts(userdata);
}

void SCPFifoStruct::DoState(PointerWrap& p)
{
  p.Do(CPBase);
  p.Do(CPEnd);
  p.Do(CPHiWatermark);
  p.Do(CPLoWatermark);
  p.Do(CPReadWriteDistance);
  p.Do(CPWritePointer);
  p.Do(CPReadPointer);
  p.Do(CPBreakpoint);
  p.Do(SafeCPReadPointer);

  p.Do(bFF_GPLinkEnable);
  p.Do(bFF_GPReadEnable);
  p.Do(bFF_BPEnable);
  p.Do(bFF_BPInt);
  p.Do(bFF_Breakpoint);

  p.Do(bFF_LoWatermarkInt);
  p.Do(bFF_HiWatermarkInt);

  p.Do(bFF_LoWatermark);
  p.Do(bFF_HiWatermark);
}

void DoState(PointerWrap& p)
{
  p.DoPOD(s_cp_status_reg);
  p.DoPOD(s_cp_ctrl_reg);
  p.DoPOD(s_cp_clear_reg);
  p.Do(s_bbox.left);
  p.Do(s_bbox.top);
  p.Do(s_bbox.right);
  p.Do(s_bbox.bottom);
  p.Do(s_token_reg);
  fifo.DoState(p);

  p.Do(s_interrupt_set);
  p.Do(s_interrupt_waiting);
}

static inline void WriteLow(volatile u32& _reg, u16 lowbits)
{
  Common::AtomicStore(_reg, (_reg & 0xFFFF0000) | lowbits);
}
static inline void WriteHigh(volatile u32& _reg, u16 highbits)
{
  Common::AtomicStore(_reg, (_reg & 0x0000FFFF) | ((u32)highbits << 16));
}
static inline u16 ReadLow(u32 _reg)
{
  return (u16)(_reg & 0xFFFF);
}
static inline u16 ReadHigh(u32 _reg)
{
  return (u16)(_reg >> 16);
}

void Init()
{
  s_cp_status_reg.Hex = 0;
  s_cp_status_reg.CommandIdle = 1;
  s_cp_status_reg.ReadIdle = 1;

  s_cp_ctrl_reg.Hex = 0;
  s_cp_clear_reg.Hex = 0;

  s_bbox = {0, 0, 640, 480};
  s_token_reg = 0;

  fifo = {};

  s_interrupt_set.Clear();
  s_interrupt_waiting.Clear();

  s_update_interrupts = CoreTiming::RegisterEvent("CPInterrupt", UpdateInterrupts_Wrapper);
}

u32 GetPhysicalAddressMask()
{
  // Physical addresses in CP seem to ignore some of the upper bits (depending on platform)
  // This can be observed in CP MMIO registers by setting to 0xffffffff and then reading back.
  return SConfig::GetInstance().bWii ? 0x1fffffff : 0x03ffffff;
}

void RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  constexpr u16 WMASK_NONE = 0x0000;
  constexpr u16 WMASK_ALL = 0xffff;
  constexpr u16 WMASK_LO_ALIGN_32BIT = 0xffe0;
  const u16 WMASK_HI_RESTRICT = GetPhysicalAddressMask() >> 16;

  struct
  {
    u32 addr;
    u16* ptr;
    bool readonly;
    // FIFO mmio regs in the range [cc000020-cc00003e] have certain bits that always read as 0
    // For _LO registers in this range, only bits 0xffe0 can be set
    // For _HI registers in this range, only bits 0x03ff can be set on GCN and 0x1fff on Wii
    u16 wmask;
  } directly_mapped_vars[] = {
      {FIFO_TOKEN_REGISTER, &s_token_reg, false, WMASK_ALL},

      // Bounding box registers are read only.
      {FIFO_BOUNDING_BOX_LEFT, &s_bbox.left, true, WMASK_NONE},
      {FIFO_BOUNDING_BOX_RIGHT, &s_bbox.right, true, WMASK_NONE},
      {FIFO_BOUNDING_BOX_TOP, &s_bbox.top, true, WMASK_NONE},
      {FIFO_BOUNDING_BOX_BOTTOM, &s_bbox.bottom, true, WMASK_NONE},
      {FIFO_BASE_LO, MMIO::Utils::LowPart(&fifo.CPBase), false, WMASK_LO_ALIGN_32BIT},
      {FIFO_BASE_HI, MMIO::Utils::HighPart(&fifo.CPBase), false, WMASK_HI_RESTRICT},
      {FIFO_END_LO, MMIO::Utils::LowPart(&fifo.CPEnd), false, WMASK_LO_ALIGN_32BIT},
      {FIFO_END_HI, MMIO::Utils::HighPart(&fifo.CPEnd), false, WMASK_HI_RESTRICT},
      {FIFO_HI_WATERMARK_LO, MMIO::Utils::LowPart(&fifo.CPHiWatermark), false,
       WMASK_LO_ALIGN_32BIT},
      {FIFO_HI_WATERMARK_HI, MMIO::Utils::HighPart(&fifo.CPHiWatermark), false, WMASK_HI_RESTRICT},
      {FIFO_LO_WATERMARK_LO, MMIO::Utils::LowPart(&fifo.CPLoWatermark), false,
       WMASK_LO_ALIGN_32BIT},
      {FIFO_LO_WATERMARK_HI, MMIO::Utils::HighPart(&fifo.CPLoWatermark), false, WMASK_HI_RESTRICT},
      // FIFO_RW_DISTANCE has some complex read code different for
      // single/dual core.
      {FIFO_WRITE_POINTER_LO, MMIO::Utils::LowPart(&fifo.CPWritePointer), false,
       WMASK_LO_ALIGN_32BIT},
      {FIFO_WRITE_POINTER_HI, MMIO::Utils::HighPart(&fifo.CPWritePointer), false,
       WMASK_HI_RESTRICT},
      // FIFO_READ_POINTER has different code for single/dual core.
  };

  for (auto& mapped_var : directly_mapped_vars)
  {
    mmio->Register(base | mapped_var.addr, MMIO::DirectRead<u16>(mapped_var.ptr),
                   mapped_var.readonly ? MMIO::InvalidWrite<u16>() :
                                         MMIO::DirectWrite<u16>(mapped_var.ptr, mapped_var.wmask));
  }

  mmio->Register(base | FIFO_BP_LO, MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPBreakpoint)),
                 MMIO::ComplexWrite<u16>([WMASK_LO_ALIGN_32BIT](u32, u16 val) {
                   WriteLow(fifo.CPBreakpoint, val & WMASK_LO_ALIGN_32BIT);
                 }));
  mmio->Register(base | FIFO_BP_HI,
                 MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&fifo.CPBreakpoint)),
                 MMIO::ComplexWrite<u16>([WMASK_HI_RESTRICT](u32, u16 val) {
                   WriteHigh(fifo.CPBreakpoint, val & WMASK_HI_RESTRICT);
                 }));

  // Timing and metrics MMIOs are stubbed with fixed values.
  struct
  {
    u32 addr;
    u16 value;
  } metrics_mmios[] = {
      {XF_RASBUSY_L, 0},
      {XF_RASBUSY_H, 0},
      {XF_CLKS_L, 0},
      {XF_CLKS_H, 0},
      {XF_WAIT_IN_L, 0},
      {XF_WAIT_IN_H, 0},
      {XF_WAIT_OUT_L, 0},
      {XF_WAIT_OUT_H, 0},
      {VCACHE_METRIC_CHECK_L, 0},
      {VCACHE_METRIC_CHECK_H, 0},
      {VCACHE_METRIC_MISS_L, 0},
      {VCACHE_METRIC_MISS_H, 0},
      {VCACHE_METRIC_STALL_L, 0},
      {VCACHE_METRIC_STALL_H, 0},
      {CLKS_PER_VTX_OUT, 4},
  };
  for (auto& metrics_mmio : metrics_mmios)
  {
    mmio->Register(base | metrics_mmio.addr, MMIO::Constant<u16>(metrics_mmio.value),
                   MMIO::InvalidWrite<u16>());
  }

  mmio->Register(base | STATUS_REGISTER, MMIO::ComplexRead<u16>([](u32) {
                   SetCpStatusRegister();
                   return s_cp_status_reg.Hex;
                 }),
                 MMIO::InvalidWrite<u16>());

  mmio->Register(base | CTRL_REGISTER, MMIO::DirectRead<u16>(&s_cp_ctrl_reg.Hex),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   UCPCtrlReg tmp(val);
                   s_cp_ctrl_reg.Hex = tmp.Hex;
                   SetCpControlRegister();
                   Fifo::RunGpu();
                 }));

  mmio->Register(base | CLEAR_REGISTER, MMIO::DirectRead<u16>(&s_cp_clear_reg.Hex),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   UCPClearReg tmp(val);
                   s_cp_clear_reg.Hex = tmp.Hex;
                   SetCpClearRegister();
                   Fifo::RunGpu();
                 }));

  mmio->Register(base | PERF_SELECT, MMIO::InvalidRead<u16>(), MMIO::Nop<u16>());

  // Some MMIOs have different handlers for single core vs. dual core mode.
  mmio->Register(base | FIFO_RW_DISTANCE_LO,
                 IsOnThread() ?
                     MMIO::ComplexRead<u16>([](u32) {
                       if (fifo.CPWritePointer >= fifo.SafeCPReadPointer)
                         return ReadLow(fifo.CPWritePointer - fifo.SafeCPReadPointer);
                       else
                         return ReadLow(fifo.CPEnd - fifo.SafeCPReadPointer + fifo.CPWritePointer -
                                        fifo.CPBase + 32);
                     }) :
                     MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPReadWriteDistance)),
                 MMIO::DirectWrite<u16>(MMIO::Utils::LowPart(&fifo.CPReadWriteDistance),
                                        WMASK_LO_ALIGN_32BIT));
  mmio->Register(base | FIFO_RW_DISTANCE_HI,
                 IsOnThread() ?
                     MMIO::ComplexRead<u16>([](u32) {
                       if (fifo.CPWritePointer >= fifo.SafeCPReadPointer)
                         return ReadHigh(fifo.CPWritePointer - fifo.SafeCPReadPointer);
                       else
                         return ReadHigh(fifo.CPEnd - fifo.SafeCPReadPointer + fifo.CPWritePointer -
                                         fifo.CPBase + 32);
                     }) :
                     MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&fifo.CPReadWriteDistance)),
                 MMIO::ComplexWrite<u16>([WMASK_HI_RESTRICT](u32, u16 val) {
                   WriteHigh(fifo.CPReadWriteDistance, val & WMASK_HI_RESTRICT);
                   Fifo::SyncGPU(Fifo::SyncGPUReason::Other);
                   Fifo::RunGpu();
                 }));
  mmio->Register(
      base | FIFO_READ_POINTER_LO,
      IsOnThread() ? MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.SafeCPReadPointer)) :
                     MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPReadPointer)),
      MMIO::DirectWrite<u16>(MMIO::Utils::LowPart(&fifo.CPReadPointer), WMASK_LO_ALIGN_32BIT));
  mmio->Register(
      base | FIFO_READ_POINTER_HI,
      IsOnThread() ? MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&fifo.SafeCPReadPointer)) :
                     MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&fifo.CPReadPointer)),
      IsOnThread() ?
          MMIO::ComplexWrite<u16>([WMASK_HI_RESTRICT](u32, u16 val) {
            WriteHigh(fifo.CPReadPointer, val & WMASK_HI_RESTRICT);
            fifo.SafeCPReadPointer = fifo.CPReadPointer;
          }) :
          MMIO::DirectWrite<u16>(MMIO::Utils::HighPart(&fifo.CPReadPointer), WMASK_HI_RESTRICT));
}

void GatherPipeBursted()
{
  SetCPStatusFromCPU();

  // if we aren't linked, we don't care about gather pipe data
  if (!s_cp_ctrl_reg.GPLinkEnable)
  {
    if (IsOnThread() && !Fifo::UseDeterministicGPUThread())
    {
      // In multibuffer mode is not allowed write in the same FIFO attached to the GPU.
      // Fix Pokemon XD in DC mode.
      if ((ProcessorInterface::Fifo_CPUEnd == fifo.CPEnd) &&
          (ProcessorInterface::Fifo_CPUBase == fifo.CPBase) && fifo.CPReadWriteDistance > 0)
      {
        Fifo::FlushGpu();
      }
    }
    Fifo::RunGpu();
    return;
  }

  // update the fifo pointer
  if (fifo.CPWritePointer == fifo.CPEnd)
    fifo.CPWritePointer = fifo.CPBase;
  else
    fifo.CPWritePointer += GATHER_PIPE_SIZE;

  if (s_cp_ctrl_reg.GPReadEnable && s_cp_ctrl_reg.GPLinkEnable)
  {
    ProcessorInterface::Fifo_CPUWritePointer = fifo.CPWritePointer;
    ProcessorInterface::Fifo_CPUBase = fifo.CPBase;
    ProcessorInterface::Fifo_CPUEnd = fifo.CPEnd;
  }

  // If the game is running close to overflowing, make the exception checking more frequent.
  if (fifo.bFF_HiWatermark)
    CoreTiming::ForceExceptionCheck(0);

  Common::AtomicAdd(fifo.CPReadWriteDistance, GATHER_PIPE_SIZE);

  Fifo::RunGpu();

  ASSERT_MSG(COMMANDPROCESSOR, fifo.CPReadWriteDistance <= fifo.CPEnd - fifo.CPBase,
             "FIFO is overflowed by GatherPipe !\nCPU thread is too fast!");

  // check if we are in sync
  ASSERT_MSG(COMMANDPROCESSOR, fifo.CPWritePointer == ProcessorInterface::Fifo_CPUWritePointer,
             "FIFOs linked but out of sync");
  ASSERT_MSG(COMMANDPROCESSOR, fifo.CPBase == ProcessorInterface::Fifo_CPUBase,
             "FIFOs linked but out of sync");
  ASSERT_MSG(COMMANDPROCESSOR, fifo.CPEnd == ProcessorInterface::Fifo_CPUEnd,
             "FIFOs linked but out of sync");
}

void UpdateInterrupts(u64 userdata)
{
  if (userdata)
  {
    s_interrupt_set.Set();
    DEBUG_LOG(COMMANDPROCESSOR, "Interrupt set");
    ProcessorInterface::SetInterrupt(INT_CAUSE_CP, true);
  }
  else
  {
    s_interrupt_set.Clear();
    DEBUG_LOG(COMMANDPROCESSOR, "Interrupt cleared");
    ProcessorInterface::SetInterrupt(INT_CAUSE_CP, false);
  }
  CoreTiming::ForceExceptionCheck(0);
  s_interrupt_waiting.Clear();
  Fifo::RunGpu();
}

void UpdateInterruptsFromVideoBackend(u64 userdata)
{
  if (!Fifo::UseDeterministicGPUThread())
    CoreTiming::ScheduleEvent(0, s_update_interrupts, userdata, CoreTiming::FromThread::NON_CPU);
}

bool IsInterruptWaiting()
{
  return s_interrupt_waiting.IsSet();
}

void SetCPStatusFromGPU()
{
  // breakpoint
  if (fifo.bFF_BPEnable)
  {
    if (fifo.CPBreakpoint == fifo.CPReadPointer)
    {
      if (!fifo.bFF_Breakpoint)
      {
        DEBUG_LOG(COMMANDPROCESSOR, "Hit breakpoint at %i", fifo.CPReadPointer);
        fifo.bFF_Breakpoint = true;
      }
    }
    else
    {
      if (fifo.bFF_Breakpoint)
        DEBUG_LOG(COMMANDPROCESSOR, "Cleared breakpoint at %i", fifo.CPReadPointer);
      fifo.bFF_Breakpoint = false;
    }
  }
  else
  {
    if (fifo.bFF_Breakpoint)
      DEBUG_LOG(COMMANDPROCESSOR, "Cleared breakpoint at %i", fifo.CPReadPointer);
    fifo.bFF_Breakpoint = false;
  }

  // overflow & underflow check
  fifo.bFF_HiWatermark = (fifo.CPReadWriteDistance > fifo.CPHiWatermark);
  fifo.bFF_LoWatermark = (fifo.CPReadWriteDistance < fifo.CPLoWatermark);

  const bool bpInt = fifo.bFF_Breakpoint && fifo.bFF_BPInt;
  const bool ovfInt = fifo.bFF_HiWatermark && fifo.bFF_HiWatermarkInt;
  const bool undfInt = fifo.bFF_LoWatermark && fifo.bFF_LoWatermarkInt;

  const bool interrupt = (bpInt || ovfInt || undfInt) && s_cp_ctrl_reg.GPReadEnable;

  if (interrupt != s_interrupt_set.IsSet() && !s_interrupt_waiting.IsSet())
  {
    u64 userdata = interrupt ? 1 : 0;
    if (IsOnThread())
    {
      if (!interrupt || bpInt || undfInt || ovfInt)
      {
        // Schedule the interrupt asynchronously
        s_interrupt_waiting.Set();
        CommandProcessor::UpdateInterruptsFromVideoBackend(userdata);
      }
    }
    else
    {
      CommandProcessor::UpdateInterrupts(userdata);
    }
  }
}

void SetCPStatusFromCPU()
{
  // overflow & underflow check
  fifo.bFF_HiWatermark = (fifo.CPReadWriteDistance > fifo.CPHiWatermark);
  fifo.bFF_LoWatermark = (fifo.CPReadWriteDistance < fifo.CPLoWatermark);

  const bool bpInt = fifo.bFF_Breakpoint && fifo.bFF_BPInt;
  const bool ovfInt = fifo.bFF_HiWatermark && fifo.bFF_HiWatermarkInt;
  const bool undfInt = fifo.bFF_LoWatermark && fifo.bFF_LoWatermarkInt;

  const bool interrupt = (bpInt || ovfInt || undfInt) && s_cp_ctrl_reg.GPReadEnable;

  if (interrupt != s_interrupt_set.IsSet() && !s_interrupt_waiting.IsSet())
  {
    u64 userdata = interrupt ? 1 : 0;
    if (IsOnThread())
    {
      if (!interrupt || bpInt || undfInt || ovfInt)
      {
        s_interrupt_set.Set(interrupt);
        DEBUG_LOG(COMMANDPROCESSOR, "Interrupt set");
        ProcessorInterface::SetInterrupt(INT_CAUSE_CP, interrupt);
      }
    }
    else
    {
      CommandProcessor::UpdateInterrupts(userdata);
    }
  }
}

void SetCpStatusRegister()
{
  // Here always there is one fifo attached to the GPU
  s_cp_status_reg.Breakpoint = fifo.bFF_Breakpoint;
  s_cp_status_reg.ReadIdle = !fifo.CPReadWriteDistance || (fifo.CPReadPointer == fifo.CPWritePointer);
  s_cp_status_reg.CommandIdle =
      !fifo.CPReadWriteDistance || Fifo::AtBreakpoint() || !fifo.bFF_GPReadEnable;
  s_cp_status_reg.UnderflowLoWatermark = fifo.bFF_LoWatermark;
  s_cp_status_reg.OverflowHiWatermark = fifo.bFF_HiWatermark;

  DEBUG_LOG(COMMANDPROCESSOR, "\t Read from STATUS_REGISTER : %04x", s_cp_status_reg.Hex);
  DEBUG_LOG(COMMANDPROCESSOR,
            "(r) status: iBP %s | fReadIdle %s | fCmdIdle %s | iOvF %s | iUndF %s",
            s_cp_status_reg.Breakpoint ? "ON" : "OFF", s_cp_status_reg.ReadIdle ? "ON" : "OFF",
            s_cp_status_reg.CommandIdle ? "ON" : "OFF",
            s_cp_status_reg.OverflowHiWatermark ? "ON" : "OFF",
            s_cp_status_reg.UnderflowLoWatermark ? "ON" : "OFF");
}

void SetCpControlRegister()
{
  fifo.bFF_BPInt = s_cp_ctrl_reg.BPInt;
  fifo.bFF_BPEnable = s_cp_ctrl_reg.BPEnable;
  fifo.bFF_HiWatermarkInt = s_cp_ctrl_reg.FifoOverflowIntEnable;
  fifo.bFF_LoWatermarkInt = s_cp_ctrl_reg.FifoUnderflowIntEnable;
  fifo.bFF_GPLinkEnable = s_cp_ctrl_reg.GPLinkEnable;

  if (fifo.bFF_GPReadEnable && !s_cp_ctrl_reg.GPReadEnable)
  {
    fifo.bFF_GPReadEnable = s_cp_ctrl_reg.GPReadEnable;
    Fifo::FlushGpu();
  }
  else
  {
    fifo.bFF_GPReadEnable = s_cp_ctrl_reg.GPReadEnable;
  }

  DEBUG_LOG(COMMANDPROCESSOR, "\t GPREAD %s | BP %s | Int %s | OvF %s | UndF %s | LINK %s",
            fifo.bFF_GPReadEnable ? "ON" : "OFF", fifo.bFF_BPEnable ? "ON" : "OFF",
            fifo.bFF_BPInt ? "ON" : "OFF", s_cp_ctrl_reg.FifoOverflowIntEnable ? "ON" : "OFF",
            s_cp_ctrl_reg.FifoUnderflowIntEnable ? "ON" : "OFF",
            s_cp_ctrl_reg.GPLinkEnable ? "ON" : "OFF");
}

// NOTE: We intentionally don't emulate this function at the moment.
// We don't emulate proper GP timing anyway at the moment, so it would just slow down emulation.
void SetCpClearRegister()
{
}

void HandleUnknownOpcode(u8 cmd_byte, void* buffer, bool preprocess)
{
  // TODO(Omega): Maybe dump FIFO to file on this error
  PanicAlertT("GFX FIFO: Unknown Opcode (0x%02x @ %p, %s).\n"
              "This means one of the following:\n"
              "* The emulated GPU got desynced, disabling dual core can help\n"
              "* Command stream corrupted by some spurious memory bug\n"
              "* This really is an unknown opcode (unlikely)\n"
              "* Some other sort of bug\n\n"
              "Further errors will be sent to the Video Backend log and\n"
              "Dolphin will now likely crash or hang. Enjoy.",
              cmd_byte, buffer, preprocess ? "preprocess=true" : "preprocess=false");

  {
    PanicAlert("Illegal command %02x\n"
               "CPBase: 0x%08x\n"
               "CPEnd: 0x%08x\n"
               "CPHiWatermark: 0x%08x\n"
               "CPLoWatermark: 0x%08x\n"
               "CPReadWriteDistance: 0x%08x\n"
               "CPWritePointer: 0x%08x\n"
               "CPReadPointer: 0x%08x\n"
               "CPBreakpoint: 0x%08x\n"
               "bFF_GPReadEnable: %s\n"
               "bFF_BPEnable: %s\n"
               "bFF_BPInt: %s\n"
               "bFF_Breakpoint: %s\n"
               "bFF_GPLinkEnable: %s\n"
               "bFF_HiWatermarkInt: %s\n"
               "bFF_LoWatermarkInt: %s\n",
               cmd_byte, fifo.CPBase, fifo.CPEnd, fifo.CPHiWatermark, fifo.CPLoWatermark,
               fifo.CPReadWriteDistance, fifo.CPWritePointer, fifo.CPReadPointer, fifo.CPBreakpoint,
               fifo.bFF_GPReadEnable ? "true" : "false", fifo.bFF_BPEnable ? "true" : "false",
               fifo.bFF_BPInt ? "true" : "false", fifo.bFF_Breakpoint ? "true" : "false",
               fifo.bFF_GPLinkEnable ? "true" : "false", fifo.bFF_HiWatermarkInt ? "true" : "false",
               fifo.bFF_LoWatermarkInt ? "true" : "false");
  }
}

}  // end of namespace CommandProcessor
