/*
 * compiler/compemu_fpp.cpp - Dynamic translation of FPU instructions
 *
 * Copyright (c) 2001-2004 Milan Jurik of ARAnyM dev team (see AUTHORS)
 * 
 * Inspired by Christian Bauer's Basilisk II
 *
 * This file is part of the ARAnyM project which builds a new and powerful
 * TOS/FreeMiNT compatible virtual machine running on almost any hardware.
 *
 * JIT compiler m68k -> IA-32 and AMD64
 *
 * Original 68040 JIT compiler for UAE, copyright 2000-2002 Bernd Meyer
 * Adaptation for Basilisk II and improvements, copyright 2000-2004 Gwenole Beauchesne
 * Portions related to CPU detection come from linux/arch/i386/kernel/setup.c
 *
 * ARAnyM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * ARAnyM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ARAnyM; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
/*
 * UAE - The Un*x Amiga Emulator
 *
 * MC68881 emulation
 *
 * Copyright 1996 Herman ten Brugge
 * Adapted for JIT compilation (c) Bernd Meyer, 2000
 */

#include "sysdeps.h"

#include <cmath>
#include <cstdio>
#include <cassert>

#include "memory-uae.h"
#include "readcpu.h"
#include "newcpu.h"
#include "compemu.h"
//#include "fpu/fpu.h"
//#include "fpu/flags.h"
//#include "fpu/exceptions.h"
//#include "fpu/rounding.h"

#define DEBUG 0
#include "debug.h"

struct jit_disable_opcodes jit_disable;

#if defined(USE_LONG_DOUBLE) || defined(USE_QUAD_DOUBLE)
#define LD(x) x ## L
#else
#define LD(x) x
#endif

// gb-- WARNING: get_fpcr() and set_fpcr() support is experimental
#define HANDLE_FPCR 0

// - IEEE-based fpu core must be used
#if defined(FPU_IEEE)
# define CAN_HANDLE_FPCR
#endif

// - Generic rounding mode and precision modes are supported if set together
#if defined(FPU_USE_GENERIC_ROUNDING_MODE) && defined(FPU_USE_GENERIC_ROUNDING_PRECISION)
# define CAN_HANDLE_FPCR
#endif

// - X86 rounding mode and precision modes are *not* supported but might work (?!)
#if defined(FPU_USE_X86_ROUNDING_MODE) && defined(FPU_USE_X86_ROUNDING_PRECISION)
# define CAN_HANDLE_FPCR
#endif

#if HANDLE_FPCR && !defined(CAN_HANDLE_FPCR)
# warning "Can't handle FPCR, will FAIL(1) at runtime"
# undef HANDLE_FPCR
# define HANDLE_FPCR 0
#endif

//#define STATIC_INLINE static inline
#define MAKE_FPSR(r) do { fmov_rr(FP_RESULT,r); } while (0)

#if 0
#define delay   nop() ;nop()  
#define delay2  nop() ;nop()   
#else
#define delay
#define delay2
#endif

#define UNKNOWN_EXTRA 0xFFFFFFFF
#if 0
static void fpuop_illg(uae_u32 opcode, uae_u32 /* extra */)
{
/*
	if (extra == UNKNOWN_EXTRA)
		printf("FPU opcode %x, extra UNKNOWN_EXTRA\n",opcode & 0xFFFF);
	else
		printf("FPU opcode %x, extra %x\n",opcode & 0xFFFF,extra & 0xFFFF);
*/
	op_illg(opcode);
}   
#endif

uae_s32 temp_fp[4];  /* To convert between FP/integer */

