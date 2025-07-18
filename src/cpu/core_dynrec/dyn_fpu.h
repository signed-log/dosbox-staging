// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "dosbox.h"

#if C_FPU

#include "cpu.h"
#include "cross.h"
#include "fpu.h"
#include "mem.h"
#include <cmath>

static void FPU_FDECSTP(){
	TOP = (TOP - 1) & 7;
}

static void FPU_FINCSTP(){
	TOP = (TOP + 1) & 7;
}

static void FPU_FNSTCW(PhysPt addr){
	mem_writew(addr,fpu.cw);
}

static void FPU_FFREE(Bitu st) {
	fpu.tags[st] = TAG_Empty;
}

	#if C_FPU_X86
		#include "../../fpu/fpu_instructions_x86.h"
	#else
		#include "../../fpu/fpu_instructions.h"
	#endif

static inline void dyn_fpu_top() {
	gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
	gen_add_imm(FC_OP2,decode.modrm.rm);
	gen_and_imm(FC_OP2,7);
	gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
}

static inline void dyn_fpu_top_swapped() {
	gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
	gen_add_imm(FC_OP1,decode.modrm.rm);
	gen_and_imm(FC_OP1,7);
	gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
}

static void dyn_eatree() {
//	Bitu group = (decode.modrm.val >> 3) & 7;
	Bitu group = decode.modrm.reg&7; //It is already that, but compilers.
	switch (group){
	case 0x00:		// FADD ST,STi
		gen_call_function_R((void*)&FPU_FADD_EA,FC_OP1);
		break;
	case 0x01:		// FMUL  ST,STi
		gen_call_function_R((void*)&FPU_FMUL_EA,FC_OP1);
		break;
	case 0x02:		// FCOM  STi
		gen_call_function_R((void*)&FPU_FCOM_EA,FC_OP1);
		break;
	case 0x03:		// FCOMP STi
		gen_call_function_R((void*)&FPU_FCOM_EA,FC_OP1);
		gen_call_function_raw((void*)&FPU_FPOP);
		break;
	case 0x04:		// FSUB  ST,STi
		gen_call_function_R((void*)&FPU_FSUB_EA,FC_OP1);
		break;	
	case 0x05:		// FSUBR ST,STi
		gen_call_function_R((void*)&FPU_FSUBR_EA,FC_OP1);
		break;
	case 0x06:		// FDIV  ST,STi
		gen_call_function_R((void*)&FPU_FDIV_EA,FC_OP1);
		break;
	case 0x07:		// FDIVR ST,STi
		gen_call_function_R((void*)&FPU_FDIVR_EA,FC_OP1);
		break;
	default:
		break;
	}
}

static void dyn_fpu_esc0(){
	dyn_get_modrm();
//	if (decode.modrm.val >= 0xc0) {
	if (decode.modrm.mod == 3) { 
		dyn_fpu_top();
		switch (decode.modrm.reg){
		case 0x00:		//FADD ST,STi
			gen_call_function_RR((void*)&FPU_FADD,FC_OP1,FC_OP2);
			break;
		case 0x01:		// FMUL  ST,STi
			gen_call_function_RR((void*)&FPU_FMUL,FC_OP1,FC_OP2);
			break;
		case 0x02:		// FCOM  STi
			gen_call_function_RR((void*)&FPU_FCOM,FC_OP1,FC_OP2);
			break;
		case 0x03:		// FCOMP STi
			gen_call_function_RR((void*)&FPU_FCOM,FC_OP1,FC_OP2);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		case 0x04:		// FSUB  ST,STi
			gen_call_function_RR((void*)&FPU_FSUB,FC_OP1,FC_OP2);
			break;	
		case 0x05:		// FSUBR ST,STi
			gen_call_function_RR((void*)&FPU_FSUBR,FC_OP1,FC_OP2);
			break;
		case 0x06:		// FDIV  ST,STi
			gen_call_function_RR((void*)&FPU_FDIV,FC_OP1,FC_OP2);
			break;
		case 0x07:		// FDIVR ST,STi
			gen_call_function_RR((void*)&FPU_FDIVR,FC_OP1,FC_OP2);
			break;
		default:
			break;
		}
	} else { 
		dyn_fill_ea(FC_ADDR);
		gen_call_function_R((void*)&FPU_FLD_F32_EA,FC_ADDR); 
		gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
		dyn_eatree();
	}
}


