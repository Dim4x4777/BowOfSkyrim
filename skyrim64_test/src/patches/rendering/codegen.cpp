#include "common.h"

ZydisDecoder g_Decoder;
uintptr_t g_CodeRegion;

void CreateXbyakPatches()
{
	// Initialize disassembler
	if (!ZYDIS_SUCCESS(ZydisDecoderInit(&g_Decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_ADDRESS_WIDTH_64)))
	{
		fputs("Failed to initialize decoder\n", stderr);
		exit(EXIT_FAILURE);
	}

	// Log file
	freopen("C:\\out.txt", "w", stdout);

#if TLS_DEBUG_MEMORY_ACCESS
	PageGuard_Monitor(g_ModuleBase + BSGRAPHICS_BASE_OFFSET, BSGRAPHICS_PATCH_SIZE);

	char buffer[512];
	sprintf_s(buffer, "tlsGlob is at 0x%llX", (uintptr_t)&tlsGlob);
	OutputDebugStringA(buffer);
#endif

	// Generate the instruction patch table (d3d11_patchlist.inl) to a file. 0x140000000
	// is the exe base address without ASLR.
	for (uintptr_t xref : XrefList)
		GenerateInstruction(xref - 0x140000000 + g_ModuleBase);

	// Do the actual code modifications
	std::unordered_map<uint32_t, void *> codeCache;

	for (auto& patch : XrefGeneratedPatches)
	{
		// Sanity check
		if (patch.Offset >= (0x2594))
			continue;

		PatchCodeGen gen(&patch, g_CodeRegion, TLS_INSTRUCTION_BLOCK_SIZE);
		uint8_t *rawCode = (uint8_t *)gen.getCode();
		uint32_t codeCRC = crc32c(rawCode, gen.getSize());

		// If it wasn't cached, we just insert it and increase the global code pointer
		if (codeCache.find(codeCRC) == codeCache.end())
		{
			codeCache.insert_or_assign(codeCRC, rawCode);
			g_CodeRegion += TLS_INSTRUCTION_BLOCK_SIZE;
		}

		WriteCodeHook(g_ModuleBase + patch.ExeOffset, codeCache[codeCRC]);
	}

	fflush(stdout);
}

void CreateXbyakCodeBlock()
{
	// Find a region within +/-2GB (minus some for tolerance)
	uintptr_t maxDelta = (1ull * 1024 * 1024 * 1024) - 4096;

	uintptr_t start = g_ModuleBase - maxDelta;
	uintptr_t end = g_ModuleBase + maxDelta;

	while (start < end)
	{
		MEMORY_BASIC_INFORMATION memInfo;
		if (VirtualQuery((LPVOID)start, &memInfo, sizeof(memInfo)) == 0)
			break;

		if (memInfo.State == MEM_FREE && memInfo.RegionSize >= TLS_INSTRUCTION_MEMORY_REGION_SIZE)
		{
			g_CodeRegion = (uintptr_t)VirtualAlloc(memInfo.BaseAddress, TLS_INSTRUCTION_MEMORY_REGION_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);

			if (g_CodeRegion)
				break;
		}

		start = (uintptr_t)memInfo.BaseAddress + 4096 + 1;
	}

	if (!g_CodeRegion)
		__debugbreak();
}

