// SPDX-FileCopyrightText:  2021-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

	CASE_0F_W(0x00)												/* GRP 6 Exxx */
		{
			if ((reg_flags & FLAG_VM) || (!cpu.pmode)) goto illegal_opcode;
			GetRM;Bitu which=(rm>>3)&7;
			switch (which) {
			case 0x00:	/* SLDT */
			case 0x01:	/* STR */
				{
					Bitu saveval;
					if (!which) saveval=CPU_SLDT();
					else saveval=CPU_STR();
					if (rm >= 0xc0) {GetEArw;*earw=saveval;}
					else {GetEAa;SaveMw(eaa,saveval);}
				}
				break;
			case 0x02:case 0x03:case 0x04:case 0x05:
				{
					Bitu loadval;
					if (rm >= 0xc0 ) {GetEArw;loadval=*earw;}
					else {GetEAa;loadval=LoadMw(eaa);}
					switch (which) {
					case 0x02:
						if (cpu.cpl) EXCEPTION(EXCEPTION_GP);
						if (CPU_LLDT(loadval)) RUNEXCEPTION();
						break;
					case 0x03:
						if (cpu.cpl) EXCEPTION(EXCEPTION_GP);
						if (CPU_LTR(loadval)) RUNEXCEPTION();
						break;
					case 0x04:
						CPU_VERR(loadval);
						break;
					case 0x05:
						CPU_VERW(loadval);
						break;
					}
				}
				break;
			default:
				goto illegal_opcode;
			}
		}
		break;
	CASE_0F_W(0x01)												/* Group 7 Ew */
		{
			GetRM;Bitu which=(rm>>3)&7;
			if (rm < 0xc0)	{ //First ones all use EA
				GetEAa;Bitu limit;
				switch (which) {
				case 0x00:										/* SGDT */
					SaveMw(eaa,CPU_SGDT_limit());
					SaveMd(eaa+2,CPU_SGDT_base());
					break;
				case 0x01:										/* SIDT */
					SaveMw(eaa,CPU_SIDT_limit());
					SaveMd(eaa+2,CPU_SIDT_base());
					break;
				case 0x02:										/* LGDT */
					if (cpu.pmode && cpu.cpl) EXCEPTION(EXCEPTION_GP);
					CPU_LGDT(LoadMw(eaa),LoadMd(eaa+2) & 0xFFFFFF);
					break;
				case 0x03:										/* LIDT */
					if (cpu.pmode && cpu.cpl) EXCEPTION(EXCEPTION_GP);
					CPU_LIDT(LoadMw(eaa),LoadMd(eaa+2) & 0xFFFFFF);
					break;
				case 0x04:										/* SMSW */
					SaveMw(eaa,CPU_SMSW());
					break;
				case 0x06:										/* LMSW */
					limit=LoadMw(eaa);
					if (CPU_LMSW(limit)) RUNEXCEPTION();
					break;
				case 0x07:										/* INVLPG */
					if (cpu.pmode && cpu.cpl) EXCEPTION(EXCEPTION_GP);
					PAGING_ClearTLB();
					break;
				}
			} else {
				GetEArw;
				switch (which) {
				case 0x02:										/* LGDT */
				case 0x03:										/* LIDT */
					if (cpu.pmode && cpu.cpl) EXCEPTION(EXCEPTION_GP);
					goto illegal_opcode;
				case 0x04:										/* SMSW */
					*earw=CPU_SMSW();
					break;
				case 0x06:										/* LMSW */
					if (CPU_LMSW(*earw)) RUNEXCEPTION();
					break;
				default:
					goto illegal_opcode;
				}
			}
		}
		break;
	CASE_0F_W(0x02)												/* LAR Gw,Ew */
		{
			if ((reg_flags & FLAG_VM) || (!cpu.pmode)) goto illegal_opcode;
			GetRMrw;Bitu ar=*rmrw;
			if (rm >= 0xc0) {
				GetEArw;CPU_LAR(*earw,ar);
			} else {
				GetEAa;CPU_LAR(LoadMw(eaa),ar);
			}
			*rmrw=(uint16_t)ar;
		}
		break;
	CASE_0F_W(0x03)												/* LSL Gw,Ew */
		{
			if ((reg_flags & FLAG_VM) || (!cpu.pmode)) goto illegal_opcode;
			GetRMrw;Bitu limit=*rmrw;
			if (rm >= 0xc0) {
				GetEArw;CPU_LSL(*earw,limit);
			} else {
				GetEAa;CPU_LSL(LoadMw(eaa),limit);
			}
			*rmrw=(uint16_t)limit;
		}
		break;
	CASE_0F_B(0x06)												/* CLTS */
		if (cpu.pmode && cpu.cpl) EXCEPTION(EXCEPTION_GP);
		cpu.cr0&=(~CR0_TASKSWITCH);
		break;
	CASE_0F_B(0x08)												/* INVD */
	CASE_0F_B(0x09)												/* WBINVD */
		if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
		if (cpu.pmode && cpu.cpl) EXCEPTION(EXCEPTION_GP);
		break;
	CASE_0F_B(0x20)												/* MOV Rd.CRx */
		{
			GetRM;
			Bitu which=(rm >> 3) & 7;
			if (rm < 0xc0 ) {
				rm |= 0xc0;
				LOG(LOG_CPU,LOG_ERROR)("MOV XXX,CR%u with non-register", static_cast<uint32_t>(which));
			}
			GetEArd;
			uint32_t crx_value;
			if (CPU_READ_CRX(which,crx_value)) RUNEXCEPTION();
			*eard=crx_value;
		}
		break;
	CASE_0F_B(0x21)												/* MOV Rd,DRx */
		{
			GetRM;
			Bitu which=(rm >> 3) & 7;
			if (rm < 0xc0 ) {
				rm |= 0xc0;
				LOG(LOG_CPU,LOG_ERROR)("MOV XXX,DR%u with non-register", static_cast<uint32_t>(which));
			}
			GetEArd;
			uint32_t drx_value;
			if (CPU_READ_DRX(which,drx_value)) RUNEXCEPTION();
			*eard=drx_value;
		}
		break;
	CASE_0F_B(0x22)												/* MOV CRx,Rd */
		{
			GetRM;
			Bitu which=(rm >> 3) & 7;
			if (rm < 0xc0 ) {
				rm |= 0xc0;
				LOG(LOG_CPU,LOG_ERROR)("MOV XXX,CR%u with non-register", static_cast<uint32_t>(which));
			}
			GetEArd;
			if (CPU_WRITE_CRX(which,*eard)) RUNEXCEPTION();
		}
		break;
	CASE_0F_B(0x23)												/* MOV DRx,Rd */
		{
			GetRM;
			Bitu which=(rm >> 3) & 7;
			if (rm < 0xc0 ) {
				rm |= 0xc0;
				LOG(LOG_CPU,LOG_ERROR)("MOV DR%u,XXX with non-register", static_cast<uint32_t>(which));
			}
			GetEArd;
			if (CPU_WRITE_DRX(which,*eard)) RUNEXCEPTION();
		}
		break;
	CASE_0F_B(0x24)												/* MOV Rd,TRx */
		{
			GetRM;
			Bitu which=(rm >> 3) & 7;
			if (rm < 0xc0 ) {
				rm |= 0xc0;
				LOG(LOG_CPU,LOG_ERROR)("MOV XXX,TR%u with non-register", static_cast<uint32_t>(which));
			}
			GetEArd;
			uint32_t trx_value;
			if (CPU_READ_TRX(which,trx_value)) RUNEXCEPTION();
			*eard=trx_value;
		}
		break;
	CASE_0F_B(0x26)												/* MOV TRx,Rd */
		{
			GetRM;
			Bitu which=(rm >> 3) & 7;
			if (rm < 0xc0 ) {
				rm |= 0xc0;
				LOG(LOG_CPU,LOG_ERROR)("MOV TR%u,XXX with non-register", static_cast<uint32_t>(which));
			}
			GetEArd;
			if (CPU_WRITE_TRX(which,*eard)) RUNEXCEPTION();
		}
		break;
	CASE_0F_B(0x31)												/* RDTSC */
		{
	                if (CPU_ArchitectureType < ArchitectureType::Pentium) {
		                goto illegal_opcode;
	                }
	                CPU_ReadTSC();
                }
                break;
	CASE_0F_W(0x80) /* JO */
		JumpCond16_w(TFLG_O);
		break;
	CASE_0F_W(0x81) /* JNO */
		JumpCond16_w(TFLG_NO);break;
	CASE_0F_W(0x82)												/* JB */
		JumpCond16_w(TFLG_B);break;
	CASE_0F_W(0x83)												/* JNB */
		JumpCond16_w(TFLG_NB);break;
	CASE_0F_W(0x84)												/* JZ */
		JumpCond16_w(TFLG_Z);break;
	CASE_0F_W(0x85)												/* JNZ */
		JumpCond16_w(TFLG_NZ);break;
	CASE_0F_W(0x86)												/* JBE */
		JumpCond16_w(TFLG_BE);break;
	CASE_0F_W(0x87)												/* JNBE */
		JumpCond16_w(TFLG_NBE);break;
	CASE_0F_W(0x88)												/* JS */
		JumpCond16_w(TFLG_S);break;
	CASE_0F_W(0x89)												/* JNS */
		JumpCond16_w(TFLG_NS);break;
	CASE_0F_W(0x8a)												/* JP */
		JumpCond16_w(TFLG_P);break;
	CASE_0F_W(0x8b)												/* JNP */
		JumpCond16_w(TFLG_NP);break;
	CASE_0F_W(0x8c)												/* JL */
		JumpCond16_w(TFLG_L);break;
	CASE_0F_W(0x8d)												/* JNL */
		JumpCond16_w(TFLG_NL);break;
	CASE_0F_W(0x8e)												/* JLE */
		JumpCond16_w(TFLG_LE);break;
	CASE_0F_W(0x8f)												/* JNLE */
		JumpCond16_w(TFLG_NLE);break;
	CASE_0F_B(0x90)												/* SETO */
		SETcc(TFLG_O);break;
	CASE_0F_B(0x91)												/* SETNO */
		SETcc(TFLG_NO);break;
	CASE_0F_B(0x92)												/* SETB */
		SETcc(TFLG_B);break;
	CASE_0F_B(0x93)												/* SETNB */
		SETcc(TFLG_NB);break;
	CASE_0F_B(0x94)												/* SETZ */
		SETcc(TFLG_Z);break;
	CASE_0F_B(0x95)												/* SETNZ */
		SETcc(TFLG_NZ);	break;
	CASE_0F_B(0x96)												/* SETBE */
		SETcc(TFLG_BE);break;
	CASE_0F_B(0x97)												/* SETNBE */
		SETcc(TFLG_NBE);break;
	CASE_0F_B(0x98)												/* SETS */
		SETcc(TFLG_S);break;
	CASE_0F_B(0x99)												/* SETNS */
		SETcc(TFLG_NS);break;
	CASE_0F_B(0x9a)												/* SETP */
		SETcc(TFLG_P);break;
	CASE_0F_B(0x9b)												/* SETNP */
		SETcc(TFLG_NP);break;
	CASE_0F_B(0x9c)												/* SETL */
		SETcc(TFLG_L);break;
	CASE_0F_B(0x9d)												/* SETNL */
		SETcc(TFLG_NL);break;
	CASE_0F_B(0x9e)												/* SETLE */
		SETcc(TFLG_LE);break;
	CASE_0F_B(0x9f)												/* SETNLE */
		SETcc(TFLG_NLE);break;

	CASE_0F_W(0xa0)												/* PUSH FS */		
		Push_16(SegValue(fs));break;
	CASE_0F_W(0xa1)												/* POP FS */	
		if (CPU_PopSeg(fs,false)) RUNEXCEPTION();
		break;
	CASE_0F_B(0xa2)												/* CPUID */
		if (!CPU_CPUID()) goto illegal_opcode;
		break;
	CASE_0F_W(0xa3)												/* BT Ew,Gw */
		{
			FillFlags();GetRMrw;
			uint16_t mask=1 << (*rmrw & 15);
			if (rm >= 0xc0 ) {
				GetEArw;
				SETFLAGBIT(CF,(*earw & mask));
			} else {
				GetEAa;eaa+=(((int16_t)*rmrw)>>4)*2;
				if (!TEST_PREFIX_ADDR) FixEA16;
				uint16_t old=LoadMw(eaa);
				SETFLAGBIT(CF,(old & mask));
			}
			break;
		}
	CASE_0F_W(0xa4)												/* SHLD Ew,Gw,Ib */
		RMEwGwOp3(DSHLW,Fetchb());
		break;
	CASE_0F_W(0xa5)												/* SHLD Ew,Gw,CL */
		RMEwGwOp3(DSHLW,reg_cl);
		break;
	CASE_0F_W(0xa8)												/* PUSH GS */		
		Push_16(SegValue(gs));break;
	CASE_0F_W(0xa9)												/* POP GS */		
		if (CPU_PopSeg(gs,false)) RUNEXCEPTION();
		break;
	CASE_0F_W(0xab)												/* BTS Ew,Gw */
		{
			FillFlags();GetRMrw;
			uint16_t mask=1 << (*rmrw & 15);
			if (rm >= 0xc0 ) {
				GetEArw;
				SETFLAGBIT(CF,(*earw & mask));
				*earw|=mask;
			} else {
				GetEAa;eaa+=(((int16_t)*rmrw)>>4)*2;
				if (!TEST_PREFIX_ADDR) FixEA16;
				uint16_t old=LoadMw(eaa);
				SETFLAGBIT(CF,(old & mask));
				SaveMw(eaa,old | mask);
			}
			break;
		}
	CASE_0F_W(0xac)												/* SHRD Ew,Gw,Ib */
		RMEwGwOp3(DSHRW,Fetchb());
		break;
	CASE_0F_W(0xad)												/* SHRD Ew,Gw,CL */
		RMEwGwOp3(DSHRW,reg_cl);
		break;
	CASE_0F_W(0xaf)												/* IMUL Gw,Ew */
		RMGwEwOp3(DIMULW,*rmrw);
		break;
	CASE_0F_B(0xb0) 										/* cmpxchg Eb,Gb */
		{
			if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
			FillFlags();
			GetRMrb;
			if (rm >= 0xc0 ) {
				GetEArb;
				if (reg_al == *earb) {
					*earb=*rmrb;
					SETFLAGBIT(ZF,1);
				} else {
					reg_al = *earb;
					SETFLAGBIT(ZF,0);
				}
			} else {
   				GetEAa;
				uint8_t val = LoadMb(eaa);
				if (reg_al == val) { 
					SaveMb(eaa,*rmrb);
					SETFLAGBIT(ZF,1);
				} else {
					SaveMb(eaa,val);	// cmpxchg always issues a write
					reg_al = val;
					SETFLAGBIT(ZF,0);
				}
			}
			break;
		}
	CASE_0F_W(0xb1) 									/* cmpxchg Ew,Gw */
		{
			if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
			FillFlags();
			GetRMrw;
			if (rm >= 0xc0 ) {
				GetEArw;
				if(reg_ax == *earw) { 
					*earw = *rmrw;
					SETFLAGBIT(ZF,1);
				} else {
					reg_ax = *earw;
					SETFLAGBIT(ZF,0);
				}
			} else {
   				GetEAa;
				uint16_t val = LoadMw(eaa);
				if(reg_ax == val) { 
					SaveMw(eaa,*rmrw);
					SETFLAGBIT(ZF,1);
				} else {
					SaveMw(eaa,val);	// cmpxchg always issues a write
					reg_ax = val;
					SETFLAGBIT(ZF,0);
				}
			}
			break;
		}

	CASE_0F_W(0xb2)												/* LSS Ew */
		{	
			GetRMrw;
			if (rm >= 0xc0) goto illegal_opcode;
			GetEAa;
			if (CPU_SetSegGeneral(ss,LoadMw(eaa+2))) RUNEXCEPTION();
			*rmrw=LoadMw(eaa);
			break;
		}
	CASE_0F_W(0xb3)												/* BTR Ew,Gw */
		{
			FillFlags();GetRMrw;
			uint16_t mask=1 << (*rmrw & 15);
			if (rm >= 0xc0 ) {
				GetEArw;
				SETFLAGBIT(CF,(*earw & mask));
				*earw&= ~mask;
			} else {
				GetEAa;eaa+=(((int16_t)*rmrw)>>4)*2;
				if (!TEST_PREFIX_ADDR) FixEA16;
				uint16_t old=LoadMw(eaa);
				SETFLAGBIT(CF,(old & mask));
				SaveMw(eaa,old & ~mask);
			}
			break;
		}
	CASE_0F_W(0xb4)												/* LFS Ew */
		{	
			GetRMrw;
			if (rm >= 0xc0) goto illegal_opcode;
			GetEAa;
			if (CPU_SetSegGeneral(fs,LoadMw(eaa+2))) RUNEXCEPTION();
			*rmrw=LoadMw(eaa);
			break;
		}
	CASE_0F_W(0xb5)												/* LGS Ew */
		{	
			GetRMrw;
			if (rm >= 0xc0) goto illegal_opcode;
			GetEAa;
			if (CPU_SetSegGeneral(gs,LoadMw(eaa+2))) RUNEXCEPTION();
			*rmrw=LoadMw(eaa);
			break;
		}
	CASE_0F_W(0xb6)												/* MOVZX Gw,Eb */
		{
			GetRMrw;															
			if (rm >= 0xc0 ) {GetEArb;*rmrw=*earb;}
			else {GetEAa;*rmrw=LoadMb(eaa);}
			break;
		}
	CASE_0F_W(0xb7)												/* MOVZX Gw,Ew */
	CASE_0F_W(0xbf)												/* MOVSX Gw,Ew */
		{
			GetRMrw;															
			if (rm >= 0xc0 ) {GetEArw;*rmrw=*earw;}
			else {GetEAa;*rmrw=LoadMw(eaa);}
			break;
		}
	CASE_0F_W(0xba)												/* GRP8 Ew,Ib */
		{
			FillFlags();GetRM;
			if (rm >= 0xc0 ) {
				GetEArw;
				uint16_t mask=1 << (Fetchb() & 15);
				SETFLAGBIT(CF,(*earw & mask));
				switch (rm & 0x38) {
				case 0x20:										/* BT */
					break;
				case 0x28:										/* BTS */
					*earw|=mask;
					break;
				case 0x30:										/* BTR */
					*earw&= ~mask;
					break;
				case 0x38:										/* BTC */
					*earw^=mask;
					break;
				default:
					E_Exit("CPU:0F:BA:Illegal subfunction %X",rm & 0x38);
				}
			} else {
				GetEAa;uint16_t old=LoadMw(eaa);
				uint16_t mask=1 << (Fetchb() & 15);
				SETFLAGBIT(CF,(old & mask));
				switch (rm & 0x38) {
				case 0x20:										/* BT */
					break;
				case 0x28:										/* BTS */
					SaveMw(eaa,old|mask);
					break;
				case 0x30:										/* BTR */
					SaveMw(eaa,old & ~mask);
					break;
				case 0x38:										/* BTC */
					SaveMw(eaa,old ^ mask);
					break;
				default:
					E_Exit("CPU:0F:BA:Illegal subfunction %X",rm & 0x38);
				}
			}
			break;
		}
	CASE_0F_W(0xbb)												/* BTC Ew,Gw */
		{
			FillFlags();GetRMrw;
			uint16_t mask=1 << (*rmrw & 15);
			if (rm >= 0xc0 ) {
				GetEArw;
				SETFLAGBIT(CF,(*earw & mask));
				*earw^=mask;
			} else {
				GetEAa;eaa+=(((int16_t)*rmrw)>>4)*2;
				if (!TEST_PREFIX_ADDR) FixEA16;
				uint16_t old=LoadMw(eaa);
				SETFLAGBIT(CF,(old & mask));
				SaveMw(eaa,old ^ mask);
			}
			break;
		}
	CASE_0F_W(0xbc)												/* BSF Gw,Ew */
		{
			GetRMrw;
			uint16_t result,value;
			if (rm >= 0xc0) { GetEArw; value=*earw; } 
			else			{ GetEAa; value=LoadMw(eaa); }
			if (value==0) {
				SETFLAGBIT(ZF,true);
			} else {
				result = 0;
				while ((value & 0x01)==0) { result++; value>>=1; }
				SETFLAGBIT(ZF,false);
				*rmrw = result;
			}
			lflags.type=t_UNKNOWN;
			break;
		}
	CASE_0F_W(0xbd)												/* BSR Gw,Ew */
		{
			GetRMrw;
			uint16_t result,value;
			if (rm >= 0xc0) { GetEArw; value=*earw; } 
			else			{ GetEAa; value=LoadMw(eaa); }
			if (value==0) {
				SETFLAGBIT(ZF,true);
			} else {
				result = 15;	// Operandsize-1
				while ((value & 0x8000)==0) { result--; value<<=1; }
				SETFLAGBIT(ZF,false);
				*rmrw = result;
			}
			lflags.type=t_UNKNOWN;
			break;
		}
	CASE_0F_W(0xbe)												/* MOVSX Gw,Eb */
		{
			GetRMrw;															
			if (rm >= 0xc0 ) {GetEArb;*rmrw=*(int8_t *)earb;}
			else {GetEAa;*rmrw=LoadMbs(eaa);}
			break;
		}
	CASE_0F_B(0xc0)												/* XADD Gb,Eb */
		{
			if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
			GetRMrb;uint8_t oldrmrb=*rmrb;
			if (rm >= 0xc0 ) {GetEArb;*rmrb=*earb;*earb+=oldrmrb;}
			else {GetEAa;*rmrb=LoadMb(eaa);SaveMb(eaa,LoadMb(eaa)+oldrmrb);}
			break;
		}
	CASE_0F_W(0xc1)												/* XADD Gw,Ew */
		{
			if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
			GetRMrw;uint16_t oldrmrw=*rmrw;
			if (rm >= 0xc0 ) {GetEArw;*rmrw=*earw;*earw+=oldrmrw;}
			else {GetEAa;*rmrw=LoadMw(eaa);SaveMw(eaa,LoadMw(eaa)+oldrmrw);}
			break;
		}
	CASE_0F_W(0xc8)												/* BSWAP AX */
		if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
		BSWAPW(reg_ax);break;
	CASE_0F_W(0xc9)												/* BSWAP CX */
		if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
		BSWAPW(reg_cx);break;
	CASE_0F_W(0xca)												/* BSWAP DX */
		if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
		BSWAPW(reg_dx);break;
	CASE_0F_W(0xcb)												/* BSWAP BX */
		if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
		BSWAPW(reg_bx);break;
	CASE_0F_W(0xcc)												/* BSWAP SP */
		if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
		BSWAPW(reg_sp);break;
	CASE_0F_W(0xcd)												/* BSWAP BP */
		if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
		BSWAPW(reg_bp);break;
	CASE_0F_W(0xce)												/* BSWAP SI */
		if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
		BSWAPW(reg_si);break;
	CASE_0F_W(0xcf)												/* BSWAP DI */
		if (CPU_ArchitectureType<ArchitectureType::Intel486OldSlow) goto illegal_opcode;
		BSWAPW(reg_di);break;

#define CASE_0F_MMX(x) CASE_0F_W(x)
#include "prefix_0f_mmx.h"
#undef CASE_0F_MMX
