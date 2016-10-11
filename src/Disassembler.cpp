/*
 * Disassembler.cpp
 *
 *  Created on: 12.11.2014
 *      Author: mueller
 */

#define __STDC_FORMAT_MACROS // Work around for older g++

#include "Disassembler.h"
#include "utils.h"

#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cinttypes>

#include "Disassembler.tables.cpp"


void Disassembler::append(const char* str)
{	auto len = strlen(str);
	memcpy(CodeAt, str, min((size_t)(Code + sizeof Code - CodeAt), len));
	CodeAt += len;
}
void Disassembler::appendf(const char* fmt, ...)
{	va_list va;
	va_start(va, fmt);
	CodeAt += vsnprintf(CodeAt, Code + sizeof Code - CodeAt, fmt, va);
	va_end(va);
}

void Disassembler::appendImmd(qpuValue value)
{	// Check whether likely a float, just a guess
	if (UseFloat && abs(((value.iValue >> 23) & 0xff) ^ 0x80) <= 20)
		CodeAt += snprintf(CodeAt, Code + sizeof Code - CodeAt, "%.6e", value.fValue);
	else if (abs(value.iValue) < 256)
		appendf("%i", value.iValue);
	else
		appendf("0x%x", value.uValue);
}

void Disassembler::appendPE(bool sign)
{	uint32_t val = Instruct.Immd.uValue;
	signed char values[16];
	if (sign)
		for (int pos = 16; pos--; val <<= 1)
			values[pos] = (((int32_t)val >> 30) & -2) | ((val >> 15) & 1);
	else
		for (int pos = 16; pos--; val <<= 1)
			values[pos] = ((val >> 30) & 2) | ((val >> 15) & 1);
	*CodeAt++ = '[';
	for (int val : values)
		appendf("%i,", val);
	CodeAt[-1] = ']';
}

void Disassembler::appendPack(bool mul)
{	if (Instruct.PM ? mul : Instruct.WS == mul)
	{	append(cPack[Instruct.PM][Instruct.Pack & 15]);
		append(cPUPX[Instruct.Pack / Inst::P_INT]);
	}
}

void Disassembler::appendSource(Inst::mux mux)
{	switch (mux)
	{case Inst::X_RA:
		append(cRreg[0][Instruct.RAddrA]);
		break;
	 case Inst::X_RB:
		if (Instruct.Sig == Inst::S_SMI)
		{	append(cSMI[Instruct.SImmd]);
			break;
		}
		append(cRreg[1][Instruct.RAddrB]);
		break;
	 default:
		append(Inst::toString(mux));
	}
	if ( (Instruct.PM && mux == Inst::X_R4) // r4 unpack
		|| (!Instruct.PM && mux == Inst::X_RA) ) // RA unpack
	{	append(cUnpack[Instruct.Unpack & 7]);
		append(cPUPX[Instruct.Unpack / Inst::U_INT]);
	}
}

void Disassembler::appendMULSource(Inst::mux mux)
{	append(", ");
	appendSource(mux);

	if ( Instruct.Sig != Inst::S_SMI || Instruct.SImmd < 48 // no rotation
		|| (mux == Inst::X_RA && Instruct.RAddrA == 32) // register not sensitive to rotation
		|| mux == Inst::X_RB )                          // small immediate value also not
		return;
	int rot = Instruct.SImmd - 48; // [0..15]
	if (rot == 0)
	{	append(">>r5");
		return;
	}
	if (rot >= 8)
		rot -= 16;
	if (mux >= Inst::X_R4)
		rot %= 4;
	if (rot < 0)
		appendf("<<%i", -rot);
	else
		appendf(">>%i", rot);
}