PatchCodeGen::PatchCodeGen(const PatchEntry *Patch, uintptr_t Memory, size_t MemorySize) : CodeGenerator(MemorySize, (void *)Memory)
{
#if TLS_DEBUG_ENABLE
	const uint32_t structMemberOffset = Patch->Offset;
#else
	// offsetof(IMAGE_TLS_DIRECTORY->StartAddressOfRawData, tlsGlob) + offsetof(BSGraphicsRendererGlobals, var)
	const uint32_t structMemberOffset = GetTlsOffset(&BSGraphics_TLSGlob) + Patch->Offset;
#endif

	auto& s = GetFreeScratch(Patch->Register, Patch->Base, Patch->Index);	// Scratch register
	auto& mem = MemOpSize(Patch->MemSize);									// Memory operand size
	auto& memop = mem[s + structMemberOffset];

	if (Patch->Base != ZYDIS_REGISTER_RIP)
	{
#if TLS_DEBUG_ENABLE
		// Sanity check the base, which **must** be the exe base address
		if (Patch->ExeOffset != 0xD6BF68 && Patch->ExeOffset != 0xD6BF4E && Patch->ExeOffset != 0xD6BFD3)
		{
			Xbyak::Label label1;
			Xbyak::Label label2;

			db(0x9C);// pushfq
			cmp(ZydisToXbyak64(Patch->Base), qword[rip + label1]);
			je(label2);
			db(0xCC);

			L(label1);
			dq(g_ModuleBase);

			L(label2);
			db(0x9D);// popfq
		}
#endif
		//
		// We need to override displacement so it's structMemberOffset instead. There's
		// a catch: we need to steal another register for the TLS address (base).
		//
		auto& base = s;// ZydisToXbyak(Patch->Base);

		if (Patch->Index != ZYDIS_REGISTER_NONE)
		{
			// Base + Index * Scale + Displacement
			auto& index = ZydisToXbyak(Patch->Index);
			uint32_t scale = max(Patch->Scale, 1);

			if (Patch->ExeOffset == 0xD6BF68 || Patch->ExeOffset == 0xD6BF4E || Patch->ExeOffset == 0xD6BFD3)
				memop = mem[s + rbx * 1 + structMemberOffset];
			else
				memop = mem[base + index * scale + structMemberOffset];
		}
		else
		{
			// Base + Displacement
			memop = mem[base + structMemberOffset];
		}
	}

	push(s);
	SetTlsBase(s);

	// SSE instructions
	if (Patch->Type == PatchType::MOVSS_REG_MEM ||
		Patch->Type == PatchType::MOVSS_MEM_REG ||
		Patch->Type == PatchType::SUBSS_REG_MEM ||
		Patch->Type == PatchType::ADDSS_REG_MEM ||
		Patch->Type == PatchType::MOVUPS_REG_MEM ||
		Patch->Type == PatchType::MOVUPS_MEM_REG ||
		Patch->Type == PatchType::MOVAPS_REG_MEM ||
		Patch->Type == PatchType::MOVAPS_MEM_REG ||
		Patch->Type == PatchType::SHUFPS_REG_MEM ||
		Patch->Type == PatchType::MOVSD_REG_MEM ||
		Patch->Type == PatchType::MOVSD_MEM_REG)
	{
		auto& xmmReg = ZydisToXbyakXmm(Patch->Register);

		switch (Patch->Type)
		{
		case PatchType::MOVSS_REG_MEM:movss(xmmReg, memop); break;		// movss   xmm, [address]
		case PatchType::MOVSS_MEM_REG:movss(memop, xmmReg); break;		// movss   [address], xmm

		case PatchType::ADDSS_REG_MEM:addss(xmmReg, memop); break;		// addss   xmm, [address]
		case PatchType::SUBSS_REG_MEM:subss(xmmReg, memop); break;		// subss   xmm, [address]

		case PatchType::MOVUPS_REG_MEM:movups(xmmReg, memop); break;	// movups  xmm, [address]
		case PatchType::MOVUPS_MEM_REG:movups(memop, xmmReg); break;	// movss   [address], xmm

		case PatchType::MOVAPS_REG_MEM:movaps(xmmReg, memop); break;	// movaps  xmm, [address]
		case PatchType::MOVAPS_MEM_REG:movaps(memop, xmmReg); break;	// movaps  [address], xmm

		case PatchType::SHUFPS_REG_MEM:shufps(xmmReg, memop, XrefGeneratedShufps[Patch->ExeOffset]); break;	// shufps xmm, [address], imm8

		case PatchType::MOVSD_REG_MEM:movsd(xmmReg, memop); break;		// movsd  xmm, [address]
		case PatchType::MOVSD_MEM_REG:movsd(memop, xmmReg); break;		// movsd  [address], xmm

		default:__debugbreak();
		}
	}
	else if (Patch->Type == PatchType::INC_MEM ||
		Patch->Type == PatchType::DEC_MEM)
	{
		// Single operands
		switch (Patch->Type)
		{
		case PatchType::INC_MEM:inc(memop); break;		// inc [address]
		case PatchType::DEC_MEM:dec(memop); break;		// dec [address]

		default:__debugbreak();
		}
	}
	else
	{
		// Normal instructions
		auto& r = ZydisToXbyak(Patch->Register);		// Original register
		auto& r64 = ZydisToXbyak64(Patch->Register);	// Temp

		switch (Patch->Type)
		{
		case PatchType::MOV_REG_MEM:mov(r, memop); break;
		case PatchType::ADD_REG_MEM:add(r, memop); break;
		case PatchType::AND_REG_MEM: and (r, memop); break;
		case PatchType::MOVSXD_REG_MEM:movsxd(r64, memop); break;
		case PatchType::MOVZX_REG_MEM:movzx(r, memop); break;
		case PatchType::LEA_REG_MEM:lea(r, memop); break;

		case PatchType::MOV_MEM_REG:mov(memop, r); break;
		case PatchType::ADD_MEM_REG:add(memop, r); break;
		case PatchType::AND_MEM_REG: and (memop, r); break;
		case PatchType::CMP_MEM_REG:cmp(memop, r); break;
		case PatchType::OR_MEM_REG: or (memop, r); break;

		case PatchType::MOV_MEM_IMM:mov(memop, (uint32_t)Patch->Immediate); break;
		case PatchType::ADD_MEM_IMM:add(memop, (uint32_t)Patch->Immediate); break;
		case PatchType::AND_MEM_IMM: and (memop, (uint32_t)Patch->Immediate); break;
		case PatchType::CMP_MEM_IMM:cmp(memop, (uint32_t)Patch->Immediate); break;
		case PatchType::OR_MEM_IMM: or (memop, (uint32_t)Patch->Immediate); break;

		default:__debugbreak();
		}
	}

	pop(s);
	ret();
}

