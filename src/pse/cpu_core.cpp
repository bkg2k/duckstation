#include "cpu_core.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
#include "cpu_disasm.h"
#include <cstdio>
Log_SetChannel(CPU::Core);

namespace CPU {
bool TRACE_EXECUTION = false;

Core::Core() = default;

Core::~Core() = default;

bool Core::Initialize(Bus* bus)
{
  m_bus = bus;

  // From nocash spec.
  m_cop0_regs.PRID = UINT32_C(0x00000002);

  m_cop2.Initialize();

  return true;
}

void Core::Reset()
{
  m_pending_ticks = 0;
  m_downcount = MAX_SLICE_SIZE;

  m_regs = {};

  m_cop0_regs.BPC = 0;
  m_cop0_regs.BDA = 0;
  m_cop0_regs.TAR = 0;
  m_cop0_regs.BadVaddr = 0;
  m_cop0_regs.BDAM = 0;
  m_cop0_regs.BPCM = 0;
  m_cop0_regs.EPC = 0;
  m_cop0_regs.sr.bits = 0;
  m_cop0_regs.cause.bits = 0;

  m_cop2.Reset();

  SetPC(RESET_VECTOR);
}

bool Core::DoState(StateWrapper& sw)
{
  sw.Do(&m_pending_ticks);
  sw.Do(&m_downcount);
  sw.DoArray(m_regs.r, countof(m_regs.r));
  sw.Do(&m_regs.pc);
  sw.Do(&m_regs.hi);
  sw.Do(&m_regs.lo);
  sw.Do(&m_regs.npc);
  sw.Do(&m_cop0_regs.BPC);
  sw.Do(&m_cop0_regs.BDA);
  sw.Do(&m_cop0_regs.TAR);
  sw.Do(&m_cop0_regs.BadVaddr);
  sw.Do(&m_cop0_regs.BDAM);
  sw.Do(&m_cop0_regs.BPCM);
  sw.Do(&m_cop0_regs.EPC);
  sw.Do(&m_cop0_regs.PRID);
  sw.Do(&m_cop0_regs.sr.bits);
  sw.Do(&m_cop0_regs.cause.bits);
  sw.Do(&m_cop0_regs.dcic.bits);
  sw.Do(&m_next_instruction.bits);
  sw.Do(&m_current_instruction.bits);
  sw.Do(&m_current_instruction_pc);
  sw.Do(&m_current_instruction_in_branch_delay_slot);
  sw.Do(&m_current_instruction_was_branch_taken);
  sw.Do(&m_next_instruction_is_branch_delay_slot);
  sw.Do(&m_branch_was_taken);
  sw.Do(&m_load_delay_reg);
  sw.Do(&m_load_delay_old_value);
  sw.Do(&m_next_load_delay_reg);
  sw.Do(&m_next_load_delay_old_value);
  sw.Do(&m_cache_control);
  sw.DoBytes(m_dcache.data(), m_dcache.size());

  if (!m_cop2.DoState(sw))
    return false;

  return !sw.HasError();
}

void Core::SetPC(u32 new_pc)
{
  m_regs.npc = new_pc;
  FlushPipeline();
}

bool Core::ReadMemoryByte(VirtualMemoryAddress addr, u8* value)
{
  u32 temp = 0;
  const bool result = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(addr, temp);
  *value = Truncate8(temp);
  if (!result)
    RaiseException(Exception::DBE);

  return result;
}

bool Core::ReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(addr))
    return false;

  u32 temp = 0;
  const bool result = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(addr, temp);
  *value = Truncate16(temp);
  if (!result)
    RaiseException(Exception::DBE);

  return result;
}

bool Core::ReadMemoryWord(VirtualMemoryAddress addr, u32* value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::Word>(addr))
    return false;

  const bool result = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(addr, *value);
  if (!result)
    RaiseException(Exception::DBE);

  return result;
}

bool Core::WriteMemoryByte(VirtualMemoryAddress addr, u8 value)
{
  u32 temp = ZeroExtend32(value);
  const bool result = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte>(addr, temp);
  if (!result)
    RaiseException(Exception::DBE);

  return result;
}

bool Core::WriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(addr))
    return false;

  u32 temp = ZeroExtend32(value);
  const bool result = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(addr, temp);
  if (!result)
    RaiseException(Exception::DBE);

  return result;
}