void Disassembler::DoADD()
{
	uint8_t opa = Instruct.OpA;
	bool isUnary = Instruct.isUnary();
	bool isImmd = Instruct.Sig == Inst::S_SMI && Instruct.MuxAA == Inst::X_RB
		&& (isUnary || Instruct.MuxAB == Inst::X_RB);

	if ( UseMOV
		&& ( (Instruct.MuxAA == Instruct.MuxAB && (0x807c2000 & (1<<opa))) // Both inputs equal and instruction that returns identity or 0
			|| isImmd ))
		opa = 32;

	append(cOpA[opa]);

	if (Instruct.isSFADD())
		append(".setf");

	if (opa == Inst::A_NOP && Instruct.WAddrA == Inst::R_NOP)
		return;

	append(cCC[Instruct.CondA]);
	append(" ");

	// Parameters for ADD ALU
	// Target
	append(cWreg[Instruct.WS][Instruct.WAddrA]);
	bool unpack = false;
	if (UseMOV && isImmd && (0x0909 & (1<<Instruct.Pack)))
		unpack = true;
	else
		appendPack(false);

	switch (opa)
	{case Inst::A_NOP:
		return;

	 case 32:
		switch (Instruct.OpA)
		{case Inst::A_SUB:
		 case Inst::A_XOR:
		 case Inst::A_V8SUBS:
			return append(", 0");
		 default:;
		}
		if (isImmd)
		{	// constant => evaluate
			auto value(Instruct.SMIValue());
			Instruct.evalADD(value, value);
			if (unpack)
				Instruct.evalPack(value, value, false);
			append(", ");
			appendImmd(value);
			return;
		}
	}

	append(", ");
	appendSource(Instruct.MuxAA);

	if (opa != 32 && !isUnary)
	{	append(", ");
		appendSource(Instruct.MuxAB);
	}
}

void Disassembler::DoMUL()
{
	if (!Instruct.isMUL())
		return;

	uint8_t opm = Instruct.OpM;
	bool isSMI = Instruct.Sig == Inst::S_SMI && Instruct.MuxMA == Inst::X_RB;

	if ( UseMOV && Instruct.MuxMA == Instruct.MuxMB // Both inputs equal and
		&& ( (0xb0 & (1<<opm)) // instruction that returns identity or 0 or
			|| isSMI )) // small immediate
		opm = 8;

	append(";  ");
	append(cOpM[opm]);

	if (Instruct.isSFMUL())
		append(".setf");

	append(cCC[Instruct.CondM]);

	append(" ");
	append(cWreg[!Instruct.WS][Instruct.WAddrM]);
	bool unpack = false;
	if (opm == 8 && isSMI && (0x0009 & (1<<Instruct.Pack)))
		unpack = true;
	else
		appendPack(true);

	switch (opm)
	{case Inst::M_NOP:
		return;

	 default:
		appendMULSource(Instruct.MuxMA);
		break;

	 case 8:
		if (Instruct.OpM == Inst::M_V8SUBS)
			return append(", 0");
		if (isSMI)
		{	// constant => evaluate
			auto value(Instruct.SMIValue());
			Instruct.evalMUL(value, value);
			if (unpack)
				Instruct.evalPack(value, value, true);
			append(", ");
			appendImmd(value);
			return;
		}
	}
	appendMULSource(Instruct.MuxMB);
}

void Disassembler::DoRead(Inst::mux regfile)
{
	if ( Instruct.MuxAA != regfile && Instruct.MuxAB != regfile
		&& Instruct.MuxMA != regfile && Instruct.MuxMB != regfile )
	{	append(";  read ");
		appendSource(regfile);
	}
}