void PatchCodeGen::SetTlsBase(const Xbyak::Reg64& Register)
{
#if TLS_DEBUG_ENABLE
	mov(Register, (uintptr_t)(g_ModuleBase + BSGRAPHICS_BASE_OFFSET));
#else
	//
	// In C++: *(uintptr_t *)(__readgsqword(0x58) + _tls_index * sizeof(void *));
	//
	// ((BYTE *)TEB->Tls + tlsBaseOffset) which is really TEB->Tls[tls_index]
	//
	const uint32_t tlsBaseOffset = _tls_index * sizeof(void *);

	// mov rax, qword ptr GS:[0x58]
	// mov rax, qword ptr DS:[rax + tlsBaseOffset]
	putSeg(gs); mov(Register, Xbyak::Address(32, false, 0x58));
	mov(Register, ptr[Register + tlsBaseOffset]);
#endif
}

const Xbyak::Reg64& PatchCodeGen::GetFreeScratch(ZydisRegister Operand, ZydisRegister Base, ZydisRegister Index)
{
	std::vector<Xbyak::Reg64> used;

	if (Operand != ZYDIS_REGISTER_NONE)
	{
		switch (ZydisRegisterGetClass(Operand))
		{
		case ZYDIS_REGCLASS_GPR8:
		case ZYDIS_REGCLASS_GPR16:
		case ZYDIS_REGCLASS_GPR32:
		case ZYDIS_REGCLASS_GPR64:
			used.push_back(ZydisToXbyak64(Operand));
			break;
		}
	}

	if (Base != ZYDIS_REGISTER_NONE && Base != ZYDIS_REGISTER_RIP)
		used.push_back(ZydisToXbyak64(Base));

	if (Index != ZYDIS_REGISTER_NONE && Index != ZYDIS_REGISTER_RIP)
		used.push_back(ZydisToXbyak64(Index));

	// Arbitrary regs used
	if (std::find(used.begin(), used.end(), rax) == used.end()) return rax;
	if (std::find(used.begin(), used.end(), rdi) == used.end()) return rdi;
	if (std::find(used.begin(), used.end(), rdx) == used.end()) return rdx;
	if (std::find(used.begin(), used.end(), rbp) == used.end()) return rbp;
	return r15;
}

