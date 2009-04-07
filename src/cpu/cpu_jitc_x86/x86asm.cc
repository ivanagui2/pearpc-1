/*
 *	PearPC
 *	x86asm.cc
 *
 *	Copyright (C) 2004 Sebastian Biallas (sb@biallas.net)
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "stdafx.h"

#include <cstring>
#include <cstdlib>

#include "tools/debug.h"
#include "tools/snprintf.h"
#include "jitc.h"
#include "jitc_asm.h"
#include "jitc_debug.h"
#include "x86asm.h"

void x86GetCaps(X86CPUCaps &caps)
{
	memset(&caps, 0, sizeof caps);

	caps.loop_align = 8;

	struct {
		uint32 level, c, d, b;
	} id;

	if (!ppc_cpuid_asm(0, &id)) {
		ht_snprintf(caps.vendor, sizeof caps.vendor, "unknown");
		return;
	}

	*((uint32 *)caps.vendor) = id.b;
	*((uint32 *)(caps.vendor+4)) = id.d;
	*((uint32 *)(caps.vendor+8)) = id.c;
	caps.vendor[12] = 0;
	ht_printf("%s\n", caps.vendor);
	if (id.level == 0) return;

	struct {
		uint32 model, features2, features, b;
	} id2;

	ppc_cpuid_asm(1, &id2);
	caps.rdtsc = id2.features & (1<<4);
	caps.cmov = id2.features & (1<<15);
	caps.mmx = id2.features & (1<<23);
	caps._3dnow = id2.features & (1<<31);
	caps._3dnow2 = id2.features & (1<<30);
	caps.sse = id2.features & (1<<25);
	caps.sse2 = id2.features & (1<<26);
	caps.sse3 = id2.features2 & (1<<0);
	
	ppc_cpuid_asm(0x80000000, &id);
	if (id.level >= 0x80000001) {
		// processor supports extended functions
		// now test for 3dnow
		ppc_cpuid_asm(0x80000001, &id2);
		
		caps._3dnow = id2.features & (1<<31);
		caps._3dnow2 = id2.features & (1<<30);
	}
	
	ht_printf("%s%s%s%s%s%s%s\n",
		caps.cmov?" CMOV":"",
		caps.mmx?" MMX":"",
		caps._3dnow?" 3DNOW":"",
		caps._3dnow2?" 3DNOW+":"",
		caps.sse?" SSE":"",
		caps.sse2?" SSE2":"",
		caps.sse3?" SSE3":"");
}

/*
 *	internal functions
 */

static inline void FASTCALL jitcMapRegister(NativeReg nreg, PPC_Register creg)
{
	gJITC.nativeReg[nreg] = creg;
	gJITC.clientReg[creg] = nreg;
}

static inline void FASTCALL jitcUnmapRegister(NativeReg reg)
{
	gJITC.clientReg[gJITC.nativeReg[reg]] = REG_NO;
	gJITC.nativeReg[reg] = PPC_REG_NO;
}

static inline void FASTCALL jitcLoadRegister(NativeReg nreg, PPC_Register creg)
{
	asmMOVRegDMem(nreg, (uint32)&gCPU+creg);
	jitcMapRegister(nreg, creg);
	gJITC.nativeRegState[nreg] = rsMapped;
}

static inline void FASTCALL jitcStoreRegister(NativeReg nreg, PPC_Register creg)
{
	asmMOVDMemReg((uint32)&gCPU+creg, nreg);
}

static inline void FASTCALL jitcStoreRegisterUndirty(NativeReg nreg, PPC_Register creg)
{
	jitcStoreRegister(nreg, creg);
	gJITC.nativeRegState[nreg] = rsMapped; // no longer dirty
}

static inline PPC_Register FASTCALL jitcGetRegisterMapping(NativeReg reg)
{
	return gJITC.nativeReg[reg];
}

NativeReg FASTCALL jitcGetClientRegisterMapping(PPC_Register creg)
{
	return gJITC.clientReg[creg];
}

static inline void FASTCALL jitcDiscardRegister(NativeReg r)
{
	// FIXME: move to front of the LRU list
	gJITC.nativeRegState[r] = rsUnused;
}

/**
 *	Puts native register to the end of the LRU list
 */
void FASTCALL jitcTouchRegister(NativeReg r)
{
	NativeRegType *reg = gJITC.nativeRegsList[r];
	if (reg->moreRU) {
		// there's a more recently used register
		if (reg->lessRU) {
			reg->lessRU->moreRU = reg->moreRU;
			reg->moreRU->lessRU = reg->lessRU;
		} else {
			// reg was LRUreg
			gJITC.LRUreg = reg->moreRU;
			reg->moreRU->lessRU = NULL;
		}
		reg->moreRU = NULL;
		reg->lessRU = gJITC.MRUreg;
		gJITC.MRUreg->moreRU = reg;
		gJITC.MRUreg = reg;
	}
}

/**
 *	clobbers and moves to end of LRU list
 */
static inline void FASTCALL jitcClobberAndTouchRegister(NativeReg reg)
{
	switch (gJITC.nativeRegState[reg]) {
	case rsDirty:
		jitcStoreRegisterUndirty(reg, jitcGetRegisterMapping(reg));
		// fall throu
	case rsMapped:
		jitcUnmapRegister(reg);
		gJITC.nativeRegState[reg] = rsUnused;
		break;
	case rsUnused:;
	}
	jitcTouchRegister(reg);
}

/**
 *	clobbers and moves to front of LRU list
 */
static inline void FASTCALL jitcClobberAndDiscardRegister(NativeReg reg)
{
	switch (gJITC.nativeRegState[reg]) {
	case rsDirty:
		jitcStoreRegisterUndirty(reg, jitcGetRegisterMapping(reg));
		// fall throu
	case rsMapped:
		jitcUnmapRegister(reg);
		jitcDiscardRegister(reg);
		break;
	case rsUnused:;
		/*
		 *	Note: it makes no sense to move this register to
		 *	the front of the LRU list here, since only
		 *	other unused register can be before it in the list
		 *
		 *	Note2: it would even be an error to move it here,
		 *	since ESP isn't in the nativeRegsList
		 */
	}
}

void FASTCALL jitcClobberSingleRegister(NativeReg reg)
{
	switch (gJITC.nativeRegState[reg]) {
	case rsDirty:
		jitcStoreRegisterUndirty(reg, jitcGetRegisterMapping(reg));
		// fall throu
	case rsMapped:
		jitcUnmapRegister(reg);
		gJITC.nativeRegState[reg] = rsUnused;
		break;
	case rsUnused:;
	}
}

/**
 *	Dirty register.
 *	Does *not* touch register
 *	Will not produce code.
 */
NativeReg FASTCALL jitcDirtyRegister(NativeReg r)
{
	gJITC.nativeRegState[r] = rsDirty;
	return r;
}

NativeReg FASTCALL jitcAllocFixedRegister(NativeReg reg)
{
	jitcClobberAndTouchRegister(reg);
	return reg;
}

/**
 *	Allocates a native register
 *	May produce a store if no registers are avaiable
 */
NativeReg FASTCALL jitcAllocRegister(int options)
{
	NativeReg reg;
	if (options & NATIVE_REG) {
		// allocate fixed register
		reg = (NativeReg)(options & 0xf);
	} else if (options & NATIVE_REG_8) {
		// allocate eax, ecx, edx or ebx
		NativeRegType *rt = gJITC.LRUreg;
		while (rt->reg > EBX) rt = rt->moreRU;
		reg = rt->reg;
	} else {
		// allocate random register
		reg = gJITC.LRUreg->reg;
	}
	return jitcAllocFixedRegister(reg);
}

/**
 *	Returns native registers that contains value of 
 *	client register or allocates new register which
 *	maps to the client register.
 *	Dirties register.
 *
 *	May produce a store if no registers are avaiable
 *	May produce a MOV/XCHG to satisfy mapping
 *	Will never produce a load
 */
NativeReg FASTCALL jitcMapClientRegisterDirty(PPC_Register creg, int options)
{
	if (options & NATIVE_REG_8) {
		// nyi
		ht_printf("unimpl x86asm:%d\n", __LINE__);
		exit(-1);
	}
	if (options & NATIVE_REG) {
		NativeReg want_reg = (NativeReg)(options & 0xf);
		PPC_Register have_mapping = jitcGetRegisterMapping(want_reg);

		if (have_mapping != PPC_REG_NO) {
			// test if we're lucky
			if (have_mapping == creg) {
				jitcDirtyRegister(want_reg);
				jitcTouchRegister(want_reg);
				return want_reg;
			}

			// we're not lucky, get a new register for the old mapping
			NativeReg temp_reg = jitcAllocRegister();
			// note that AllocRegister also touches temp_reg

			// make new mapping
			jitcMapRegister(want_reg, creg);

			gJITC.nativeRegState[temp_reg] = gJITC.nativeRegState[want_reg];
			// now we can mess with want_reg
			jitcDirtyRegister(want_reg);

			// maybe the old mapping was discarded and we're done
			if (temp_reg == want_reg) return want_reg;

			// ok, restore old mapping
			if (temp_reg == EAX || want_reg == EAX) {
				asmALURegReg(X86_XCHG, temp_reg, want_reg);
			} else {
				asmALURegReg(X86_MOV, temp_reg, want_reg);				
			}
			jitcMapRegister(temp_reg, have_mapping);
		} else {
			// want_reg is free
			// unmap creg if needed
			NativeReg reg = jitcGetClientRegisterMapping(creg);
			if (reg != REG_NO) {
				jitcUnmapRegister(reg);
				jitcDiscardRegister(reg);
			}
			jitcMapRegister(want_reg, creg);
			jitcDirtyRegister(want_reg);
		}
		jitcTouchRegister(want_reg);
		return want_reg;
	} else {
		NativeReg reg = jitcGetClientRegisterMapping(creg);
		if (reg == REG_NO) {
			reg = jitcAllocRegister();
			jitcMapRegister(reg, creg);
		} else {
			jitcTouchRegister(reg);
		}
		return jitcDirtyRegister(reg);
	}
}

 
/**
 *	Returns native registers that contains value of 
 *	client register or allocates new register with
 *	this content.
 *
 *	May produce a store if no registers are avaiable
 *	May produce a load if client registers isn't mapped
 *	May produce a MOV/XCHG to satisfy mapping
 */
NativeReg FASTCALL jitcGetClientRegister(PPC_Register creg, int options)
{
	if (options & NATIVE_REG_8) {
		NativeReg client_reg_maps_to = jitcGetClientRegisterMapping(creg);
		if (client_reg_maps_to == REG_NO) {
			NativeReg reg = jitcAllocRegister(NATIVE_REG_8);
			jitcLoadRegister(reg, creg);
			return reg;
		} else {
			if (client_reg_maps_to <= EBX) {
				jitcTouchRegister(client_reg_maps_to);
				return client_reg_maps_to;
			}
			NativeReg want_reg = jitcAllocRegister(NATIVE_REG_8);
			asmALURegReg(X86_MOV, want_reg, client_reg_maps_to);
			jitcUnmapRegister(client_reg_maps_to);
			jitcMapRegister(want_reg, creg);
			gJITC.nativeRegState[want_reg] = gJITC.nativeRegState[client_reg_maps_to];
			gJITC.nativeRegState[client_reg_maps_to] = rsUnused;
			return want_reg;
		}
	}
	if (options & NATIVE_REG) {
		NativeReg want_reg = (NativeReg)(options & 0xf);
		PPC_Register native_reg_maps_to = jitcGetRegisterMapping(want_reg);
		NativeReg client_reg_maps_to = jitcGetClientRegisterMapping(creg);
		if (native_reg_maps_to != PPC_REG_NO) {
			// test if we're lucky
			if (native_reg_maps_to == creg) {
				jitcTouchRegister(want_reg);
			} else {
				// we need to satisfy mapping
				if (client_reg_maps_to != REG_NO) {
					asmALURegReg(X86_XCHG, want_reg, client_reg_maps_to);
					RegisterState rs = gJITC.nativeRegState[want_reg];
					gJITC.nativeRegState[want_reg] = gJITC.nativeRegState[client_reg_maps_to];
					gJITC.nativeRegState[client_reg_maps_to] = rs;
					jitcMapRegister(want_reg, creg);
					jitcMapRegister(client_reg_maps_to, native_reg_maps_to);
					jitcTouchRegister(want_reg);
				} else {
					// client register isn't mapped
					jitcAllocFixedRegister(want_reg);
					jitcLoadRegister(want_reg, creg);
				}
			}
			return want_reg;
		} else {
			// want_reg is free 
			jitcTouchRegister(want_reg);
			if (client_reg_maps_to != REG_NO) {
				asmALURegReg(X86_MOV, want_reg, client_reg_maps_to);
				gJITC.nativeRegState[want_reg] = gJITC.nativeRegState[client_reg_maps_to];
				jitcUnmapRegister(client_reg_maps_to);
				jitcDiscardRegister(client_reg_maps_to);
				jitcMapRegister(want_reg, creg);
			} else {
				jitcLoadRegister(want_reg, creg);
			}
			return want_reg;
		}
	} else {
		NativeReg client_reg_maps_to = jitcGetClientRegisterMapping(creg);
		if (client_reg_maps_to != REG_NO) {
			jitcTouchRegister(client_reg_maps_to);
			return client_reg_maps_to;
		} else {
			NativeReg reg = jitcAllocRegister();
			jitcLoadRegister(reg, creg);
			return reg;
		}
	}
}