void Disassembler::DoALU()
{
	if (PrintFields)
		sprintf(Comment, " sig%X ra%02d rb%02d pm%d upk%d pck%X"
		                 " Aop%02d Acc%d Aw%02d Aa%d Ab%d"
		                 " Mop%d Mcc%d Mw%02d Ma%d Mb%d"
		                 " sf%d ws%d",
			Instruct.Sig, Instruct.RAddrA, Instruct.RAddrB, Instruct.PM, Instruct.Unpack, Instruct.Pack,
			Instruct.OpA, Instruct.CondA, Instruct.WAddrA, Instruct.MuxAA, Instruct.MuxAB,
			Instruct.OpM, Instruct.CondM, Instruct.WAddrM, Instruct.MuxMA, Instruct.MuxMB,
			Instruct.SF, Instruct.WS);

	DoADD();

	DoMUL();

	if (Instruct.RAddrA != Inst::R_NOP)
		DoRead(Inst::X_RA);
	if ( Instruct.Sig == Inst::S_SMI
		? Instruct.SImmd < 48
		: Instruct.RAddrB != Inst::R_NOP )
	DoRead(Inst::X_RB);

	append(cOpS[Instruct.Sig]);
}

void Disassembler::DoLDI()
{
	if (PrintFields)
		sprintf(Comment, " md%d pm%d pck%X"
		                 " Acc%d Aw%02d Mcc%d Mw%02d"
		                 " sf%d ws%d",
			Instruct.LdMode, Instruct.PM, Instruct.Pack,
			Instruct.CondA, Instruct.WAddrA, Instruct.CondM, Instruct.WAddrM,
			Instruct.SF, Instruct.WS);

	append(cOpL[Instruct.LdMode]);
	if (Instruct.LdMode >= Inst::L_SEMA)
		append(Instruct.SA() ? "acq" : "rel");
	if (Instruct.SF)
		append(".setf");
	append(" ");

	if (Instruct.WAddrA != Inst::R_NOP)
	{	append(cWreg[Instruct.WS][Instruct.WAddrA]);
		appendPack(false);
		append(cCC[Instruct.CondA]);
		append(", ");
	}
	if (Instruct.WAddrM != Inst::R_NOP)
	{	append(cWreg[!Instruct.WS][Instruct.WAddrM]);
		appendPack(true);
		append(cCC[Instruct.CondM]);
		append(", ");
	}
	if (Instruct.WAddrA == Inst::R_NOP && Instruct.WAddrM == Inst::R_NOP)
		append("-, ");

	switch (Instruct.LdMode)
	{default:
		return appendImmd(Instruct.Immd);
	 case Inst::L_PES:
		return appendPE(true);
	 case Inst::L_PEU:
		return appendPE(false);
	}
}

void Disassembler::DoBranch()
{
	if (PrintFields)
		sprintf(Comment, " rel%d reg%d ra%02d pm%d upk%d Bcc%X"
		                 " Aw%02d Mw%02d ws%d",
			Instruct.Rel, Instruct.Reg, Instruct.RAddrA, Instruct.PM, Instruct.Unpack, Instruct.CondBr,
			Instruct.WAddrA, Instruct.WAddrM, Instruct.WS);

	appendf("%s%s ", Instruct.Rel ? "brr" : "bra", cBCC[Instruct.CondBr]);
	if (Instruct.WAddrA != Inst::R_NOP)
		appendf("%s, ", cWreg[Instruct.WS][Instruct.WAddrA]);
	if (Instruct.WAddrM != Inst::R_NOP)
		appendf("%s, ", cWreg[!Instruct.WS][Instruct.WAddrM]);
	if (Instruct.WAddrA == Inst::R_NOP && Instruct.WAddrM == Inst::R_NOP)
		append("-, ");
	if (Instruct.Reg)
	{	append(cRreg[0][Instruct.RAddrA]);
		if (Instruct.Immd.iValue)
			append(", ");
	} else if (Instruct.WAddrA != Inst::R_NOP && Instruct.WAddrM != Inst::R_NOP)
		append("-, ");
	// try label
	if (Instruct.Immd.iValue)
	{	uint32_t target = Instruct.Immd.uValue;
		if (Instruct.Rel)
			target += Addr + 4*sizeof(uint64_t);
		auto l = Labels.find(target);
		if (l != Labels.end())
		{ int label_num;
			return appendf(Instruct.Rel ? "r:%s%s" : ":%s%s",
					l->second.c_str(),
					// Add 'f' if branch jumps forward to local label.
					(sscanf(l->second.c_str(), "%i", &label_num)
					 && target > Addr+4*sizeof(uint64_t))?"f":"");
			return appendf(Instruct.Rel ? "r:%s" : ":%s", l->second.c_str());
		}
		return appendf(Instruct.Rel ? "%+d # 0x%04x" : "%d # 0x%04x", Instruct.Immd.iValue, target);
	}
	if (Instruct.Immd.iValue || !Instruct.Reg)
		appendf(Instruct.Rel ? "%+d" : "0x%x", Instruct.Immd.iValue);
}