const Xbyak::AddressFrame& PatchCodeGen::MemOpSize(int BitSize)
{
	switch (BitSize)
	{
	case 8:return byte;
	case 16:return word;
	case 32:return dword;
	case 64:return qword;
	case 128:return xword;
	case 256:return yword;
	case 512:return zword;
	}

	// For non-standard memory operand sizes
	return ptr;
}

const Xbyak::Reg& PatchCodeGen::ZydisToXbyak(ZydisRegister Register)
{
	switch (Register)
	{
	case ZYDIS_REGISTER_AL:return al;
	case ZYDIS_REGISTER_AH:return ah;
	case ZYDIS_REGISTER_AX:return ax;
	case ZYDIS_REGISTER_EAX:return eax;
	case ZYDIS_REGISTER_RAX:return rax;

	case ZYDIS_REGISTER_CL:return cl;
	case ZYDIS_REGISTER_CH:return ch;
	case ZYDIS_REGISTER_CX:return cx;
	case ZYDIS_REGISTER_ECX:return ecx;
	case ZYDIS_REGISTER_RCX:return rcx;

	case ZYDIS_REGISTER_DL:return dl;
	case ZYDIS_REGISTER_DH:return dh;
	case ZYDIS_REGISTER_DX:return dx;
	case ZYDIS_REGISTER_EDX:return edx;
	case ZYDIS_REGISTER_RDX:return rdx;

	case ZYDIS_REGISTER_BL:return bl;
	case ZYDIS_REGISTER_BH:return bh;
	case ZYDIS_REGISTER_BX:return bx;
	case ZYDIS_REGISTER_EBX:return ebx;
	case ZYDIS_REGISTER_RBX:return rbx;

	case ZYDIS_REGISTER_SPL:return spl;
	case ZYDIS_REGISTER_SP:return sp;
	case ZYDIS_REGISTER_ESP:return esp;
	case ZYDIS_REGISTER_RSP:return rsp;

	case ZYDIS_REGISTER_BPL:return bpl;
	case ZYDIS_REGISTER_BP:return bp;
	case ZYDIS_REGISTER_EBP:return ebp;
	case ZYDIS_REGISTER_RBP:return rbp;

	case ZYDIS_REGISTER_SIL:return sil;
	case ZYDIS_REGISTER_SI:return si;
	case ZYDIS_REGISTER_ESI:return esi;
	case ZYDIS_REGISTER_RSI:return rsi;

	case ZYDIS_REGISTER_DIL:return dil;
	case ZYDIS_REGISTER_DI:return di;
	case ZYDIS_REGISTER_EDI:return edi;
	case ZYDIS_REGISTER_RDI:return rdi;

	case ZYDIS_REGISTER_R8B:return r8b;
	case ZYDIS_REGISTER_R8W:return r8w;
	case ZYDIS_REGISTER_R8D:return r8d;
	case ZYDIS_REGISTER_R8:return r8;

	case ZYDIS_REGISTER_R9B:return r9b;
	case ZYDIS_REGISTER_R9W:return r9w;
	case ZYDIS_REGISTER_R9D:return r9d;
	case ZYDIS_REGISTER_R9:return r9;

	case ZYDIS_REGISTER_R10B:return r10b;
	case ZYDIS_REGISTER_R10W:return r10w;
	case ZYDIS_REGISTER_R10D:return r10d;
	case ZYDIS_REGISTER_R10:return r10;

	case ZYDIS_REGISTER_R11B:return r11b;
	case ZYDIS_REGISTER_R11W:return r11w;
	case ZYDIS_REGISTER_R11D:return r11d;
	case ZYDIS_REGISTER_R11:return r11;

	case ZYDIS_REGISTER_R12B:return r12b;
	case ZYDIS_REGISTER_R12W:return r12w;
	case ZYDIS_REGISTER_R12D:return r12d;
	case ZYDIS_REGISTER_R12:return r12;

	case ZYDIS_REGISTER_R13B:return r13b;
	case ZYDIS_REGISTER_R13W:return r13w;
	case ZYDIS_REGISTER_R13D:return r13d;
	case ZYDIS_REGISTER_R13:return r13;

	case ZYDIS_REGISTER_R14B:return r14b;
	case ZYDIS_REGISTER_R14W:return r14w;
	case ZYDIS_REGISTER_R14D:return r14d;
	case ZYDIS_REGISTER_R14:return r14;

	case ZYDIS_REGISTER_R15B:return r15b;
	case ZYDIS_REGISTER_R15W:return r15w;
	case ZYDIS_REGISTER_R15D:return r15d;
	case ZYDIS_REGISTER_R15:return r15;
	}

	const static Xbyak::Reg unused;
	return unused;
}