/**
 *	Same as jitcGetClientRegister() but also dirties result
 */
NativeReg FASTCALL jitcGetClientRegisterDirty(PPC_Register creg, int options)
{
	return jitcDirtyRegister(jitcGetClientRegister(creg, options));
}

static inline void FASTCALL jitcFlushSingleRegister(NativeReg reg)
{
	if (gJITC.nativeRegState[reg] == rsDirty) {
		jitcStoreRegisterUndirty(reg, jitcGetRegisterMapping(reg));
	}
}

static inline void FASTCALL jitcFlushSingleRegisterDirty(NativeReg reg)
{
	if (gJITC.nativeRegState[reg] == rsDirty) {
		jitcStoreRegister(reg, jitcGetRegisterMapping(reg));
	}
}

/**
 *	Flushes native register(s).
 *	Resets dirty flags.
 *	Will produce a store if register is dirty.
 */
void FASTCALL jitcFlushRegister(int options)
{
	if (options == NATIVE_REGS_ALL) {
		for (NativeReg i = EAX; i <= EDI; i = (NativeReg)(i+1)) jitcFlushSingleRegister(i);
	} else if (options & NATIVE_REG) {
		NativeReg reg = (NativeReg)(options & 0xf);
		jitcFlushSingleRegister(reg);
	}
}

/**
 *	Flushes native register(s).
 *	Doesnt reset dirty flags.
 *	Will produce a store if register is dirty.
 */
void FASTCALL jitcFlushRegisterDirty(int options)
{
	if (options == NATIVE_REGS_ALL) {
		for (NativeReg i = EAX; i <= EDI; i = (NativeReg)(i+1)) jitcFlushSingleRegisterDirty(i);
	} else if (options & NATIVE_REG) {
		NativeReg reg = (NativeReg)(options & 0xf);
		jitcFlushSingleRegisterDirty(reg);
	}
}
/**
 *	Clobbers native register(s).
 *	Register is unused afterwards.
 *	Will produce a store if register was dirty.
 */          
void FASTCALL jitcClobberRegister(int options)
{
	if (options == NATIVE_REGS_ALL) {
		/*
		 *	We dont use clobberAndDiscard here
		 *	since it make no sense to move one register
		 *	if we clobber all
		 */
		for (NativeReg i = EAX; i <= EDI; i=(NativeReg)(i+1)) jitcClobberSingleRegister(i);
	} else if (options & NATIVE_REG) {
		NativeReg reg = (NativeReg)(options & 0xf);
		jitcClobberAndDiscardRegister(reg);
	}
}

/**
 *
 */
void FASTCALL jitcFlushAll()
{
	jitcClobberCarryAndFlags();
	jitcFlushRegister();
	jitcFlushVectorRegister();
}

/**
 *
 */
void FASTCALL jitcClobberAll()
{
	jitcClobberCarryAndFlags();
	jitcClobberRegister();
	jitcFloatRegisterClobberAll();
	jitcTrashVectorRegister();
}

/**
 *	Invalidates all mappings
 *
 *	Will never produce code
 */          
void FASTCALL jitcInvalidateAll()
{
#if 0
	for (int i=EAX; i<=EDI; i++) {
		if(gJITC.nativeRegState[i] != rsDirty) {
			printf("!!! Unflushed register invalidated!\n");
		}
	}
#endif

	memset(gJITC.nativeReg, PPC_REG_NO, sizeof gJITC.nativeReg);
	memset(gJITC.nativeRegState, rsUnused, sizeof gJITC.nativeRegState);
	memset(gJITC.clientReg, REG_NO, sizeof gJITC.clientReg);
	gJITC.nativeCarryState = gJITC.nativeFlagsState = rsUnused;

	for (unsigned int i=XMM0; i<=XMM7; i++) {
		if(gJITC.nativeVectorRegState[i] == rsDirty) {
			printf("!!! Unflushed vector register invalidated! (XMM%u)\n", i);
		}
	}

	memset(gJITC.n2cVectorReg, PPC_VECTREG_NO, sizeof gJITC.n2cVectorReg);
	memset(gJITC.c2nVectorReg, VECTREG_NO, sizeof gJITC.c2nVectorReg);
	memset(gJITC.nativeVectorRegState, rsUnused, sizeof gJITC.nativeVectorRegState);

	gJITC.nativeVectorReg = VECTREG_NO;
}

/**
 *	Gets the client carry flags into the native carry flag
 *
 *	
 */
void FASTCALL jitcGetClientCarry()
{
	if (gJITC.nativeCarryState == rsUnused) {
		jitcClobberFlags();

#if 0
		// bt [gCPU.xer], XER_CA
		byte modrm[6];
		asmBTxMemImm(X86_BT, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.xer), 29);
#else
		// bt [gCPU.xer_ca], 0
		byte modrm[6];
		asmBTxMemImm(X86_BT, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.xer_ca), 0);
#endif
		gJITC.nativeCarryState = rsMapped;
	}
}

void FASTCALL jitcMapFlagsDirty(PPC_CRx cr)
{
	gJITC.nativeFlags = cr;
	gJITC.nativeFlagsState = rsDirty;
}

PPC_CRx FASTCALL jitcGetFlagsMapping()
{
	return gJITC.nativeFlags;
}

bool FASTCALL jitcFlagsMapped()
{
	return gJITC.nativeFlagsState != rsUnused;
}

bool FASTCALL jitcCarryMapped()
{
	return gJITC.nativeCarryState != rsUnused;
}

void FASTCALL jitcMapCarryDirty()
{
	gJITC.nativeCarryState = rsDirty;
}

static inline void FASTCALL jitcFlushCarry()
{
	byte modrm[6];
	asmSETMem(X86_C, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.xer_ca));
}

#if 0

static inline void FASTCALL jitcFlushFlags()
{
	asmCALL((NativeAddress)ppc_flush_flags_asm);
}

#else

uint8 jitcFlagsMapping[257];
uint8 jitcFlagsMapping2[256];
uint8 jitcFlagsMappingCMP_U[257];
uint8 jitcFlagsMappingCMP_L[257];

static inline void FASTCALL jitcFlushFlags()
{
#if 1
	byte modrm[6];
	NativeReg r = jitcAllocRegister(NATIVE_REG_8);
	asmSETReg8(X86_S, (NativeReg8)r);
	asmSETReg8(X86_Z, (NativeReg8)(r+4));
	asmMOVxxRegReg16(X86_MOVZX, r, r);
	asmALUMemImm8(X86_AND, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.cr+3), 0x0f);
	asmALURegMem8(X86_MOV, (NativeReg8)r, modrm, x86_mem(modrm, r, (uint32)&jitcFlagsMapping));
	asmALUMemReg8(X86_OR, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.cr+3), (NativeReg8)r);
#else 
	byte modrm[6];
	jitcAllocRegister(NATIVE_REG | EAX);
	asmSimple(X86_LAHF);
	asmMOVxxRegReg8(X86_MOVZX, EAX, AH);
	asmALUMemImm8(X86_AND, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.cr+3), 0x0f);
	asmALURegMem8(X86_MOV, AL, modrm, x86_mem(modrm, EAX, (uint32)&jitcFlagsMapping2));
	asmALUMemReg8(X86_OR, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.cr+3), AL);
#endif
}

#endif

static inline void jitcFlushFlagsAfterCMP(X86FlagTest t1, X86FlagTest t2, byte mask, int disp, uint32 map)
{
	byte modrm[6];
	NativeReg r = jitcAllocRegister(NATIVE_REG_8);
	asmSETReg8(t1, (NativeReg8)r);
	asmSETReg8(t2, (NativeReg8)(r+4));
	asmMOVxxRegReg16(X86_MOVZX, r, r);
	asmALUMemImm8(X86_AND, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.cr+disp), mask);
	asmALURegMem8(X86_MOV, (NativeReg8)r, modrm, x86_mem(modrm, r, map));
	asmALUMemReg8(X86_OR, modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.cr+disp), (NativeReg8)r);
}

void FASTCALL jitcFlushFlagsAfterCMPL_U(int disp)
{
	jitcFlushFlagsAfterCMP(X86_A, X86_B, 0x0f, disp, (uint32)&jitcFlagsMappingCMP_U);
}

void FASTCALL jitcFlushFlagsAfterCMPL_L(int disp)
{
	jitcFlushFlagsAfterCMP(X86_A, X86_B, 0xf0, disp, (uint32)&jitcFlagsMappingCMP_L);
}

void FASTCALL jitcFlushFlagsAfterCMP_U(int disp)
{
	jitcFlushFlagsAfterCMP(X86_G, X86_L, 0x0f, disp, (uint32)&jitcFlagsMappingCMP_U);
}

void FASTCALL jitcFlushFlagsAfterCMP_L(int disp)
{
	jitcFlushFlagsAfterCMP(X86_G, X86_L, 0xf0, disp, (uint32)&jitcFlagsMappingCMP_L);
}

void FASTCALL jitcClobberFlags()
{
	if (gJITC.nativeFlagsState == rsDirty) {
		if (gJITC.nativeCarryState == rsDirty) {
			jitcFlushCarry();
		}
		jitcFlushFlags();
		gJITC.nativeCarryState = rsUnused;
	}
	gJITC.nativeFlagsState = rsUnused;
}

void FASTCALL jitcClobberCarry()
{
	if (gJITC.nativeCarryState == rsDirty) {
		jitcFlushCarry();
	}
	gJITC.nativeCarryState = rsUnused;
}

void FASTCALL jitcClobberCarryAndFlags()
{
	if (gJITC.nativeCarryState == rsDirty) {
		if (gJITC.nativeFlagsState == rsDirty) {
			jitcFlushCarry();
			jitcFlushFlags();
			gJITC.nativeCarryState = gJITC.nativeFlagsState = rsUnused;
		} else {
			jitcClobberCarry();
		}
	} else {
		jitcClobberFlags();
	}
}

/**
 *	ONLY FOR DEBUG! DON'T CALL (unless you know what you are doing)
 */
void FASTCALL jitcFlushCarryAndFlagsDirty()
{
	if (gJITC.nativeCarryState == rsDirty) {
		jitcFlushCarry();
		if (gJITC.nativeFlagsState == rsDirty) {
			jitcFlushFlags();
		}
	} else {
		if (gJITC.nativeFlagsState == rsDirty) {
			jitcFlushFlags();
		}
	}
}

/**
 *	jitcFloatRegisterToNative converts the stack-independent 
 *	register r to a stack-dependent register ST(i)
 */
NativeFloatReg FASTCALL jitcFloatRegisterToNative(JitcFloatReg r)
{
	return X86_FLOAT_ST(gJITC.nativeFloatTOP-gJITC.floatRegPerm[r]);
}

/**
 *	jitcFloatRegisterFromNative converts the stack-dependent 
 *	register ST(r) to a stack-independent JitcFloatReg
 */
JitcFloatReg FASTCALL jitcFloatRegisterFromNative(NativeFloatReg r)
{
	ASSERT(gJITC.nativeFloatTOP > r);
	return gJITC.floatRegPermInverse[gJITC.nativeFloatTOP-r];
}

/**
 *	Returns true iff r is on top of the floating point register
 *	stack.
 */
bool FASTCALL jitcFloatRegisterIsTOP(JitcFloatReg r)
{
	ASSERT(r != JITC_FLOAT_REG_NONE);
	return gJITC.floatRegPerm[r] == gJITC.nativeFloatTOP;
}

/**
 *	Exchanges r to the front of the stack.
 */
JitcFloatReg FASTCALL jitcFloatRegisterXCHGToFront(JitcFloatReg r)
{
	ASSERT(r != JITC_FLOAT_REG_NONE);
	if (jitcFloatRegisterIsTOP(r)) return r;
	
	asmFXCHSTi(jitcFloatRegisterToNative(r));
	JitcFloatReg s = jitcFloatRegisterFromNative(Float_ST0);
	ASSERT(s != r);
	// set floatRegPerm := floatRegPerm * (s r)
	int tmp = gJITC.floatRegPerm[r];
	gJITC.floatRegPerm[r] = gJITC.floatRegPerm[s];
	gJITC.floatRegPerm[s] = tmp;
	
	// set floatRegPermInverse := (s r) * floatRegPermInverse
	r = gJITC.floatRegPerm[r];
	s = gJITC.floatRegPerm[s];
	tmp = gJITC.floatRegPermInverse[r];
	gJITC.floatRegPermInverse[r] = gJITC.floatRegPermInverse[s];
	gJITC.floatRegPermInverse[s] = tmp;

	return r;
}

/**
 *	Dirties r
 */
JitcFloatReg FASTCALL jitcFloatRegisterDirty(JitcFloatReg r)
{
	gJITC.nativeFloatRegState[r] = rsDirty;
	return r;
}

void FASTCALL jitcFloatRegisterInvalidate(JitcFloatReg r)
{
	jitcFloatRegisterXCHGToFront(r);
	asmFFREEPSTi(Float_ST0);	
	int creg = gJITC.nativeFloatRegStack[r];
	gJITC.clientFloatReg[creg] = JITC_FLOAT_REG_NONE;
	gJITC.nativeFloatTOP--;
}