static void dyn_fpu_esc1(){
	dyn_get_modrm();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		switch (decode.modrm.reg){
		case 0x00: /* FLD STi */
			gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
			gen_add_imm(FC_OP1,decode.modrm.rm);
			gen_and_imm(FC_OP1,7);
			gen_protect_reg(FC_OP1);
			gen_call_function_raw((void*)&FPU_PREP_PUSH); 
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_restore_reg(FC_OP1);
			gen_call_function_RR((void*)&FPU_FST,FC_OP1,FC_OP2);
			break;
		case 0x01: /* FXCH STi */
			dyn_fpu_top();
			gen_call_function_RR((void*)&FPU_FXCH,FC_OP1,FC_OP2);
			break;
		case 0x02: /* FNOP */
			gen_call_function_raw((void*)&FPU_FNOP);
			break;
		case 0x03: /* FSTP STi */
			dyn_fpu_top();
			gen_call_function_RR((void*)&FPU_FST,FC_OP1,FC_OP2);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;   
		case 0x04:
			switch(decode.modrm.rm){
			case 0x00:       /* FCHS */
				gen_call_function_raw((void*)&FPU_FCHS);
				break;
			case 0x01:       /* FABS */
				gen_call_function_raw((void*)&FPU_FABS);
				break;
			case 0x02:       /* UNKNOWN */
			case 0x03:       /* ILLEGAL */
				LOG(LOG_FPU,LOG_WARN)("ESC 1:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg), static_cast<uint32_t>(decode.modrm.rm));
				break;
			case 0x04:       /* FTST */
				gen_call_function_raw((void*)&FPU_FTST);
				break;
			case 0x05:       /* FXAM */
				gen_call_function_raw((void*)&FPU_FXAM);
				break;
			case 0x06:       /* FTSTP (cyrix)*/
			case 0x07:       /* UNKNOWN */
				LOG(LOG_FPU,LOG_WARN)("ESC 1:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
				break;
			}
			break;
		case 0x05:
			switch(decode.modrm.rm){	
			case 0x00:       /* FLD1 */
				gen_call_function_raw((void*)&FPU_FLD1);
				break;
			case 0x01:       /* FLDL2T */
				gen_call_function_raw((void*)&FPU_FLDL2T);
				break;
			case 0x02:       /* FLDL2E */
				gen_call_function_raw((void*)&FPU_FLDL2E);
				break;
			case 0x03:       /* FLDPI */
				gen_call_function_raw((void*)&FPU_FLDPI);
				break;
			case 0x04:       /* FLDLG2 */
				gen_call_function_raw((void*)&FPU_FLDLG2);
				break;
			case 0x05:       /* FLDLN2 */
				gen_call_function_raw((void*)&FPU_FLDLN2);
				break;
			case 0x06:       /* FLDZ*/
				gen_call_function_raw((void*)&FPU_FLDZ);
				break;
			case 0x07:       /* ILLEGAL */
				LOG(LOG_FPU,LOG_WARN)("ESC 1:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
				break;
			}
			break;
		case 0x06:
			switch(decode.modrm.rm){
			case 0x00:	/* F2XM1 */
				gen_call_function_raw((void*)&FPU_F2XM1);
				break;
			case 0x01:	/* FYL2X */
				gen_call_function_raw((void*)&FPU_FYL2X);
				break;
			case 0x02:	/* FPTAN  */
				gen_call_function_raw((void*)&FPU_FPTAN);
				break;
			case 0x03:	/* FPATAN */
				gen_call_function_raw((void*)&FPU_FPATAN);
				break;
			case 0x04:	/* FXTRACT */
				gen_call_function_raw((void*)&FPU_FXTRACT);
				break;
			case 0x05:	/* FPREM1 */
				gen_call_function_raw((void*)&FPU_FPREM1);
				break;
			case 0x06:	/* FDECSTP */
				gen_call_function_raw((void*)&FPU_FDECSTP);
				break;
			case 0x07:	/* FINCSTP */
				gen_call_function_raw((void*)&FPU_FINCSTP);
				break;
			default:
				LOG(LOG_FPU,LOG_WARN)("ESC 1:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
				break;
			}
			break;
		case 0x07:
			switch(decode.modrm.rm){
			case 0x00:		/* FPREM */
				gen_call_function_raw((void*)&FPU_FPREM);
				break;
			case 0x01:		/* FYL2XP1 */
				gen_call_function_raw((void*)&FPU_FYL2XP1);
				break;
			case 0x02:		/* FSQRT */
				gen_call_function_raw((void*)&FPU_FSQRT);
				break;
			case 0x03:		/* FSINCOS */
				gen_call_function_raw((void*)&FPU_FSINCOS);
				break;
			case 0x04:		/* FRNDINT */
				gen_call_function_raw((void*)&FPU_FRNDINT);
				break;
			case 0x05:		/* FSCALE */
				gen_call_function_raw((void*)&FPU_FSCALE);
				break;
			case 0x06:		/* FSIN */
				gen_call_function_raw((void*)&FPU_FSIN);
				break;
			case 0x07:		/* FCOS */
				gen_call_function_raw((void*)&FPU_FCOS);
				break;
			default:
				LOG(LOG_FPU,LOG_WARN)("ESC 1:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
				break;
			}
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 1:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
			break;
		}
	} else {
		switch(decode.modrm.reg){
		case 0x00: /* FLD float*/
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_OP1);
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FLD_F32,FC_OP1,FC_OP2);
			break;
		case 0x01: /* UNKNOWN */
			LOG(LOG_FPU,LOG_WARN)("ESC EA 1:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
			break;
		case 0x02: /* FST float*/
			dyn_fill_ea(FC_ADDR);
			gen_call_function_R((void*)&FPU_FST_F32,FC_ADDR);
			break;
		case 0x03: /* FSTP float*/
			dyn_fill_ea(FC_ADDR);
			gen_call_function_R((void*)&FPU_FST_F32,FC_ADDR);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		case 0x04: /* FLDENV */
			dyn_fill_ea(FC_ADDR);
			gen_call_function_R((void*)&FPU_FLDENV,FC_ADDR);
			break;
		case 0x05: /* FLDCW */
			dyn_fill_ea(FC_ADDR);
			gen_call_function_R((void *)&FPU_FLDCW,FC_ADDR);
			break;
		case 0x06: /* FSTENV */
			dyn_fill_ea(FC_ADDR);
			gen_call_function_R((void *)&FPU_FSTENV,FC_ADDR);
			break;
		case 0x07:  /* FNSTCW*/
			dyn_fill_ea(FC_ADDR);
			gen_call_function_R((void *)&FPU_FNSTCW,FC_ADDR);
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC EA 1:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
			break;
		}
	}
}

static void dyn_fpu_esc2(){
	dyn_get_modrm();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		switch(decode.modrm.reg){
		case 0x05:
			switch(decode.modrm.rm){
			case 0x01:		/* FUCOMPP */
				gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
				gen_add_imm(FC_OP2,1);
				gen_and_imm(FC_OP2,7);
				gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
				gen_call_function_RR((void *)&FPU_FUCOM,FC_OP1,FC_OP2);
				gen_call_function_raw((void *)&FPU_FPOP);
				gen_call_function_raw((void *)&FPU_FPOP);
				break;
			default:
				LOG(LOG_FPU,LOG_WARN)("ESC 2:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
				break;
			}
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 2:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
			break;
		}
	} else {
		dyn_fill_ea(FC_ADDR);
		gen_call_function_R((void*)&FPU_FLD_I32_EA,FC_ADDR); 
		gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
		dyn_eatree();
	}
}

static void dyn_fpu_esc3()
{
	dyn_get_modrm();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		switch (decode.modrm.reg) {
		case 0x04:
			switch (decode.modrm.rm) {
			case 0x00:				//FNENI
			case 0x01:				//FNDIS
				LOG(LOG_FPU, LOG_ERROR)("8087 only fpu code used esc 3: group 4: subfunction: %u",
				                        decode.modrm.rm);
				break;
			case 0x02:				//FNCLEX FCLEX
				gen_call_function_raw((void*)&FPU_FCLEX);
				break;
			case 0x03:				//FNINIT FINIT
				gen_call_function_raw((void*)&FPU_FINIT);
				break;
			case 0x04:				//FNSETPM
			case 0x05:				//FRSTPM
//				LOG(LOG_FPU,LOG_ERROR)("80267 protected mode (un)set. Nothing done");
				break;
			default:
				E_Exit("ESC 3:ILLEGAL OPCODE group %u subfunction %u",
				       decode.modrm.reg, decode.modrm.rm);
			}
			break;
		default:
			FPU_LOG_WARN(3, false, decode.modrm.reg, decode.modrm.rm);
			break;
		}
	} else {
		switch(decode.modrm.reg){
		case 0x00:	/* FILD */
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_OP1); 
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FLD_I32,FC_OP1,FC_OP2);
			break;
		case 0x01:	/* FISTTP */
			FPU_LOG_WARN(3, true, decode.modrm.reg, decode.modrm.rm);
			break;
		case 0x02:	/* FIST */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_I32,FC_ADDR);
			break;
		case 0x03:	/* FISTP */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_I32,FC_ADDR);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		case 0x05:	/* FLD 80 Bits Real */
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FLD_F80,FC_ADDR);
			break;
		case 0x07:	/* FSTP 80 Bits Real */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_F80,FC_ADDR);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		default:
			FPU_LOG_WARN(3, true, decode.modrm.reg, decode.modrm.rm);
		}
	}
}