const Xbyak::Reg64& PatchCodeGen::ZydisToXbyak64(ZydisRegister Register)
{
	switch (Register)
	{
	case ZYDIS_REGISTER_AL:
	case ZYDIS_REGISTER_AH:
	case ZYDIS_REGISTER_AX:
	case ZYDIS_REGISTER_EAX:
	case ZYDIS_REGISTER_RAX:
		return rax;

	case ZYDIS_REGISTER_CL:
	case ZYDIS_REGISTER_CH:
	case ZYDIS_REGISTER_CX:
	case ZYDIS_REGISTER_ECX:
	case ZYDIS_REGISTER_RCX:
		return rcx;

	case ZYDIS_REGISTER_DL:
	case ZYDIS_REGISTER_DH:
	case ZYDIS_REGISTER_DX:
	case ZYDIS_REGISTER_EDX:
	case ZYDIS_REGISTER_RDX:
		return rdx;

	case ZYDIS_REGISTER_BL:
	case ZYDIS_REGISTER_BH:
	case ZYDIS_REGISTER_BX:
	case ZYDIS_REGISTER_EBX:
	case ZYDIS_REGISTER_RBX:
		return rbx;

	case ZYDIS_REGISTER_SPL:
	case ZYDIS_REGISTER_SP:
	case ZYDIS_REGISTER_ESP:
	case ZYDIS_REGISTER_RSP:
		return rsp;

	case ZYDIS_REGISTER_BPL:
	case ZYDIS_REGISTER_BP:
	case ZYDIS_REGISTER_EBP:
	case ZYDIS_REGISTER_RBP:
		return rbp;

	case ZYDIS_REGISTER_SIL:
	case ZYDIS_REGISTER_SI:
	case ZYDIS_REGISTER_ESI:
	case ZYDIS_REGISTER_RSI:
		return rsi;

	case ZYDIS_REGISTER_DIL:
	case ZYDIS_REGISTER_DI:
	case ZYDIS_REGISTER_EDI:
	case ZYDIS_REGISTER_RDI:
		return rdi;

	case ZYDIS_REGISTER_R8B:
	case ZYDIS_REGISTER_R8W:
	case ZYDIS_REGISTER_R8D:
	case ZYDIS_REGISTER_R8:
		return r8;

	case ZYDIS_REGISTER_R9B:
	case ZYDIS_REGISTER_R9W:
	case ZYDIS_REGISTER_R9D:
	case ZYDIS_REGISTER_R9:
		return r9;

	case ZYDIS_REGISTER_R10B:
	case ZYDIS_REGISTER_R10W:
	case ZYDIS_REGISTER_R10D:
	case ZYDIS_REGISTER_R10:
		return r10;

	case ZYDIS_REGISTER_R11B:
	case ZYDIS_REGISTER_R11W:
	case ZYDIS_REGISTER_R11D:
	case ZYDIS_REGISTER_R11:
		return r11;

	case ZYDIS_REGISTER_R12B:
	case ZYDIS_REGISTER_R12W:
	case ZYDIS_REGISTER_R12D:
	case ZYDIS_REGISTER_R12:
		return r12;

	case ZYDIS_REGISTER_R13B:
	case ZYDIS_REGISTER_R13W:
	case ZYDIS_REGISTER_R13D:
	case ZYDIS_REGISTER_R13:
		return r13;

	case ZYDIS_REGISTER_R14B:
	case ZYDIS_REGISTER_R14W:
	case ZYDIS_REGISTER_R14D:
	case ZYDIS_REGISTER_R14:
		return r14;

	case ZYDIS_REGISTER_R15B:
	case ZYDIS_REGISTER_R15W:
	case ZYDIS_REGISTER_R15D:
	case ZYDIS_REGISTER_R15:
		return r15;
	}

	const static Xbyak::Reg64 unused(-1);
	return unused;
}