void FASTCALL jitcPopFloatStack(JitcFloatReg hint1, JitcFloatReg hint2)
{
	ASSERT(gJITC.nativeFloatTOP > 0);
	
	JitcFloatReg r;
	for (int i=0; i<4; i++) {
		r = jitcFloatRegisterFromNative(X86_FLOAT_ST(gJITC.nativeFloatTOP-i-1));
		if (r != hint1 && r != hint2) break;
	}
	
	// we can now free r
	int creg = gJITC.nativeFloatRegStack[r];
	jitcFloatRegisterXCHGToFront(r);
	if (gJITC.nativeFloatRegState[r] == rsDirty) {
		byte modrm[6];
		asmFSTPDoubleMem(modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.fpr[creg]));
	} else {
		asmFFREEPSTi(Float_ST0);
	}
	gJITC.nativeFloatRegState[r] = rsUnused;
	gJITC.clientFloatReg[creg] = JITC_FLOAT_REG_NONE;
	gJITC.nativeFloatTOP--;
}

static JitcFloatReg FASTCALL jitcPushFloatStack(int creg)
{
	ASSERT(gJITC.nativeFloatTOP < 8);
	gJITC.nativeFloatTOP++;
	int r = gJITC.floatRegPermInverse[gJITC.nativeFloatTOP];
	byte modrm[6];
	asmFLDDoubleMem(modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.fpr[creg]));
	return r;
}

/**
 *	Creates a copy of r on the stack. If the stack is full, it will
 *	clobber an entry. It will not clobber r nor hint.
 */
JitcFloatReg FASTCALL jitcFloatRegisterDup(JitcFloatReg freg, JitcFloatReg hint)
{
//	ht_printf("dup %d\n", freg);
	if (gJITC.nativeFloatTOP == 8) {
		// stack is full
		jitcPopFloatStack(freg, hint);
	}
	asmFLDSTi(jitcFloatRegisterToNative(freg));
	gJITC.nativeFloatTOP++;
	int r = gJITC.floatRegPermInverse[gJITC.nativeFloatTOP];
	gJITC.nativeFloatRegState[r] = rsUnused; // not really mapped
	return r;
}

void FASTCALL jitcFloatRegisterClobberAll()
{
	if (!gJITC.nativeFloatTOP) return;
	
	do  {
		JitcFloatReg r = jitcFloatRegisterFromNative(Float_ST0);
		int creg = gJITC.nativeFloatRegStack[r];
		switch (gJITC.nativeFloatRegState[r]) {
		case rsDirty: {
			byte modrm[6];
			asmFSTPDoubleMem(modrm, x86_mem(modrm, REG_NO, (uint32)&gCPU.fpr[creg]));
			gJITC.clientFloatReg[creg] = JITC_FLOAT_REG_NONE;
			break;
		}
		case rsMapped:
			asmFFREEPSTi(Float_ST0);
			gJITC.clientFloatReg[creg] = JITC_FLOAT_REG_NONE;
			break;
		case rsUnused: {ASSERT(0);}
		}
	} while (--gJITC.nativeFloatTOP);
}

void FASTCALL jitcFloatRegisterStoreAndPopTOP(JitcFloatReg r)
{
	asmFSTDPSTi(jitcFloatRegisterToNative(r));
	gJITC.nativeFloatTOP--;
}

void FASTCALL jitcClobberClientRegisterForFloat(int creg)
{
	NativeReg r = jitcGetClientRegisterMapping(PPC_FPR_U(creg));
	if (r != REG_NO) jitcClobberRegister(r | NATIVE_REG);
	r = jitcGetClientRegisterMapping(PPC_FPR_L(creg));
	if (r != REG_NO) jitcClobberRegister(r | NATIVE_REG);
}

void FASTCALL jitcInvalidateClientRegisterForFloat(int creg)
{
	// FIXME: no need to clobber, invalidate would be enough
	jitcClobberClientRegisterForFloat(creg);
}

JitcFloatReg FASTCALL jitcGetClientFloatRegisterMapping(int creg)
{
	return gJITC.clientFloatReg[creg];
}

JitcFloatReg FASTCALL jitcGetClientFloatRegisterUnmapped(int creg, int hint1, int hint2)
{
	JitcFloatReg r = jitcGetClientFloatRegisterMapping(creg);
	if (r == JITC_FLOAT_REG_NONE) {
		if (gJITC.nativeFloatTOP == 8) {
			jitcPopFloatStack(hint1, hint2);
		}
		r = jitcPushFloatStack(creg);
		gJITC.nativeFloatRegState[r] = rsUnused;
	}
	return r;
}

JitcFloatReg FASTCALL jitcGetClientFloatRegister(int creg, int hint1, int hint2)
{
	JitcFloatReg r = jitcGetClientFloatRegisterMapping(creg);
	if (r == JITC_FLOAT_REG_NONE) {
		if (gJITC.nativeFloatTOP == 8) {
			jitcPopFloatStack(hint1, hint2);
		}
		r = jitcPushFloatStack(creg);
		gJITC.clientFloatReg[creg] = r;
		gJITC.nativeFloatRegStack[r] = creg;
		gJITC.nativeFloatRegState[r] = rsMapped;
	}
	return r;
}

JitcFloatReg FASTCALL jitcMapClientFloatRegisterDirty(int creg, JitcFloatReg freg)
{
	if (freg == JITC_FLOAT_REG_NONE) {
		freg = jitcFloatRegisterFromNative(Float_ST0);
	}
	gJITC.clientFloatReg[creg] = freg;
	gJITC.nativeFloatRegStack[freg] = creg;
	gJITC.nativeFloatRegState[freg] = rsDirty;
	return freg;
}

/**
 *
 */
NativeAddress FASTCALL asmHERE()
{
	return gJITC.currentPage->tcp;
}

void FASTCALL asmNOP(int n)
{
	if (n <= 0) return;
	byte instr[15];
	for (int i=0; i < (n-1); i++) {
		instr[i] = 0x66;
	}
	instr[n-1] = 0x90;
	jitcEmit(instr, n);	
}