static void dyn_fpu_esc4(){
	dyn_get_modrm();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		switch(decode.modrm.reg){
		case 0x00:	/* FADD STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FADD,FC_OP1,FC_OP2);
			break;
		case 0x01:	/* FMUL STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FMUL,FC_OP1,FC_OP2);
			break;
		case 0x02:  /* FCOM*/
			dyn_fpu_top();
			gen_call_function_RR((void*)&FPU_FCOM,FC_OP1,FC_OP2);
			break;
		case 0x03:  /* FCOMP*/
			dyn_fpu_top();
			gen_call_function_RR((void*)&FPU_FCOM,FC_OP1,FC_OP2);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		case 0x04:  /* FSUBR STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FSUBR,FC_OP1,FC_OP2);
			break;
		case 0x05:  /* FSUB  STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FSUB,FC_OP1,FC_OP2);
			break;
		case 0x06:  /* FDIVR STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FDIVR,FC_OP1,FC_OP2);
			break;
		case 0x07:  /* FDIV STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FDIV,FC_OP1,FC_OP2);
			break;
		default:
			break;
		}
	} else { 
		dyn_fill_ea(FC_ADDR);
		gen_call_function_R((void*)&FPU_FLD_F64_EA,FC_ADDR); 
		gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
		dyn_eatree();
	}
}

static void dyn_fpu_esc5(){
	dyn_get_modrm();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		dyn_fpu_top();
		switch(decode.modrm.reg){
		case 0x00: /* FFREE STi */
			gen_call_function_R((void*)&FPU_FFREE,FC_OP2);
			break;
		case 0x01: /* FXCH STi*/
			gen_call_function_RR((void*)&FPU_FXCH,FC_OP1,FC_OP2);
			break;
		case 0x02: /* FST STi */
			gen_call_function_RR((void*)&FPU_FST,FC_OP1,FC_OP2);
			break;
		case 0x03:  /* FSTP STi*/
			gen_call_function_RR((void*)&FPU_FST,FC_OP1,FC_OP2);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		case 0x04:	/* FUCOM STi */
			gen_call_function_RR((void*)&FPU_FUCOM,FC_OP1,FC_OP2);
			break;
		case 0x05:	/*FUCOMP STi */
			gen_call_function_RR((void*)&FPU_FUCOM,FC_OP1,FC_OP2);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 5:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
			break;
        }
	} else {
		switch(decode.modrm.reg){
		case 0x00:  /* FLD double real*/
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_OP1); 
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FLD_F64,FC_OP1,FC_OP2);
			break;
		case 0x01:  /* FISTTP longint*/
			LOG(LOG_FPU,LOG_WARN)("ESC 5 EA:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
			break;
		case 0x02:   /* FST double real*/
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_F64,FC_ADDR);
			break;
		case 0x03:	/* FSTP double real*/
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_F64,FC_ADDR);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		case 0x04:	/* FRSTOR */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FRSTOR,FC_ADDR);
			break;
		case 0x06:	/* FSAVE */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FSAVE,FC_ADDR);
			break;
		case 0x07:   /*FNSTSW */
			gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
			gen_call_function_R((void*)&FPU_SET_TOP,FC_OP1);
			dyn_fill_ea(FC_OP1); 
			gen_mov_word_to_reg(FC_OP2,(void*)(&fpu.sw),false);
			gen_call_function_RR((void*)&mem_writew,FC_OP1,FC_OP2);
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 5 EA:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
		}
	}
}