/* return register number, or -1 for failure */
STATIC_INLINE int get_fp_value(uae_u32 opcode, uae_u16 extra)
{
	int size;
	int mode;
	int reg;
	uae_u32 ad = 0;
	static int const sz1[8] = { 4, 4, 12, 12, 2, 8, 1, 0 };
	static int const sz2[8] = { 4, 4, 12, 12, 2, 8, 2, 0 };

	if ((extra & 0x4000) == 0)
	{
		return ((extra >> 10) & 7);
	}

	mode = (opcode >> 3) & 7;
	reg = opcode & 7;
	size = (extra >> 10) & 7;
	switch (mode)
	{
	case 0: /* Dn */
		switch (size)
		{
		case 6: /* byte */
			sign_extend_8_rr(S1, reg);
			mov_l_mr(JITPTR temp_fp, S1);
			delay2;
			fmovi_rm(FS1, JITPTR temp_fp);
			return FS1;
		case 4: /* word */
			sign_extend_16_rr(S1, reg);
			mov_l_mr(JITPTR temp_fp, S1);
			delay2;
			fmovi_rm(FS1, JITPTR temp_fp);
			return FS1;
		case 0: /* long */
			mov_l_mr(JITPTR temp_fp, reg);
			delay2;
			fmovi_rm(FS1, JITPTR temp_fp);
			return FS1;
		case 1: /* single precision */
			mov_l_mr(JITPTR temp_fp, reg);
			delay2;
			fmovs_rm(FS1, JITPTR temp_fp);
			return FS1;
		default:
			return -1;
		}
		return -1;						/* Should be unreachable */
	case 1: /* An */
		return -1;						/* Genuine invalid instruction */
	default:
		break;
	}

	/* OK, we *will* have to load something from an address. Let's make
	   sure we know how to handle that, or quit early --- i.e. *before*
	   we do any postincrement/predecrement that we may regret */
	switch (size)
	{
	case 0: /* long */
	case 1: /* single precision */
	case 2: /* extended precision */
	case 4: /* word */
	case 5: /* double precision */
	case 6: /* byte */
		break;
	case 3: /* packed decimal static */
	default:
		return -1;
	}

	switch (mode)
	{
	case 2: /* (An) */
		ad = S1;						/* We will change it, anyway ;-) */
		mov_l_rr(ad, reg + 8);
		break;
	case 3: /* (An)+ */
		ad = S1;
		mov_l_rr(ad, reg + 8);
		lea_l_brr(reg + 8, reg + 8, (reg == 7 ? sz2[size] : sz1[size]));
		break;
	case 4: /* -(An) */
		ad = S1;
		lea_l_brr(reg + 8, reg + 8, -(reg == 7 ? sz2[size] : sz1[size]));
		mov_l_rr(ad, reg + 8);
		break;
	case 5: /* d16(An) */
		{
			uae_u32 off = (uae_s32) (uae_s16) comp_get_iword((m68k_pc_offset += 2) - 2);

			ad = S1;
			mov_l_rr(ad, reg + 8);
			lea_l_brr(ad, ad, off);
		}
		break;
	case 6: /* d8(An,Xn) */
		{
			uae_u32 dp = comp_get_iword((m68k_pc_offset += 2) - 2);

			ad = S1;
			calc_disp_ea_020(reg + 8, dp, ad, S2);
		}
		break;
	case 7:
		switch (reg)
		{
		case 0: /* abs.w */
			{
				uae_u32 off = (uae_s32) (uae_s16) comp_get_iword((m68k_pc_offset += 2) - 2);

				ad = S1;
				mov_l_ri(ad, off);
			}
			break;
		case 1: /* abs.l */
			{
				uae_u32 off = comp_get_ilong((m68k_pc_offset += 4) - 4);

				ad = S1;
				mov_l_ri(ad, off);
			}
			break;
		case 2: /* d16(pc) */
			{
				uae_u32 address = (uae_u32)(start_pc + ((char *) comp_pc_p - (char *) start_pc_p) + m68k_pc_offset);
				uae_s32 PC16off = (uae_s32) (uae_s16) comp_get_iword((m68k_pc_offset += 2) - 2);

				ad = S1;
				mov_l_ri(ad, address + PC16off);
			}
			break;
		case 3: /* d8(pc,Xn) */
			return -1;
		case 4: /* #imm */
			{
				uae_u32 address = (uae_u32)(start_pc + ((char *) comp_pc_p - (char *) start_pc_p) + m68k_pc_offset);

				ad = S1;
				// Immediate addressing mode && Operation Length == Byte -> 
				// Use the low-order byte of the extension word.
				if (size == 6)
					address++;
				mov_l_ri(ad, address);
				m68k_pc_offset += sz2[size];
			}
			break;
		default:
			return -1;
		}
	}

	switch (size)
	{
	case 0: /* long */
		readlong(ad, S2, S3);
		mov_l_mr(JITPTR temp_fp, S2);
		delay2;
		fmovi_rm(FS1, JITPTR temp_fp);
		break;
	case 1: /* single precision */
		readlong(ad, S2, S3);
		mov_l_mr(JITPTR temp_fp, S2);
		delay2;
		fmovs_rm(FS1, JITPTR temp_fp);
		break;
	case 2: /* extended precision */
		readword(ad, S2, S3);
		mov_w_mr((JITPTR temp_fp) + 8, S2);
		add_l_ri(ad, 4);
		readlong(ad, S2, S3);
		// always set the explicit integer bit.
		or_l_ri(S2, 0x80000000);
		mov_l_mr(JITPTR (temp_fp) + 4, S2);
		add_l_ri(ad, 4);
		readlong(ad, S2, S3);
		mov_l_mr(JITPTR (temp_fp), S2);
		delay2;
		fmov_ext_rm(FS1, JITPTR (temp_fp));
		break;
	case 3: /* packed decimal static */
		return -1;						/* Some silly "packed" stuff */
	case 4: /* word */
		readword(ad, S2, S3);
		sign_extend_16_rr(S2, S2);
		mov_l_mr(JITPTR temp_fp, S2);
		delay2;
		fmovi_rm(FS1, JITPTR temp_fp);
		break;
	case 5: /* double precision */
		readlong(ad, S2, S3);
		mov_l_mr((JITPTR temp_fp) + 4, S2);
		add_l_ri(ad, 4);
		readlong(ad, S2, S3);
		mov_l_mr(JITPTR (temp_fp), S2);
		delay2;
		fmov_rm(FS1, JITPTR (temp_fp));
		break;
	case 6: /* byte */
		readbyte(ad, S2, S3);
		sign_extend_8_rr(S2, S2);
		mov_l_mr(JITPTR temp_fp, S2);
		delay2;
		fmovi_rm(FS1, JITPTR temp_fp);
		break;
	default:
		return -1;
	}
	return FS1;
}

static struct {
	fpu_register b[2];
	fpu_register w[2];
	fpu_register l[2];
} clamp_bounds = {
	{ -128.0, 127.0 },
	{ -32768.0, 32767.0 },
	{ -2147483648.0, 2147483647.0 }
};