static void FASTCALL asmSimpleMODRM(uint8 opc, NativeReg reg1, NativeReg reg2)
{
	byte instr[2] = {opc, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

static void FASTCALL asmSimpleMODRM(uint8 opc, NativeReg16 reg1, NativeReg16 reg2)
{
	byte instr[3] = {0x66, opc, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

static void FASTCALL asmSimpleMODRM(uint8 opc, NativeReg8 reg1, NativeReg8 reg2)
{
	byte instr[2] = {opc, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

static void FASTCALL asmTEST_D(NativeReg reg1, uint32 imm)
{
	if (reg1 <= EBX) {
		if (imm <= 0xff) {
			// test al, 1
			if (reg1 == EAX) {
				byte instr[2] = {0xa8, imm};
				jitcEmit(instr, sizeof(instr));
			} else {
				byte instr[3] = {0xf6, 0xc0+reg1, imm};
				jitcEmit(instr, sizeof(instr));
			}
			return;
		} else if (!(imm & 0xffff00ff)) {
			// test ah, 1
			byte instr[3] = {0xf6, 0xc4+reg1, (imm>>8)};
			jitcEmit(instr, sizeof(instr));
			return;
		}
	}
	// test eax, 1001
	if (reg1 == EAX) {
		byte instr[5];
		instr[0] = 0xa9;
		*((uint32 *)&instr[1]) = imm;
		jitcEmit(instr, sizeof(instr));
	} else {
		byte instr[6];
		instr[0] = 0xf7;
		instr[1] = 0xc0+reg1;
		*((uint32 *)&instr[2]) = imm;
		jitcEmit(instr, sizeof(instr));
	}
}

static void FASTCALL asmTEST_W(NativeReg16 reg1, uint16 imm)
{
	if (reg1 <= BX) {
		if (imm <= 0xff) {
			// test al, 1
			if (reg1 == AX) {
				byte instr[2] = {0xa8, imm};
				jitcEmit(instr, sizeof(instr));
			} else {
				byte instr[3] = {0xf6, 0xc0+reg1, imm};
				jitcEmit(instr, sizeof(instr));
			}
			return;
		} else if (!(imm & 0xffff00ff)) {
			// test ah, 1
			byte instr[3] = {0xf6, 0xc4+reg1, (imm>>8)};
			jitcEmit(instr, sizeof(instr));
			return;
		}
	}
	// test eax, 1001
	if (reg1 == AX) {
		byte instr[4];
		instr[0] = 0x66;
		instr[1] = 0xa9;
		*((uint16 *)&instr[2]) = imm;
		jitcEmit(instr, sizeof(instr));
	} else {
		byte instr[5];
		instr[0] = 0x66;
		instr[1] = 0xf7;
		instr[2] = 0xc0+reg1;
		*((uint16 *)&instr[3]) = imm;
		jitcEmit(instr, sizeof(instr));
	}
}

static void FASTCALL asmSimpleALU(X86ALUopc opc, NativeReg reg1, NativeReg reg2)
{
	byte instr[2] = {0x03+(opc<<3), 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

static void FASTCALL asmSimpleALU(X86ALUopc opc, NativeReg16 reg1, NativeReg16 reg2)
{
	byte instr[3] = {0x66, 0x03+(opc<<3), 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

static void FASTCALL asmSimpleALU(X86ALUopc opc, NativeReg8 reg1, NativeReg8 reg2)
{
	byte instr[2] = {0x02+(opc<<3), 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}


void FASTCALL asmALU(X86ALUopc opc, NativeReg reg1, NativeReg reg2)
{
	switch (opc) {
	case X86_MOV: 
		asmSimpleMODRM(0x8b, reg1, reg2);
	        break;
	case X86_TEST:
		asmSimpleMODRM(0x85, reg1, reg2);
	        break;
	case X86_XCHG:
		if (reg1 == EAX) {
			jitcEmit1(0x90+reg2);
		} else if (reg2 == EAX) {
			jitcEmit1(0x90+reg1);
		} else {
			asmSimpleMODRM(0x87, reg1, reg2);
		}
	        break;
	default:
		asmSimpleALU(opc, reg1, reg2);
	}	
}
void FASTCALL asmALURegReg(X86ALUopc opc, NativeReg reg1, NativeReg reg2)
{
	asmALU(opc, reg1, reg2);
}

void FASTCALL asmALU(X86ALUopc opc, NativeReg16 reg1, NativeReg16 reg2)
{
	switch (opc) {
	case X86_MOV: 
		asmSimpleMODRM(0x8b, reg1, reg2);
	        break;
	case X86_TEST:
		asmSimpleMODRM(0x85, reg1, reg2);
	        break;
	case X86_XCHG:
		if (reg1 == AX) {
			byte instr[2] = { 0x66, 0x90+reg2 };
			jitcEmit(instr, sizeof instr);
		} else if (reg2 == AX) {
			byte instr[2] = { 0x66, 0x90+reg1 };
			jitcEmit(instr, sizeof instr);
		} else {
			asmSimpleMODRM(0x87, reg1, reg2);
		}
	        break;
	default:
		asmSimpleALU(opc, reg1, reg2);
	}	
}
void FASTCALL asmALURegReg16(X86ALUopc opc, NativeReg reg1, NativeReg reg2)
{
	asmALU(opc, (NativeReg16)reg1, (NativeReg16)reg2);
}

void FASTCALL asmALU(X86ALUopc opc, NativeReg8 reg1, NativeReg8 reg2)
{
	switch (opc) {
	case X86_MOV: 
		asmSimpleMODRM(0x8a, reg1, reg2);
	        break;
	case X86_TEST:
		asmSimpleMODRM(0x84, reg1, reg2);
	        break;
	case X86_XCHG:
		asmSimpleMODRM(0x86, reg1, reg2);
	        break;
	default:
		asmSimpleALU(opc, reg1, reg2);
	}	
}
void FASTCALL asmALURegReg8(X86ALUopc opc, NativeReg8 reg1, NativeReg8 reg2)
{
	asmALU(opc, reg1, reg2);
}

void FASTCALL asmALU(X86ALUopc opc, NativeReg8 reg1, uint8 imm)
{
	byte instr[5];	
	switch (opc) {
	case X86_MOV:
		instr[0] = 0xb0 + reg1;
		instr[1] = imm;
		jitcEmit(instr, 2);
		break;
	case X86_TEST:
		if (reg1 == AL) {
			instr[0] = 0xa8;
			instr[1] = imm;
			jitcEmit(instr, 2);		
		} else {
			instr[0] = 0xf6;
			instr[1] = 0xc0 + reg1;
			instr[2] = imm;
			jitcEmit(instr, 3);		
		}
		break;	
	case X86_XCHG:
		// internal error
		break;
	default: {
		if (reg1 == AL) {
			instr[0] = (opc<<3)|0x4;
			instr[1] = imm;
			jitcEmit(instr, 2);			
		} else {
			instr[0] = 0x80;
			instr[1] = 0xc0+(opc<<3)+reg1;
			instr[2] = imm;
			jitcEmit(instr, 3);
		}
		break;
	}
	}
}
void FASTCALL asmALURegImm8(X86ALUopc opc, NativeReg8 reg1, uint8 imm)
{
	asmALU(opc, reg1, imm);
}

static void FASTCALL asmSimpleALU(X86ALUopc opc, NativeReg reg1, uint32 imm)
{
	if (imm <= 0x7f || imm >= 0xffffff80) {
		byte instr[3] = {0x83, 0xc0+(opc<<3)+reg1, imm};
		jitcEmit(instr, sizeof(instr));
	} else {
		if (reg1 == EAX) {
			byte instr[5];
			instr[0] = 0x05+(opc<<3);
			*((uint32 *)&instr[1]) = imm;
			jitcEmit(instr, sizeof(instr));
		} else {
			byte instr[6];
			instr[0] = 0x81;
			instr[1] = 0xc0+(opc<<3)+reg1;
			*((uint32 *)&instr[2]) = imm;
			jitcEmit(instr, sizeof(instr));
		}
	}
}

static void FASTCALL asmSimpleALU(X86ALUopc opc, NativeReg16 reg1, uint32 imm)
{
	if (imm <= 0x7f || imm >= 0xffffff80) {
		byte instr[3] = {0x83, 0xc0+(opc<<3)+reg1, imm};
		jitcEmit(instr, sizeof(instr));
	} else {
		if (reg1 == AX) {
			byte instr[4];
			instr[0] = 0x66;
			instr[1] = 0x05+(opc<<3);
			*((uint16 *)&instr[2]) = imm;
			jitcEmit(instr, sizeof(instr));
		} else {
			byte instr[5];
			instr[0] = 0x66;
			instr[1] = 0x81;
			instr[2] = 0xc0+(opc<<3)+reg1;
			*((uint16 *)&instr[3]) = imm;
			jitcEmit(instr, sizeof(instr));
		}
	}
}

void FASTCALL asmALU(X86ALUopc opc, NativeReg reg1, uint32 imm)
{
	switch (opc) {
	case X86_MOV:
		if (imm == 0) {
			asmALU(X86_XOR, reg1, reg1);
		} else {
			asmMOV_NoFlags(reg1, imm);
		}
		break;
	case X86_XCHG:
		// internal error
		break;
	case X86_TEST:
		asmTEST_D(reg1, imm);
		break;
	case X86_CMP:
//		if (imm == 0) {
//			asmALU(X86_OR, reg1, reg1);
//		} else {
			asmSimpleALU(opc, reg1, imm);
//		}
		break;
	default:
		asmSimpleALU(opc, reg1, imm);
	}
}
void FASTCALL asmALURegImm(X86ALUopc opc, NativeReg reg1, uint32 imm)
{
	asmALU(opc, reg1, imm);
}

void FASTCALL asmALU(X86ALUopc opc, NativeReg16 reg1, uint16 imm)
{
	switch (opc) {
	case X86_MOV:
		if (imm == 0) {
			asmALU(X86_XOR, reg1, reg1);
		} else {
			asmMOV_NoFlags(reg1, imm);
		}
		break;
	case X86_XCHG:
		// internal error
		break;
	case X86_TEST:
		asmTEST_W(reg1, imm);
		break;
	case X86_CMP:
//		if (imm == 0) {
//			asmALU(X86_OR, reg1, reg1);
//		} else {
			asmSimpleALU(opc, reg1, imm);
//		}
		break;
	default:
		asmSimpleALU(opc, reg1, imm);
	}
}
void FASTCALL asmALURegImm16(X86ALUopc opc, NativeReg reg1, uint32 imm)
{
	asmALU(opc, (NativeReg16)reg1, imm);
}

void FASTCALL asmMOV_NoFlags(NativeReg reg1, uint32 imm)
{
	byte instr[5];
	instr[0] = 0xb8+reg1;
	*((uint32 *)&instr[1]) = imm;
	jitcEmit(instr, sizeof(instr));
}
void FASTCALL asmMOVRegImm_NoFlags(NativeReg reg1, uint32 imm)
{
	asmMOV_NoFlags(reg1, imm);
}

void FASTCALL asmMOV_NoFlags(NativeReg16 reg1, uint16 imm)
{
	byte instr[4];
	instr[0] = 0x66;
	instr[1] = 0xb8+reg1;
	*((uint16 *)&instr[2]) = imm;
	jitcEmit(instr, sizeof(instr));
}
void FASTCALL asmMOVRegImm16_NoFlags(NativeReg reg1, uint16 imm)
{
	asmMOV_NoFlags((NativeReg16)reg1, imm);
}

void FASTCALL asmALU(X86ALUopc1 opc, NativeReg reg1)
{
	byte instr[2];
	switch (opc) {
	case X86_NOT:
		instr[0] = 0xf7;
		instr[1] = 0xd0+reg1;
		break;
	case X86_NEG:
		instr[0] = 0xf7;
		instr[1] = 0xd8+reg1;
		break;
	case X86_MUL:
		instr[0] = 0xf7;
		instr[1] = 0xe0+reg1;
		break;
	case X86_IMUL:
		instr[0] = 0xf7;
		instr[1] = 0xe8+reg1;
		break;
	case X86_DIV:
		instr[0] = 0xf7;
		instr[1] = 0xf0+reg1;
		break;
	case X86_IDIV:
		instr[0] = 0xf7;
		instr[1] = 0xf8+reg1;
		break;
	}
	jitcEmit(instr, 2);
}
void FASTCALL asmALUReg(X86ALUopc1 opc, NativeReg reg1)
{
	asmALU(opc, reg1);
}

void FASTCALL asmALU(X86ALUopc1 opc, NativeReg16 reg1)
{
	byte instr[3];
	instr[0] = 0x66;

	switch (opc) {
	case X86_NOT:
		instr[1] = 0xf7;
		instr[2] = 0xd0+reg1;
		break;
	case X86_NEG:
		instr[1] = 0xf7;
		instr[2] = 0xd8+reg1;
		break;
	case X86_MUL:
		instr[1] = 0xf7;
		instr[2] = 0xe0+reg1;
		break;
	case X86_IMUL:
		instr[1] = 0xf7;
		instr[2] = 0xe8+reg1;
		break;
	case X86_DIV:
		instr[1] = 0xf7;
		instr[2] = 0xf0+reg1;
		break;
	case X86_IDIV:
		instr[1] = 0xf7;
		instr[2] = 0xf8+reg1;
		break;
	}
	jitcEmit(instr, 3);
}
void FASTCALL asmALUReg16(X86ALUopc1 opc, NativeReg reg1)
{
	asmALU(opc, (NativeReg16)reg1);
}

void FASTCALL asmALUMemReg(X86ALUopc opc, byte *modrm, int len, NativeReg reg2)
{
	byte instr[15];

	switch (opc) {
	case X86_MOV:
		instr[0] = 0x89;
		break;
	case X86_XCHG:
		instr[0] = 0x87;
		break;
	case X86_TEST:
		instr[0] = 0x85;
		break;
	default:
		instr[0] = 0x01+(opc<<3);
	}
	memcpy(&instr[1], modrm, len);
	instr[1] |= (reg2<<3);
	jitcEmit(instr, len+1);
}
void FASTCALL asmALU(X86ALUopc opc, modrm_p modrm, NativeReg reg2)
{
	byte instr[15];
	int len = modrm++[0];

	switch (opc) {
	case X86_MOV:
		instr[0] = 0x89;
		break;
	case X86_XCHG:
		instr[0] = 0x87;
		break;
	case X86_TEST:
		instr[0] = 0x85;
		break;
	default:
		instr[0] = 0x01+(opc<<3);
	}
	memcpy(&instr[1], modrm, len);
	instr[1] |= (reg2<<3);
	jitcEmit(instr, len+1);
}

void FASTCALL asmALUMemReg16(X86ALUopc opc, byte *modrm, int len, NativeReg reg2)
{
	byte instr[16];

	instr[0] = 0x66;
	switch (opc) {
	case X86_MOV:
		instr[1] = 0x89;
		break;
	case X86_XCHG:
		instr[1] = 0x87;
		break;
	case X86_TEST:
		instr[1] = 0x85;
		break;
	default:
		instr[1] = 0x01+(opc<<3);
	}
	memcpy(&instr[2], modrm, len);
	instr[2] |= (reg2<<3);
	jitcEmit(instr, len+2);
}
void FASTCALL asmALU(X86ALUopc opc, modrm_p modrm, NativeReg16 reg2)
{
	byte instr[16];
	int len = modrm++[0];

	instr[0] = 0x66;
	switch (opc) {
	case X86_MOV:
		instr[1] = 0x89;
		break;
	case X86_XCHG:
		instr[1] = 0x87;
		break;
	case X86_TEST:
		instr[1] = 0x85;
		break;
	default:
		instr[1] = 0x01+(opc<<3);
	}
	memcpy(&instr[2], modrm, len);
	instr[2] |= (reg2<<3);
	jitcEmit(instr, len+2);
}


static void FASTCALL asmSimpleALU_D(X86ALUopc opc, byte *modrm, int len, uint32 imm)
{
	byte instr[15];

	if (imm <= 0x7f || imm >= 0xffffff80) {
		instr[0] = 0x83;
		memcpy(&instr[1], modrm, len);
		instr[1] |= (opc<<3);
		instr[len+1] = imm;
		jitcEmit(instr, len+2);
	} else {
		instr[0] = 0x81;
		memcpy(&instr[1], modrm, len);
		instr[1] |= (opc<<3);
		*((uint32 *)&instr[len+1]) = imm;
		jitcEmit(instr, len+5);
	}
}

static void FASTCALL asmSimpleALU_W(X86ALUopc opc, byte *modrm, int len, uint16 imm)
{
	byte instr[16];

	instr[0] = 0x66;

	if (imm <= 0x7f || imm >= 0xff80) {
		instr[0] = 0x83;
		memcpy(&instr[1], modrm, len);
		instr[1] |= (opc<<3);
		instr[len+1] = imm;
		jitcEmit(instr, len+2);
	} else {
		instr[1] = 0x81;
		memcpy(&instr[2], modrm, len);
		instr[2] |= (opc<<3);
		*((uint16 *)&instr[len+2]) = imm;
		jitcEmit(instr, len+4);
	}
}

void FASTCALL asmALUMemImm(X86ALUopc opc, byte *modrm, int len, uint32 imm)
{
	byte instr[15];
	switch (opc) {
	case X86_MOV: {
		instr[0] = 0xc7;
		memcpy(&instr[1], modrm, len);
		*((uint32 *)&instr[len+1]) = imm;
		jitcEmit(instr, len+5);
		break;
	}
	case X86_XCHG:
		// internal error
		break;
	case X86_TEST:
		instr[0] = 0xf7;
		memcpy(&instr[1], modrm, len);
		*((uint32 *)&instr[len+1]) = imm;
		jitcEmit(instr, len+5);
		break;
	default:
		asmSimpleALU_D(opc, modrm, len, imm);
	}
}

void FASTCALL asmALU_D(X86ALUopc opc, modrm_p modrm, uint32 imm)
{
	int len = modrm++[0];

	asmALUMemImm(opc, modrm, len, imm);
}

void FASTCALL asmALUMemImm16(X86ALUopc opc, byte *modrm, int len, uint16 imm)
{
	byte instr[16];
	instr[0] = 0x66;

	switch (opc) {
	case X86_MOV: {
		instr[1] = 0xc7;
		memcpy(&instr[2], modrm, len);
		*((uint16 *)&instr[len+2]) = imm;
		jitcEmit(instr, len+4);
		break;
	}
	case X86_XCHG:
		// internal error
		break;
	case X86_TEST:
		instr[1] = 0xf7;
		memcpy(&instr[2], modrm, len);
		*((uint16 *)&instr[len+2]) = imm;
		jitcEmit(instr, len+4);
		break;
	default:
		asmSimpleALU_W(opc, modrm, len, imm);
	}
}
void FASTCALL asmALU_W(X86ALUopc opc, modrm_p modrm, uint16 imm)
{
	int len = modrm++[0];

	asmALUMemImm16(opc, modrm, len, imm);
}

void FASTCALL asmALURegMem(X86ALUopc opc, NativeReg reg1, byte *modrm, int len)
{
	byte instr[15];
	switch (opc) {
	case X86_MOV:
		instr[0] = 0x8b;
		break;
	case X86_XCHG:
		// XCHG is symmetric
		instr[0] = 0x87;
		break;
	case X86_TEST:
		// TEST is symmetric
		instr[0] = 0x85;
		break;
	default:
		instr[0] = 0x03+(opc<<3);
	}
	memcpy(&instr[1], modrm, len);
	instr[1] |= (reg1<<3);
	jitcEmit(instr, len+1);
}
void FASTCALL asmALU(X86ALUopc opc, NativeReg reg1, modrm_p modrm)
{
	int len = modrm++[0];

	asmALURegMem(opc, reg1, modrm, len);
}

void FASTCALL asmALURegMem16(X86ALUopc opc, NativeReg reg1, byte *modrm, int len)
{
	byte instr[16];
	instr[0] = 0x66;
	switch (opc) {
	case X86_MOV:
		instr[1] = 0x8b;
		break;
	case X86_XCHG:
		// XCHG is symmetric
		instr[1] = 0x87;
		break;
	case X86_TEST:
		// TEST is symmetric
		instr[1] = 0x85;
		break;
	default:
		instr[1] = 0x03+(opc<<3);
	}
	memcpy(&instr[2], modrm, len);
	instr[2] |= (reg1<<3);
	jitcEmit(instr, len+2);
}
void FASTCALL asmALU(X86ALUopc opc, NativeReg16 reg1, modrm_p modrm)
{
	int len = modrm++[0];

	asmALURegMem16(opc, (NativeReg)reg1, modrm, len);
}

void FASTCALL asmALURegMem8(X86ALUopc opc, NativeReg8 reg1, byte *modrm, int len)
{
	byte instr[15];
	switch (opc) {
	case X86_MOV:
		instr[0] = 0x8a;
		break;
	case X86_XCHG:
		// XCHG is symmetric
		instr[0] = 0x86;
		break;
	case X86_TEST:
		// TEST is symmetric
		instr[0] = 0x84;
		break;
	default:
		instr[0] = 0x02+(opc<<3);
	}
	memcpy(&instr[1], modrm, len);
	instr[1] |= (reg1<<3);
	jitcEmit(instr, len+1);
}
void FASTCALL asmALU(X86ALUopc opc, NativeReg8 reg1, modrm_p modrm)
{
	int len = modrm++[0];

	asmALURegMem8(opc, reg1, modrm, len);
}

void FASTCALL asmALUMemReg8(X86ALUopc opc, byte *modrm, int len, NativeReg8 reg2)
{
	byte instr[15];
	switch (opc) {
	case X86_MOV:
		instr[0] = 0x88;
		break;
	case X86_XCHG:
		instr[0] = 0x86;
		break;
	case X86_TEST:
		instr[0] = 0x84;
		break;
	default:
		instr[0] = 0x00+(opc<<3);
	}
	memcpy(&instr[1], modrm, len);
	instr[1] |= (reg2<<3);
	jitcEmit(instr, len+1);
}
void FASTCALL asmALU(X86ALUopc opc, modrm_p modrm, NativeReg8 reg2)
{
	int len = modrm++[0];

	asmALUMemReg8(opc, modrm, len, reg2);
}

void FASTCALL asmALUMemImm8(X86ALUopc opc, byte *modrm, int len, uint8 imm)
{
	byte instr[15];
	switch (opc) {
	case X86_MOV:
		instr[0] = 0xc6;
		break;
	case X86_XCHG:
		// internal error
		break;
	case X86_TEST:
		instr[0] = 0xf6;
		break;
	default:
		instr[0] = 0x80;
		memcpy(&instr[1], modrm, len);
		instr[1] |= (opc<<3);
		instr[len+1] = imm;
		jitcEmit(instr, len+2);
		return;
	}
	memcpy(&instr[1], modrm, len);
	instr[len+1] = imm;
	jitcEmit(instr, len+2);
}
void FASTCALL asmALU_B(X86ALUopc opc, modrm_p modrm, uint8 imm)
{
	int len = modrm++[0];

	asmALUMemImm8(opc, modrm, len, imm);
}

void FASTCALL asmMOV(const void *disp, NativeReg reg1)
{
	byte instr[6];
	if (reg1==EAX) {
		instr[0] = 0xa3;
		*((uint32 *)&instr[1]) = (uint32)disp;
		jitcEmit(instr, 5);
	} else {
		instr[0] = 0x89;
		instr[1] = 0x05 | (reg1 << 3);
		*((uint32 *)&instr[2]) = (uint32)disp;
		jitcEmit(instr, 6);
	}
}
void FASTCALL asmMOVDMemReg(uint32 disp, NativeReg reg1)
{
	asmMOV((const void *)disp, reg1);
}

void FASTCALL asmMOV(const void *disp, NativeReg16 reg1)
{
	byte instr[7];
	instr[0] = 0x66;
	if (reg1==AX) {
		instr[1] = 0xa3;
		*((uint32 *)&instr[2]) = (uint32)disp;
		jitcEmit(instr, 6);
	} else {
		instr[1] = 0x89;
		instr[2] = 0x05 | (reg1 << 3);
		*((uint32 *)&instr[3]) = (uint32)disp;
		jitcEmit(instr, 7);
	}
}
void FASTCALL asmMOVDMemReg16(uint32 disp, NativeReg reg1)
{
	asmMOV((const void *)disp, (NativeReg16)reg1);
}

void FASTCALL asmMOV(NativeReg reg1, const void *disp)
{
	byte instr[6];
	if (reg1==EAX) {
		instr[0] = 0xa1;
		*((uint32 *)&instr[1]) = (uint32)disp;
		jitcEmit(instr, 5);
	} else {
		instr[0] = 0x8b;
		instr[1] = 0x05 | (reg1 << 3);
		*((uint32 *)&instr[2]) = (uint32)disp;
		jitcEmit(instr, 6);
	}
}
void FASTCALL asmMOVRegDMem(NativeReg reg1, uint32 disp)
{
	asmMOV(reg1, (const void *)disp);
}

void FASTCALL asmMOV(NativeReg16 reg1, const void *disp)
{
	byte instr[7];
	instr[0] = 0x66;
	if (reg1==AX) {
		instr[1] = 0xa1;
		*((uint32 *)&instr[2]) = (uint32)disp;
		jitcEmit(instr, 6);
	} else {
		instr[1] = 0x8b;
		instr[2] = 0x05 | (reg1 << 3);
		*((uint32 *)&instr[3]) = (uint32)disp;
		jitcEmit(instr, 7);
	}
}
void FASTCALL asmMOVRegDMem16(NativeReg reg1, uint32 disp)
{
	asmMOV((NativeReg16)reg1, (const void *)disp);
}

void FASTCALL asmTEST(const void *disp, uint32 imm)
{
	byte instr[15];
	instr[1] = 0x05;
	if (!(imm & 0xffffff00)) {
		instr[0] = 0xf6;
		*((uint32 *)&instr[2]) = (uint32)disp;
		instr[6] = imm;
	} else if (!(imm & 0xffff00ff)) {
		instr[0] = 0xf6;
		*((uint32 *)&instr[2]) = (uint32)disp+1;
		instr[6] = imm >> 8;
	} else if (!(imm & 0xff00ffff)) {
		instr[0] = 0xf6;
		*((uint32 *)&instr[2]) = (uint32)disp+2;
		instr[6] = imm >> 16;
	} else if (!(imm & 0x00ffffff)) {
		instr[0] = 0xf6;
		*((uint32 *)&instr[2]) = (uint32)disp+3;
		instr[6] = imm >> 24;
	} else {
		instr[0] = 0xf7;
		*((uint32 *)&instr[2]) = (uint32)disp;
		*((uint32 *)&instr[6]) = imm;
		jitcEmit(instr, 10);
		return;
	}
	jitcEmit(instr, 7);
}
void FASTCALL asmTESTDMemImm(uint32 disp, uint32 imm)
{
	asmTEST((const void *)disp, imm);
}

void FASTCALL asmAND(const void *disp, uint32 imm)
{
	byte instr[15];
	instr[1] = 0x25;
	if ((imm & 0xffffff00)==0xffffff00) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = (uint32)disp;
		instr[6] = imm;
	} else if ((imm & 0xffff00ff)==0xffff00ff) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = (uint32)disp+1;
		instr[6] = imm >> 8;
	} else if ((imm & 0xff00ffff)==0xff00ffff) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = (uint32)disp+2;
		instr[6] = imm >> 16;
	} else if ((imm & 0x00ffffff)==0x00ffffff) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = (uint32)disp+3;
		instr[6] = imm >> 24;
	} else {
		instr[0] = 0x81;
		*((uint32 *)&instr[2]) = (uint32)disp;
		*((uint32 *)&instr[6]) = imm;
		jitcEmit(instr, 10);
		return;
	}
	jitcEmit(instr, 7);
}
void FASTCALL asmANDDMemImm(uint32 disp, uint32 imm)
{
	asmAND((const void *)disp, imm);
}

void FASTCALL asmOR(const void *disp, uint32 imm)
{
	byte instr[15];
	instr[1] = 0x0d;
	if (!(imm & 0xffffff00)) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = (uint32)disp;
		instr[6] = imm;
	} else if (!(imm & 0xffff00ff)) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = (uint32)disp+1;
		instr[6] = imm >> 8;
	} else if (!(imm & 0xff00ffff)) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = (uint32)disp+2;
		instr[6] = imm >> 16;
	} else if (!(imm & 0x00ffffff)) {
		instr[0] = 0x80;
		*((uint32 *)&instr[2]) = (uint32)disp+3;
		instr[6] = imm >> 24;
	} else {
		instr[0] = 0x81;
		*((uint32 *)&instr[2]) = (uint32)disp;
		*((uint32 *)&instr[6]) = imm;
		jitcEmit(instr, 10);
		return;
	}
	jitcEmit(instr, 7);
}
void FASTCALL asmORDMemImm(uint32 disp, uint32 imm)
{
	asmOR((const void *)disp, imm);
}


void FASTCALL asmMOVxx(X86MOVxx opc, NativeReg reg1, NativeReg8 reg2)
{
	byte instr[3] = {0x0f, opc, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}
void FASTCALL asmMOVxxRegReg8(X86MOVxx opc, NativeReg reg1, NativeReg8 reg2)
{
	asmMOVxx(opc, reg1, reg2);
}

void FASTCALL asmMOVxx(X86MOVxx opc, NativeReg reg1, NativeReg16 reg2)
{
	byte instr[3] = {0x0f, opc+1, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}
void FASTCALL asmMOVxxRegReg16(X86MOVxx opc, NativeReg reg1, NativeReg reg2)
{
	asmMOVxx(opc, reg1, (NativeReg16)reg2);
}

void FASTCALL asmMOVxxRegMem8(X86MOVxx opc, NativeReg reg1, byte *modrm, int len)
{
	byte instr[16] = { 0x0f };

	instr[1] = opc;
	memcpy(&instr[2], modrm, len);
	instr[2] |= (reg1 << 3);

	jitcEmit(instr, len+2);
}
void FASTCALL asmMOVxx_B(X86MOVxx opc, NativeReg reg1, modrm_p modrm)
{
	int len = modrm++[0];

	asmMOVxxRegMem8(opc, reg1, modrm, len);
}

void FASTCALL asmMOVxxRegMem16(X86MOVxx opc, NativeReg reg1, byte *modrm, int len)
{
	byte instr[16] = { 0x0f };

	instr[1] = opc+1;
	memcpy(&instr[2], modrm, len);
	instr[2] |= (reg1 << 3);

	jitcEmit(instr, len+2);
}
void FASTCALL asmMOVxx_W(X86MOVxx opc, NativeReg reg1, modrm_p modrm)
{
	int len = modrm++[0];

	asmMOVxxRegMem16(opc, reg1, modrm, len);
}

void FASTCALL asmSET(X86FlagTest flags, NativeReg8 reg1)
{
	byte instr[3] = {0x0f, 0x90+flags, 0xc0+reg1};
	jitcEmit(instr, sizeof(instr));
}
void FASTCALL asmSETReg8(X86FlagTest flags, NativeReg8 reg1)
{
	asmSET(flags, reg1);
}

void FASTCALL asmSETMem(X86FlagTest flags, byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0x0f;
	instr[1] = 0x90+flags;
	memcpy(instr+2, modrm, len);
	jitcEmit(instr, len+2);
}
void FASTCALL asmSET(X86FlagTest flags, modrm_p modrm)
{
	int len = modrm++[0];

	asmSETMem(flags, modrm, len);
}

void FASTCALL asmCMOV(X86FlagTest flags, NativeReg reg1, NativeReg reg2)
{
	if (gJITC.hostCPUCaps.cmov) {
		byte instr[3] = {0x0f, 0x40+flags, 0xc0+(reg1<<3)+reg2};
		jitcEmit(instr, sizeof(instr));
	} else {
		byte instr[4] = {
			0x70+(flags ^ 1), 0x02, 	// jnCC $+2
			0x8b, 0xc0+(reg1<<3)+reg2,	// mov	reg1, reg2
		};
		jitcEmit(instr, sizeof instr);
	}
}
void FASTCALL asmCMOVRegReg(X86FlagTest flags, NativeReg reg1, NativeReg reg2)
{
	asmCMOV(flags, reg1, reg2);
}

void FASTCALL asmCMOVRegMem(X86FlagTest flags, NativeReg reg1, byte *modrm, int len)
{
	if (gJITC.hostCPUCaps.cmov) {
		byte instr[16] = {0x0f, 0x40+flags };
		memcpy(&instr[2], modrm, len);
		instr[2] |= (reg1<<3);
		jitcEmit(instr, len+2);
	} else {
		byte instr[17] = {
			0x70+(flags ^ 1), 1 + len, 	// jnCC $+2
			0x8b, 				// mov	reg1, *
		};
		memcpy(&instr[3], modrm, len);
		instr[3] |= (reg1<<3);
		jitcEmit(instr, len+3);
	}
}
void FASTCALL asmCMOV(X86FlagTest flags, NativeReg reg1, modrm_p modrm)
{
	int len = modrm++[0];

	asmCMOVRegMem(flags, reg1, modrm, len);
}

void FASTCALL asmShift(X86ShiftOpc opc, NativeReg reg1, uint32 imm)
{
	if (imm == 1) {
		byte instr[2] = {0xd1, 0xc0+opc+reg1};
		jitcEmit(instr, sizeof(instr));
	} else {
		byte instr[3] = {0xc1, 0xc0+opc+reg1, imm};
		jitcEmit(instr, sizeof(instr));
	}
}
void FASTCALL asmShiftRegImm(X86ShiftOpc opc, NativeReg reg1, uint32 imm)
{
	asmShift(opc, reg1, imm);
}

void FASTCALL asmShift_CL(X86ShiftOpc opc, NativeReg reg1)
{
	// 0xd3 [ModR/M]
	byte instr[2] = {0xd3, 0xc0+opc+reg1};
	jitcEmit(instr, sizeof(instr));
}
void FASTCALL asmShiftRegCL(X86ShiftOpc opc, NativeReg reg1)
{
	asmShift_CL(opc, reg1);
}

void FASTCALL asmShift(X86ShiftOpc opc, NativeReg16 reg1, uint32 imm)
{
	if (imm == 1) {
		byte instr[3] = {0x66, 0xd1, 0xc0+opc+reg1};
		jitcEmit(instr, sizeof(instr));
	} else {
		byte instr[4] = {0x66, 0xc1, 0xc0+opc+reg1, imm};
		jitcEmit(instr, sizeof(instr));
	}
}
void FASTCALL asmShiftReg16Imm(X86ShiftOpc opc, NativeReg reg1, uint32 imm)
{
	asmShift(opc, (NativeReg16)reg1, imm);
}

void FASTCALL asmShift_CL(X86ShiftOpc opc, NativeReg16 reg1)
{
	// 0xd3 [ModR/M]
	byte instr[3] = {0x66, 0xd3, 0xc0+opc+reg1};
	jitcEmit(instr, sizeof(instr));
}
void FASTCALL asmShiftReg16CL(X86ShiftOpc opc, NativeReg reg1)
{
	asmShift_CL(opc, (NativeReg16)reg1);
}

void FASTCALL asmShift(X86ShiftOpc opc, NativeReg8 reg1, uint32 imm)
{
	if (imm == 1) {
		byte instr[2] = {0xd0, 0xc0+opc+reg1};
		jitcEmit(instr, sizeof(instr));
	} else {
		byte instr[3] = {0xc0, 0xc0+opc+reg1, imm};
		jitcEmit(instr, sizeof(instr));
	}
}
void FASTCALL asmShiftReg8Imm(X86ShiftOpc opc, NativeReg8 reg1, uint32 imm)
{
	asmShift(opc, reg1, imm);
}

void FASTCALL asmShift_CL(X86ShiftOpc opc, NativeReg8 reg1)
{
	// 0xd3 [ModR/M]
	byte instr[2] = {0xd2, 0xc0+opc+reg1};
	jitcEmit(instr, sizeof(instr));
}
void FASTCALL asmShiftReg8CL(X86ShiftOpc opc, NativeReg8 reg1)
{
	asmShift_CL(opc, reg1);
}

void FASTCALL asmIMUL(NativeReg reg1, NativeReg reg2, uint32 imm)
{
	if (imm <= 0x7f || imm >= 0xffffff80) {	
		byte instr[3] = {0x6b, 0xc0+(reg1<<3)+reg2, imm};
		jitcEmit(instr, sizeof(instr));
	} else {
		byte instr[6] = {0x69, 0xc0+(reg1<<3)+reg2};
		*((uint32*)(&instr[2])) = imm;
		jitcEmit(instr, sizeof(instr));
	}
}

void FASTCALL asmIMUL(NativeReg reg1, NativeReg reg2)
{
	byte instr[3] = {0x0f, 0xaf, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}

void FASTCALL asmIMULRegRegImm(NativeReg reg1, NativeReg reg2, uint32 imm)
{
	asmIMUL(reg1, reg2, imm);
}

void FASTCALL asmIMULRegReg(NativeReg reg1, NativeReg reg2)
{
	asmIMUL(reg1, reg2);
}

void FASTCALL asmINC(NativeReg reg1)
{
	jitcEmit1(0x40+reg1);
}
void FASTCALL asmINCReg(NativeReg reg1)
{
	asmINC(reg1);
}

void FASTCALL asmDECReg(NativeReg reg1)
{
	jitcEmit1(0x48+reg1);
}
void FASTCALL asmDEC(NativeReg reg1)
{
	asmDEC(reg1);
}

void FASTCALL asmLEA(NativeReg reg1, byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0x8d;
	memcpy(instr+1, modrm, len);
	instr[1] |= reg1<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmLEA(NativeReg reg1, modrm_p modrm)
{
	int len = modrm++[0];

	asmLEA(reg1, modrm, len);
}

void FASTCALL asmBTx(X86BitTest opc, NativeReg reg1, int value)
{
	byte instr[4] = {0x0f, 0xba, 0xc0+(opc<<3)+reg1, value};
	jitcEmit(instr, sizeof instr);
}
void FASTCALL asmBTxRegImm(X86BitTest opc, NativeReg reg1, int value)
{
	asmBTx(opc, reg1, value);
}

void FASTCALL asmBTxMemImm(X86BitTest opc, byte *modrm, int len, int value)
{
	byte instr[15];
	instr[0] = 0x0f;
	instr[1] = 0xba;
	memcpy(instr+2, modrm, len);
	instr[2] |= opc<<3;
	instr[len+2] = value;
	jitcEmit(instr, len+3);
}
void FASTCALL asmBTx(X86BitTest opc, modrm_p modrm, int value)
{
	int len = modrm++[0];

	asmBTxMemImm(opc, modrm, len, value);
}

void FASTCALL asmBSx(X86BitSearch opc, NativeReg reg1, NativeReg reg2)
{
	byte instr[3] = {0x0f, opc, 0xc0+(reg1<<3)+reg2};
	jitcEmit(instr, sizeof(instr));
}
void FASTCALL asmBSxRegReg(X86BitSearch opc, NativeReg reg1, NativeReg reg2)
{
	asmBSx(opc, reg1, reg2);
}

void FASTCALL asmBSWAP(NativeReg reg)
{
	byte instr[2];
	instr[0] = 0x0f;
	instr[1] = 0xc8+reg;
	jitcEmit(instr, sizeof(instr));	
}

void FASTCALL asmJMP(NativeAddress to)
{
	/*
	 *	We use jitcEmitAssure here, since
	 *	we have to know the exact address of the jump
	 *	instruction (since it is relative)
	 */
restart:
	byte instr[5];
	uint32 rel = (uint32)(to - (gJITC.currentPage->tcp+2));
	if (rel <= 0x7f || rel >= 0xffffff80) {	
		if (!jitcEmitAssure(2)) goto restart;
		instr[0] = 0xeb;
		instr[1] = rel;
		jitcEmit(instr, 2);
	} else {
		if (!jitcEmitAssure(5)) goto restart;
		instr[0] = 0xe9;
		*((uint32 *)&instr[1]) = (uint32)(to - (gJITC.currentPage->tcp+5));
//		*((uint32 *)&instr[1]) = rel - 3;
		jitcEmit(instr, 5);
	}
}

void FASTCALL asmJxx(X86FlagTest flags, NativeAddress to)
{
restart:
	byte instr[6];
	uint32 rel = (uint32)(to - (gJITC.currentPage->tcp+2));
	if (rel <= 0x7f || rel >= 0xffffff80) {
		if (!jitcEmitAssure(2)) goto restart;
		instr[0] = 0x70+flags;
		instr[1] = rel;
		jitcEmit(instr, 2);
	} else {
		if (!jitcEmitAssure(6)) goto restart;
		instr[0] = 0x0f;
		instr[1] = 0x80+flags;
		*((uint32 *)&instr[2]) = (uint32)(to - (gJITC.currentPage->tcp+6));
//		*((uint32 *)&instr[2]) = rel - 3;
		jitcEmit(instr, 6);
	}
}

NativeAddress FASTCALL asmJMPFixup()
{
	byte instr[5];
	instr[0] = 0xe9;
	jitcEmit(instr, 5);
	return gJITC.currentPage->tcp - 4;
}

NativeAddress FASTCALL asmJxxFixup(X86FlagTest flags)
{
	byte instr[6];
	instr[0] = 0x0f;
	instr[1] = 0x80+flags;
	jitcEmit(instr, 6);	
	return gJITC.currentPage->tcp - 4;
}

void FASTCALL asmResolveFixup(NativeAddress at, NativeAddress to)
{
	/*
	 *	yes, I also didn't believe this could be real code until
	 *	I had written it.	-Sebastian
	 */
	if (to == 0) {
		to = gJITC.currentPage->tcp;
	}
	*((uint32 *)at) = (uint32)(to - ((uint32)at+4));
}

void FASTCALL asmCALL(NativeAddress to)
{
	jitcEmitAssure(5);
	byte instr[5];
	instr[0] = 0xe8;
	*((uint32 *)&instr[1]) = (uint32)(to - (gJITC.currentPage->tcp+5));
	jitcEmit(instr, 5);
}

void FASTCALL asmSimple(X86SimpleOpc simple)
{
	if (simple > 0xff) {
		jitcEmit((byte*)&simple, 2);
	} else {
		jitcEmit1(simple);
	}
}

void FASTCALL asmFComp(X86FloatCompOp op, NativeFloatReg sti)
{
	byte instr[2];

	memcpy(instr, &op, 2);
	instr[1] += sti;

	jitcEmit(instr, 2);
}
void FASTCALL asmFCompSTi(X86FloatCompOp op, NativeFloatReg sti)
{
	asmFComp(op, sti);
}

void FASTCALL asmFICompMem(X86FloatICompOp op, byte *modrm, int len)
{
	byte instr[16];

	instr[0] = op;
	memcpy(&instr[1], modrm, len);
	instr[1] |= 2<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmFIComp(X86FloatICompOp op, modrm_p modrm)
{
	int len = modrm++[0];

	asmFICompMem(op, modrm, len);
}

void FASTCALL asmFICompPMem(X86FloatICompOp op, byte *modrm, int len)
{
	byte instr[16];

	instr[0] = op;
	memcpy(&instr[1], modrm, len);
	instr[1] |= 3<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmFICompP(X86FloatICompOp op, modrm_p modrm)
{
	int len = modrm++[0];

	asmFICompPMem(op, modrm, len);
}

void FASTCALL asmFArithMem(X86FloatArithOp op, byte *modrm, int len)
{
	int mod = 0;
	switch (op) {
	case X86_FADD:
		mod = 0;
		break;
	case X86_FMUL:
		mod = 1;
		break;
	case X86_FDIV:
		mod = 6;
		break;
	case X86_FDIVR:
		mod = 7;
		break;
	case X86_FSUB:
		mod = 4;
		break;
	case X86_FSUBR:
		mod = 5;
		break;
	}
	byte instr[15];
	instr[0] = 0xdc;
	memcpy(instr+1, modrm, len);
	instr[1] |= mod<<3;
	jitcEmit(instr, len+1);	
}
void FASTCALL asmFArith(X86FloatArithOp op, modrm_p modrm)
{
	int len = modrm++[0];

	asmFArithMem(op, modrm, len);
}

void FASTCALL asmFArith_ST0(X86FloatArithOp op, NativeFloatReg sti)
{
	byte instr[2] = {0xd8, op+sti};
	jitcEmit(instr, sizeof instr);
}
void FASTCALL asmFArithST0(X86FloatArithOp op, NativeFloatReg sti)
{
	asmFArith_ST0(op, sti);
}

void FASTCALL asmFArith_STi(X86FloatArithOp op, NativeFloatReg sti)
{
	byte instr[2] = {0xdc, op+sti};
	jitcEmit(instr, sizeof instr);
}
void FASTCALL asmFArithSTi(X86FloatArithOp op, NativeFloatReg sti)
{
	asmFArith_STi(op, sti);
}

void FASTCALL asmFArithP_STi(X86FloatArithOp op, NativeFloatReg sti)
{
	byte instr[2] = {0xde, op+sti};
	jitcEmit(instr, sizeof instr);
}
void FASTCALL asmFArithSTiP(X86FloatArithOp op, NativeFloatReg sti)
{
	asmFArithP_STi(op, sti);
}

void FASTCALL asmFXCH(NativeFloatReg sti)
{
	byte instr[2] = {0xd9, 0xc8+sti};
	jitcEmit(instr, sizeof instr);
}
void FASTCALL asmFXCHSTi(NativeFloatReg sti)
{
	asmFXCH(sti);
}

void FASTCALL asmFFREE(NativeFloatReg sti)
{
	byte instr[2] = {0xdd, 0xc0+sti};
	jitcEmit(instr, sizeof instr);
}
void FASTCALL asmFFREESTi(NativeFloatReg sti)
{
	asmFFREE(sti);
}

void FASTCALL asmFFREEP(NativeFloatReg sti)
{
	/* 
	 * AMD says:
	 * "Note that the FREEP instructions, although insufficiently 
	 * documented in the past, is supported by all 32-bit x86 processors."
	 */
	byte instr[2] = {0xdf, 0xc0+sti};
	jitcEmit(instr, sizeof instr);
}
void FASTCALL asmFFREEPSTi(NativeFloatReg sti)
{
	asmFFREEP(sti);
}

void FASTCALL asmFSimple(X86FloatOp op)
{
	jitcEmit((byte*)&op, 2);
}
void FASTCALL asmFSimpleST0(X86FloatOp op)
{
	asmFSimple(op);
}

void FASTCALL asmFLDSingleMem(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xd9;
	memcpy(instr+1, modrm, len);
	jitcEmit(instr, len+1);
}
void FASTCALL asmFLD_Single(modrm_p modrm)
{
	int len = modrm++[0];

	asmFLDSingleMem(modrm, len);
}

void FASTCALL asmFLDDoubleMem(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xdd;
	memcpy(instr+1, modrm, len);
	jitcEmit(instr, len+1);
}
void FASTCALL asmFLD_Double(modrm_p modrm)
{
	int len = modrm++[0];

	asmFLDDoubleMem(modrm, len);
}

void FASTCALL asmFLD(NativeFloatReg sti)
{
	byte instr[2] = {0xd9, 0xc0+sti};
	jitcEmit(instr, sizeof instr);
}
void FASTCALL asmFLDSTi(NativeFloatReg sti)
{
	asmFLD(sti);
}

void FASTCALL asmFILD16(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xdf;
	memcpy(instr+1, modrm, len);
	jitcEmit(instr, len+1);
}
void FASTCALL asmFILD_W(modrm_p modrm)
{
	int len = modrm++[0];

	asmFILD16(modrm, len);
}

void FASTCALL asmFILD(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xdb;
	memcpy(instr+1, modrm, len);
	jitcEmit(instr, len+1);
}
void FASTCALL asmFILD_D(modrm_p modrm)
{
	int len = modrm++[0];

	asmFILD(modrm, len);
}

void FASTCALL asmFILD_Q(modrm_p modrm)
{
	byte instr[15];
	instr[0] = 0xdf;
	memcpy(instr+1, modrm+1, modrm[0]);
	instr[1] |= 5<<3;
	jitcEmit(instr, modrm[0]+1);
}

void FASTCALL asmFSTSingleMem(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xd9;
	memcpy(instr+1, modrm, len);
	instr[1] |= 2<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmFST_Single(modrm_p modrm)
{
	int len = modrm++[0];

	asmFSTSingleMem(modrm, len);
}

void FASTCALL asmFSTPSingleMem(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xd9;
	memcpy(instr+1, modrm, len);
	instr[1] |= 3<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmFSTP_Single(modrm_p modrm)
{
	int len = modrm++[0];

	asmFSTPSingleMem(modrm, len);
}

void FASTCALL asmFSTDoubleMem(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xdd;
	memcpy(instr+1, modrm, len);
	instr[1] |= 2<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmFST_Double(modrm_p modrm)
{
	int len = modrm++[0];

	asmFSTDoubleMem(modrm, len);
}

void FASTCALL asmFSTPDoubleMem(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xdd;
	memcpy(instr+1, modrm, len);
	instr[1] |= 3<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmFSTP_Double(modrm_p modrm)
{
	int len = modrm++[0];

	asmFSTPDoubleMem(modrm, len);
}

void FASTCALL asmFST(NativeFloatReg sti)
{
	byte instr[2] = {0xdd, 0xd0+sti};
	jitcEmit(instr, sizeof instr);
}
void FASTCALL asmFSTDSTi(NativeFloatReg sti)
{
	asmFST(sti);
}

void FASTCALL asmFSTP(NativeFloatReg sti)
{
	byte instr[2] = {0xdd, 0xd8+sti};
	jitcEmit(instr, sizeof instr);
}
void FASTCALL asmFSTDPSTi(NativeFloatReg sti)
{
	asmFSTP(sti);
}

void FASTCALL asmFISTP_W(modrm_p modrm)
{
	byte instr[15];
	instr[0] = 0xdf;
	memcpy(instr+1, modrm+1, modrm[0]);
	instr[1] |= 3<<3;
	jitcEmit(instr, modrm[0]+1);
}

void FASTCALL asmFISTPMem(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xdb;
	memcpy(instr+1, modrm, len);
	instr[1] |= 3<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmFISTP_D(modrm_p modrm)
{
	int len = modrm++[0];

	asmFISTPMem(modrm, len);
}

void FASTCALL asmFISTPMem64(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xdf;
	memcpy(instr+1, modrm, len);
	instr[1] |= 7<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmFISTP_Q(modrm_p modrm)
{
	int len = modrm++[0];

	asmFISTPMem64(modrm, len);
}

void FASTCALL asmFISTTPMem(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xdb;
	memcpy(instr+1, modrm, len);
	instr[1] |= 1<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmFISTTP(modrm_p modrm)
{
	int len = modrm++[0];

	asmFISTTPMem(modrm, len);
}

void FASTCALL asmFLDCWMem(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xd9;
	memcpy(instr+1, modrm, len);
	instr[1] |= 5<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmFLDCW(modrm_p modrm)
{
	int len = modrm++[0];

	asmFLDCWMem(modrm, len);
}

void FASTCALL asmFSTCWMem(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xd9;
	memcpy(instr+1, modrm, len);
	instr[1] |= 7<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmFSTCW(modrm_p modrm)
{
	int len = modrm++[0];

	asmFSTCWMem(modrm, len);
}

void FASTCALL asmFSTSWMem(byte *modrm, int len)
{
	byte instr[15];
	instr[0] = 0xdd;
	memcpy(instr+1, modrm, len);
	instr[1] |= 7<<3;
	jitcEmit(instr, len+1);
}
void FASTCALL asmFSTSW(modrm_p modrm)
{
	int len = modrm++[0];

	asmFSTSWMem(modrm, len);
}

void FASTCALL asmFSTSW_EAX(void)
{
	byte instr[15] = { 0xdf, 0xe0 };
	jitcEmit(instr, 2);
}

/**
 *	Maps one client vector register to one native vector register
 *	Will never emit any code.
 */
static inline void FASTCALL jitcMapVectorRegister(NativeVectorReg nreg, JitcVectorReg creg)
{
	//printf("*** map: XMM%u (vr%u)\n", nreg, creg);
	gJITC.n2cVectorReg[nreg] = creg;
	gJITC.c2nVectorReg[creg] = nreg;

	gJITC.nativeVectorRegState[nreg] = rsMapped;
}

/**
 *	Unmaps the native vector register from any client vector register
 *	Will never emit any code.
 */
static inline void FASTCALL jitcUnmapVectorRegister(NativeVectorReg nreg)
{
	JitcVectorReg creg = gJITC.n2cVectorReg[nreg];

	if (nreg != VECTREG_NO && creg != PPC_VECTREG_NO) {
		//printf("*** unmap: XMM%u (vr%u)\n", nreg, gJITC.n2cVectorReg[nreg]);

		gJITC.n2cVectorReg[nreg] = PPC_VECTREG_NO;
		gJITC.c2nVectorReg[creg] = VECTREG_NO;

		gJITC.nativeVectorRegState[nreg] = rsUnused;
	}
}

/**
 *	Marks the native vector register as dirty.
 *	Does *not* touch native vector register.
 *	Will not produce code.
 */
void FASTCALL jitcDirtyVectorRegister(NativeVectorReg nreg)
{
	JitcVectorReg creg = gJITC.n2cVectorReg[nreg];

	//printf("*** dirty(%u) with creg = %u\n", nreg, creg);

	if (creg == JITC_VECTOR_NEG1 || creg == PPC_VECTREG_NO) {
		//printf("*** dirty: %u = %u or %u\n", creg, JITC_VECTOR_NEG1, PPC_REG_NO);
		return;
	}

	if (gJITC.nativeVectorRegState[nreg] == rsUnused) {
		printf("!!! Attemped dirty of an anonymous vector register!\n");
		return;
	}

	if (creg == gJITC.nativeVectorReg) {
		gJITC.nativeVectorReg = VECTREG_NO;
	}

	gJITC.nativeVectorRegState[nreg] = rsDirty;
}

/**
 *	Marks the native vector register as non-dirty.
 *	Does *not* flush native vector register.
 *	Will not produce code.
 */
static inline void FASTCALL jitcUndirtyVectorRegister(NativeVectorReg nreg)
{
	if (gJITC.nativeVectorRegState[nreg] > rsMapped) {
		//printf("*** undirty: XMM%u (vr%u)\n", nreg, gJITC.n2cVectorReg[nreg]);

		gJITC.nativeVectorRegState[nreg] = rsMapped;
	}
}

/**
 *	Loads a native vector register with its mapped value.
 *	Does not alter the native vector register's markings.
 *	Will always emit an load.
 */
static inline void FASTCALL jitcLoadVectorRegister(NativeVectorReg nreg)
{
	JitcVectorReg creg = gJITC.n2cVectorReg[nreg];

	if (creg == JITC_VECTOR_NEG1 && gJITC.hostCPUCaps.sse2) {
		//printf("*** load neg1: XMM%u\n", nreg);

		/* On a P4, we can load -1 far faster with logic */
		asmPALU(PALUD(X86_PCMPEQ), nreg, nreg);
		return;
	}

	//printf("*** load: XMM%u (vr%u)\n", nreg, creg);
	asmMOVAPS(nreg, &gCPU.vr[creg]);
}

/**
 *	Stores a native vector register to its mapped client vector register.
 *	Does not alter the native vector register's markings.
 *	Will always emit a store.
 */
static inline void FASTCALL jitcStoreVectorRegister(NativeVectorReg nreg)
{
	JitcVectorReg creg = gJITC.n2cVectorReg[nreg];

	if (creg == JITC_VECTOR_NEG1 || creg == PPC_VECTREG_NO)
		return;

	//printf("*** store: XMM%u (vr%u)\n", nreg, creg);

	asmMOVAPS(&gCPU.vr[creg], nreg);
}

/**
 *	Returns the native vector register that is mapped to the client
 *		vector register.
 *	Will never emit any code.
 */
NativeVectorReg FASTCALL jitcGetClientVectorRegisterMapping(JitcVectorReg creg)
{
	return gJITC.c2nVectorReg[creg];
}

/**
 *	Makes the vector register the least recently used vector register.
 *	Will never emit any code.
 */
static inline void FASTCALL jitcDiscardVectorRegister(NativeVectorReg nreg)
{
	NativeVectorReg lreg, mreg;

	mreg = gJITC.MRUvregs[nreg];
	lreg = gJITC.LRUvregs[nreg];

	// remove from the list
	gJITC.MRUvregs[lreg] = mreg;
	gJITC.LRUvregs[mreg] = lreg;

	mreg = gJITC.MRUvregs[XMM_SENTINEL];

	// insert into the list in the LRU spot
	gJITC.LRUvregs[nreg] = XMM_SENTINEL;
	gJITC.MRUvregs[nreg] = mreg;

	gJITC.LRUvregs[mreg] = nreg;
	gJITC.MRUvregs[XMM_SENTINEL] = nreg;
}

/**
 *	Makes the vector register the most recently used vector register.
 *	Will never emit any code.
 */
void FASTCALL jitcTouchVectorRegister(NativeVectorReg nreg)
{
	NativeVectorReg lreg, mreg;

	mreg = gJITC.MRUvregs[nreg];
	lreg = gJITC.LRUvregs[nreg];

	// remove from the list
	gJITC.MRUvregs[lreg] = mreg;
	gJITC.LRUvregs[mreg] = lreg;

	lreg = gJITC.LRUvregs[XMM_SENTINEL];

	// insert into the list in the LRU spot
	gJITC.MRUvregs[nreg] = XMM_SENTINEL;
	gJITC.LRUvregs[nreg] = lreg;

	gJITC.MRUvregs[lreg] = nreg;
	gJITC.LRUvregs[XMM_SENTINEL] = nreg;
}

/**
 *	Unmaps a native vector register, and marks it least recently used.
 *	Will not emit any code.
 */
void FASTCALL jitcDropSingleVectorRegister(NativeVectorReg nreg)
{
	jitcDiscardVectorRegister(nreg);
	jitcUnmapVectorRegister(nreg);
}

int FASTCALL jitcAssertFlushedVectorRegister(JitcVectorReg creg)
{
	NativeVectorReg nreg = gJITC.c2nVectorReg[creg];

	if (nreg != VECTREG_NO && gJITC.nativeVectorRegState[nreg] == rsDirty) {
		printf("!!! Unflushed vector XMM%u (vr%u)!\n", nreg, creg);
		return 1;
	}
	return 0;
}
int FASTCALL jitcAssertFlushedVectorRegisters()
{
	int ret = 0;

	for (JitcVectorReg i=0; i<32; i++)
		ret |= jitcAssertFlushedVectorRegister(i);

	return ret;
}

void FASTCALL jitcShowVectorRegisterStatus(JitcVectorReg creg)
{
	NativeVectorReg nreg = gJITC.c2nVectorReg[creg];

	if (nreg != VECTREG_NO) {
		int status = gJITC.nativeVectorRegState[nreg];
		char *text;

		if (status == rsUnused)
			text = "unused";
		else if (status == rsMapped)
			text = "mapped";
		else if (status == rsDirty)
			text = "dirty";
		else
			text = "unknown";

		//printf("*** vr%u => XMM%u (%s)\n", creg, nreg, text);
	} else {
		//printf("*** vr%u => memory\n", creg);
	}
}

/**
 *	If the native vector register is marked dirty, then it writes that
 *		value out to the client vector register store.
 *	Will produce a store, if the native vector register is dirty.
 */
static inline void FASTCALL jitcFlushSingleVectorRegister(NativeVectorReg nreg)
{
	if (gJITC.nativeVectorRegState[nreg] == rsDirty) {
		//printf("*** flush: XMM%u (vr%u)\n", nreg, gJITC.n2cVectorReg[nreg]);
		jitcStoreVectorRegister(nreg);
	}
}

/**
 *	Flushes the register, frees it, and makes it least recently used.
 *	Will produce a store, if the native vector register was dirty.
 */
static inline void FASTCALL jitcTrashSingleVectorRegister(NativeVectorReg nreg)
{
	if (gJITC.nativeVectorRegState[nreg] > rsUnused) {
		//printf("*** trash: XMM%u (vr%u)\n", nreg, gJITC.n2cVectorReg[nreg]);
	}

	jitcFlushSingleVectorRegister(nreg);
	jitcDropSingleVectorRegister(nreg);
}

/**
 *	Flushes the register, frees it, and makes it most recently used.
 *	Will produce a store, if the native vector register was dirty.
 */
static inline void FASTCALL jitcClobberSingleVectorRegister(NativeVectorReg nreg)
{
	if (gJITC.nativeVectorRegState[nreg] > rsUnused) {
		//printf("*** clobber: XMM%u (vr%u)\n", nreg, gJITC.n2cVectorReg[nreg]);
	}

	jitcFlushSingleVectorRegister(nreg);
	jitcTouchVectorRegister(nreg);
	jitcUnmapVectorRegister(nreg);
}

/**
 *	Allocates a native vector register.
 *	If hint is non-zero, then it indicates that the value is unlikely
 *		to be re-used soon, so to keep it at the end of the LRU.
 *	To use hints, pass hint == the number of temporary registers
 *	May produce a store, if no native vector registers are available.
 */
NativeVectorReg FASTCALL jitcAllocVectorRegister(int hint)
{
	NativeVectorReg nreg = gJITC.MRUvregs[XMM_SENTINEL];

	if (hint >= XMM_SENTINEL) {
		nreg = gJITC.LRUvregs[nreg];

		jitcTrashSingleVectorRegister(nreg);
	} else if (hint) {
		for (int i=1; i<hint; i++) {
			nreg = gJITC.MRUvregs[nreg];
		}

		jitcTrashSingleVectorRegister(nreg);
	} else {
		jitcClobberSingleVectorRegister(nreg);
	}

	return nreg;
}

/**
 *	Returns native vector register that contains value of client
 *		register or allocates new vector register which maps to
 *		the client register.
 *	Marks the register dirty.
 *
 *	May produce a store, if no registers are available.
 *	Will never produce a load.
 */
NativeVectorReg FASTCALL jitcMapClientVectorRegisterDirty(JitcVectorReg creg, int hint)
{
	NativeVectorReg nreg = gJITC.c2nVectorReg[creg];

	if (nreg == VECTREG_NO) {
		nreg = jitcAllocVectorRegister(hint);

		jitcMapVectorRegister(nreg, creg);
	} else if (hint) {
		jitcDiscardVectorRegister(nreg);
	} else {
		jitcTouchVectorRegister(nreg);
	}

	jitcDirtyVectorRegister(nreg);

	return nreg;
}

/**
 *	Returns native vector register that contains the value of the
 *		client vector register, or allocates new register, and
 *		loads this value into it.
 *
 *	May produce a store, if no register are available.
 *	May produce a load, if client vector register isn't mapped.
 */
NativeVectorReg FASTCALL jitcGetClientVectorRegister(JitcVectorReg creg, int hint)
{
	NativeVectorReg nreg = gJITC.c2nVectorReg[creg];

	if (nreg == VECTREG_NO) {
		nreg = jitcAllocVectorRegister(hint);
		jitcMapVectorRegister(nreg, creg);

		jitcLoadVectorRegister(nreg);
	} else if (hint) {
		jitcDiscardVectorRegister(nreg);
	} else {
		jitcTouchVectorRegister(nreg);
	}

	return nreg;
}

/**
 *	Returns native vector register that contains the value of the
 *		client vector register, or allocates new register, and
 *		loads this value into it.
 *	Will mark the native vector register as dirty.
 *
 *	May produce a store, if no register are available.
 *	May produce a load, if client vector register isn't mapped.
 */
NativeVectorReg FASTCALL jitcGetClientVectorRegisterDirty(JitcVectorReg creg, int hint)
{
	NativeVectorReg nreg = jitcGetClientVectorRegister(creg, hint);

	jitcDirtyVectorRegister(nreg);

	return nreg;
}

/**
 *	Flushes native vector register(s).
 *	Resets dirty flags.
 *	Will produce stores, if vector registers are dirty.
 */
void FASTCALL jitcFlushVectorRegister(int options)
{
	if (options == JITC_VECTOR_REGS_ALL) {
		for (unsigned int i = XMM0; i <= XMM7; i++) {
			jitcFlushSingleVectorRegister((NativeVectorReg)i);
			jitcUndirtyVectorRegister((NativeVectorReg)i);
		}
	} else if (options & NATIVE_REG) {
		NativeVectorReg nreg = (NativeVectorReg)(options & 0xf);

		jitcFlushSingleVectorRegister(nreg);
		jitcUndirtyVectorRegister(nreg);
	}
}

/**
 *	Flushes native vector register(s).
 *	Doesn't reset dirty flags.
 *	Will produce stores, if vector registers are dirty.
 */
void FASTCALL jitcFlushVectorRegisterDirty(int options)
{
	if (options == JITC_VECTOR_REGS_ALL) {
		for (unsigned int i = XMM0; i <= XMM7; i++) {
			jitcFlushSingleVectorRegister((NativeVectorReg)i);
		}
	} else if (options & NATIVE_REG) {
		NativeVectorReg nreg = (NativeVectorReg)(options & 0xf);

		jitcFlushSingleVectorRegister(nreg);
	}
}

/**
 *	Clobbers native vector register(s).
 *	Will produce stores, if vector registers are dirty.
 */
void FASTCALL jitcClobberVectorRegister(int options)
{
	if (options == JITC_VECTOR_REGS_ALL) {
		for (unsigned int i = XMM0; i <= XMM7; i++) {
			jitcClobberSingleVectorRegister((NativeVectorReg)i);
		}
	} else if (options & NATIVE_REG) {
		NativeVectorReg nreg = (NativeVectorReg)(options & 0xf);

		jitcClobberSingleVectorRegister(nreg);
	}
}

/**
 *	Trashes native vector register(s).
 *	Will produce stores, if vector registers are dirty.
 */
void FASTCALL jitcTrashVectorRegister(int options)
{
	if (options == JITC_VECTOR_REGS_ALL) {
		for (unsigned int i = XMM0; i <= XMM7; i++) {
			jitcTrashSingleVectorRegister((NativeVectorReg)i);
		}
	} else if (options & NATIVE_REG) {
		NativeVectorReg nreg = (NativeVectorReg)(options & 0xf);

		jitcTrashSingleVectorRegister(nreg);
	}
}

/**
 *	Drops native vector register(s).
 *	Will not produce any code.
 */
void FASTCALL jitcDropVectorRegister(int options)
{
	if (options == JITC_VECTOR_REGS_ALL) {
		for (unsigned int i = XMM0; i <= XMM7; i++) {
			jitcDropSingleVectorRegister((NativeVectorReg)i);
		}
	} else if (options & NATIVE_REG) {
		NativeVectorReg nreg = (NativeVectorReg)(options & 0xf);

		jitcDropSingleVectorRegister(nreg);
	}
}

void FASTCALL jitcFlushClientVectorRegister(JitcVectorReg creg)
{
	NativeVectorReg nreg = gJITC.c2nVectorReg[creg];

	if (nreg != VECTREG_NO) {
		jitcFlushSingleVectorRegister(nreg);
		jitcUndirtyVectorRegister(nreg);
	}
}

void FASTCALL jitcTrashClientVectorRegister(JitcVectorReg creg)
{
	NativeVectorReg nreg = gJITC.c2nVectorReg[creg];

	if (nreg != VECTREG_NO) {
		jitcTrashSingleVectorRegister(nreg);
	}
}

void FASTCALL jitcClobberClientVectorRegister(JitcVectorReg creg)
{
	NativeVectorReg nreg = gJITC.c2nVectorReg[creg];

	if (nreg != VECTREG_NO) {
		jitcClobberSingleVectorRegister(nreg);
	}
}

void FASTCALL jitcDropClientVectorRegister(JitcVectorReg creg)
{
	NativeVectorReg nreg = gJITC.c2nVectorReg[creg];

	if (nreg != VECTREG_NO) {
		jitcDropSingleVectorRegister(nreg);
	}
}

/**
 *	Renames a native vector register to a different client register.
 *	Will not emit a load.
 *	May emit a reg->reg move, if the vector register was in memory.
 *	May emit a store, if the vector register was dirty
 */
NativeVectorReg FASTCALL jitcRenameVectorRegisterDirty(NativeVectorReg reg, JitcVectorReg creg, int hint)
{
	NativeVectorReg nreg = gJITC.c2nVectorReg[creg];

	if (nreg == reg) {
		/*	That's weird... it's already mapped...	*/
	} else if (nreg != VECTREG_NO) {
		/*	It's already in a register, so rather than losing
		 *	reg pool depth, just move the value.
		 */
		asmALUPS(X86_MOVAPS, nreg, reg);
	} else {
		/*	Otherwise, only the source register is in the reg
		 *	pool, so flush it, then remap it.
	 	*/
		JitcVectorReg reg2 = gJITC.n2cVectorReg[reg];

		if (reg2 != VECTREG_NO) {
			jitcFlushSingleVectorRegister(reg);
			jitcUnmapVectorRegister(reg);
		}

		nreg = reg;
		jitcMapVectorRegister(nreg, creg);
	}

	if (hint) jitcDiscardVectorRegister(nreg);
	else      jitcTouchVectorRegister(nreg);

	jitcDirtyVectorRegister(nreg);

	return nreg;
}

void asmMOVAPS(NativeVectorReg reg, const void *disp)
{
	byte instr[8] = { 0x0f, 0x28 };

	instr[2] = 0x05 | (reg << 3);
	*((uint32 *)&instr[3]) = (uint32)disp;

	jitcEmit(instr, 7);
}

void asmMOVAPS(const void *disp, NativeVectorReg reg)
{
	byte instr[8] = { 0x0f, 0x29 };

	instr[2] = 0x05 | (reg << 3);
	*((uint32 *)&instr[3]) = (uint32)disp;

	jitcEmit(instr, 7);
}

void asmMOVUPS(NativeVectorReg reg, const void *disp)
{
	byte instr[8] = { 0x0f, 0x10 };

	instr[2] = 0x05 | (reg << 3);
	*((uint32 *)&instr[3]) = (uint32)disp;

	jitcEmit(instr, 7);
}

void asmMOVUPS(const void *disp, NativeVectorReg reg)
{
	byte instr[8] = { 0x0f, 0x11 };

	instr[2] = 0x05 | (reg << 3);
	*((uint32 *)&instr[3]) = (uint32)disp;

	jitcEmit(instr, 7);
}

void asmMOVSS(NativeVectorReg reg, const void *disp)
{
	byte instr[10] = { 0xf3, 0x0f, 0x10 };

	instr[3] = 0x05 | (reg << 3);
	*((uint32 *)&instr[4]) = (uint32)disp;

	jitcEmit(instr, 8);
}

void asmMOVSS(const void *disp, NativeVectorReg reg)
{
	byte instr[10] = { 0xf3, 0x0f, 0x11 };

	instr[3] = 0x05 | (reg << 3);
	*((uint32 *)&instr[4]) = (uint32)disp;
 
	jitcEmit(instr, 8);
}

void asmALUPS(X86ALUPSopc opc, NativeVectorReg reg1, NativeVectorReg reg2)
{
	byte instr[4] = { 0x0f };

	instr[1] = opc;
	instr[2] = 0xc0 + (reg1 << 3) + reg2;

	jitcEmit(instr, 3);
}

void asmALUPS(X86ALUPSopc opc, NativeVectorReg reg1, modrm_p modrm)
{
	byte instr[16] = { 0x0f };
	int len = modrm++[0];

	instr[1] = opc;
	memcpy(&instr[2], modrm, len);
	instr[2] |= (reg1 << 3);

	jitcEmit(instr, len+2);
}

void asmPALU(X86PALUopc opc, NativeVectorReg reg1, NativeVectorReg reg2)
{
	byte instr[5] = { 0x66, 0x0f };

	instr[2] = opc;
	instr[3] = 0xc0 + (reg1 << 3) + reg2;

	jitcEmit(instr, 4);
}

void asmPALU(X86PALUopc opc, NativeVectorReg reg1, modrm_p modrm)
{
	byte instr[5] = { 0x66, 0x0f };
	int len = modrm++[0];

	instr[2] = opc;
	memcpy(&instr[3], modrm, len);
	instr[3] |= (reg1 << 3);

	jitcEmit(instr, len+3);
}

void asmSHUFPS(NativeVectorReg reg1, NativeVectorReg reg2, int order)
{
	byte instr[5] = { 0x0f, 0xc6, 0xc0+(reg1<<3)+reg2, order };

	jitcEmit(instr, 4);
}

void asmSHUFPS(NativeVectorReg reg1, modrm_p modrm, int order)
{
	byte instr[16] = { 0x0f, 0xc6 };
	int len = modrm++[0];

	memcpy(&instr[2], modrm, len);
	instr[2] |= (reg1 << 3);
	instr[len+2] = order;

	jitcEmit(instr, len+3);
}

void asmPSHUFD(NativeVectorReg reg1, NativeVectorReg reg2, int order)
{
	byte instr[6] = { 0x66, 0x0f, 0x70, 0xc0+(reg1<<3)+reg2, order };

	jitcEmit(instr, 5);
}

void asmPSHUFD(NativeVectorReg reg1, modrm_p modrm, int order)
{
	byte instr[5] = { 0x66, 0x0f, 0x70 };
	int len = modrm++[0];

	memcpy(&instr[3], modrm, len);
	instr[3] |= (reg1 << 3);
	instr[len+3] = order;

	jitcEmit(instr, len+4);
}