const Xbyak::Xmm& PatchCodeGen::ZydisToXbyakXmm(ZydisRegister Register)
{
	switch (Register)
	{
	case ZYDIS_REGISTER_XMM0:return xm0;
	case ZYDIS_REGISTER_XMM1:return xm1;
	case ZYDIS_REGISTER_XMM2:return xm2;
	case ZYDIS_REGISTER_XMM3:return xm3;
	case ZYDIS_REGISTER_XMM4:return xm4;
	case ZYDIS_REGISTER_XMM5:return xm5;
	case ZYDIS_REGISTER_XMM6:return xm6;
	case ZYDIS_REGISTER_XMM7:return xm7;
	case ZYDIS_REGISTER_XMM8:return xm8;
	case ZYDIS_REGISTER_XMM9:return xm9;
	case ZYDIS_REGISTER_XMM10:return xm10;
	case ZYDIS_REGISTER_XMM11:return xm11;
	case ZYDIS_REGISTER_XMM12:return xm12;
	case ZYDIS_REGISTER_XMM13:return xm13;
	case ZYDIS_REGISTER_XMM14:return xm14;
	case ZYDIS_REGISTER_XMM15:return xm15;
	case ZYDIS_REGISTER_XMM16:return xm16;
	case ZYDIS_REGISTER_XMM17:return xm17;
	case ZYDIS_REGISTER_XMM18:return xm18;
	case ZYDIS_REGISTER_XMM19:return xm19;
	case ZYDIS_REGISTER_XMM20:return xm20;
	case ZYDIS_REGISTER_XMM21:return xm21;
	case ZYDIS_REGISTER_XMM22:return xm22;
	case ZYDIS_REGISTER_XMM23:return xm23;
	case ZYDIS_REGISTER_XMM24:return xm24;
	case ZYDIS_REGISTER_XMM25:return xm25;
	case ZYDIS_REGISTER_XMM26:return xm26;
	case ZYDIS_REGISTER_XMM27:return xm27;
	case ZYDIS_REGISTER_XMM28:return xm28;
	case ZYDIS_REGISTER_XMM29:return xm29;
	case ZYDIS_REGISTER_XMM30:return xm30;
	case ZYDIS_REGISTER_XMM31:return xm31;
	}

	__debugbreak();
	__assume(0);
}

void GenerateInstruction(uintptr_t Address)
{
	ZydisDecodedInstruction instruction;
	ZydisDecodedOperand *operands = instruction.operands;

	if (!ZYDIS_SUCCESS(ZydisDecoderDecodeBuffer(&g_Decoder, (BYTE *)Address, ZYDIS_MAX_INSTRUCTION_LENGTH, Address, &instruction)))
		__debugbreak();

	auto foundItem = std::find_if(OpTable.begin(), OpTable.end(),
		[&](const OpTableEntry &Entry)
	{
		if (Entry.Mnemonic != instruction.mnemonic)
			return false;

		if (Entry.Operand1 != ZYDIS_OPERAND_TYPE_UNUSED && Entry.Operand1 != instruction.operands[0].type)
			return false;

		if (Entry.Operand2 != ZYDIS_OPERAND_TYPE_UNUSED && Entry.Operand2 != instruction.operands[1].type)
			return false;

		return true;
	});

	if (foundItem != OpTable.end())
		GenerateCommonInstruction(&instruction, operands, foundItem->OutputType);
	else
		__debugbreak();

	if (instruction.mnemonic == ZYDIS_MNEMONIC_SHUFPS)
		printf("DO_SHUFPS_FIXUP(0x%llX, %lld)\n", Address - g_ModuleBase, instruction.operands[2].imm.value.s);
}