void Disassembler::DoInstruction()
{
	CodeAt = Code;
	*Comment = 0;
	switch (Instruct.Sig)
	{case Inst::S_BRANCH:
		DoBranch(); break;
	 case Inst::S_LDI:
		DoLDI(); break;
	 default:
		DoALU(); break;
	}
}

void Disassembler::ScanLabels()
{
	Addr = BaseAddr + 3*sizeof(uint64_t);
	for (uint64_t i : Instructions)
	{	Addr += sizeof(uint64_t); // base now points to branch point.
		Instruct.decode(i);
		if (Instruct.Sig != Inst::S_BRANCH) // only branch instructions
			continue;
		// link address
		if (Instruct.WAddrA != Inst::R_NOP)
			Labels.emplace(Addr, stringf("LL%zu_%s", Labels.size(), cWreg[Instruct.WS][Instruct.WAddrA]));
		if (Instruct.WAddrM != Inst::R_NOP)
			Labels.emplace(Addr, stringf("LL%zu_%s", Labels.size(), cWreg[!Instruct.WS][Instruct.WAddrM]));
		if (!Instruct.Immd.iValue)
			continue;
		if (Instruct.Rel)
			Labels.emplace(Addr + Instruct.Immd.iValue, stringf("L%x_%x", Addr + Instruct.Immd.iValue, Addr - 4*(unsigned)sizeof(uint64_t)));
		else
			Labels.emplace(Instruct.Immd.uValue, stringf("L%x_%x", Instruct.Immd.uValue, Addr - 4*(unsigned)sizeof(uint64_t)));
	}
}

void Disassembler::ProvideLabels(map<size_t,string> new_labels)
{
	Labels.clear();
	Labels.insert(new_labels.begin(), new_labels.end());
}

void Disassembler::Disassemble()
{
	Addr = BaseAddr;
	for (uint64_t i : Instructions)
	{	Instruct.decode(i);
		// Label?
		auto l = Labels.find(Addr);
		if (l != Labels.end())
			fprintf(Out, ":%s\n", l->second.c_str());

		DoInstruction();
		*CodeAt = 0;
		if (PrintComment)
			fprintf(Out, "\t%-55s # %04x: %016" PRIx64 " %s\n", Code, Addr, i, Comment);
		else
			fprintf(Out, "\t%s\n", Code);
		Addr += sizeof(uint64_t);
	}
}

void Disassembler::Disassemble(stringstream &s, bool one_line)
{
	char Line[400];

	Addr = BaseAddr;
	for (uint64_t i : Instructions)
	{	Instruct.decode(i);
		if (!one_line)
		{ // Label?
			auto l = Labels.find(Addr);
			if (l != Labels.end())
			{ snprintf(Line, sizeof(Line), ":%s\n", l->second.c_str());
				s << Line;
			}
		}

		DoInstruction();
		*CodeAt = 0;
		if (PrintComment)
		{	snprintf(Line, sizeof(Line), "%-55s # %04x: %016" PRIx64 " %s\n", Code, Addr, i, Comment);
			s << Line;
		}else
		{	snprintf(Line, sizeof(Line), "%s\n", Code);
			s << Line;
		}
		Addr += sizeof(uint64_t);
	}
}