bool Core::WriteMemoryWord(VirtualMemoryAddress addr, u32 value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::Word>(addr))
    return false;

  const bool result = DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(addr, value);
  if (!result)
    RaiseException(Exception::DBE);

  return result;
}

bool Core::SafeReadMemoryByte(VirtualMemoryAddress addr, u8* value)
{
  u32 temp = 0;
  const bool result = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(addr, temp);
  *value = Truncate8(temp);
  return result;
}

bool Core::SafeReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value)
{
  u32 temp = 0;
  const bool result = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(addr, temp);
  *value = Truncate16(temp);
  return result;
}

bool Core::SafeReadMemoryWord(VirtualMemoryAddress addr, u32* value)
{
  return DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(addr, *value);
}

bool Core::SafeWriteMemoryByte(VirtualMemoryAddress addr, u8 value)
{
  u32 temp = ZeroExtend32(value);
  return DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte>(addr, temp);
}

bool Core::SafeWriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value)
{
  u32 temp = ZeroExtend32(value);
  return DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(addr, temp);
}

bool Core::SafeWriteMemoryWord(VirtualMemoryAddress addr, u32 value)
{
  return DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(addr, value);
}

void Core::Branch(u32 target)
{
  m_regs.npc = target;
  m_branch_was_taken = true;
}

u32 Core::GetExceptionVector(Exception excode) const
{
  const u32 base = m_cop0_regs.sr.BEV ? UINT32_C(0xbfc00100) : UINT32_C(0x80000000);

#if 0
  // apparently this isn't correct...
  switch (excode)
  {
    case Exception::BP:
      return base | UINT32_C(0x00000040);

    default:
      return base | UINT32_C(0x00000080);
  }
#else
  return base | UINT32_C(0x00000080);
#endif
}

void Core::RaiseException(Exception excode)
{
#ifdef Y_BUILD_CONFIG_RELEASE
  if (excode == Exception::RI)
  {
    // Invalid op.
    Log_DevPrintf("Invalid instruction at 0x%08X", m_current_instruction_pc);
    DisassembleAndPrint(m_current_instruction_pc, 4, 0);
  }
#endif

  RaiseException(excode, m_current_instruction_pc, m_current_instruction_in_branch_delay_slot,
                 m_current_instruction_was_branch_taken, m_current_instruction.cop.cop_n);
}

void Core::RaiseException(Exception excode, u32 EPC, bool BD, bool BT, u8 CE)
{
  Log_DevPrintf("Exception %u at 0x%08X (epc=0x%08X, BD=%s, CE=%u)", static_cast<u32>(excode), m_current_instruction_pc,
                EPC, BD ? "true" : "false", ZeroExtend32(CE));
#ifdef Y_BUILD_CONFIG_DEBUG
  DisassembleAndPrint(m_current_instruction_pc, 4, 0);
#endif

  m_cop0_regs.EPC = EPC;
  m_cop0_regs.cause.Excode = excode;
  m_cop0_regs.cause.BD = BD;
  m_cop0_regs.cause.BT = BT;
  m_cop0_regs.cause.CE = CE;

  if (BD)
  {
    // TAR is set to the address which was being fetched in this instruction, or the next instruction to execute if the
    // exception hadn't occurred in the delay slot.
    m_cop0_regs.EPC -= UINT32_C(4);
    m_cop0_regs.TAR = m_regs.pc;
  }

  // current -> previous, switch to kernel mode and disable interrupts
  m_cop0_regs.sr.mode_bits <<= 2;

  // flush the pipeline - we don't want to execute the previously fetched instruction
  m_regs.npc = GetExceptionVector(excode);
  FlushPipeline();
}

void Core::SetExternalInterrupt(u8 bit)
{
  m_cop0_regs.cause.Ip |= static_cast<u8>(1u << bit);
}

void Core::ClearExternalInterrupt(u8 bit)
{
  m_cop0_regs.cause.Ip &= static_cast<u8>(~(1u << bit));
}

bool Core::DispatchInterrupts()
{
  // If the instruction we're about to execute is a GTE instruction, delay dispatching the interrupt until the next
  // instruction. For some reason, if we don't do this, we end up with incorrectly sorted polygons and flickering..
  if (m_next_instruction.IsCop2Instruction())
    return false;

  // const bool do_interrupt = m_cop0_regs.sr.IEc && ((m_cop0_regs.cause.Ip & m_cop0_regs.sr.Im) != 0);
  const bool do_interrupt =
    m_cop0_regs.sr.IEc && (((m_cop0_regs.cause.bits & m_cop0_regs.sr.bits) & (UINT32_C(0xFF) << 8)) != 0);
  if (!do_interrupt)
    return false;

  RaiseException(Exception::INT);
  return true;
}