void GenerateCommonInstruction(ZydisDecodedInstruction *Instruction, ZydisDecodedOperand *Operands, const char *Type)
{
	//
	// NOTE: Base/index/scale must always be rip/0/0
	//
	// DO_THREADING_PATCH_<TYPE>(<SUBTYPE>, <INSTRUCTION EXE OFFSET>, <REGISTER | IMMEDIATE>, <OFFSET INTO BSGRAPHICS STRUCT>, <MEMORY OPERAND SIZE>, <BASE>, <INDEX>, <SCALE>);
	//
	char reg[256];
	char base[256];
	char index[256];

	memset(reg, 0, sizeof(reg));
	memset(base, 0, sizeof(base));
	memset(index, 0, sizeof(index));

	if (Instruction->operandCount == 1)
	{
		if (Operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY)
		{
			uint64_t outAddr;
			ZydisCalcAbsoluteAddress(Instruction, &Operands[0], &outAddr);

			if (Operands[0].mem.base != ZYDIS_REGISTER_RIP || Operands[0].mem.scale != 0)
				return;// TODO

			printf("DO_THREADING_PATCH_%s(0x%llX, %s, 0x%llX, %d)\n", Type, Instruction->instrAddress - g_ModuleBase, "NONE", outAddr - g_ModuleBase, Operands[0].size);
		}
		else
		{
			__debugbreak();
		}
	}
	else if (Instruction->operandCount >= 2)
	{
		if (Operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
		{
			// Write to memory from register
			uint64_t outAddr;

			if (Operands[0].mem.base != ZYDIS_REGISTER_RIP)
				outAddr = Operands[0].mem.disp.value;
			else if (ZYDIS_SUCCESS(ZydisCalcAbsoluteAddress(Instruction, &Operands[0], &outAddr)))
				outAddr -= g_ModuleBase;
			else
				__debugbreak();

			outAddr -= BSGRAPHICS_BASE_OFFSET;

			strcpy_s(reg, ZydisRegisterGetString(Operands[1].reg.value));
			_strupr_s(reg);

			strcpy_s(base, ZydisRegisterGetString(Operands[0].mem.base));
			_strupr_s(base);

			strcpy_s(index, ZydisRegisterGetString(Operands[0].mem.index));
			_strupr_s(index);

			printf("DO_THREADING_PATCH_REG(%s, 0x%llX, %s, 0x%llX, %d, %s, %s, %d)\n", Type, Instruction->instrAddress - g_ModuleBase, reg, outAddr, Operands[0].size, base, index, Operands[0].mem.scale);
		}
		else if (Operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
		{
			// Write to memory from immediate
			uint64_t outAddr;

			if (Operands[0].mem.base != ZYDIS_REGISTER_RIP)
				outAddr = Operands[0].mem.disp.value;
			else if (ZYDIS_SUCCESS(ZydisCalcAbsoluteAddress(Instruction, &Operands[0], &outAddr)))
				outAddr -= g_ModuleBase;
			else
				__debugbreak();

			outAddr -= BSGRAPHICS_BASE_OFFSET;

			strcpy_s(base, ZydisRegisterGetString(Operands[0].mem.base));
			_strupr_s(base);

			strcpy_s(index, ZydisRegisterGetString(Operands[0].mem.index));
			_strupr_s(index);

			if (Operands[1].imm.isSigned)
				printf("DO_THREADING_PATCH_IMM(%s, 0x%llX, %lldi64, 0x%llX, %d, %s, %s, %d)\n", Type, Instruction->instrAddress - g_ModuleBase, Operands[1].imm.value.s, outAddr, Operands[0].size, base, index, Operands[0].mem.scale);
			else
				printf("DO_THREADING_PATCH_IMM(%s, 0x%llX, 0x%llXui64, 0x%llX, %d, %s, %s, %d)\n", Type, Instruction->instrAddress - g_ModuleBase, Operands[1].imm.value.u, outAddr, Operands[0].size, base, index, Operands[0].mem.scale);
		}
		else if (Operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY)
		{
			// Write to register from memory
			uint64_t outAddr;

			// If the base register isn't RIP, we assume it's a relative virtual address
			if (Operands[1].mem.base != ZYDIS_REGISTER_RIP)
				outAddr = Operands[1].mem.disp.value;
			else if (ZYDIS_SUCCESS(ZydisCalcAbsoluteAddress(Instruction, &Operands[1], &outAddr)))
				outAddr -= g_ModuleBase;
			else
				__debugbreak();

			outAddr -= BSGRAPHICS_BASE_OFFSET;

			strcpy_s(reg, ZydisRegisterGetString(Operands[0].reg.value));
			_strupr_s(reg);

			strcpy_s(base, ZydisRegisterGetString(Operands[1].mem.base));
			_strupr_s(base);

			strcpy_s(index, ZydisRegisterGetString(Operands[1].mem.index));
			_strupr_s(index);

			printf("DO_THREADING_PATCH_REG(%s, 0x%llX, %s, 0x%llX, %d, %s, %s, %d)\n", Type, Instruction->instrAddress - g_ModuleBase, reg, outAddr, Operands[1].size, base, index, Operands[1].mem.scale);
		}
		else
		{
			__debugbreak();
		}
	}
	else
	{
		__debugbreak();
	}
}

void WriteCodeHook(uintptr_t TargetAddress, void *Code)
{
	ZydisDecodedInstruction instruction;
	if (!ZYDIS_SUCCESS(ZydisDecoderDecodeBuffer(&g_Decoder, (BYTE *)TargetAddress, ZYDIS_MAX_INSTRUCTION_LENGTH, TargetAddress, &instruction)))
		__debugbreak();

	if (instruction.length < 5)
		__debugbreak();

	BYTE data[ZYDIS_MAX_INSTRUCTION_LENGTH];
	memset(data, 0x90, sizeof(data));

	// Relative CALL
	data[0] = 0xE8;
	*(uint32_t *)&data[1] = (uint32_t)((uintptr_t)Code - TargetAddress - 5);

	// Pad nops so it shows up nicely in the debugger
	switch (instruction.length - 5)
	{
	case 1: data[5] = 0x90; break;
	case 2: data[5] = 0x66; data[6] = 0x90; break;
	case 3: data[5] = 0x0F; data[6] = 0x1F; data[7] = 0x00; break;
	case 4: data[5] = 0x0F; data[6] = 0x1F; data[7] = 0x40; data[8] = 0x00; break;
	default: break;
	}

	PatchMemory(TargetAddress, data, instruction.length);
}

uint32_t crc32c(unsigned char *Data, size_t Len)
{
	// http://www.hackersdelight.org/hdcodetxt/crc.c.txt
	int i, j;
	unsigned int byte, crc, mask;
	static unsigned int table[256];

	// Set up the table, if necessary.
	if (table[1] == 0) {
		for (byte = 0; byte <= 255; byte++) {
			crc = byte;
			for (j = 7; j >= 0; j--) {    // Do eight times.
				mask = -(crc & 1);
				crc = (crc >> 1) ^ (0xEDB88320 & mask);
			}
			table[byte] = crc;
		}
	}

	// Through with table setup, now calculate the CRC.
	i = 0;
	crc = 0xFFFFFFFF;
	while (i < Len) {
		byte = Data[i];
		crc = (crc >> 8) ^ table[(crc ^ byte) & 0xFF];
		i = i + 1;
	}
	return ~crc;
}