//
// Copyright (c) 2014, Facebook, Inc.
// All rights reserved.
//
// This source code is licensed under the University of Illinois/NCSA Open
// Source License found in the LICENSE file in the root directory of this
// source tree. An additional grant of patent rights can be found in the
// PATENTS file in the same directory.
//

#define __DS2_LOG_CLASS_NAME__ "Target::Thread"

#include "DebugServer2/Target/Thread.h"
#include "DebugServer2/Target/Process.h"
#include "DebugServer2/Architecture/ARM/Branching.h"
#include "DebugServer2/BreakpointManager.h"
#include "DebugServer2/Utils/Log.h"

using ds2::Target::Linux::Process;

namespace ds2 {
namespace Target {
namespace Linux {

namespace {

static ErrorCode PrepareThumbSoftwareSingleStep(
    Process *process, uint32_t pc, ds2::Architecture::CPUState const &state,
    bool &link, uint32_t &nextPC, uint32_t &nextPCSize, uint32_t &branchPC,
    uint32_t &branchPCSize) {
  ErrorCode error;
  uint32_t insns[2];

  DS2LOG(Debug, "process=%p", process);

  error = process->readMemory(pc, insns, sizeof(insns));
  if (error != ds2::kSuccess)
    return error;

  ds2::Architecture::ARM::BranchInfo info;
  if (!ds2::Architecture::ARM::GetThumbBranchInfo(insns, info)) {
    nextPC = pc + static_cast<uint8_t>(
                      ds2::Architecture::ARM::GetThumbInstSize(insns[0]));
    // Even if the next instruction is a 4-byte Thumb2 instruction, we are fine
    // with a 2-byte breakpoint because we won't ever jump over that
    // instruction.
    nextPCSize = 2;
    return ds2::kSuccess;
  }

  DS2LOG(Debug, "Thumb branch/IT found at %#x (it=%s[%u])", pc,
         info.it ? "true" : "false", info.it ? info.itCount : 0);

  //
  // If it's inside an IT block, we need to set the branch after the IT block.
  //
  if (info.it) {
    nextPC = pc + 2;

    //
    // We need to read all the instructions in the IT block and skip past them.
    //
    uint16_t itinsns[4 * 2]; // At most 4 instructions in the IT block.

    error = process->readMemory(nextPC, itinsns, sizeof(itinsns));
    if (error != ds2::kSuccess)
      return error;

    size_t skip = 0;
    for (size_t n = 0; n < info.itCount; n++) {
      skip += static_cast<uint8_t>(
          ds2::Architecture::ARM::GetThumbInstSize(itinsns[skip / 2]));
    }

    nextPC += skip;
    //
    // Even if the next instruction is a 4-byte Thumb2 instruction, we are fine
    // with a 2-byte breakpoint because we won't ever jump over that
    // instruction.
    //
    nextPCSize = 2;
    return ds2::kSuccess;
  }

  link = (info.type == ds2::Architecture::ARM::kBranchTypeBL_i ||
          info.type == ds2::Architecture::ARM::kBranchTypeBLX_i ||
          info.type == ds2::Architecture::ARM::kBranchTypeBLX_r);

  DS2LOG(Debug, "link=%d", link);
  //
  // If it's a conditional branch, we need to move
  // the nextPC by the size of this instruction.
  //
  if (info.type == ds2::Architecture::ARM::kBranchTypeBcc_i ||
      info.type == ds2::Architecture::ARM::kBranchTypeCB_i || link) {
    nextPC = pc + static_cast<uint8_t>(
                      ds2::Architecture::ARM::GetThumbInstSize(insns[0]));
    nextPCSize = 2;
  }

  uint32_t address;
  int16_t offset;

  switch (info.type) {
  //
  // None means a conditional instruction.
  //
  case ds2::Architecture::ARM::kBranchTypeNone:
    break;

  //
  // Simple branches
  //
  case ds2::Architecture::ARM::kBranchTypeB_i:
  case ds2::Architecture::ARM::kBranchTypeBL_i:
  case ds2::Architecture::ARM::kBranchTypeBcc_i:
  case ds2::Architecture::ARM::kBranchTypeCB_i:
    branchPC = pc + info.disp;
    branchPCSize = 2;
    break;

  //
  // Branch to registers
  //
  case ds2::Architecture::ARM::kBranchTypeBX_r:
  case ds2::Architecture::ARM::kBranchTypeBLX_r:
  case ds2::Architecture::ARM::kBranchTypeMOV_pc:
    branchPC = state.gp.regs[info.reg1];
    break;

  //
  // Load and Branch
  //
  case ds2::Architecture::ARM::kBranchTypeLDR_pc:
    address = state.gp.regs[info.reg1];
    if (info.mode == ds2::Architecture::ARM::kBranchDispLSL) {
      address += state.gp.regs[info.reg2] << info.disp;
    } else {
      if (info.reg2 >= 0) {
        address += state.gp.regs[info.reg2];
      }
      address += info.disp;
    }

    error = process->readMemory(address, &branchPC, sizeof(branchPC));
    if (error != ds2::kSuccess)
      return error;
    break;

  case ds2::Architecture::ARM::kBranchTypeLDM_pc:
  case ds2::Architecture::ARM::kBranchTypePOP_pc:
    address = state.gp.regs[info.reg1] + info.disp;
    error = process->readMemory(address, &branchPC, sizeof(branchPC));
    if (error != ds2::kSuccess)
      return error;
    break;

  //
  // Simple branch with switch-back to ARM
  //
  case ds2::Architecture::ARM::kBranchTypeBLX_i:
    branchPC = pc + info.disp;
    branchPC = (branchPC + info.align - 1) & -info.align;
    branchPCSize = 4;
    break;

  //
  // ALU operations
  //
  case ds2::Architecture::ARM::kBranchTypeSUB_pc:
    branchPC = state.gp.regs[info.reg1] - info.disp;
    break;

  case ds2::Architecture::ARM::kBranchTypeTBB:
  case ds2::Architecture::ARM::kBranchTypeTBH:
    address = state.gp.regs[info.reg1];
    if (info.reg1 == 15) // Instruction Pointer | TODO: Generate register names
      address += 4;
    address += state.gp.regs[info.reg2] << info.disp;
    offset = 0;
    error = process->readMemory(
        address, &offset,
        info.type == ds2::Architecture::ARM::kBranchTypeTBB ? 1 : 2);
    if (error != ds2::kSuccess)
      return error;
    branchPC = pc + 4 + offset * 2;
    branchPCSize = 2;
    break;

  default:
    return ds2::kErrorUnsupported;
  }

  //
  // If we didn't set the breakpoint size in the switch earlier, it means we
  // are dealing with an instruction that might or might not switch back to ARM
  // mode. We need to check the value that we are loading in the register and
  // determine the branch size based on that.
  //
  if (branchPCSize == 0) {
    branchPCSize = (branchPC & 1) ? 2 : 4;
    branchPC &= ~1ULL;
  }

  return ds2::kSuccess;
}

static ErrorCode
PrepareARMSoftwareSingleStep(Process *process, uint32_t pc,
                             ds2::Architecture::CPUState const &state,
                             bool &link, uint32_t &nextPC, uint32_t &nextPCSize,
                             uint32_t &branchPC, uint32_t &branchPCSize) {
  ErrorCode error;
  uint32_t insn;

  error = process->readMemory(pc, &insn, sizeof(insn));
  if (error != ds2::kSuccess)
    return error;

  ds2::Architecture::ARM::BranchInfo info;
  if (!ds2::Architecture::ARM::GetARMBranchInfo(insn, info)) {
    // We couldn't find a branch, the next instruction is standard ARM.
    nextPC = pc + 4;
    nextPCSize = 4;
    return ds2::kSuccess;
  }

  uint32_t address;

  DS2LOG(Debug, "ARM branch found at %#x", pc);

  link = (info.type == ds2::Architecture::ARM::kBranchTypeBL_i ||
          info.type == ds2::Architecture::ARM::kBranchTypeBLX_i ||
          info.type == ds2::Architecture::ARM::kBranchTypeBLX_r);

  //
  // If it's a conditional branch, we need to set nextPC to the next
  // instruction. Otherwise, we leave nextPC to its undefined value.
  //
  if (info.cond != ds2::Architecture::ARM::kCondAL || link) {
    nextPC = pc + 4;
    nextPCSize = 4;
  }

  switch (info.type) {
  //
  // Simple branches
  //
  case ds2::Architecture::ARM::kBranchTypeB_i:
  case ds2::Architecture::ARM::kBranchTypeBL_i:
  case ds2::Architecture::ARM::kBranchTypeBcc_i:
    branchPC = pc + info.disp;
    branchPCSize = 4;
    break;

  //
  // Branch to registers
  //
  case ds2::Architecture::ARM::kBranchTypeBX_r:
  case ds2::Architecture::ARM::kBranchTypeBLX_r:
    branchPC = state.gp.regs[info.reg1];
    break;

  case ds2::Architecture::ARM::kBranchTypeMOV_pc:
    switch (info.mode) {
    case ds2::Architecture::ARM::kBranchDispNormal:
      branchPC = state.gp.regs[info.reg1];
      break;

    default:
      return ds2::kErrorUnsupported;
    }
    break;

  //
  // Load and Branch
  //
  case ds2::Architecture::ARM::kBranchTypeLDR_pc:
    address = state.gp.regs[info.reg1];
    switch (info.mode) {
    case ds2::Architecture::ARM::kBranchDispLSL:
      address += state.gp.regs[info.reg2] << info.disp;
      break;
    case ds2::Architecture::ARM::kBranchDispNormal:
      if (!(info.reg2 < 0)) {
        address += state.gp.regs[info.reg2];
      }
      address += info.disp;
      break;
    default:
      return ds2::kErrorUnsupported;
    }

    error = process->readMemory(address, &branchPC, sizeof(branchPC));
    if (error != ds2::kSuccess)
      return error;
    break;

  case ds2::Architecture::ARM::kBranchTypeLDM_pc:
  case ds2::Architecture::ARM::kBranchTypePOP_pc:
    address = state.gp.regs[info.reg1] + info.disp;
    error = process->readMemory(address, &branchPC, sizeof(branchPC));
    if (error != ds2::kSuccess)
      return error;
    break;

  //
  // Switch to Thumb has the added twist that the breakpoint to be set is of
  // Thumb type.
  //
  case ds2::Architecture::ARM::kBranchTypeBLX_i:
    branchPC = pc + info.disp;
    break;

  //
  // ALU operations
  //
  case ds2::Architecture::ARM::kBranchTypeSUB_pc:
    branchPC = state.gp.regs[info.reg1] - info.disp;
    break;

  default:
    return ds2::kErrorUnsupported;
  }

  //
  // We can switch to Thumb with an LDR pc. If that's the case, adjust the
  // target and size.
  //
  if (branchPC & 1) {
    branchPC &= ~1ULL;
    branchPCSize = 2;
  } else {
    branchPCSize = 4;
  }

  return ds2::kSuccess;
}
}

ErrorCode Thread::prepareSoftwareSingleStep(Address const &address) {
  Architecture::CPUState state;

  ErrorCode error = readCPUState(state);
  if (error != kSuccess)
    return error;

  bool link = false;
  bool isThumb = (state.gp.cpsr & (1 << 5));
  uint32_t pc = address.valid() ? address.value() : state.pc();
  uint32_t nextPC = static_cast<uint32_t>(-1);
  uint32_t nextPCSize = 0;
  uint32_t branchPC = static_cast<uint32_t>(-1);
  uint32_t branchPCSize = 0;

  if (isThumb) {
    error = PrepareThumbSoftwareSingleStep(process(), pc, state, link, nextPC,
                                           nextPCSize, branchPC, branchPCSize);
  } else {
    error = PrepareARMSoftwareSingleStep(process(), pc, state, link, nextPC,
                                         nextPCSize, branchPC, branchPCSize);
  }

  DS2LOG(Debug, "PC=%#x, branchPC=%#x[size=%d, link=%s] nextPC=%#x[size=%d]",
         pc, branchPC, branchPCSize, link ? "true" : "false", nextPC,
         nextPCSize);

  if (branchPC != static_cast<uint32_t>(-1)) {
    DS2ASSERT(branchPCSize != 0);
    error = process()->breakpointManager()->add(
        branchPC, BreakpointManager::kTypeTemporaryOneShot, branchPCSize);
    if (error != kSuccess)
      return error;
  }

  if (nextPC != static_cast<uint32_t>(-1)) {
    DS2ASSERT(nextPCSize != 0);
    error = process()->breakpointManager()->add(
        nextPC, BreakpointManager::kTypeTemporaryOneShot, nextPCSize);
    if (error != kSuccess)
      return error;
  }

  return kSuccess;
}
}
}
}
