/*
 *	PearPC
 *	ppc_mmu.h
 *
 *	Copyright (C) 2003, 2004 Sebastian Biallas (sb@biallas.net)
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

#ifndef __PPC_MMU_H__
#define __PPC_MMU_H__

#include "system/types.h"

extern byte *gMemory;
extern uint32 gMemorySize;

#define PPC_MMU_READ  1
#define PPC_MMU_WRITE 2
#define PPC_MMU_CODE  4
#define PPC_MMU_SV    8
#define PPC_MMU_NO_EXC 16

#define PPC_MMU_OK 0
#define PPC_MMU_EXC 1
#define PPC_MMU_FATAL 2

int FASTCALL ppc_effective_to_physical(uint32 addr, int flags, uint32 &result);
int FASTCALL ppc_effective_to_physical_vm(uint32 addr, int flags, uint32 &result);
bool FASTCALL ppc_mmu_set_sdr1(uint32 newval, bool quiesce);
void ppc_mmu_tlb_invalidate();

int FASTCALL ppc_read_physical_dword(uint32 addr, uint64 &result);
int FASTCALL ppc_read_physical_word(uint32 addr, uint32 &result);
int FASTCALL ppc_read_physical_half(uint32 addr, uint16 &result);
int FASTCALL ppc_read_physical_byte(uint32 addr, uint8 &result);
 
int FASTCALL ppc_read_effective_code(uint32 addr, uint32 &result);
int FASTCALL ppc_read_effective_dword(uint32 addr, uint64 &result);
int FASTCALL ppc_read_effective_word(uint32 addr, uint32 &result);
int FASTCALL ppc_read_effective_half(uint32 addr, uint16 &result);
int FASTCALL ppc_read_effective_byte(uint32 addr, uint8 &result);

int FASTCALL ppc_write_physical_dword(uint32 addr, uint64 data);
int FASTCALL ppc_write_physical_word(uint32 addr, uint32 data);
int FASTCALL ppc_write_physical_half(uint32 addr, uint16 data);
int FASTCALL ppc_write_physical_byte(uint32 addr, uint8 data);

int FASTCALL ppc_write_effective_dword(uint32 addr, uint64 data);
int FASTCALL ppc_write_effective_word(uint32 addr, uint32 data);
int FASTCALL ppc_write_effective_half(uint32 addr, uint16 data);
int FASTCALL ppc_write_effective_byte(uint32 addr, uint8 data);

int FASTCALL ppc_direct_physical_memory_handle(uint32 addr, byte *&ptr);
int FASTCALL ppc_direct_effective_memory_handle(uint32 addr, byte *&ptr);
int FASTCALL ppc_direct_effective_memory_handle_code(uint32 addr, byte *&ptr);
bool FASTCALL ppc_mmu_page_create(uint32 ea, uint32 pa);
bool FASTCALL ppc_mmu_page_free(uint32 ea);
bool FASTCALL ppc_init_physical_memory(uint size);

/**
pte: (page table entry)
1st word:
0     V    Valid
1-24  VSID Virtual Segment ID
25    H    Hash function
26-31 API  Abbreviated page index
2nd word:
0-19  RPN  Physical page number
20-22 res
23    R    Referenced bit
24    C    Changed bit
25-28 WIMG Memory/cache control bits
29    res
30-31 PP   Page protection bits
*/

/*
 *	MMU Opcodes
 */
void ppc_opc_dcbz();

void ppc_opc_lbz();
void ppc_opc_lbzu();
void ppc_opc_lbzux();
void ppc_opc_lbzx();
void ppc_opc_lfd();
void ppc_opc_lfdu();
void ppc_opc_lfdux();
void ppc_opc_lfdx();
void ppc_opc_lfs();
void ppc_opc_lfsu();
void ppc_opc_lfsux();
void ppc_opc_lfsx();
void ppc_opc_lha();
void ppc_opc_lhau();
void ppc_opc_lhaux();
void ppc_opc_lhax();
void ppc_opc_lhbrx();
void ppc_opc_lhz();
void ppc_opc_lhzu();
void ppc_opc_lhzux();
void ppc_opc_lhzx();
void ppc_opc_lmw();
void ppc_opc_lswi();
void ppc_opc_lswx();
void ppc_opc_lwarx();
void ppc_opc_lwbrx();
void ppc_opc_lwz();
void ppc_opc_lwzu();
void ppc_opc_lwzux();
void ppc_opc_lwzx();
void ppc_opc_lvx();             /* for altivec support */
void ppc_opc_lvxl();
void ppc_opc_lvebx();
void ppc_opc_lvehx();
void ppc_opc_lvewx();
void ppc_opc_lvsl();
void ppc_opc_lvsr();
void ppc_opc_dst();