/* return of -1 means failure, >=0 means OK */
STATIC_INLINE int put_fp_value(int val, uae_u32 opcode, uae_u16 extra)
{
	int size;
	int mode;
	int reg;
	uae_u32 ad;
	static int const sz1[8] = { 4, 4, 12, 12, 2, 8, 1, 0 };
	static int const sz2[8] = { 4, 4, 12, 12, 2, 8, 2, 0 };

	if ((extra & 0x4000) == 0)
	{
		const int dest_reg = (extra >> 10) & 7;

		fmov_rr(dest_reg, val);
		// gb-- status register is affected
		MAKE_FPSR(dest_reg);
		return 0;
	}

	mode = (opcode >> 3) & 7;
	reg = opcode & 7;
	size = (extra >> 10) & 7;
	ad = (uae_u32) -1;
	switch (mode)
	{
	case 0: /* Dn */
		switch (size)
		{
		case 6: /* byte */
			fmovi_mrb(JITPTR temp_fp, val, clamp_bounds.b);
			delay;
			mov_b_rm(reg, JITPTR temp_fp);
			return 0;
		case 4: /* word */
			fmovi_mrb(JITPTR temp_fp, val, clamp_bounds.w);
			delay;
			mov_w_rm(reg, JITPTR temp_fp);
			return 0;
		case 0: /* long */
			fmovi_mrb(JITPTR temp_fp, val, clamp_bounds.l);
			fmovi_mr(JITPTR temp_fp, val);
			delay;
			mov_l_rm(reg, JITPTR temp_fp);
			return 0;
		case 1: /* single precision */
			fmovs_mr(JITPTR temp_fp, val);
			delay;
			mov_l_rm(reg, JITPTR temp_fp);
			return 0;
		default:
			return -1;
		}
	case 1: /* An */
		return -1;						/* genuine invalid instruction */
	default:
		break;
	}

	/* Let's make sure we get out *before* doing something silly if
	   we can't handle the size */
	switch (size)
	{
	case 0: /* long */
	case 1: /* single precision */
	case 2: /* extended precision */
	case 4: /* word */
	case 5: /* double precision */
	case 6: /* byte */
		break;
	case 3: /* packed decimal static */
	default:
		return -1;
	}

	switch (mode)
	{
	case 2: /* (An) */
		ad = S1;
		mov_l_rr(ad, reg + 8);
		break;
	case 3: /* (An)+ */
		ad = S1;
		mov_l_rr(ad, reg + 8);
		lea_l_brr(reg + 8, reg + 8, (reg == 7 ? sz2[size] : sz1[size]));
		break;
	case 4: /* -(An) */
		ad = S1;
		lea_l_brr(reg + 8, reg + 8, -(reg == 7 ? sz2[size] : sz1[size]));
		mov_l_rr(ad, reg + 8);
		break;
	case 5: /* d16(An) */
		{
			uae_u32 off = (uae_s32) (uae_s16) comp_get_iword((m68k_pc_offset += 2) - 2);

			ad = S1;
			mov_l_rr(ad, reg + 8);
			add_l_ri(ad, off);
		}
		break;
	case 6: /* d8(An,Xn) */
		{
			uae_u32 dp = comp_get_iword((m68k_pc_offset += 2) - 2);

			ad = S1;
			calc_disp_ea_020(reg + 8, dp, ad, S2);
		}
		break;
	case 7:
		switch (reg)
		{
		case 0: /* abs.w */
			{
				uae_u32 off = (uae_s32) (uae_s16) comp_get_iword((m68k_pc_offset += 2) - 2);

				ad = S1;
				mov_l_ri(ad, off);
			}
			break;
		case 1: /* abs.l */
			{
				uae_u32 off = comp_get_ilong((m68k_pc_offset += 4) - 4);

				ad = S1;
				mov_l_ri(ad, off);
			}
			break;
		case 2: /* d16(pc) */
			{
				uae_u32 address = (uae_u32)(start_pc + ((char *) comp_pc_p - (char *) start_pc_p) + m68k_pc_offset);
				uae_s32 PC16off = (uae_s32) (uae_s16) comp_get_iword((m68k_pc_offset += 2) - 2);

				ad = S1;
				mov_l_ri(ad, address + PC16off);
			}
			break;
		case 3: /* d8(pc,Xn) */
			return -1;
		case 4: /* #imm */
			{
				uae_u32 address = (uae_u32)(start_pc + ((char *) comp_pc_p - (char *) start_pc_p) + m68k_pc_offset);

				ad = S1;
				mov_l_ri(ad, address);
				m68k_pc_offset += sz2[size];
			}
			break;
		default:
			return -1;
		}
	}

	switch (size)
	{
	case 0: /* long */
		fmovi_mrb(JITPTR temp_fp, val, clamp_bounds.l);
		delay;
		mov_l_rm(S2, JITPTR temp_fp);
		writelong_clobber(ad, S2, S3);
		break;
	case 1: /* single precision */
		fmovs_mr(JITPTR temp_fp, val);
		delay;
		mov_l_rm(S2, JITPTR temp_fp);
		writelong_clobber(ad, S2, S3);
		break;
	case 2: /* extended precision */
		fmov_ext_mr(JITPTR temp_fp, val);
		delay;
		mov_w_rm(S2, JITPTR temp_fp + 8);
		writeword_clobber(ad, S2, S3);
		add_l_ri(ad, 4);
		mov_l_rm(S2, JITPTR temp_fp + 4);
		writelong_clobber(ad, S2, S3);
		add_l_ri(ad, 4);
		mov_l_rm(S2, JITPTR temp_fp);
		writelong_clobber(ad, S2, S3);
		break;
	case 3: /* packed decimal static */
		return -1;						/* Packed */
	case 4: /* word */
		fmovi_mrb(JITPTR temp_fp, val, clamp_bounds.w);
		delay;
		mov_l_rm(S2, JITPTR temp_fp);
		writeword_clobber(ad, S2, S3);
		break;
	case 5: /* double precision */
		fmov_mr(JITPTR temp_fp, val);
		delay;
		mov_l_rm(S2, JITPTR temp_fp + 4);
		writelong_clobber(ad, S2, S3);
		add_l_ri(ad, 4);
		mov_l_rm(S2, JITPTR temp_fp);
		writelong_clobber(ad, S2, S3);
		break;
	case 6: /* byte */
		fmovi_mrb(JITPTR temp_fp, val, clamp_bounds.b);
		delay;
		mov_l_rm(S2, JITPTR temp_fp);
		writebyte(ad, S2, S3);
		break;
	default:
		return -1;
	}
	return 0;
}


/* return -1 for failure, or register number for success */
STATIC_INLINE int get_fp_ad(uae_u32 opcode)
{
	int mode;
	int reg;
	uae_s32 off;

	mode = (opcode >> 3) & 7;
	reg = opcode & 7;
	switch (mode)
	{
	case 0: /* Dn */
	case 1: /* An */
		return -1;
	case 2: /* (An) */
	case 3: /* (An)+ */
	case 4: /* -(An) */
		mov_l_rr(S1, 8 + reg);
		return S1;
	case 5: /* d16(An) */
		off = (uae_s32) (uae_s16) comp_get_iword((m68k_pc_offset += 2) - 2);
		mov_l_rr(S1, 8 + reg);
		add_l_ri(S1, off);
		return S1;
	case 6: /* d8(An,Xn) */
		return -1;
		break;
	case 7:
		switch (reg)
		{
		case 0: /* abs.w */
			off = (uae_s32) (uae_s16) comp_get_iword((m68k_pc_offset += 2) - 2);
			mov_l_ri(S1, off);
			return S1;
		case 1: /* abs.l */
			off = comp_get_ilong((m68k_pc_offset += 4) - 4);
			mov_l_ri(S1, off);
			return S1;
		case 2: /* d16(pc) */
			off = (uae_s32)(start_pc + ((char *) comp_pc_p - (char *) start_pc_p) + m68k_pc_offset);
			off += (uae_s32) (uae_s16) comp_get_iword((m68k_pc_offset += 2) - 2);
			mov_l_ri(S1, off);
			return S1;
		case 3: /* d8(pc,Xn) */
			return -1;
		default:
			return -1;
		}
	}
	abort();
}


/* return -1 for failure, or register number for success */
void comp_fdbcc_opp (uae_u32 /* opcode */, uae_u16 /* extra */)
{
	if (jit_disable.fdbcc)
	{
		FAIL(1);
		return;
	}
    FAIL(1);
    return;
}