void Core::FlushLoadDelay()
{
  m_load_delay_reg = Reg::count;
  m_load_delay_old_value = 0;
  m_next_load_delay_reg = Reg::count;
  m_next_load_delay_old_value = 0;
}

void Core::FlushPipeline()
{
  // loads are flushed
  FlushLoadDelay();

  // not in a branch delay slot
  m_branch_was_taken = false;
  m_next_instruction_is_branch_delay_slot = false;

  // prefetch the next instruction
  FetchInstruction();
}

u32 Core::ReadReg(Reg rs)
{
  return rs == m_load_delay_reg ? m_load_delay_old_value : m_regs.r[static_cast<u8>(rs)];
}

void Core::WriteReg(Reg rd, u32 value)
{
  if (rd != Reg::zero)
    m_regs.r[static_cast<u8>(rd)] = value;
}

void Core::WriteRegDelayed(Reg rd, u32 value)
{
  Assert(m_next_load_delay_reg == Reg::count);
  if (rd == Reg::zero)
    return;

  // save the old value, this will be returned if the register is read in the next instruction
  m_next_load_delay_reg = rd;
  m_next_load_delay_old_value = ReadReg(rd);
  m_regs.r[static_cast<u8>(rd)] = value;
}

std::optional<u32> Core::ReadCop0Reg(Cop0Reg reg)
{
  switch (reg)
  {
    case Cop0Reg::BPC:
      return m_cop0_regs.BPC;

    case Cop0Reg::BPCM:
      return m_cop0_regs.BPCM;

    case Cop0Reg::BDA:
      return m_cop0_regs.BDA;

    case Cop0Reg::BDAM:
      return m_cop0_regs.BDAM;

    case Cop0Reg::DCIC:
      return m_cop0_regs.dcic.bits;

    case Cop0Reg::JUMPDEST:
      return m_cop0_regs.TAR;

    case Cop0Reg::BadVaddr:
      return m_cop0_regs.BadVaddr;

    case Cop0Reg::SR:
      return m_cop0_regs.sr.bits;

    case Cop0Reg::CAUSE:
      return m_cop0_regs.cause.bits;

    case Cop0Reg::EPC:
      return m_cop0_regs.EPC;

    case Cop0Reg::PRID:
      return m_cop0_regs.PRID;

    default:
      Log_DevPrintf("Unknown COP0 reg %u", ZeroExtend32(static_cast<u8>(reg)));
      return std::nullopt;
  }
}

void Core::WriteCop0Reg(Cop0Reg reg, u32 value)
{
  switch (reg)
  {
    case Cop0Reg::BPC:
    {
      m_cop0_regs.BPC = value;
      Log_WarningPrintf("COP0 BPC <- %08X", value);
    }
    break;

    case Cop0Reg::BPCM:
    {
      m_cop0_regs.BPCM = value;
      Log_WarningPrintf("COP0 BPCM <- %08X", value);
    }
    break;

    case Cop0Reg::BDA:
    {
      m_cop0_regs.BDA = value;
      Log_WarningPrintf("COP0 BDA <- %08X", value);
    }
    break;

    case Cop0Reg::BDAM:
    {
      m_cop0_regs.BDAM = value;
      Log_WarningPrintf("COP0 BDAM <- %08X", value);
    }
    break;

    case Cop0Reg::JUMPDEST:
    {
      Log_WarningPrintf("Ignoring write to Cop0 JUMPDEST");
    }
    break;

    case Cop0Reg::DCIC:
    {
      m_cop0_regs.dcic.bits =
        (m_cop0_regs.dcic.bits & ~Cop0Registers::DCIC::WRITE_MASK) | (value & Cop0Registers::DCIC::WRITE_MASK);
      Log_WarningPrintf("COP0 DCIC <- %08X (now %08X)", value, m_cop0_regs.dcic.bits);
    }
    break;

    case Cop0Reg::SR:
    {
      m_cop0_regs.sr.bits =
        (m_cop0_regs.sr.bits & ~Cop0Registers::SR::WRITE_MASK) | (value & Cop0Registers::SR::WRITE_MASK);
      Log_DebugPrintf("COP0 SR <- %08X (now %08X)", value, m_cop0_regs.sr.bits);
    }
    break;

    case Cop0Reg::CAUSE:
    {
      m_cop0_regs.cause.bits =
        (m_cop0_regs.cause.bits & ~Cop0Registers::CAUSE::WRITE_MASK) | (value & Cop0Registers::CAUSE::WRITE_MASK);
      Log_DebugPrintf("COP0 CAUSE <- %08X (now %08X)", value, m_cop0_regs.cause.bits);
    }
    break;

    default:
      Log_DevPrintf("Unknown COP0 reg %u", ZeroExtend32(static_cast<u8>(reg)));
      break;
  }
}