void ppc_opc_stb();
void ppc_opc_stbu();
void ppc_opc_stbux();
void ppc_opc_stbx();
void ppc_opc_stfd();
void ppc_opc_stfdu();
void ppc_opc_stfdux();
void ppc_opc_stfdx();
void ppc_opc_stfiwx();
void ppc_opc_stfs();
void ppc_opc_stfsu();
void ppc_opc_stfsux();
void ppc_opc_stfsx();
void ppc_opc_sth();
void ppc_opc_sthbrx();
void ppc_opc_sthu();
void ppc_opc_sthux();
void ppc_opc_sthx();
void ppc_opc_stmw();
void ppc_opc_stswi();
void ppc_opc_stswx();
void ppc_opc_stw();
void ppc_opc_stwbrx();
void ppc_opc_stwcx_();
void ppc_opc_stwu();
void ppc_opc_stwux();
void ppc_opc_stwx();
void ppc_opc_stvx();            /* for altivec support */
void ppc_opc_stvxl();
void ppc_opc_stvebx();
void ppc_opc_stvehx();
void ppc_opc_stvewx();
void ppc_opc_dstst();
void ppc_opc_dss();

#include "jitc_types.h"

JITCFlow ppc_opc_gen_dcbz();

JITCFlow ppc_opc_gen_lbz();
JITCFlow ppc_opc_gen_lbzu();
JITCFlow ppc_opc_gen_lbzux();
JITCFlow ppc_opc_gen_lbzx();
JITCFlow ppc_opc_gen_lfd();
JITCFlow ppc_opc_gen_lfdu();
JITCFlow ppc_opc_gen_lfdux();
JITCFlow ppc_opc_gen_lfdx();
JITCFlow ppc_opc_gen_lfs();
JITCFlow ppc_opc_gen_lfsu();
JITCFlow ppc_opc_gen_lfsux();
JITCFlow ppc_opc_gen_lfsx();
JITCFlow ppc_opc_gen_lha();
JITCFlow ppc_opc_gen_lhau();
JITCFlow ppc_opc_gen_lhaux();
JITCFlow ppc_opc_gen_lhax();
JITCFlow ppc_opc_gen_lhbrx();
JITCFlow ppc_opc_gen_lhz();
JITCFlow ppc_opc_gen_lhzu();
JITCFlow ppc_opc_gen_lhzux();
JITCFlow ppc_opc_gen_lhzx();
JITCFlow ppc_opc_gen_lmw();
JITCFlow ppc_opc_gen_lswi();
JITCFlow ppc_opc_gen_lswx();
JITCFlow ppc_opc_gen_lwarx();
JITCFlow ppc_opc_gen_lwbrx();
JITCFlow ppc_opc_gen_lwz();
JITCFlow ppc_opc_gen_lwzu();
JITCFlow ppc_opc_gen_lwzux();
JITCFlow ppc_opc_gen_lwzx();
JITCFlow ppc_opc_gen_lvx();             /* for altivec support */
JITCFlow ppc_opc_gen_lvxl();
JITCFlow ppc_opc_gen_lvebx();
JITCFlow ppc_opc_gen_lvehx();
JITCFlow ppc_opc_gen_lvewx();
JITCFlow ppc_opc_gen_lvsl();
JITCFlow ppc_opc_gen_lvsr();
JITCFlow ppc_opc_gen_dst();

JITCFlow ppc_opc_gen_stb();
JITCFlow ppc_opc_gen_stbu();
JITCFlow ppc_opc_gen_stbux();
JITCFlow ppc_opc_gen_stbx();
JITCFlow ppc_opc_gen_stfd();
JITCFlow ppc_opc_gen_stfdu();
JITCFlow ppc_opc_gen_stfdux();
JITCFlow ppc_opc_gen_stfdx();
JITCFlow ppc_opc_gen_stfiwx();
JITCFlow ppc_opc_gen_stfs();
JITCFlow ppc_opc_gen_stfsu();
JITCFlow ppc_opc_gen_stfsux();
JITCFlow ppc_opc_gen_stfsx();
JITCFlow ppc_opc_gen_sth();
JITCFlow ppc_opc_gen_sthbrx();
JITCFlow ppc_opc_gen_sthu();
JITCFlow ppc_opc_gen_sthux();
JITCFlow ppc_opc_gen_sthx();
JITCFlow ppc_opc_gen_stmw();
JITCFlow ppc_opc_gen_stswi();
JITCFlow ppc_opc_gen_stswx();
JITCFlow ppc_opc_gen_stw();
JITCFlow ppc_opc_gen_stwbrx();
JITCFlow ppc_opc_gen_stwcx_();
JITCFlow ppc_opc_gen_stwu();
JITCFlow ppc_opc_gen_stwux();
JITCFlow ppc_opc_gen_stwx();
JITCFlow ppc_opc_gen_stvx();            /* for altivec support */
JITCFlow ppc_opc_gen_stvxl();
JITCFlow ppc_opc_gen_stvebx();
JITCFlow ppc_opc_gen_stvehx();
JITCFlow ppc_opc_gen_stvewx();
JITCFlow ppc_opc_gen_dstst();
JITCFlow ppc_opc_gen_dss();

#endif