void comp_fscc_opp(uae_u32 opcode, uae_u16 extra)
{
	int reg;

	if (jit_disable.fscc)
	{
		FAIL(1);
		return;
	}

	if (extra & 0x20)
	{									/* only cc from 00 to 1f are defined */
		FAIL(1);
		return;
	}
	if ((opcode & 0x38) != 0)
	{									/* We can only do to integer register */
		FAIL(1);
		return;
	}

	fflags_into_flags(S2);
	reg = (opcode & 7);

	mov_l_ri(S1, 255);
	mov_l_ri(S4, 0);
	switch (extra & 0x0f)
	{									/* according to fpp.c, the 0x10 bit is ignored
										 */
	case 0:
		break;							/* set never */
	case 1:
		mov_l_rr(S2, S4);
		cmov_l_rr(S4, S1, 4);
		cmov_l_rr(S4, S2, 10);
		break;
	case 2:
		cmov_l_rr(S4, S1, 7);
		break;
	case 3:
		cmov_l_rr(S4, S1, 3);
		break;
	case 4:
		mov_l_rr(S2, S4);
		cmov_l_rr(S4, S1, 2);
		cmov_l_rr(S4, S2, 10);
		break;
	case 5:
		mov_l_rr(S2, S4);
		cmov_l_rr(S4, S1, 6);
		cmov_l_rr(S4, S2, 10);
		break;
	case 6:
		cmov_l_rr(S4, S1, 5);
		break;
	case 7:
		cmov_l_rr(S4, S1, 11);
		break;
	case 8:
		cmov_l_rr(S4, S1, 10);
		break;
	case 9:
		cmov_l_rr(S4, S1, 4);
		break;
	case 10:
		cmov_l_rr(S4, S1, 10);
		cmov_l_rr(S4, S1, 7);
		break;
	case 11:
		cmov_l_rr(S4, S1, 4);
		cmov_l_rr(S4, S1, 3);
		break;
	case 12:
		cmov_l_rr(S4, S1, 2);
		break;
	case 13:
		cmov_l_rr(S4, S1, 6);
		break;
	case 14:
		cmov_l_rr(S4, S1, 5);
		cmov_l_rr(S4, S1, 10);
		break;
	case 15:
		mov_l_rr(S4, S1);
		break;
	}

	if ((opcode & 0x38) == 0)
	{
		mov_b_rr(reg, S4);
	} else
	{
		abort();
#if 0
		int cc;

		if (get_fp_ad(opcode) < 0)
		{
			FAIL(1);
		} else
		{
			put_byte(ad, cc ? 0xff : 0x00);
		}
#endif
	}
}


void comp_ftrapcc_opp (uae_u32 /* opcode */, uaecptr /* oldpc */)
{
	FAIL(1);
	return;
}


void comp_fbcc_opp(uae_u32 opcode)
{
	uae_u32 start_68k_offset = m68k_pc_offset;
	uae_u32 off;
	uae_u32 v1;
	uae_u32 v2;
	int cc;

	// comp_pc_p is expected to be bound to 32-bit addresses
	assert((uintptr) comp_pc_p <= 0xffffffffUL);

	if (jit_disable.fbcc)
	{
		FAIL(1);
		return;
	}
	if (opcode & 0x20)
	{									/* only cc from 00 to 1f are defined */
		FAIL(1);
		return;
	}
	if ((opcode & 0x40) == 0)
	{
		off = (uae_s32) (uae_s16) comp_get_iword((m68k_pc_offset += 2) - 2);
	} else
	{
		off = comp_get_ilong((m68k_pc_offset += 4) - 4);
	}
	mov_l_ri(S1, JITPTR (comp_pc_p + off - (m68k_pc_offset - start_68k_offset)));
	mov_l_ri(PC_P, JITPTR comp_pc_p);

	/* Now they are both constant. Might as well fold in m68k_pc_offset */
	add_l_ri(S1, m68k_pc_offset);
	add_l_ri(PC_P, m68k_pc_offset);
	m68k_pc_offset = 0;

	/* according to fpp.c, the 0x10 bit is ignored
	   (it handles exception handling, which we don't
	   do, anyway ;-) */
	cc = opcode & 0x0f;
	v1 = get_const(PC_P);
	v2 = get_const(S1);
	fflags_into_flags(S2);

	switch (cc)
	{
	case 0:
		break;							/* jump never */
	case 1:
		mov_l_rr(S2, PC_P);
		cmov_l_rr(PC_P, S1, 4);
		cmov_l_rr(PC_P, S2, 10);
		break;
	case 2:
		register_branch(v1, v2, 7);
		break;
	case 3:
		register_branch(v1, v2, 3);
		break;
	case 4:
		mov_l_rr(S2, PC_P);
		cmov_l_rr(PC_P, S1, 2);
		cmov_l_rr(PC_P, S2, 10);
		break;
	case 5:
		mov_l_rr(S2, PC_P);
		cmov_l_rr(PC_P, S1, 6);
		cmov_l_rr(PC_P, S2, 10);
		break;
	case 6:
		register_branch(v1, v2, 5);
		break;
	case 7:
		register_branch(v1, v2, 11);
		break;
	case 8:
		register_branch(v1, v2, 10);
		break;
	case 9:
		register_branch(v1, v2, 4);
		break;
	case 10:
		cmov_l_rr(PC_P, S1, 10);
		cmov_l_rr(PC_P, S1, 7);
		break;
	case 11:
		cmov_l_rr(PC_P, S1, 4);
		cmov_l_rr(PC_P, S1, 3);
		break;
	case 12:
		register_branch(v1, v2, 2);
		break;
	case 13:
		register_branch(v1, v2, 6);
		break;
	case 14:
		cmov_l_rr(PC_P, S1, 5);
		cmov_l_rr(PC_P, S1, 10);
		break;
	case 15:
		mov_l_rr(PC_P, S1);
		break;
	}
}


    /* Floating point conditions 
       The "NotANumber" part could be problematic; Howver, when NaN is
       encountered, the ftst instruction sets bot N and Z to 1 on the x87,
       so quite often things just fall into place. This is probably not
       accurate wrt the 68k FPU, but it is *as* accurate as this was before.
       However, some more thought should go into fixing this stuff up so
       it accurately emulates the 68k FPU.
>=<U 
0000    0x00: 0                        ---   Never jump
0101    0x01: Z                        ---   jump if zero (x86: 4)
1000    0x02: !(NotANumber || Z || N)  --- Neither Z nor N set (x86: 7)
1101    0x03: Z || !(NotANumber || N); --- Z or !N (x86: 4 and 3)
0010    0x04: N && !(NotANumber || Z); --- N and !Z (x86: hard!)
0111    0x05: Z || (N && !NotANumber); --- Z or N (x86: 6)
1010    0x06: !(NotANumber || Z);      --- not Z (x86: 5)
1110    0x07: !NotANumber;             --- not NaN (x86: 11, not parity)
0001    0x08: NotANumber;              --- NaN (x86: 10)
0101    0x09: NotANumber || Z;         --- Z (x86: 4)
1001    0x0a: NotANumber || !(N || Z); --- NaN or neither N nor Z (x86: 10 and 7)
1101    0x0b: NotANumber || Z || !N;   --- Z or !N (x86: 4 and 3)
0011    0x0c: NotANumber || (N && !Z); --- N (x86: 2)
0111    0x0d: NotANumber || Z || N;    --- Z or N (x86: 6)
1010    0x0e: !Z;                      --- not Z (x86: 5)
1111    0x0f: 1;                       --- always

This is not how the 68k handles things, though --- it sets Z to 0 and N
to the NaN's sign.... ('o' and 'i' denote differences from the above
table)

>=<U 
0000    0x00: 0                        ---   Never jump
010o    0x01: Z                        ---   jump if zero (x86: 4, not 10)
1000    0x02: !(NotANumber || Z || N)  --- Neither Z nor N set (x86: 7)
110o    0x03: Z || !(NotANumber || N); --- Z or !N (x86: 3)
0010    0x04: N && !(NotANumber || Z); --- N and !Z (x86: 2, not 10)
011o    0x05: Z || (N && !NotANumber); --- Z or N (x86: 6, not 10)
1010    0x06: !(NotANumber || Z);      --- not Z (x86: 5)
1110    0x07: !NotANumber;             --- not NaN (x86: 11, not parity)
0001    0x08: NotANumber;              --- NaN (x86: 10)
0101    0x09: NotANumber || Z;         --- Z (x86: 4)
1001    0x0a: NotANumber || !(N || Z); --- NaN or neither N nor Z (x86: 10 and 7)
1101    0x0b: NotANumber || Z || !N;   --- Z or !N (x86: 4 and 3)
0011    0x0c: NotANumber || (N && !Z); --- N (x86: 2)
0111    0x0d: NotANumber || Z || N;    --- Z or N (x86: 6)
101i    0x0e: !Z;                      --- not Z (x86: 5 and 10)
1111    0x0f: 1;                       --- always

Of course, this *still* doesn't mean that the x86 and 68k conditions are
equivalent --- the handling of infinities is different, for one thing.
On the 68k, +infinity minus +infinity is NotANumber (as it should be). On
the x86, it is +infinity, and some exception is raised (which I suspect
is promptly ignored) STUPID! 
The more I learn about their CPUs, the more I detest Intel....

You can see this in action if you have "Benoit" (see Aminet) and
set the exponent to 16. Wait for a long time, and marvel at the extra black
areas outside the center one. That's where Benoit expects NaN, and the x86
gives +infinity. [Ooops --- that must have been some kind of bug in my code.
it no longer happens, and the resulting graphic looks much better, too]

x86 conditions
0011    : 2
1100    : 3
0101    : 4
1010    : 5
0111    : 6
1000    : 7
0001    : 10
1110    : 11
    */