void Core::WriteCacheControl(u32 value)
{
  Log_WarningPrintf("Cache control <- 0x%08X", value);
  m_cache_control = value;
}

static void PrintInstruction(u32 bits, u32 pc, Core* state)
{
  TinyString instr;
  DisassembleInstruction(&instr, pc, bits, state);

  std::printf("%08x: %08x %s\n", pc, bits, instr.GetCharArray());
}

static constexpr bool AddOverflow(u32 old_value, u32 add_value, u32 new_value)
{
  return (((new_value ^ old_value) & (new_value ^ add_value)) & UINT32_C(0x80000000)) != 0;
}

static constexpr bool SubOverflow(u32 old_value, u32 sub_value, u32 new_value)
{
  return (((new_value ^ old_value) & (old_value ^ sub_value)) & UINT32_C(0x80000000)) != 0;
}

void Core::DisassembleAndPrint(u32 addr)
{
  u32 bits = 0;
  DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(addr, bits);
  PrintInstruction(bits, addr, this);
}

void Core::DisassembleAndPrint(u32 addr, u32 instructions_before /* = 0 */, u32 instructions_after /* = 0 */)
{
  u32 disasm_addr = addr - (instructions_before * sizeof(u32));
  for (u32 i = 0; i < instructions_before; i++)
  {
    DisassembleAndPrint(disasm_addr);
    disasm_addr += sizeof(u32);
  }

  std::printf("----> ");

  // <= to include the instruction itself
  for (u32 i = 0; i <= instructions_after; i++)
  {
    DisassembleAndPrint(disasm_addr);
    disasm_addr += sizeof(u32);
  }
}

void Core::Execute()
{
  while (m_downcount >= 0)
  {
    m_pending_ticks += 2;
    m_downcount -= 2;

    // now executing the instruction we previously fetched
    m_current_instruction = m_next_instruction;
    m_current_instruction_pc = m_regs.pc;
    m_current_instruction_in_branch_delay_slot = m_next_instruction_is_branch_delay_slot;
    m_current_instruction_was_branch_taken = m_branch_was_taken;
    m_next_instruction_is_branch_delay_slot = false;
    m_branch_was_taken = false;

    // fetch the next instruction
    if (DispatchInterrupts() || !FetchInstruction())
      continue;

    // execute the instruction we previously fetched
    ExecuteInstruction();

    // next load delay
    m_load_delay_reg = m_next_load_delay_reg;
    m_next_load_delay_reg = Reg::count;
    m_load_delay_old_value = m_next_load_delay_old_value;
    m_next_load_delay_old_value = 0;
  }
}

bool Core::FetchInstruction()
{
  if (!Common::IsAlignedPow2(m_regs.npc, 4))
  {
    // The EPC must be set to the fetching address, not the instruction about to execute.
    m_cop0_regs.BadVaddr = m_regs.npc;
    RaiseException(Exception::AdEL, m_regs.npc, false, false, 0);
    return false;
  }
  else if (!DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(m_regs.npc, m_next_instruction.bits))
  {
    // Bus errors don't set BadVaddr.
    RaiseException(Exception::IBE, m_regs.npc, false, false, 0);
    return false;
  }

  m_regs.pc = m_regs.npc;
  m_regs.npc += sizeof(m_next_instruction.bits);
  return true;
}