static void dyn_fpu_esc6(){
	dyn_get_modrm();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		switch(decode.modrm.reg){
		case 0x00:	/*FADDP STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FADD,FC_OP1,FC_OP2);
			break;
		case 0x01:	/* FMULP STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FMUL,FC_OP1,FC_OP2);
			break;
		case 0x02:  /* FCOMP5*/
			dyn_fpu_top();
			gen_call_function_RR((void*)&FPU_FCOM,FC_OP1,FC_OP2);
			break;	/* TODO IS THIS ALLRIGHT ????????? */
		case 0x03:  /*FCOMPP*/
			if(decode.modrm.rm != 1) {
				LOG(LOG_FPU,LOG_WARN)("ESC 6:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
				return;
			}
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_add_imm(FC_OP2,1);
			gen_and_imm(FC_OP2,7);
			gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FCOM,FC_OP1,FC_OP2);
			gen_call_function_raw((void*)&FPU_FPOP); /* extra pop at the bottom*/
			break;
		case 0x04:  /* FSUBRP STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FSUBR,FC_OP1,FC_OP2);
			break;
		case 0x05:  /* FSUBP  STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FSUB,FC_OP1,FC_OP2);
			break;
		case 0x06:	/* FDIVRP STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FDIVR,FC_OP1,FC_OP2);
			break;
		case 0x07:  /* FDIVP STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FDIV,FC_OP1,FC_OP2);
			break;
		default:
			break;
		}
		gen_call_function_raw((void*)&FPU_FPOP);		
	} else {
		dyn_fill_ea(FC_ADDR);
		gen_call_function_R((void*)&FPU_FLD_I16_EA,FC_ADDR); 
		gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
		dyn_eatree();
	}
}

static void dyn_fpu_esc7(){
	dyn_get_modrm();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		switch (decode.modrm.reg){
		case 0x00: /* FFREEP STi */
			dyn_fpu_top();
			gen_call_function_R((void*)&FPU_FFREE,FC_OP2);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		case 0x01: /* FXCH STi*/
			dyn_fpu_top();
			gen_call_function_RR((void*)&FPU_FXCH,FC_OP1,FC_OP2);
			break;
		case 0x02:  /* FSTP STi*/
		case 0x03:  /* FSTP STi*/
			dyn_fpu_top();
			gen_call_function_RR((void*)&FPU_FST,FC_OP1,FC_OP2);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		case 0x04:
			switch(decode.modrm.rm){
				case 0x00:     /* FNSTSW AX*/
					gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
					gen_call_function_R((void*)&FPU_SET_TOP,FC_OP1); 
					gen_mov_word_to_reg(FC_OP1,(void*)(&fpu.sw),false);
					MOV_REG_WORD16_FROM_HOST_REG(FC_OP1,DRC_REG_EAX);
					break;
				default:
					LOG(LOG_FPU,LOG_WARN)("ESC 7:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
					break;
			}
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 7:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
			break;
		}
	} else {
		switch(decode.modrm.reg){
		case 0x00:  /* FILD int16_t */
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_OP1); 
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FLD_I16,FC_OP1,FC_OP2);
			break;
		case 0x01:
			LOG(LOG_FPU,LOG_WARN)("ESC 7 EA:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
			break;
		case 0x02:   /* FIST int16_t */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_I16,FC_ADDR);
			break;
		case 0x03:	/* FISTP int16_t */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_I16,FC_ADDR);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		case 0x04:   /* FBLD packed BCD */
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_OP1);
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FBLD,FC_OP1,FC_OP2);
			break;
		case 0x05:  /* FILD int64_t */
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_OP1);
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FLD_I64,FC_OP1,FC_OP2);
			break;
		case 0x06:	/* FBSTP packed BCD */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FBST,FC_ADDR);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		case 0x07:  /* FISTP int64_t */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_I64,FC_ADDR);
			gen_call_function_raw((void*)&FPU_FPOP);
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 7 EA:Unhandled group %X subfunction %X",static_cast<uint32_t>(decode.modrm.reg),static_cast<uint32_t>(decode.modrm.rm));
			break;
		}
	}
}

#endif