#ifndef UAE
void comp_fsave_opp(uae_u32 opcode)
{
	int incr = (opcode & 0x38) == 0x20 ? -1 : 1;
	int i;
	int ad;

	if (jit_disable.fsave)
	{
		FAIL(1);
		return;
	}
	FAIL(1);
	return;

	if ((ad = get_fp_ad(opcode)) < 0)
	{
		FAIL(1);
		return;
	}

	if (currprefs.fpu_model == 68040)
	{
		/* 4 byte 68040 IDLE frame.  */
		if (incr < 0)
		{
			ad -= 4;
			put_long(ad, 0x41000000);
		} else
		{
			put_long(ad, 0x41000000);
			ad += 4;
		}
	} else
	{
		if (incr < 0)
		{
			ad -= 4;
			put_long(ad, 0x70000000);
			for (i = 0; i < 5; i++)
			{
				ad -= 4;
				put_long(ad, 0x00000000);
			}
			ad -= 4;
			put_long(ad, 0x1f180000);
		} else
		{
			put_long(ad, 0x1f180000);
			ad += 4;
			for (i = 0; i < 5; i++)
			{
				put_long(ad, 0x00000000);
				ad += 4;
			}
			put_long(ad, 0x70000000);
			ad += 4;
		}
	}
	if ((opcode & 0x38) == 0x18)
		m68k_areg(regs, opcode & 7) = ad;
	if ((opcode & 0x38) == 0x20)
		m68k_areg(regs, opcode & 7) = ad;
}


void comp_frestore_opp(uae_u32 opcode)
{
	uae_u32 d;
	int incr = (opcode & 0x38) == 0x20 ? -1 : 1;
	int ad;

	if (jit_disable.frestore)
	{
		FAIL(1);
		return;
	}
	FAIL(1);
	return;

	if ((ad = get_fp_ad(opcode)) < 0)
	{
		FAIL(1);
		return;
	}
	if (currprefs.fpu_model == 68040)
	{
		/* 68040 */
		if (incr < 0)
		{
			/* @@@ This may be wrong.  */
			ad -= 4;
			d = get_long(ad);
			if ((d & 0xff000000) != 0)
			{							/* Not a NULL frame? */
				if ((d & 0x00ff0000) == 0)
				{						/* IDLE */
				} else if ((d & 0x00ff0000) == 0x00300000)
				{						/* UNIMP */
					ad -= 44;
				} else if ((d & 0x00ff0000) == 0x00600000)
				{						/* BUSY */
					ad -= 92;
				}
			}
		} else
		{
			d = get_long(ad);
			ad += 4;
			if ((d & 0xff000000) != 0)
			{							/* Not a NULL frame? */
				if ((d & 0x00ff0000) == 0)
				{						/* IDLE */
				} else if ((d & 0x00ff0000) == 0x00300000)
				{						/* UNIMP */
					ad += 44;
				} else if ((d & 0x00ff0000) == 0x00600000)
				{						/* BUSY */
					ad += 92;
				}
			}
		}
	} else
	{
		if (incr < 0)
		{
			ad -= 4;
			d = get_long(ad);
			if ((d & 0xff000000) != 0)
			{
				if ((d & 0x00ff0000) == 0x00180000)
					ad -= 6 * 4;
				else if ((d & 0x00ff0000) == 0x00380000)
					ad -= 14 * 4;
				else if ((d & 0x00ff0000) == 0x00b40000)
					ad -= 45 * 4;
			}
		} else
		{
			d = get_long(ad);
			ad += 4;
			if ((d & 0xff000000) != 0)
			{
				if ((d & 0x00ff0000) == 0x00180000)
					ad += 6 * 4;
				else if ((d & 0x00ff0000) == 0x00380000)
					ad += 14 * 4;
				else if ((d & 0x00ff0000) == 0x00b40000)
					ad += 45 * 4;
			}
		}
	}
	if ((opcode & 0x38) == 0x18)
		m68k_areg(regs, opcode & 7) = ad;
	if ((opcode & 0x38) == 0x20)
		m68k_areg(regs, opcode & 7) = ad;
}
#endif

#if defined(USE_LONG_DOUBLE) || defined(USE_QUAD_DOUBLE)
static const fpu_register const_e	= LD(2.7182818284590452353); // LD(2.7182818284590452353602874713526625);
static const fpu_register const_log10_e	= LD(0.4342944819032518276511289189166051);
static const fpu_register const_loge_10	= LD(2.3025850929940456840179914546843642);
#else
static const fpu_register const_e	= 2.7182818284590452354;
static const fpu_register const_log10_e	= 0.43429448190325182765;
static const fpu_register const_loge_10	= 2.30258509299404568402;
#endif

static const fpu_register power10[]		= {
	LD(1e0), LD(1e1), LD(1e2), LD(1e4), LD(1e8), LD(1e16), LD(1e32), LD(1e64), LD(1e128), LD(1e256)
#if defined(USE_LONG_DOUBLE) || defined(USE_QUAD_DOUBLE)
,	LD(1e512), LD(1e1024), LD(1e2048), LD(1e4096)
#endif
};