void Core::ExecuteInstruction()
{
  const Instruction inst = m_current_instruction;

#if 0
  if (inst_pc == 0xBFC06FF0)
  {
    TRACE_EXECUTION = true;
    __debugbreak();
  }
#endif

  if (TRACE_EXECUTION)
    PrintInstruction(inst.bits, m_current_instruction_pc, this);

  switch (inst.op)
  {
    case InstructionOp::funct:
    {
      switch (inst.r.funct)
      {
        case InstructionFunct::sll:
        {
          const u32 new_value = ReadReg(inst.r.rt) << inst.r.shamt;
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::srl:
        {
          const u32 new_value = ReadReg(inst.r.rt) >> inst.r.shamt;
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::sra:
        {
          const u32 new_value = static_cast<u32>(static_cast<s32>(ReadReg(inst.r.rt)) >> inst.r.shamt);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::sllv:
        {
          const u32 shift_amount = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 new_value = ReadReg(inst.r.rt) << shift_amount;
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::srlv:
        {
          const u32 shift_amount = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 new_value = ReadReg(inst.r.rt) >> shift_amount;
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::srav:
        {
          const u32 shift_amount = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 new_value = static_cast<u32>(static_cast<s32>(ReadReg(inst.r.rt)) >> shift_amount);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::and_:
        {
          const u32 new_value = ReadReg(inst.r.rs) & ReadReg(inst.r.rt);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::or_:
        {
          const u32 new_value = ReadReg(inst.r.rs) | ReadReg(inst.r.rt);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::xor_:
        {
          const u32 new_value = ReadReg(inst.r.rs) ^ ReadReg(inst.r.rt);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::nor:
        {
          const u32 new_value = ~(ReadReg(inst.r.rs) | ReadReg(inst.r.rt));
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::add:
        {
          const u32 old_value = ReadReg(inst.r.rs);
          const u32 add_value = ReadReg(inst.r.rt);
          const u32 new_value = old_value + add_value;
          if (AddOverflow(old_value, add_value, new_value))
          {
            RaiseException(Exception::Ov);
            return;
          }

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::addu:
        {
          const u32 new_value = ReadReg(inst.r.rs) + ReadReg(inst.r.rt);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::sub:
        {
          const u32 old_value = ReadReg(inst.r.rs);
          const u32 sub_value = ReadReg(inst.r.rt);
          const u32 new_value = old_value - sub_value;
          if (SubOverflow(old_value, sub_value, new_value))
          {
            RaiseException(Exception::Ov);
            return;
          }

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::subu:
        {
          const u32 new_value = ReadReg(inst.r.rs) - ReadReg(inst.r.rt);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::slt:
        {
          const u32 result = BoolToUInt32(static_cast<s32>(ReadReg(inst.r.rs)) < static_cast<s32>(ReadReg(inst.r.rt)));
          WriteReg(inst.r.rd, result);
        }
        break;

        case InstructionFunct::sltu:
        {
          const u32 result = BoolToUInt32(ReadReg(inst.r.rs) < ReadReg(inst.r.rt));
          WriteReg(inst.r.rd, result);
        }
        break;

        case InstructionFunct::mfhi:
        {
          WriteReg(inst.r.rd, m_regs.hi);
        }
        break;

        case InstructionFunct::mthi:
        {
          const u32 value = ReadReg(inst.r.rs);
          m_regs.hi = value;
        }
        break;

        case InstructionFunct::mflo:
        {
          WriteReg(inst.r.rd, m_regs.lo);
        }
        break;

        case InstructionFunct::mtlo:
        {
          const u32 value = ReadReg(inst.r.rs);
          m_regs.lo = value;
        }
        break;

        case InstructionFunct::mult:
        {
          const u32 lhs = ReadReg(inst.r.rs);
          const u32 rhs = ReadReg(inst.r.rt);
          const u64 result =
            static_cast<u64>(static_cast<s64>(SignExtend64(lhs)) * static_cast<s64>(SignExtend64(rhs)));
          m_regs.hi = Truncate32(result >> 32);
          m_regs.lo = Truncate32(result);
        }
        break;

        case InstructionFunct::multu:
        {
          const u32 lhs = ReadReg(inst.r.rs);
          const u32 rhs = ReadReg(inst.r.rt);
          const u64 result = ZeroExtend64(lhs) * ZeroExtend64(rhs);
          m_regs.hi = Truncate32(result >> 32);
          m_regs.lo = Truncate32(result);
        }
        break;

        case InstructionFunct::div:
        {
          const s32 num = static_cast<s32>(ReadReg(inst.r.rs));
          const s32 denom = static_cast<s32>(ReadReg(inst.r.rt));

          if (denom == 0)
          {
            // divide by zero
            m_regs.lo = (num >= 0) ? UINT32_C(0xFFFFFFFF) : UINT32_C(1);
            m_regs.hi = static_cast<u32>(num);
          }
          else if (static_cast<u32>(num) == UINT32_C(0x80000000) && denom == -1)
          {
            // unrepresentable
            m_regs.lo = UINT32_C(0x80000000);
            m_regs.hi = 0;
          }
          else
          {
            m_regs.lo = static_cast<u32>(num / denom);
            m_regs.hi = static_cast<u32>(num % denom);
          }
        }
        break;

        case InstructionFunct::divu:
        {
          const u32 num = ReadReg(inst.r.rs);
          const u32 denom = ReadReg(inst.r.rt);

          if (denom == 0)
          {
            // divide by zero
            m_regs.lo = (num >= 0) ? UINT32_C(0xFFFFFFFF) : UINT32_C(1);
            m_regs.hi = static_cast<u32>(num);
          }
          else
          {
            m_regs.lo = num / denom;
            m_regs.hi = num % denom;
          }
        }
        break;

        case InstructionFunct::jr:
        {
          m_next_instruction_is_branch_delay_slot = true;
          const u32 target = ReadReg(inst.r.rs);
          Branch(target);
        }
        break;

        case InstructionFunct::jalr:
        {
          m_next_instruction_is_branch_delay_slot = true;
          const u32 target = ReadReg(inst.r.rs);
          WriteReg(inst.r.rd, m_regs.npc);
          Branch(target);
        }
        break;

        case InstructionFunct::syscall:
        {
          Log_DebugPrintf("Syscall 0x%X(0x%X)", m_regs.s0, m_regs.a0);
          RaiseException(Exception::Syscall);
        }
        break;

        case InstructionFunct::break_:
        {
          RaiseException(Exception::BP);
        }
        break;

        default:
        {
          RaiseException(Exception::RI);
          break;
        }
      }
    }
    break;

    case InstructionOp::lui:
    {
      WriteReg(inst.i.rt, inst.i.imm_zext32() << 16);
    }
    break;

    case InstructionOp::andi:
    {
      WriteReg(inst.i.rt, ReadReg(inst.i.rs) & inst.i.imm_zext32());
    }
    break;

    case InstructionOp::ori:
    {
      WriteReg(inst.i.rt, ReadReg(inst.i.rs) | inst.i.imm_zext32());
    }
    break;

    case InstructionOp::xori:
    {
      WriteReg(inst.i.rt, ReadReg(inst.i.rs) ^ inst.i.imm_zext32());
    }
    break;

    case InstructionOp::addi:
    {
      const u32 old_value = ReadReg(inst.i.rs);
      const u32 add_value = inst.i.imm_sext32();
      const u32 new_value = old_value + add_value;
      if (AddOverflow(old_value, add_value, new_value))
      {
        RaiseException(Exception::Ov);
        return;
      }

      WriteReg(inst.i.rt, new_value);
    }
    break;

    case InstructionOp::addiu:
    {
      WriteReg(inst.i.rt, ReadReg(inst.i.rs) + inst.i.imm_sext32());
    }
    break;

    case InstructionOp::slti:
    {
      const u32 result = BoolToUInt32(static_cast<s32>(ReadReg(inst.i.rs)) < static_cast<s32>(inst.i.imm_sext32()));
      WriteReg(inst.i.rt, result);
    }
    break;

    case InstructionOp::sltiu:
    {
      const u32 result = BoolToUInt32(ReadReg(inst.i.rs) < inst.i.imm_sext32());
      WriteReg(inst.i.rt, result);
    }
    break;

    case InstructionOp::lb:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u8 value;
      if (!ReadMemoryByte(addr, &value))
        return;

      WriteRegDelayed(inst.i.rt, SignExtend32(value));
    }
    break;

    case InstructionOp::lh:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u16 value;
      if (!ReadMemoryHalfWord(addr, &value))
        return;

      WriteRegDelayed(inst.i.rt, SignExtend32(value));
    }
    break;

    case InstructionOp::lw:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u32 value;
      if (!ReadMemoryWord(addr, &value))
        return;

      WriteRegDelayed(inst.i.rt, value);
    }
    break;

    case InstructionOp::lbu:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u8 value;
      if (!ReadMemoryByte(addr, &value))
        return;

      WriteRegDelayed(inst.i.rt, ZeroExtend32(value));
    }
    break;

    case InstructionOp::lhu:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u16 value;
      if (!ReadMemoryHalfWord(addr, &value))
        return;

      WriteRegDelayed(inst.i.rt, ZeroExtend32(value));
    }
    break;

    case InstructionOp::lwl:
    case InstructionOp::lwr:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const VirtualMemoryAddress aligned_addr = addr & ~UINT32_C(3);
      u32 aligned_value;
      if (!ReadMemoryWord(aligned_addr, &aligned_value))
        return;

      // note: bypasses load delay on the read
      const u32 existing_value = m_regs.r[static_cast<u8>(inst.i.rt.GetValue())];
      const u8 shift = (Truncate8(addr) & u8(3)) * u8(8);
      u32 new_value;
      if (inst.op == InstructionOp::lwl)
      {
        const u32 mask = UINT32_C(0x00FFFFFF) >> shift;
        new_value = (existing_value & mask) | (aligned_value << (24 - shift));
      }
      else
      {
        const u32 mask = UINT32_C(0xFFFFFF00) << (24 - shift);
        new_value = (existing_value & mask) | (aligned_value >> shift);
      }

      WriteRegDelayed(inst.i.rt, new_value);
    }
    break;

    case InstructionOp::sb:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u8 value = Truncate8(ReadReg(inst.i.rt));
      WriteMemoryByte(addr, value);
    }
    break;

    case InstructionOp::sh:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u16 value = Truncate16(ReadReg(inst.i.rt));
      WriteMemoryHalfWord(addr, value);
    }
    break;

    case InstructionOp::sw:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u32 value = ReadReg(inst.i.rt);
      WriteMemoryWord(addr, value);
    }
    break;

    case InstructionOp::swl:
    case InstructionOp::swr:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const VirtualMemoryAddress aligned_addr = addr & ~UINT32_C(3);
      const u32 reg_value = ReadReg(inst.i.rt);
      const u8 shift = (Truncate8(addr) & u8(3)) * u8(8);
      u32 mem_value;
      if (!ReadMemoryWord(aligned_addr, &mem_value))
        return;

      u32 new_value;
      if (inst.op == InstructionOp::swl)
      {
        const u32 mem_mask = UINT32_C(0xFFFFFF00) << shift;
        new_value = (mem_value & mem_mask) | (reg_value >> (24 - shift));
      }
      else
      {
        const u32 mem_mask = UINT32_C(0x00FFFFFF) >> (24 - shift);
        new_value = (mem_value & mem_mask) | (reg_value << shift);
      }

      WriteMemoryWord(aligned_addr, new_value);
    }
    break;

    case InstructionOp::j:
    {
      m_next_instruction_is_branch_delay_slot = true;
      Branch((m_regs.pc & UINT32_C(0xF0000000)) | (inst.j.target << 2));
    }
    break;

    case InstructionOp::jal:
    {
      m_regs.ra = m_regs.npc;
      m_next_instruction_is_branch_delay_slot = true;
      Branch((m_regs.pc & UINT32_C(0xF0000000)) | (inst.j.target << 2));
    }
    break;

    case InstructionOp::beq:
    {
      // We're still flagged as a branch delay slot even if the branch isn't taken.
      m_next_instruction_is_branch_delay_slot = true;
      const bool branch = (ReadReg(inst.i.rs) == ReadReg(inst.i.rt));
      if (branch)
        Branch(m_regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::bne:
    {
      m_next_instruction_is_branch_delay_slot = true;
      const bool branch = (ReadReg(inst.i.rs) != ReadReg(inst.i.rt));
      if (branch)
        Branch(m_regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::bgtz:
    {
      m_next_instruction_is_branch_delay_slot = true;
      const bool branch = (static_cast<s32>(ReadReg(inst.i.rs)) > 0);
      if (branch)
        Branch(m_regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::blez:
    {
      m_next_instruction_is_branch_delay_slot = true;
      const bool branch = (static_cast<s32>(ReadReg(inst.i.rs)) <= 0);
      if (branch)
        Branch(m_regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::b:
    {
      m_next_instruction_is_branch_delay_slot = true;
      const u8 rt = static_cast<u8>(inst.i.rt.GetValue());

      // bgez is the inverse of bltz, so simply do ltz and xor the result
      const bool bgez = ConvertToBoolUnchecked(rt & u8(1));
      const bool branch = (static_cast<s32>(ReadReg(inst.i.rs)) < 0) ^ bgez;

      // register is still linked even if the branch isn't taken
      const bool link = (rt & u8(0x1E)) == u8(0x10);
      if (link)
        m_regs.ra = m_regs.npc;

      if (branch)
        Branch(m_regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::cop0:
    {
      if (InUserMode() && !m_cop0_regs.sr.CU0)
      {
        Log_WarningPrintf("Coprocessor 0 not present in user mode");
        RaiseException(Exception::CpU);
        return;
      }

      ExecuteCop0Instruction();
    }
    break;

    case InstructionOp::cop2:
    {
      if (InUserMode() && !m_cop0_regs.sr.CU2)
      {
        Log_WarningPrintf("Coprocessor 2 not present in user mode");
        RaiseException(Exception::CpU);
        return;
      }

      ExecuteCop2Instruction();
    }
    break;

    case InstructionOp::lwc2:
    {
      if (InUserMode() && !m_cop0_regs.sr.CU2)
      {
        Log_WarningPrintf("Coprocessor 2 not present in user mode");
        RaiseException(Exception::CpU);
        return;
      }

      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u32 value;
      if (!ReadMemoryWord(addr, &value))
        return;

      m_cop2.WriteDataRegister(ZeroExtend32(static_cast<u8>(inst.i.rt.GetValue())), value);
    }
    break;

    case InstructionOp::swc2:
    {
      if (InUserMode() && !m_cop0_regs.sr.CU2)
      {
        Log_WarningPrintf("Coprocessor 2 not present in user mode");
        RaiseException(Exception::CpU);
        return;
      }

      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u32 value = m_cop2.ReadDataRegister(ZeroExtend32(static_cast<u8>(inst.i.rt.GetValue())));
      WriteMemoryWord(addr, value);
    }
    break;

    // swc0/lwc0/cop1/cop3 are essentially no-ops
    case InstructionOp::cop1:
    case InstructionOp::cop3:
    case InstructionOp::lwc0:
    case InstructionOp::lwc1:
    case InstructionOp::lwc3:
    case InstructionOp::swc0:
    case InstructionOp::swc1:
    case InstructionOp::swc3:
    {
    }
    break;

    // everything else is reserved/invalid
    default:
    {
      RaiseException(Exception::RI);
    }
    break;
  }
}

void Core::ExecuteCop0Instruction()
{
  const Instruction inst = m_current_instruction;

  if (inst.cop.IsCommonInstruction())
  {
    switch (inst.cop.CommonOp())
    {
      case CopCommonInstruction::mfcn:
      {
        const std::optional<u32> value = ReadCop0Reg(static_cast<Cop0Reg>(inst.r.rd.GetValue()));
        if (value)
          WriteRegDelayed(inst.r.rt, value.value());
        else
          RaiseException(Exception::RI);
      }
      break;

      case CopCommonInstruction::mtcn:
      {
        WriteCop0Reg(static_cast<Cop0Reg>(inst.r.rd.GetValue()), ReadReg(inst.r.rt));
      }
      break;

      default:
        Panic("Missing implementation");
        break;
    }
  }
  else
  {
    switch (inst.cop.Cop0Op())
    {
      case Cop0Instruction::rfe:
      {
        // restore mode
        m_cop0_regs.sr.mode_bits = (m_cop0_regs.sr.mode_bits & UINT32_C(0b110000)) | (m_cop0_regs.sr.mode_bits >> 2);
      }
      break;

      default:
        Panic("Missing implementation");
        break;
    }
  }
}

void Core::ExecuteCop2Instruction()
{
  const Instruction inst = m_current_instruction;

  if (inst.cop.IsCommonInstruction())
  {
    // TODO: Combine with cop0.
    switch (inst.cop.CommonOp())
    {
      case CopCommonInstruction::cfcn:
        WriteRegDelayed(inst.r.rt, m_cop2.ReadControlRegister(static_cast<u32>(inst.r.rd.GetValue())));
        break;

      case CopCommonInstruction::ctcn:
        m_cop2.WriteControlRegister(static_cast<u32>(inst.r.rd.GetValue()), ReadReg(inst.r.rt));
        break;

      case CopCommonInstruction::mfcn:
        WriteRegDelayed(inst.r.rt, m_cop2.ReadDataRegister(static_cast<u32>(inst.r.rd.GetValue())));
        break;

      case CopCommonInstruction::mtcn:
        m_cop2.WriteDataRegister(static_cast<u32>(inst.r.rd.GetValue()), ReadReg(inst.r.rt));
        break;

      case CopCommonInstruction::bcnc:
      default:
        Panic("Missing implementation");
        break;
    }
  }
  else
  {
    m_cop2.ExecuteInstruction(GTE::Instruction{inst.bits});
  }
}

} // namespace CPU