/* 128 words, indexed through the low byte of the 68k fpu control word */
#if 1
/* unused*/
static uae_u16 x86_fpucw[]={
    0x137f, 0x137f, 0x137f, 0x137f, 0x137f, 0x137f, 0x137f, 0x137f, /* p0r0 */
    0x1f7f, 0x1f7f, 0x1f7f, 0x1f7f, 0x1f7f, 0x1f7f, 0x1f7f, 0x1f7f, /* p0r1 */
    0x177f, 0x177f, 0x177f, 0x177f, 0x177f, 0x177f, 0x177f, 0x177f, /* p0r2 */
    0x1b7f, 0x1b7f, 0x1b7f, 0x1b7f, 0x1b7f, 0x1b7f, 0x1b7f, 0x1b7f, /* p0r3 */

    0x107f, 0x107f, 0x107f, 0x107f, 0x107f, 0x107f, 0x107f, 0x107f, /* p1r0 */
    0x1c7f, 0x1c7f, 0x1c7f, 0x1c7f, 0x1c7f, 0x1c7f, 0x1c7f, 0x1c7f, /* p1r1 */
    0x147f, 0x147f, 0x147f, 0x147f, 0x147f, 0x147f, 0x147f, 0x147f, /* p1r2 */
    0x187f, 0x187f, 0x187f, 0x187f, 0x187f, 0x187f, 0x187f, 0x187f, /* p1r3 */

    0x127f, 0x127f, 0x127f, 0x127f, 0x127f, 0x127f, 0x127f, 0x127f, /* p2r0 */
    0x1e7f, 0x1e7f, 0x1e7f, 0x1e7f, 0x1e7f, 0x1e7f, 0x1e7f, 0x1e7f, /* p2r1 */
    0x167f, 0x167f, 0x167f, 0x167f, 0x167f, 0x167f, 0x167f, 0x167f, /* p2r2 */
    0x1a7f, 0x1a7f, 0x1a7f, 0x1a7f, 0x1a7f, 0x1a7f, 0x1a7f, 0x1a7f, /* p2r3 */

    0x137f, 0x137f, 0x137f, 0x137f, 0x137f, 0x137f, 0x137f, 0x137f, /* p3r0 */
    0x1f7f, 0x1f7f, 0x1f7f, 0x1f7f, 0x1f7f, 0x1f7f, 0x1f7f, 0x1f7f, /* p3r1 */
    0x177f, 0x177f, 0x177f, 0x177f, 0x177f, 0x177f, 0x177f, 0x177f, /* p3r2 */
    0x1b7f, 0x1b7f, 0x1b7f, 0x1b7f, 0x1b7f, 0x1b7f, 0x1b7f, 0x1b7f  /* p3r3 */
};
#endif


void comp_fpp_opp(uae_u32 opcode, uae_u16 extra)
{
	int reg;
	int src;

	switch ((extra >> 13) & 0x7)
	{
	case 1:							/* illegal */
		break;

	case 3:							/* FMOVE Fpn,<ea> */
		/* 2nd most common */
		if (jit_disable.fmove)
		{
			FAIL(1);
			return;
		}

		if (put_fp_value((extra >> 7) & 7, opcode, extra) < 0)
		{
			FAIL(1);
			return;
		}
		return;

	case 6:							/* FMOVEM <ea>,<reglist> */
	case 7:							/* FMOVEM <reglist>,<ea> */
		if (jit_disable.fmovem)
		{
			FAIL(1);
			return;
		}

		{
			int ad;
			uae_u32 list = 0;
			int incr = 0;

			if (extra & 0x2000)
			{
				/* FMOVEM FPP->memory */
				switch ((extra >> 11) & 3)
				{						/* Get out early if failure */
				case 0:				/* static pred */
				case 2:				/* static postinc */
					break;
				case 1:				/* dynamic pred */
				case 3:				/* dynamic postinc */
				default:
					FAIL(1);
					return;
				}
				if ((ad = get_fp_ad(opcode)) < 0)
				{
					FAIL(1);
					return;
				}
				switch ((extra >> 11) & 3)
				{
				case 0:				/* static pred */
					list = extra & 0xff;
					incr = -1;
					break;
				case 2:				/* static postinc */
					list = extra & 0xff;
					incr = 1;
					break;
				case 1:				/* dynamic pred */
				case 3:				/* dynamic postinc */
					abort();
				}
				if (incr < 0)
				{						/* Predecrement */
					for (reg = 7; reg >= 0; reg--)
					{
						if (list & 0x80)
						{
							fmov_ext_mr(JITPTR temp_fp, reg);
							delay;
							sub_l_ri(ad, 4);
							mov_l_rm(S2, JITPTR temp_fp);
							writelong_clobber(ad, S2, S3);
							sub_l_ri(ad, 4);
							mov_l_rm(S2, JITPTR temp_fp + 4);
							writelong_clobber(ad, S2, S3);
							sub_l_ri(ad, 4);
							mov_w_rm(S2, JITPTR temp_fp + 8);
							writeword_clobber(ad, S2, S3);
						}
						list <<= 1;
					}
				} else
				{						/* Postincrement */
					for (reg = 0; reg < 8; reg++)
					{
						if (list & 0x80)
						{
							fmov_ext_mr(JITPTR temp_fp, reg);
							delay;
							mov_w_rm(S2, JITPTR temp_fp + 8);
							writeword_clobber(ad, S2, S3);
							add_l_ri(ad, 4);
							mov_l_rm(S2, JITPTR temp_fp + 4);
							writelong_clobber(ad, S2, S3);
							add_l_ri(ad, 4);
							mov_l_rm(S2, JITPTR temp_fp);
							writelong_clobber(ad, S2, S3);
							add_l_ri(ad, 4);
						}
						list <<= 1;
					}
				}
				if ((opcode & 0x38) == 0x18)
					mov_l_rr((opcode & 7) + 8, ad);
				if ((opcode & 0x38) == 0x20)
					mov_l_rr((opcode & 7) + 8, ad);
			} else
			{
				/* FMOVEM memory->FPP */

				int ad;

				switch ((extra >> 11) & 3)
				{						/* Get out early if failure */
				case 0:				/* static pred */
				case 2:				/* static postinc */
					break;
				case 1:				/* dynamic pred */
				case 3:				/* dynamic postinc */
				default:
					FAIL(1);
					return;
				}
				ad = get_fp_ad(opcode);
				if (ad < 0)
				{
					D(bug("no ad\n"));
					FAIL(1);
					return;
				}
				switch ((extra >> 11) & 3)
				{
				case 0:				/* static pred */
					list = extra & 0xff;
					incr = -1;
					break;
				case 2:				/* static postinc */
					list = extra & 0xff;
					incr = 1;
					break;
				case 1:				/* dynamic pred */
				case 3:				/* dynamic postinc */
					abort();
				}

				if (incr < 0)
				{
					// not reached
					for (reg = 7; reg >= 0; reg--)
					{
						if (list & 0x80)
						{
							sub_l_ri(ad, 4);
							readlong(ad, S2, S3);
							mov_l_mr(JITPTR (temp_fp), S2);
							sub_l_ri(ad, 4);
							readlong(ad, S2, S3);
							mov_l_mr(JITPTR (temp_fp) + 4, S2);
							sub_l_ri(ad, 4);
							readword(ad, S2, S3);
							mov_w_mr((JITPTR temp_fp) + 8, S2);
							delay2;
							fmov_ext_rm(reg, JITPTR (temp_fp));
						}
						list <<= 1;
					}
				} else
				{
					for (reg = 0; reg < 8; reg++)
					{
						if (list & 0x80)
						{
							readword(ad, S2, S3);
							mov_w_mr((JITPTR temp_fp) + 8, S2);
							add_l_ri(ad, 4);
							readlong(ad, S2, S3);
							mov_l_mr(JITPTR (temp_fp) + 4, S2);
							add_l_ri(ad, 4);
							readlong(ad, S2, S3);
							mov_l_mr(JITPTR(temp_fp), S2);
							add_l_ri(ad, 4);
							delay2;
							fmov_ext_rm(reg, JITPTR (temp_fp));
						}
						list <<= 1;
					}
				}
				if ((opcode & 0x38) == 0x18)
					mov_l_rr((opcode & 7) + 8, ad);
				if ((opcode & 0x38) == 0x20)
					mov_l_rr((opcode & 7) + 8, ad);
			}
		}
		return;

	case 4:							/* FMOVEM <ea>,<control> */
	case 5:							/* FMOVEM <control>,<ea> */
		if (jit_disable.fmovec)
		{
			FAIL(1);
			return;
		}

		/* rare */
		if ((opcode & 0x30) == 0)
		{
			/* <ea> = Dn or An */
			if (extra & 0x2000)
			{
				if (extra & 0x1000)
				{
#if HANDLE_FPCR
					mov_l_rm(opcode & 15, (uintptr) & fpu.fpcr.rounding_mode);
					or_l_rm(opcode & 15, (uintptr) & fpu.fpcr.rounding_precision);
#else
					FAIL(1);
					return;
#endif
				}
				if (extra & 0x0800)
				{
					FAIL(1);
					return;
				}
				if (extra & 0x0400)
				{
					/* FPIAR: fixme; we cannot correctly return the address from compiled code */
#ifdef UAE
					mov_l_rm(opcode & 15, JITPTR &regs.fpiar);
#else
					mov_l_rm(opcode & 15, JITPTR &fpu.instruction_address);
#endif
					return;
				}
			} else
			{
				// gb-- moved here so that we may FAIL() without generating any code
				if (extra & 0x0800)
				{
					// set_fpsr(m68k_dreg (regs, opcode & 15));
					FAIL(1);
					return;
				}
				if (extra & 0x1000)
				{
#if HANDLE_FPCR
#if defined(FPU_USE_X86_ROUNDING_MODE) && defined(FPU_USE_X86_ROUNDING_PRECISION)
					FAIL(1);
					return;
#endif
					mov_l_rr(S1, opcode & 15);
					mov_l_rr(S2, opcode & 15);
					and_l_ri(S1, FPCR_ROUNDING_PRECISION);
					and_l_ri(S2, FPCR_ROUNDING_MODE);
					mov_l_mr((uintptr) & fpu.fpcr.rounding_precision, S1);
					mov_l_mr((uintptr) & fpu.fpcr.rounding_mode, S2);
#else
					FAIL(1);
					return;
#endif
				}
				if (extra & 0x0400)
				{
					/* FPIAR: does that make sense at all? */
#ifdef UAE
					mov_l_mr(JITPTR &regs.fpiar, opcode & 15);
#else
					mov_l_mr(JITPTR &fpu.instruction_address, opcode & 15);
#endif
				}
				return;
			}
		} else if ((opcode & 0x3f) == 0x3c)
		{
			/* <ea> = #imm */
			if ((extra & 0x2000) == 0)
			{
				// gb-- moved here so that we may FAIL() without generating any code
				if (extra & 0x0800)
				{
					FAIL(1);
					return;
				}
				if (extra & 0x1000)
				{
					comp_get_ilong((m68k_pc_offset += 4) - 4);
#if HANDLE_FPCR
#if defined(FPU_USE_X86_ROUNDING_MODE) && defined(FPU_USE_X86_ROUNDING_PRECISION)
					FAIL(1);
					return;
#endif
					// mov_l_mi((uintptr)&regs.fpcr,val);
					mov_l_ri(S1, val);
					mov_l_ri(S2, val);
					and_l_ri(S1, FPCR_ROUNDING_PRECISION);
					and_l_ri(S2, FPCR_ROUNDING_MODE);
					mov_l_mr((uintptr) & fpu.fpcr.rounding_precision, S1);
					mov_l_mr((uintptr) & fpu.fpcr.rounding_mode, S2);
#else
					FAIL(1);
					return;
#endif
				}
				if (extra & 0x0400)
				{
					uae_u32 val = comp_get_ilong((m68k_pc_offset += 4) - 4);
#ifdef UAE
					mov_l_mi(JITPTR &regs.fpiar, val);
#else
					mov_l_mi(JITPTR &fpu.instruction_address, val);
#endif
				}
				return;
			}
			FAIL(1);
			return;
		} else if (extra & 0x2000)
		{
			FAIL(1);
			return;
		} else
		{
			FAIL(1);
			return;
		}
		FAIL(1);
		return;

	case 0:
	case 2:							/* Extremely common */
		reg = (extra >> 7) & 7;
		if ((extra & 0xfc00) == 0x5c00)
		{
			if (jit_disable.fmovecr)
			{
				FAIL(1);
				return;
			}

			switch (extra & 0x7f)
			{
			case 0x00:
				fmov_pi(reg);
				break;
			case 0x0b:
				fmov_log10_2(reg);
				break;
			case 0x0c:
#if defined(USE_LONG_DOUBLE) || defined(USE_QUAD_DOUBLE)
				fmov_ext_rm(reg, (uintptr) & const_e);
#else
				fmov_rm(reg, (uintptr) & const_e);
#endif
				break;
			case 0x0d:
				fmov_log2_e(reg);
				break;
			case 0x0e:
#if defined(USE_LONG_DOUBLE) || defined(USE_QUAD_DOUBLE)
				fmov_ext_rm(reg, (uintptr) & const_log10_e);
#else
				fmov_rm(reg, (uintptr) & const_log10_e);
#endif
				break;
			case 0x0f:
				fmov_0(reg);
				break;
			case 0x30:
				fmov_loge_2(reg);
				break;
			case 0x31:
#if defined(USE_LONG_DOUBLE) || defined(USE_QUAD_DOUBLE)
				fmov_ext_rm(reg, (uintptr) & const_loge_10);
#else
				fmov_rm(reg, (uintptr) & const_loge_10);
#endif
				break;
			case 0x32:
				fmov_1(reg);
				break;
			case 0x33:
			case 0x34:
			case 0x35:
			case 0x36:
			case 0x37:
			case 0x38:
			case 0x39:
			case 0x3a:
			case 0x3b:
#if defined(USE_LONG_DOUBLE) || defined(USE_QUAD_DOUBLE)
			case 0x3c:
			case 0x3d:
			case 0x3e:
			case 0x3f:
				fmov_ext_rm(reg, (uintptr) (power10 + (extra & 0x7f) - 0x32));
#else
				fmov_rm(reg, (uintptr) (power10 + (extra & 0x7f) - 0x32));
#endif
				break;
			default:
				/* This is not valid, so we fail */
				FAIL(1);
				return;
			}
			return;
		}

		switch (extra & 0x7f)
		{
		case 0x00:						/* FMOVE */
		case 0x40:						/* FSMOVE: Explicit rounding. This is just a quick fix. Same
										 * for all other cases that have three choices */
		case 0x44:						/* FDMOVE */
			if (jit_disable.fmove)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fmov_rr(reg, src);
			MAKE_FPSR(src);
			break;
		case 0x01:						/* FINT */
			if (jit_disable.fint)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x02:						/* FSINH */
			if (jit_disable.fsinh)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x03:						/* FINTRZ */
			if (jit_disable.fintrz)
			{
				FAIL(1);
				return;
			}
#ifdef USE_X86_FPUCW
			/* If we have control over the CW, we can do this */
			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			mov_l_ri(S1, 16);			/* Switch to "round to zero" mode */
			fldcw_m_indexed(S1, JITPTR x86_fpucw);

			frndint_rr(reg, src);

			/* restore control word */
			mov_l_rm(S1, JITPTR &regs.fpcr);
			and_l_ri(S1, 0x000000f0);
			fldcw_m_indexed(S1, JITPTR x86_fpucw);

			MAKE_FPSR(reg);
			break;
#endif
			FAIL(1);
			return;
			break;
		case 0x04:						/* FSQRT */
		case 0x41:						/* FSSQRT */
		case 0x45:						/* FDSQRT */
			if (jit_disable.fsqrt)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fsqrt_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x06:						/* FLOGNP1 */
			if (jit_disable.flognp1)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x08:						/* FETOXM1 */
			if (jit_disable.fetoxm1)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x09:						/* FTANH */
			if (jit_disable.ftanh)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x0a:						/* FATAN */
			if (jit_disable.fatan)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x0c:						/* FASIN */
			if (jit_disable.fasin)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x0d:						/* FATANH */
			if (jit_disable.fatanh)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x0e:						/* FSIN */
			if (jit_disable.fsin)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fsin_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x0f:						/* FTAN */
			if (jit_disable.ftan)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x10:						/* FETOX */
			if (jit_disable.fetox)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fetox_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x11:						/* FTWOTOX */
			if (jit_disable.ftwotox)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			ftwotox_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x12:						/* FTENTOX */
			if (jit_disable.ftentox)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x14:						/* FLOGN */
			if (jit_disable.flogn)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x15:						/* FLOG10 */
			if (jit_disable.flog10)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x16:						/* FLOG2 */
			if (jit_disable.flog2)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			flog2_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x18:						/* FABS */
		case 0x58:						/* FSABS */
		case 0x5c:						/* FDABS */
			if (jit_disable.fabs)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fabs_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x19:						/* FCOSH */
			if (jit_disable.fcosh)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x1a:						/* FNEG */
		case 0x5a:						/* FSNEG */
		case 0x5e:						/* FDNEG */
			if (jit_disable.fneg)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fneg_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x1c:						/* FACOS */
			if (jit_disable.facos)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x1d:						/* FCOS */
			if (jit_disable.fcos)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fcos_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x1e:						/* FGETEXP */
			if (jit_disable.fgetexp)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x1f:						/* FGETMAN */
			if (jit_disable.fgetman)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x20:						/* FDIV */
		case 0x60:						/* FSDIV */
		case 0x64:						/* FDDIV */
			if (jit_disable.fdiv)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fdiv_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x21:						/* FMOD */
			if (jit_disable.fmod)
			{
				FAIL(1);
				return;
			}

			// FIXME: the quotient byte must be computed
			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			frem_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x22:						/* FADD */
		case 0x62:						/* FSADD */
		case 0x66:						/* FDADD */
			if (jit_disable.fadd)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fadd_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x23:						/* FMUL */
		case 0x63:						/* FSMUL */
		case 0x67:						/* FDMUL */
			if (jit_disable.fmul)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fmul_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x24:						/* FSGLDIV */
			if (jit_disable.fsgldiv)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fdiv_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x25:						/* FREM */
			if (jit_disable.frem)
			{
				FAIL(1);
				return;
			}
			// gb-- disabled because the quotient byte must be computed
			// otherwise, free rotation in ClarisWorks doesn't work.
			FAIL(1);
			return;
			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			frem1_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x26:						/* FSCALE */
			if (jit_disable.fscale)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			break;
		case 0x27:						/* FSGLMUL */
			if (jit_disable.fsglmul)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fmul_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x28:						/* FSUB */
		case 0x68:						/* FSSUB */
		case 0x6c:						/* FDSUB */
			if (jit_disable.fsub)
			{
				FAIL(1);
				return;
			}

			dont_care_fflags();
			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fsub_rr(reg, src);
			MAKE_FPSR(reg);
			break;
		case 0x30:						/* FSINCOS */
		case 0x31:
		case 0x32:
		case 0x33:
		case 0x34:
		case 0x35:
		case 0x36:
		case 0x37:
			if (jit_disable.fsincos)
			{
				FAIL(1);
				return;
			}

			FAIL(1);
			return;
			dont_care_fflags();
			break;
		case 0x38:						/* FCMP */
			if (jit_disable.fcmp)
			{
				FAIL(1);
				return;
			}

			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fmov_rr(FP_RESULT, reg);
			fsub_rr(FP_RESULT, src);	/* Right way? */
			break;
		case 0x3a:						/* FTST */
			if (jit_disable.ftst)
			{
				FAIL(1);
				return;
			}

			src = get_fp_value(opcode, extra);
			if (src < 0)
			{
				FAIL(1);				/* Illegal instruction */
				return;
			}
			fmov_rr(FP_RESULT, src);
			break;
		default:
			FAIL(1);
			return;
			break;
		}
		return;
	}
	FAIL(1);
}
