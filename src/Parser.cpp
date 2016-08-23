/*
 * Parser.cpp
 *
 *  Created on: 30.08.2014
 *      Author: mueller
 */

#define __STDC_FORMAT_MACROS // Work around for older g++

#include "Parser.h"

#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <algorithm>
#include <cctype>
#include <sys/stat.h>
#include <inttypes.h>

#include "Parser.tables.cpp"

#ifndef UINT64_MAX
#define UINT64_MAX (~(uint64_t)0)
#endif

Parser::saveContext::saveContext(Parser& parent, fileContext* ctx)
:	Parent(parent)
,	Context(ctx)
{	parent.Context.emplace_back(ctx);
}

Parser::saveContext::~saveContext()
{	auto& ctx = *Parent.Context.back();
	if (Context != &ctx)
		throw stringf("Unterminated context block in the current file at line %u.", ctx.Line);
	Parent.Context.pop_back();
}

Parser::saveLineContext::saveLineContext(Parser& parent, fileContext* ctx)
:	saveContext(parent, ctx)
, LineBak(parent.Line)
, AtBak(parent.At)
{}

Parser::saveLineContext::~saveLineContext()
{	strncpy(Parent.Line, LineBak.c_str(), sizeof(Line));
	Parent.At = AtBak;
}


string Parser::enrichMsg(string msg)
{	// Show context
	contextType type = CTX_CURRENT;
	for (auto i = Context.rbegin(); i != Context.rend(); ++i)
	{	const fileContext& ctx = **i;
		if (ctx.Line)
		{	auto fname = fName(ctx.File);
			switch (type)
			{case CTX_CURRENT:
				msg = stringf("%s (%u,%zi): ", fname, ctx.Line, At - Line - Token.size() + 1) + msg;
				break;
			 case CTX_INCLUDE:
				msg += stringf("\n  Included from %s (%u)", fname, ctx.Line);
				break;
			 case CTX_MACRO:
				msg += stringf("\n  At invocation of macro from %s (%u)", fname, ctx.Line);
				break;
			 case CTX_FUNCTION:
				msg += stringf("\n  At function invocation from %s (%u)", fname, ctx.Line);
				break;
			 default:;
			}
		}
		type = ctx.Type;
	}
	return msg;
}

void Parser::Fail(const char* fmt, ...)
{	va_list va;
	va_start(va, fmt);
	const string& msg = vstringf(fmt, va);
	va_end(va);
	throw enrichMsg(msg);
}

static const char msgpfx[][10]
{	"ERROR: "
,	"Warning: "
,	"Info: "
};

void Parser::Msg(severity level, const char* fmt, ...)
{	if (!Pass2 || Verbose < level)
		return;
	Success &= level > ERROR;
	va_list va;
	va_start(va, fmt);
	fputs(msgpfx[level], stderr);
	fputs(enrichMsg(vstringf(fmt, va)).c_str(), stderr);
	va_end(va);
	fputc('\n', stderr);
}

void Parser::FlagsSize(size_t min)
{	if (InstFlags.size() < min)
		InstFlags.resize(min, IF_NONE);
}

void Parser::StoreInstruction(uint64_t inst)
{
	if (!Pass2)
		Instructions.emplace_back();
	LineNumbers.emplace_back();
	uint64_t* ptr = &Instructions[PC+Back];
	uint64_t* ip  = ptr - Back;
	location* lp  = &LineNumbers[PC+Back];
	while (ptr != ip)
	{	*ptr = ptr[-1];
		*lp  = lp[-1];
		--ptr;
		--lp;
	}
	*ptr = inst;
	*lp  = *Context.back();
}

Parser::token_t Parser::NextToken()
{	size_t i;
	token_t ret;
 restart:
	switch (*At)
	{case 0:
	 case '#':
		Token.clear();
		return END;
	 case ' ':
	 case '\t':
	 case '\r':
	 case '\n':
		At += strspn(At, " \t\r\n");
		goto restart;
	 case '*':
	 case '>':
	 case '<':
	 case '&':
	 case '^':
	 case '|':
		if (At[1] == At[0])
		{op2:
			if (At[2] == At[1])
			{	Token.assign(At, 3);
				At += 3;
				return OP;
			}
			Token.assign(At, 2);
			At += 2;
			return OP;
		}
		goto opg;
	 case '!':
		if (At[1] == '^')
			goto op2;
	 case '=':
	 opg:
		if (At[1] == '=')
			goto op2;
	 case '+':
	 case '-':
	 case '/':
	 case '%':
	 case '~':
		Token.assign(At, 1);
		++At;
		return OP;
	 case '(':
	 case ')':
	 case '[':
	 case ']':
	 case '.':
	 case ',':
	 case ';':
	 case ':':
		Token.assign(At, 1);
		return (token_t)*At++;
	 case '0':
	 case '1':
	 case '2':
	 case '3':
	 case '4':
	 case '5':
	 case '6':
	 case '7':
	 case '8':
	 case '9':
		i = strcspn(At + 1, ",;:+-*/%()[]&|^~!=<># \t\r\n") + 1;
		ret = NUM;
		break;
	 default:
		i = strcspn(At + 1, ".,;:+-*/%()[]&|^~!=<># \t\r\n") + 1;
		ret = WORD;
	}
	Token.assign(At, i);
	At += i;
	return ret;
}

size_t Parser::parseInt(const char* src, int64_t& dst)
{	dst = 0;
	const char* cp = src;
	uint64_t basis = 10;
	int64_t limit = UINT64_MAX / 10;
	for (char c; (c = *cp) != 0; ++cp)
	{	int digit = c - '0';
		if (digit >= 10)
		{	digit = toupper(c) - 'A';
			if (digit < 6)
				digit += 10;
			else
			{	if (dst == 0 && cp - src == 1)
					switch (c)
					{	case 'b': basis = 2; limit = UINT64_MAX/2; continue;
						case 'o': basis = 8; limit = UINT64_MAX/8; continue;
						case 'x': basis = 16; limit = UINT64_MAX/16; continue;
						case 'd': continue;
					}
				break;
			}
		}
		if (dst > limit)
			break;
		dst *= basis;
		if ((uint64_t)dst > UINT64_MAX - digit)
			break;
		dst += digit;
	}
	return cp - src;
}

Parser::label& Parser::labelRef(string name, bool forward)
{
	const auto& l = LabelsByName.emplace(name, LabelCount);
	if (!!l.second || (forward && !!Labels[l.first->second].Definition))
	{	// new label
		l.first->second = LabelCount;
		if (!Pass2)
			Labels.emplace_back(Token);
		else if (Labels.size() <= LabelCount || Labels[LabelCount].Name != Token)
			Fail("Inconsistent label definition during Pass 2.");
		Labels[LabelCount].Reference = *Context.back();

		++LabelCount;
	}
	return Labels[l.first->second];
}

exprValue Parser::parseElemInt()
{	uint32_t value = 0;
	int pos = 0;
	signed char sign = 0;

	while (true)
	{	auto val = ParseExpression();
		if (val.Type != V_INT)
			Fail("Only integers are allowed between [...].");
		if (val.iValue < -2 || val.iValue > 3)
			Fail("Load per QPU element can only deal with integer constants in the range of [-2..3].");
		if (val.iValue < 0)
		{	if (sign > 0)
				Fail("All integers in load per QPU element must be either in the range [-2..1] or in the range [0..3].");
			sign = -1;
			val.iValue &= 3;
		} else if (val.iValue > 1)
		{	if (sign < 0)
				Fail("All integers in load per QPU element must be either in the range [-2..1] or in the range [0..3].");
			sign = 1;
		}
		value |= (val.iValue * 0x8001 & 0x10001) << pos;

		switch (NextToken())
		{case END:
			Fail("Incomplete expression.");
		 default:
			Fail("Unexpected '%s' in per QPU element constant.", Token.c_str());
		 case COMMA:
			if (++pos >= 16)
				Fail("Too many initializers for per QPU element constant.");
			continue;
		 case SQBRC2:
			if (pos != 15)
				Fail("Too few initializers for per QPU element constant.");
		}
		break;
	}
	// Make LDIPEx
	return exprValue(value, (valueType)(sign + V_LDPE));
}

exprValue Parser::ParseExpression()
{
	Eval eval;
	exprValue value;
	try
	{next:
		switch (NextToken())
		{case WORD:
			{	// Expand constants
				for (auto i = Context.end(); i != Context.begin(); )
				{	auto c = (*--i)->Consts.find(Token);
					if (c != (*i)->Consts.end())
					{	value = c->second.Value;
						goto have_value;
					}
				}
			}
			{	// try function
				auto fp = Functions.find(Token);
				if (fp != Functions.end())
				{	// Hit!
					value = doFUNC(fp);
					break;
				}
			}
			{	// try functional macro
				auto mp = MacroFuncs.find(Token);
				if (mp != MacroFuncs.end())
				{	value = doFUNCMACRO(mp);
					break;
				}
			}
			{	// try register
				auto rp = binary_search(regMap, Token.c_str());
				if (rp)
				{	value = rp->Value;
					break;
				}
			}
			{	// try alphanumeric operator
				auto op = binary_search(operatorMap2, Token.c_str());
				if (op)
				{	if (!eval.PushOperator(op->Op))
						goto discard;
					goto next;
				}
			}
			// Try label prefix
			{	string identifier = Token;
				if (NextToken() != COLON)
					Fail("Invalid expression. The identifier %s is undefined.", identifier.c_str());
				if (identifier != "r")
					Fail("'%s:' is no valid label prefix.", identifier.c_str());
			}
		 case COLON: // Label
			{	// Is forward reference?
				bool forward = false;

				// Search for '::name' labels
				bool double_colon = false;
				token_t nt = NextToken();
				if (nt == COLON)
				{ nt = NextToken();
					double_colon = true;
				}

				switch (nt)
				{default:
					Fail("Expected Label after '%s'.", double_colon?"::":":");

				 case BRACE1: // Internal label constant
					value = ParseExpression();
					if (value.Type != V_INT)
						Fail("Expecting integer constant, found %s.", type2string(value.Type));
					if (NextToken() != BRACE2)
						Fail("Expected ')', found '%s'.", Token.c_str());
					value.Type = V_LABEL;
					goto have_value;

				 case SQBRC1: // Internal register constant
					{	reg_t reg;
						value = ParseExpression();
						if (value.Type != V_INT)
							Fail("Expecting integer constant, found %s.", type2string(value.Type));
						reg.Num = (uint8_t)value.iValue;
						if (NextToken() != COMMA)
							Fail("Expected ',', found '%s'.", Token.c_str());
						value = ParseExpression();
						if (value.Type != V_INT)
							Fail("Expecting integer constant, found %s.", type2string(value.Type));
						reg.Type = (regType)value.iValue;
						switch (NextToken())
						{default:
							Fail("Expected ',' or ']', found '%s'.", Token.c_str());
						 case COMMA:
							value = ParseExpression();
							if (value.Type != V_INT)
								Fail("Expecting integer constant, found %s.", type2string(value.Type));
							reg.Rotate = (uint8_t)value.iValue;
							if (NextToken() != SQBRC2)
								Fail("Expected ']', found '%s'.", Token.c_str());
							break;
						 case SQBRC2:
							reg.Rotate = 0;
						}
						value = reg;
						goto have_value;
					}
				 case NUM:
					// NextToken accepts some letters in numeric constants
					if (Token.back() == 'f') // forward reference
					{	forward = true;
						Token.erase(Token.size()-1);
					}
				 case WORD:;
				}
				value = exprValue(labelRef(Token, forward).Value, V_LABEL);
				break;
			}

		 default:
		 discard:
			At -= Token.size();
			return eval.Evaluate();

		 case OP:
		 case BRACE1:
		 case BRACE2:
			{	const opInfo* op = binary_search(operatorMap, Token.c_str());
				if (!op)
					Fail("Invalid operator: %s", Token.c_str());
				if (!eval.PushOperator(op->Op))
					goto discard;
				goto next;
			}
		 case SQBRC1: // per QPU element constant
			value = parseElemInt();
			break;

		 case NUM:
			// parse number
			if (Token.find('.') == string::npos)
			{	// integer
				//size_t len;  sscanf of gcc4.8.2/Linux x32 can't read "0x80000000".
				//if (sscanf(Token.c_str(), "%i%n", &stack.front().iValue, &len) != 1 || len != Token.size())
				if (parseInt(Token.c_str(), value.iValue) != Token.size())
					Fail("%s is no integral number.", Token.c_str());
				value.Type = V_INT;
			} else
			{	// float number
				size_t len;
				if (sscanf(Token.c_str(), "%lf%zn", &value.fValue, &len) != 1 || len != Token.size())
					Fail("%s is no real number.", Token.c_str());
				value.Type = V_FLOAT;
			}
			break;
		}
	 have_value:
		eval.PushValue(value);
		goto next;
	} catch (const Eval::Fail& msg) // Messages from Eval are not yet enriched.
	{	throw enrichMsg(msg);
	}
}

Inst::mux Parser::muxReg(reg_t reg)
{	if (reg.Type & R_SEMA)
		Fail("Cannot use semaphore source in ALU or read instruction.");
	if (!(reg.Type & R_READ))
	{	// direct read access for r0..r5.
		if ((reg.Num ^ 32U) <= 5U)
			return (Inst::mux)(reg.Num ^ 32);
		Fail("The register is not readable.");
	}
	// try RA
	if (reg.Type & R_A)
	{	if ( Instruct.RAddrA == reg.Num
			|| ( Instruct.MuxAA != Inst::X_RA && Instruct.MuxAB != Inst::X_RA
				&& Instruct.MuxMA != Inst::X_RA && Instruct.MuxMB != Inst::X_RA ))
		{RA:
			if (!(reg.Type & R_B))
				Flags() |= IF_NOSWAP;
			Instruct.RAddrA = reg.Num;
			return Inst::X_RA;
		}
	}
	// try RB
	if (reg.Type & R_B)
	{	if (Instruct.Sig >= Inst::S_SMI)
			Fail("Access to register file B conflicts with small immediate value.");
		if ( Instruct.RAddrB == reg.Num
			|| ( Instruct.MuxAA != Inst::X_RB && Instruct.MuxAB != Inst::X_RB
				&& Instruct.MuxMA != Inst::X_RB && Instruct.MuxMB != Inst::X_RB ))
		{RB:
			if (!(reg.Type & R_A))
				Flags() |= IF_NOSWAP;
			Instruct.RAddrB = reg.Num;
			return Inst::X_RB;
		}
	}
	// try to swap RA and RB of existing instruction
	switch (reg.Type & R_AB)
	{case R_A:
		if (( Instruct.RAddrB == reg.Num
				|| ( Instruct.MuxAA != Inst::X_RB && Instruct.MuxAB != Inst::X_RB
					&& Instruct.MuxMA != Inst::X_RB && Instruct.MuxMB != Inst::X_RB ))
			&& !(Flags() & IF_NOSWAP) && Instruct.tryRABSwap() )
			goto RA;
		break;
	 case R_B:
		if (( Instruct.RAddrA == reg.Num
				|| ( Instruct.MuxAA != Inst::X_RA && Instruct.MuxAB != Inst::X_RA
					&& Instruct.MuxMA != Inst::X_RA && Instruct.MuxMB != Inst::X_RA ))
			&& !(Flags() & IF_NOSWAP) && Instruct.tryRABSwap() )
			goto RB;
	}
	Fail("Read access to register conflicts with another access to the same register file.");
}

const Parser::smiEntry* Parser::getSmallImmediateALU(uint32_t i)
{	size_t l = 0;
	size_t r = sizeof smiMap / sizeof *smiMap - 1;
	while (l < r)
	{	size_t m = (l+r) >> 1;
		if (i <= smiMap[m].Value)
			r = m;
		else
			l = m + 1;
	}
	return smiMap + r;
}

void Parser::doSMI(uint8_t si)
{	switch (Instruct.Sig)
	{default:
		Fail("Small immediate value or vector rotation cannot be used together with signals.");
	 case Inst::S_SMI:
		if (Instruct.SImmd == si)
			return; // value hit
		if ((si & 16) && ((Instruct.SImmd ^ si) & 31) == 0)
		{	// Vector rotation codes return the same value than [16..31]
			// => only ensure the rotation bit
			Instruct.SImmd |= si;
			return;
		}
		Fail("Only one distinct small immediate value supported per instruction. Requested value: %u, current Value: %u.", si, Instruct.SImmd);
	 case Inst::S_NONE:
		if (Instruct.RAddrB != Inst::R_NOP)
			Fail("Small immediate cannot be used together with register file B read access.");
	}
	Instruct.Sig   = Inst::S_SMI;
	Instruct.SImmd = si;
}

bool Parser::trySwap()
{	if ((InstCtx & IC_CANSWAP) == 0 || !Instruct.tryALUSwap())
		return false;
	InstCtx ^= IC_ADD|IC_MUL;
	return true;
}

void Parser::applyRot(int count)
{
	if (count)
	{	if (UseRot & (XR_OP | toExtReq(InstCtx)))
			Fail("Only one vector rotation per ALU instruction, please.");

		if ((InstCtx & IC_BOTH) != IC_MUL && !trySwap())
			Fail("Vector rotation is only available to the MUL ALU.");
		if (count == 16)
			Fail("Cannot rotate ALU target right by r5.");

		InstCtx &= ~IC_CANSWAP;
		auto mux = InstCtx & IC_B ? Instruct.MuxMB : Instruct.MuxMA;
		if (mux == Inst::X_R5)
			Msg(WARNING, "r5 does not support vector rotations.");

		count = 48 + (count & 0xf);
		auto isaccu = Inst::isAccu(mux);
		if (!isaccu && count > 48+3 && count < 64-3)
			Msg(WARNING, "%s does not support full MUL ALU vector rotation.", Inst::toString(mux));

		if (Instruct.Sig == Inst::S_SMI)
		{	int mask = !isaccu
				|| ( (InstCtx & IC_B) && !Inst::isAccu(Instruct.MuxMA)
					&& Instruct.MuxAA != Inst::X_RB && Instruct.MuxAB != Inst::X_RB
					&& Instruct.MuxMA != Inst::X_RB && Instruct.MuxMB != Inst::X_RB )
				? 0x13 : 0x1f;
			if ((Instruct.SImmd ^ count) & mask)
				Fail( Instruct.SImmd < 48
					? "Vector rotation is in conflict with small immediate value."
					: "Cannot use different vector rotations within one instruction." );

			if (!isaccu)
				count = Instruct.SImmd | 0x20;
			else
				Instruct.SImmd = count;
		}

		if ( (InstCtx & IC_B)
			&& (Instruct.Sig != Inst::S_SMI || Instruct.SImmd < 48)
			&& (Inst::isAccu(Instruct.MuxMA) || (count & 0x3) || !count) )
			Msg(WARNING, "The Vector rotation of the second MUL ALU source argument silently applies also to the first source.");

		UseRot |= toExtReq(InstCtx);

		doSMI(count);
	} else if ( (InstCtx & IC_B) && (UseRot & XR_SRCA)
			&& (Inst::isAccu(Instruct.MuxMB) || (Instruct.SImmd & 0x3) || Instruct.SImmd == 48) )
		Msg(WARNING, "The vector rotation of the first MUL ALU source argument silently applies also to the second argument.");
}

void Parser::addIf(int cond)
{
	if (Instruct.Sig == Inst::S_BRANCH)
		Fail("Cannot apply conditional store (.ifxx) to branch instruction.");
	auto& target = InstCtx & IC_MUL ? Instruct.CondM : Instruct.CondA;
	if (target != Inst::C_AL)
		Fail("Store condition (.if) already specified.");
	target = (Inst::conda)cond;
}

void Parser::addUnpack(int mode)
{
	if (Instruct.Sig >= Inst::S_LDI)
		Fail("Cannot apply .unpack to branch and load immediate instructions.");
	if (UseUnpack & (XR_OP | toExtReq(InstCtx)))
		Fail("Only one .unpack per ALU instruction, please.");
	if (Instruct.Unpack != mode)
	{	if (Instruct.Unpack != Inst::U_32)
			Fail("Cannot use different unpack modes within one instruction.");
		Instruct.Unpack = (Inst::unpack)mode;
		UseUnpack |= XR_NEW; // Signal
	}
	UseUnpack |= toExtReq(InstCtx);
	// The check of unpack mode against used source register
	// and the pack mode is done in doALUExpr.
}

void Parser::addPack(int mode)
{
	if (Instruct.Sig == Inst::S_BRANCH)
		Fail("Cannot apply .pack to branch instruction.");
	if (Instruct.Pack != Inst::P_32)
		Fail("Only one .pack per instruction, please.");
	// Use intermediate pack mode matching the ALU
	bool pm = !!(InstCtx & IC_MUL);
	if (InstCtx & IC_DST)
	{	if (!pm)
		{	// At target register of ADD ALU = Target must be regfile A
			if (Instruct.WS || Instruct.WAddrA >= 32)
				Fail("Target of ADD ALU must be of regfile A to use pack.");
		} else
		{	// At target register of MUL ALU
			// Prefer regfile A pack even for MUL ALU
			if (Instruct.WS && Instruct.WAddrM < 32 && !Instruct.PM)
				pm = false;
			// Check for pack mode supported by MUL ALU
			else if (mode < Inst::P_8abcdS)
				Fail("MUL ALU only supports saturated pack modes with 8 bit.");
			else
				// Use intermediate pack mode matching the ALU
				mode &= 7;
		}
	}
	if (Instruct.Unpack && Instruct.PM != pm)
		Fail("Type of pack conflicts with type of unpack in PM flag.");
	// Commit
	Instruct.PM = pm;
	Instruct.Pack = (Inst::pack)mode;
}

void Parser::addSetF(int)
{
	if ( Instruct.Sig < Inst::S_LDI && (InstCtx & IC_MUL)
		&& (Instruct.WAddrA != Inst::R_NOP || Instruct.OpA != Inst::A_NOP)
		&& !trySwap() )
		Fail("Cannot apply .setf because the flags of the ADD ALU will be used.");
	if (Instruct.SF)
		Fail("Don't use .setf twice.");
	Instruct.SF = true;
	InstCtx &= ~IC_CANSWAP;
}

void Parser::addCond(int cond)
{
	if (Instruct.Sig != Inst::S_BRANCH)
		Fail("Branch condition codes can only be applied to branch instructions.");
	if (Instruct.CondBr != Inst::B_AL)
		Fail("Only one branch condition per instruction, please.");
	Instruct.CondBr = (Inst::condb)cond;
}

void Parser::addRot(int)
{
	auto count = ParseExpression();
	if (NextToken() != COMMA)
		Fail("Expected ',' after rotation count.");

	int si;
	switch (count.Type)
	{case V_REG:
		si = 0;
		if ((count.rValue.Type & R_READ) && count.rValue.Num == 37) // r5
			break;
	 default:
		Fail("QPU rotation needs an integer argument or r5 for the rotation count.");
	 case V_INT:
		si = (int)count.iValue & 0xf;
		if (si == 0)
			return; // Rotation is a multiple of 16 => nothing to do
	}

	applyRot(si);
}

void Parser::doInstrExt()
{
	while (NextToken() == DOT)
	{	switch (NextToken())
		{default:
			Fail("Expected instruction extension after dot.");
		 case WORD:
		 case NUM:;
		}
		const opExtEntry* ep = binary_search(extMap, Token.c_str());
		if (!ep)
			Fail("Unknown instruction extension '%s'.", Token.c_str());
		/* make problems with mov
		if ((ctx & IC_MUL) ? Instruct.OpM == Inst::M_NOP : Instruct.OpA == Inst::A_NOP)
			Fail("Instruction extensions cannot be applied to nop instruction.");*/
		opExtFlags filter = InstCtx & IC_SRC ? E_SRC : InstCtx & IC_DST ? E_DST : E_OP;
		while ((ep->Flags & filter) == 0)
			if (strcmp(Token.c_str(), (++ep)->Name) != 0)
				Fail("Invalid instruction extension '%s' within this context.", Token.c_str());
		(this->*(ep->Func))(ep->Arg);
	}
	At -= Token.size();
}

void Parser::check4Unpack(Inst::mux mux)
{
	// check 4 unpack
	if (UseUnpack & (XR_OP | toExtReq(InstCtx)))
	{	// Current source argument should be unpacked.
		bool pm = Instruct.PM;
		if (mux == Inst::X_R4)
			pm = true;
		else if (mux == Inst::X_RA && Instruct.RAddrA < 32)
			pm = false;
		else if (!(UseUnpack & XR_OP))
			Fail("Cannot unpack this source argument.");
		else if ((InstCtx & IC_SRCB)
			&& !(((InstCtx & IC_MUL) || !Instruct.isUnary())
				&& isUnpackable(InstCtx & IC_MUL ? Instruct.MuxMA : Instruct.MuxAA) >= 0 )) // Operands must contain either r4 xor regfile A
			Fail("The unpack option works for none of the source operands of the current opcode.");
		if ((UseUnpack & XR_NEW) && ( (InstCtx & IC_MUL)
				? isUnpackable(Instruct.MuxAB) == pm || (!Instruct.isUnary() && isUnpackable(Instruct.MuxAA) == pm)
				: isUnpackable(Instruct.MuxMB) == pm || isUnpackable(Instruct.MuxMA) == pm ))
			Fail("Using unpack changes the semantic of the other ALU.");
		if (pm != Instruct.PM)
		{	if (Instruct.Pack)
				// TODO: in a few constellations the pack mode of regfile A and the MUL ALU are interchangeable.
				Fail("Requested unpack mode conflicts with pack mode of the current instruction.");
			if ((InstCtx & IC_B) && (UseUnpack & XR_SRCA))
				Fail("Conflicting unpack modes of first and second source operand.");
			// Check for unpack sensitivity of second ALU instruction.
			if ( (InstCtx & IC_MUL)
				? isUnpackable(Instruct.MuxAB) >= 0 || (!Instruct.isUnary() && isUnpackable(Instruct.MuxAA) >= 0)
				: isUnpackable(Instruct.MuxMB) >= 0 || isUnpackable(Instruct.MuxMA) >= 0 )
				Fail("The unpack modes of ADD ALU and MUL ALU are different.");
			Instruct.PM = pm;
		}
	} else if ( Instruct.Unpack != Inst::U_32 // Current source should not be unpacked
		&& (Instruct.PM ? mux == Inst::X_R4 : mux == Inst::X_RA && Instruct.RAddrA < 32) )
		Fail("The unpack option silently applies to %s source argument.", InstCtx & IC_B ? "2nd" : "1st");
}

void Parser::doALUTarget(exprValue param)
{	InstCtx |= IC_DST;
	if (param.Type != V_REG)
		Fail("The target argument to a ALU or branch instruction must be a register or '-', found %s.", param.toString().c_str());
	if (!(param.rValue.Type & R_WRITE))
		Fail("The register is not writable.");
	bool mul = (InstCtx & IC_MUL) != 0;
	if ((param.rValue.Type & R_AB) != R_AB)
	{	bool wsfreeze = !Inst::isWRegAB(mul ? Instruct.WAddrA : Instruct.WAddrM); // Can't swap the other target register.
		if ((param.rValue.Type & R_A) && (!wsfreeze || Instruct.WS == mul))
			Instruct.WS = mul;
		else if ((param.rValue.Type & R_B) && (!wsfreeze || Instruct.WS != mul))
			Instruct.WS = !mul;
		else
			Fail("ADD ALU and MUL ALU cannot write to the same register file.");
	}
	// Check pack mode from /this/ opcode if any
	if ( Instruct.Sig != Inst::S_BRANCH && Instruct.Pack != Inst::P_32
		&& Instruct.PM == mul && !(!mul && Instruct.WS) ) // Pack matches my ALU and not RA pack of MUL ALU.
	{	if (!(InstCtx & IC_MUL))
		{	if (!Instruct.WS && Instruct.WAddrA < 32)
				goto packOK;
			Fail("ADD ALU can only pack with regfile A target.");
		}
		// MUL ALU => prefer Regfile A pack
		if ( Instruct.WS && Instruct.WAddrA < 32 // Regfile A write and
			&& !Instruct.Unpack )                  // pack mode not frozen.
		{	Instruct.PM = false;
			goto packOK;
		}
		if (Instruct.Pack < Inst::P_8abcdS)
			Fail("MUL ALU only supports saturated pack modes with 8 bit.");
		// MUL ALU encodes saturated pack modes like unsaturated
		Instruct.Pack = (Inst::pack)(Instruct.Pack & 7);
	}
 packOK:
	(mul ? Instruct.WAddrM : Instruct.WAddrA) = param.rValue.Num;
	// vector rotation
	applyRot(param.rValue.Rotate);

	doInstrExt();
}

void Parser::doALUExpr()
{	InstCtx = (InstCtx & ~IC_DST) | IC_SRC;
	// destination multiplexer
	Inst::mux& ret = [this]() -> Inst::mux&
	{	switch (InstCtx & (IC_MUL|IC_B))
		{case IC_NONE:
			return Instruct.MuxAA;
		 case IC_B:
			return Instruct.MuxAB;
		 case IC_MUL:
			return Instruct.MuxMA;
		 default:
			return Instruct.MuxMB;
		}
	}();

	if (NextToken() != COMMA)
		Fail("Expected ',' before next argument to ALU instruction, found '%s'.", Token.c_str());
	exprValue param = ParseExpression();
	switch (param.Type)
	{default:
		Fail("The second argument of a binary ALU instruction must be a register or a small immediate value.");
	 case V_REG:
		{	ret = muxReg(param.rValue);
			applyRot(-param.rValue.Rotate);
			break;
		}
	 case V_FLOAT:
	 case V_INT:
		{	qpuValue value; value = param;
			// some special hacks for ADD ALU
			if (InstCtx == (IC_SRCB|IC_ADD))
			{	switch (Instruct.OpA)
				{case Inst::A_ADD: // swap ADD and SUB in case of constant 16 or negative SMI match
				 case Inst::A_SUB:
					if ( value.iValue == 16
						|| (Instruct.Sig == Inst::S_SMI && Instruct.SMIValue().iValue == -value.iValue) )
					{	(uint8_t&)Instruct.OpA ^= Inst::A_ADD ^ Inst::A_SUB; // swap add <-> sub
						value.iValue = -value.iValue;
					}
					break;
				 case Inst::A_ASR: // shift instructions ignore all bits except for the last 5
				 case Inst::A_SHL:
				 case Inst::A_SHR:
				 case Inst::A_ROR:
					value.iValue = (value.iValue << 27) >> 27; // sign extend
				 default:;
				}
			}
			uint8_t si = Inst::AsSMIValue(value);
			if (si == 0xff)
				Fail("Value 0x%x does not fit into the small immediate field.", value.uValue);
			doSMI(si);
			ret = Inst::X_RB;
			break;
		}
	}

	doInstrExt();

	check4Unpack(ret);
}

void Parser::doBRASource(exprValue param)
{
	switch (param.Type)
	{default:
		Fail("Data type is not allowed as branch target.");
	 case V_LABEL:
		if (Instruct.Rel)
			param.iValue -= (PC + 4) * sizeof(uint64_t);
		else
			Msg(WARNING, "Using value of label as target of a absolute branch instruction creates non-relocatable code.");
		param.Type = V_INT;
	 case V_INT:
		if (Instruct.Immd.uValue)
			Fail("Cannot specify two immediate values as branch target.");
		if (!param.iValue)
			return;
		Instruct.Immd = param;
		break;
	 case V_REG:
		if (param.rValue.Num == Inst::R_NOP && !param.rValue.Rotate && (param.rValue.Type & R_AB))
			return;
		if (!(param.rValue.Type & R_A) || param.rValue.Num >= 32)
			Fail("Branch target must be from register file A and no hardware register.");
		if (param.rValue.Rotate)
			Fail("Cannot use vector rotation with branch instruction.");
		if (Instruct.Reg)
			Fail("Cannot specify two registers as branch target.");
		if ((param.rValue.Num & 1) != Instruct.SF)
		{	if (Instruct.SF)
				Fail("Branch instruction with .setf cannot use even register numbers.");
			else
				Msg(WARNING, "Using an odd register number as branch target implies .setf. Use explicit .setf to avoid this warning.");
		}
		Instruct.Reg = true;
		Instruct.RAddrA = param.rValue.Num;
		break;
	}
}

bool Parser::trySmallImmd(uint32_t value)
{	const smiEntry* si;
	if (!value)
	{	// mov ... ,0 does not require small immediate value
		si = smiMap + !!(InstCtx & IC_MUL);
		goto mov0;
	}
	switch (Instruct.Sig)
	{default:
		Fail ("Immediate values cannot be used together with signals.");
	 case Inst::S_NONE:
		if (Instruct.RAddrB != Inst::R_NOP)
			Fail ("Immediate value collides with read from register file B.");
	 case Inst::S_SMI:;
	}
	for (si = getSmallImmediateALU(value); si->Value == value; ++si)
	{	// conflicting signal or small immediate value
		if (Instruct.Sig != Inst::S_NONE && !(Instruct.Sig == Inst::S_SMI && Instruct.SImmd == si->SImmd))
			continue;
		// Check pack mode
		if (!!si->Pack)
		{	// conflicting pack mode?
			if (Instruct.Pack != Inst::P_32 && !(Instruct.Pack == si->Pack.pack() && Instruct.PM == si->Pack.mode()))
				continue;
			// Regfile A target
			if (!si->Pack.mode())
			{	// .setf may return unexpected results in pack mode in sign bit. TODO: accept all cases where this does not happen.
				if (Instruct.SF)
					continue;
				// Not regfile A target?
				if (InstCtx & IC_MUL)
				{	if (!Instruct.WS || Instruct.WAddrM >= 32)
						continue;
				} else
				{	if (Instruct.WS || Instruct.WAddrA >= 32)
						continue;
				}
			}
		}
		// other ALU needed?
		if ((!si->OpCode.isMul() ^ !(InstCtx & IC_MUL)) && !trySwap())
			continue;

		// Match!
		Instruct.Sig   = Inst::S_SMI;
		Instruct.SImmd = si->SImmd;
		if (!!si->Pack)
		{	Instruct.Pack = si->Pack.pack();
			Instruct.PM   = si->Pack.mode();
		}
	 mov0:
		if (si->OpCode.isMul())
		{	Instruct.MuxMA = Instruct.MuxMB = Inst::X_RB;
			Instruct.OpM   = si->OpCode.asMul();
		} else
		{	Instruct.MuxAA = Instruct.MuxAB = Inst::X_RB;
			Instruct.OpA   = si->OpCode.asAdd();
		}

		doInstrExt();
		check4Unpack(Inst::X_RB);
		return true;
	}
	// nope, can't help
	return false;
}

void Parser::assembleADD(int add_op)
{	InstCtx = IC_ADD;

	if (Instruct.Sig >= Inst::S_LDI)
		Fail("Cannot use ADD ALU in load immediate or branch instruction.");
	if (Instruct.isADD())
	{	switch (add_op)
		{default:
			if (Instruct.OpM == Inst::M_NOP && Instruct.tryALUSwap())
				goto cont;
			Fail("The ADD ALU has already been used in this instruction.");
		 case Inst::A_NOP:
			add_op = Inst::M_NOP; break;
		 case Inst::A_V8ADDS:
			add_op = Inst::M_V8ADDS; break;
		 case Inst::A_V8SUBS:
			add_op = Inst::M_V8SUBS; break;
		}
		// retry with MUL ALU
		return assembleMUL(add_op | 0x100); // The added number is discarded at the cast but avoids recursive retry.
	}
 cont:
	Instruct.OpA = (Inst::opadd)add_op;
	UseRot = UseUnpack = 0;

	doInstrExt();

	if (add_op == Inst::A_NOP)
	{	Flags() |= IF_HAVE_NOP;
		return;
	}

	doALUTarget(ParseExpression());

	doALUExpr();
	InstCtx |= IC_B;
	if (Instruct.isUnary())
		Instruct.MuxAB = Instruct.MuxAA;
	else
		doALUExpr();
}

void Parser::assembleMUL(int mul_op)
{	InstCtx = IC_MUL;

	if (Instruct.Sig >= Inst::S_LDI)
		Fail("Cannot use MUL ALU in load immediate or branch instruction.");
	if (Instruct.isMUL())
	{	switch (mul_op)
		{default:
			if (Instruct.OpA == Inst::A_NOP && Instruct.tryALUSwap())
				goto cont;
			Fail("The MUL ALU has already been used by the current instruction.");
		 case Inst::M_NOP:
			mul_op = Inst::A_NOP; break;
		 case Inst::M_V8ADDS:
			mul_op = Inst::A_V8ADDS; break;
		 case Inst::M_V8SUBS:
			mul_op = Inst::A_V8SUBS; break;
		}
		// retry with MUL ALU
		return assembleADD(mul_op | 0x100); // The added numbed is discarded at the cast but avoids recursive retry.
	}
 cont:
	Instruct.OpM = (Inst::opmul)mul_op;
	UseRot = UseUnpack = 0;

	doInstrExt();

	if (mul_op == Inst::M_NOP)
		return;

	doALUTarget(ParseExpression());

	doALUExpr();
	InstCtx |= IC_B;
	doALUExpr();
}

void Parser::assembleMOV(int mode)
{
	if (Instruct.Sig == Inst::S_BRANCH)
		Fail("Cannot use MOV together with branch instruction.");
	bool isLDI = Instruct.Sig == Inst::S_LDI;
	InstCtx = IC_CANSWAP
		| ( (Flags() & IF_HAVE_NOP) || Instruct.WAddrA != Inst::R_NOP || (!isLDI && Instruct.OpA != Inst::A_NOP)
			? IC_MUL : IC_ADD );
	if ((InstCtx & IC_MUL) && (Instruct.WAddrM != Inst::R_NOP || (!isLDI && Instruct.OpM != Inst::M_NOP)))
		Fail("Both ALUs are already used by the current instruction.");
	UseRot = UseUnpack = 0;

	doInstrExt();

	doALUTarget(ParseExpression());

	if (NextToken() != COMMA)
		Fail("Expected source or second target after first argument to mov/ldi instruction, found %s.", Token.c_str());

	exprValue param = ParseExpression();
	switch (NextToken())
	{default:
		Fail("Expected source or end of instruction after second argument to mov/ldi instruction, found %s.", Token.c_str());

	 case DOT:   // extension
		if (!HaveMoreOperands(At--))
			break;
	 case COMMA: // second target
		if ( InstCtx & IC_MUL
			? ((Flags() & IF_HAVE_NOP) || Instruct.WAddrA != Inst::R_NOP || (!isLDI && Instruct.OpA != Inst::A_NOP))
			: (Instruct.WAddrM != Inst::R_NOP || (!isLDI && Instruct.OpM != Inst::M_NOP)) )
			Fail("ALU instruction with two targets can only be used if both ALUs are available.");

		InstCtx = (InstCtx & ~IC_CANSWAP) ^ (IC_ADD|IC_MUL); // switch ALU
		doALUTarget(param);
		// From here we are double ALU
		InstCtx |= IC_ADD|IC_MUL; // now we are at both ALUs
		param = ParseExpression();
		break;
	 case SEMI:  // no second target
		--At;
	 case END:;
	}

	InstCtx ^= IC_DST|IC_SRC; // Swap to source context
	qpuValue value;
	Inst::mux mux;
	switch (param.Type)
	{default:
		Fail("The last parameter of a MOV instruction must be a register or a immediate value. Found %s", type2string(param.Type));

	 case V_REG:
		if (param.rValue.Type & R_SEMA)
		{	// semaphore access by LDI like instruction
			mode = Inst::L_SEMA;
			value.uValue = param.rValue.Num | (param.rValue.Type & R_SACQ);
			goto ldi;
		}
		if (isLDI)
			Fail("mov instruction with register source cannot be combined with load immediate.");

		mux = muxReg(param.rValue);
		if (InstCtx & IC_MUL)
		{	Instruct.MuxMA = Instruct.MuxMB = mux;
			Instruct.OpM = Inst::M_V8MIN;
		} else
		{	Instruct.MuxAA = Instruct.MuxAB = mux;
			Instruct.OpA = Inst::A_OR;
		}
		applyRot(-param.rValue.Rotate);

		doInstrExt();
		check4Unpack(mux);
		return;

	 case V_LDPES:
		if (mode != Inst::L_LDI && mode != Inst::L_PES)
			Fail("Load immediate mode conflicts with per QPU element constant.");
		mode = Inst::L_PES;
		goto ldpe_cont;
	 case V_LDPEU:
		if (mode != Inst::L_LDI && mode != Inst::L_PEU)
			Fail("Load immediate mode conflicts with per QPU element constant.");
		mode = Inst::L_PEU;
		goto ldpe_cont;
	 case V_LDPE:
		if (mode >= Inst::L_SEMA)
			Fail("Load immediate mode conflicts with per QPU element constant.");
		if (mode < Inst::L_PES)
			mode = Inst::L_PES;
	 ldpe_cont:
		value = param;
		goto ldi;

	 case V_LABEL:
		/* Convert type to V_INT to use V_LABEL target as int. */
		param.Type = V_INT;
		//no break

	 case V_INT:
	 case V_FLOAT:
		value = param;
		// try small immediate first
		if ( !isLDI && mode < 0 && (InstCtx & IC_BOTH) != IC_BOTH
			&& trySmallImmd(value.uValue))
			return;
		// LDI
		if (mode < 0)
			mode = Inst::L_LDI;
	 ldi:
		switch (Instruct.Sig)
		{default:
			Fail("Load immediate cannot be used with signals.");
		 case Inst::S_SMI:
			Fail("This pair of immediate values cannot be handled in one instruction word.");
		 case Inst::S_LDI:
			if (Instruct.Immd.uValue != value.uValue || Instruct.LdMode != mode)
				Fail("Tried to load two different immediate values in one instruction. (0x%x vs. 0x%x)", Instruct.Immd.uValue, value.uValue);
			break;
		 case Inst::S_NONE:
			if (Instruct.OpA != Inst::A_NOP || Instruct.OpM != Inst::M_NOP)
				Fail("Cannot combine load immediate with value %s with ALU instructions.", param.toString().c_str());
		}
		// LDI or semaphore
		Instruct.Sig = Inst::S_LDI;
		Instruct.LdMode = (Inst::ldmode)mode;
		Instruct.Immd = value;

		doInstrExt();
	}
}

void Parser::assembleREAD(int)
{	InstCtx = IC_SRC;

	if (Instruct.Sig == Inst::S_LDI || Instruct.Sig == Inst::S_BRANCH)
		Fail("read cannot be combined with load immediate, semaphore or branch instruction.");

	switch (NextToken())
	{case DOT:
		Fail("read does not support instruction extensions.");
	 case END:
	 case COMMA:
	 case SEMI:
		Fail("Expected source register or small immediate value after read.");
	 default:
		At -= Token.size();
	}

	exprValue param = ParseExpression();
	switch (param.Type)
	{default:
		Fail("read instruction requires register file or small immediate source, found '%s'.", param.toString().c_str());
	 case V_REG:
		if (param.rValue.Rotate)
			Fail("Vector rotations cannot be used at read.");
		if (muxReg(param.rValue) < Inst::X_RA)
			Fail("Accumulators cannot be used at read.");
		break;
	 case V_INT:
	 case V_FLOAT:
		qpuValue value; value = param;
		uint8_t si = Inst::AsSMIValue(value);
		if (si == 0xff)
			Fail("Value 0x%" PRIx32 " does not fit into the small immediate field.", value.uValue);
		doSMI(si);
	}
}

void Parser::assembleBRANCH(int relative)
{	InstCtx = IC_ADD;

	if ( Instruct.OpA != Inst::A_NOP || Instruct.OpM != Inst::M_NOP || Instruct.Sig != Inst::S_NONE
		|| Instruct.RAddrA != Inst::R_NOP || Instruct.RAddrB != Inst::R_NOP )
		Fail("A branch instruction must be the only one in a line.");

	Instruct.Sig = Inst::S_BRANCH;
	Instruct.CondBr = Inst::B_AL;
	Instruct.Rel = !!relative;
	Instruct.RAddrA = 0;
	Instruct.Reg = false;
	Instruct.Immd.uValue = 0;

	doInstrExt();

	doALUTarget(ParseExpression());

	if (NextToken() != COMMA)
		Fail("Expected ', <branch target>' after first argument to branch instruction, found %s.", Token.c_str());
	auto param2 = ParseExpression();
	switch (NextToken())
	{default:
		Fail("Expected ',' or end of line, found '%s'.", Token.c_str());

	 case END:
		doBRASource(param2);
		break;

	 case COMMA:
		auto param3 = ParseExpression();
		switch (NextToken())
		{default:
			Fail("Expected ',' or end of line, found '%s'.", Token.c_str());
		 case END: // we have 3 arguments => #2 and #3 are branch target
			doBRASource(param2);
			InstCtx |= IC_B;
			doBRASource(param3);
			break;
		 case COMMA: // we have 4 arguments, so #2 is a target and #3 the first source
			InstCtx = IC_MUL;
			doALUTarget(param2);
			doBRASource(param3);
			InstCtx |= IC_B;
			doBRASource(ParseExpression());
			if (NextToken() != END)
				Fail("Expected end of line after branch instruction.");
		}
	}

	if (Instruct.Immd.uValue & 3)
		Msg(WARNING, "A branch target without 32 bit alignment probably does not hit the nail on the head.");

	// add branch target flag for the branch point unless the target is 0
	if (Instruct.Reg || Instruct.Immd.uValue != 0)
	{	size_t pos = PC + 4;
		FlagsSize(pos + 1);
		InstFlags[pos] |= IF_BRANCH_TARGET;
	}
}

void Parser::assembleSEMA(int type)
{	InstCtx = IC_ADD;

	bool combine;
	switch (Instruct.Sig)
	{case Inst::S_LDI:
		combine = true;
		if (Instruct.LdMode == Inst::L_LDI)
			break;
	 default:
	 fail:
		Fail("Semaphore Instructions normally cannot be combined with any other instruction.");
	 case Inst::S_NONE:
		if ( Instruct.OpA != Inst::A_NOP || Instruct.OpM != Inst::M_NOP
			|| Instruct.RAddrA != Inst::R_NOP || Instruct.RAddrB != Inst::R_NOP )
			goto fail;
		combine = false;
		Instruct.Sig = Inst::S_LDI;
	}
	Instruct.LdMode = Inst::L_SEMA;

	doInstrExt();

	doALUTarget(ParseExpression());
	if (NextToken() != COMMA)
		Fail("Expected ', <number>' after first argument to semaphore instruction, found %s.", Token.c_str());

	auto param = ParseExpression();
	if (param.Type != V_INT || ((uint64_t)param.iValue & ~(type << 4)) >= 16)
		Fail("Semaphore instructions require a single integer argument less than 16 with the semaphore number.");
	uint32_t value = param.iValue | (type << 4);

	if (combine && (Instruct.Immd.uValue & 0x1f) != (value & 0x1f))
		Fail("Combining a semaphore instruction with load immediate requires the low order 5 bits to match the semaphore number and the direction bit.");
	else
		Instruct.Immd.uValue = value;
}

void Parser::assembleSIG(int bits)
{
	switch (Instruct.Sig)
	{default:
		Fail("You must not use more than one signaling flag per line.");
	 case Inst::S_BRANCH:
	 case Inst::S_LDI:
	 case Inst::S_SMI:
		Fail("Signaling bits cannot be combined with branch instruction or immediate values.");
	 case Inst::S_NONE:
		Instruct.Sig = (Inst::sig)bits;
	}
}

void Parser::ParseInstruction()
{
	auto& flags = Flags();
	while (true)
	{	flags &= ~IF_CMB_ALLOWED;
		const opEntry<8>* op = binary_search(opcodeMap, Token.c_str());
		if (!op)
			Fail("Invalid opcode or unknown macro: %s", Token.c_str());

		if (Preprocessed)
			fputs(Token.c_str(), Preprocessed);
		(this->*op->Func)(op->Arg);

		switch (NextToken())
		{default:
			Fail("Expected end of line or ';' after instruction. Found '%s'.", Token.c_str());
		 case END:
			return;
		 case SEMI:
			flags |= IF_CMB_ALLOWED;
			switch (NextToken())
			{default:
				Fail("Expected additional instruction or end of line after ';'. Found '%s'.", Token.c_str());
			 case END:
				return;
			 case WORD:;
			}
		}
	}
}

void Parser::defineLabel(bool exportable_label)
{
	// Lookup symbol
	const auto& lname = LabelsByName.emplace(Token, LabelCount);
	label* lp;
	if (lname.second)
	{	// new label, not yet referenced
	 new_label:
		if (!Pass2)
			Labels.emplace_back(Token);
		lp = &Labels[LabelCount];
		++LabelCount;
	} else
	{	// Label already in the lookup table.
		lp = &Labels[lname.first->second];
		if (!!lp->Definition)
		{	// redefinition
			if (!isdigit(lp->Name[0]))
				Fail("Redefinition of non-local label %s, previously defined at %s (%u).",
					Token.c_str(), fName(lp->Definition.File), lp->Definition.Line);
			// redefinition allowed, but this is always a new label
			lname.first->second = LabelCount;
			goto new_label;
		}
	}
	if (!Pass2)
	{	if (BitOffset & 7)
			Fail("Cannot set a label at a bit boundary. At least byte alignment is required.");
		lp->Value = PC * sizeof(uint64_t) + (BitOffset >> 3);
	}// else if (lp->Name != Token || lp->Value != PC * sizeof(uint64_t))
	//	Fail("Inconsistent label definition during pass 2.");
	lp->Definition = *Context.back();

	lp->Exported = exportable_label;

	if (Preprocessed)
	{	fputs(Token.c_str(), Preprocessed);
		fputs(": ", Preprocessed);
	}
}

void Parser::parseLabel()
{
	if (!isspace(*At))
	{ token_t nt = NextToken();

		// Search for '::name' labels
		bool double_colon = false;
		if (nt == COLON)
		{ nt = NextToken();
			double_colon = true;
		}
		switch (nt)
		{default:
			Fail("Expected label name after '%s', found '%s'.",
					double_colon?"::":":", Token.c_str());
			case WORD:
			case NUM:
			defineLabel(double_colon);
			case END:;
		}
	}
	Flags() |= IF_BRANCH_TARGET;
}

void Parser::parseGLOBAL(int)
{
	if (NextToken() != WORD)
		Fail("Expected global symbol name after .global. Found '%s'.", Token.c_str());
	string name = Token;
	exprValue value;
	switch (NextToken())
	{default:
		Fail("Expected ',' or end of line after symbol name.");
	 case COMMA: // name, value
		value = ParseExpression();
		if (NextToken() != END)
			Fail("Expected end of line.");
		switch (value.Type)
		{default:
			Fail("The value of a global symbol definition must be either a label or an integer constant. Found %s.", type2string(value.Type));
		 case V_INT:
			if (value.iValue < -0x80000000LL || value.iValue > 0xffffffffLL)
				Fail("Cannot export 64 bit constant 0x%" PRIx64 "as symbol.", value.iValue);
		 case V_LABEL:;
		}
		break;
	 case COLON:
		{ // Search for '::name' labels
			token_t nt = NextToken();
			if (nt == COLON)
				nt = NextToken();

			if (nt != WORD)
				Fail("Label name expected. Found '%s'.", Token.c_str());
			name = Token;
			if (NextToken() != END)
				Fail("Expected end of line.");
		}
	 case END: // only label name
		value = exprValue(labelRef(name, false).Value, V_LABEL);
	}
	auto p = GlobalsByName.emplace(name, value);
	if (!p.second && Pass2)
	{	// Doubly defined
		if (p.first->second == value)
			Msg(INFO, "Label '%s' has already been marked as global.", name.c_str());
		else
			Msg(ERROR, "Another label or value has already been assigned to the global symbol '%s'.", name.c_str());
	}
}

void Parser::parseDATA(int bits)
{	if (doPreprocessor())
		return;
	int alignment = 0;
 next:
	exprValue value = ParseExpression();
	if (value.Type != V_INT && value.Type != V_FLOAT)
		Fail("Immediate data instructions require integer or floating point constants. Found %s.", type2string(value.Type));
	switch (bits)
	{case -64: // double
		if (value.Type == V_INT)
			value.fValue = value.iValue;
		goto flt;
	 case -32: // float
		{	union
			{	uint32_t uVal;
				float    fVal;
			} cvt;
			cvt.fVal = value.Type == V_INT ? value.iValue : value.fValue;
			value.iValue = cvt.uVal;
		}
	 flt:
		bits = -bits;
		break;
	 case -16: // half precision float
		try
		{	value.iValue = Eval::toFloat16(value);
			goto flt;
		} catch (const Eval::Fail& msg) // Messages from Eval are not yet enriched.
		{	throw enrichMsg(msg);
		}
	 case 1: // bit
		if (value.iValue & ~1)
			Msg(WARNING, "Bit value out of range: 0x%" PRIx64, value.iValue);
		break;
	 default: // 2 to 32 bit integer
		{	int32_t lower = -1 << (bits - 1);
			int64_t upper = (1ULL << bits) -1;
			if (value.iValue > upper || value.iValue < lower)
				Msg(WARNING, "%u bits integer value out of range [%" PRIi32 ", %" PRIi64 "]: 0x%" PRIx64, bits, lower, upper, value.iValue);
			value.iValue &= upper;
			break;
		}
	 case 64:;
	}
	// Ensure slot
	if (!BitOffset)
		StoreInstruction(0);
	// Check alignment
	else if (!alignment && (alignment = BitOffset & (bits-1)) != 0)
		Msg(WARNING, "Unaligned immediate data directive. %i bits missing for correct alignment.", bits - alignment);
	// Prevent optimizer across .data segment
	Flags() |= IF_BRANCH_TARGET|IF_DATA;
	// store value
	uint64_t& target = Instructions[PC];
	target |= value.iValue << BitOffset;
	if ((BitOffset += bits) >= 64)
	{	++PC;
		// If value crosses instruction boundary => store remaining part
		if ((BitOffset -= 64) != 0)
		{	StoreInstruction((uint64_t)value.iValue >> (bits - BitOffset));
			// Prevent optimizer across .data segment
			Flags() |= IF_BRANCH_TARGET|IF_DATA;
	}	}
	switch (NextToken())
	{default:
		Fail("Syntax error. Expected ',' or end of line.");
	 case COMMA:
		goto next;
	 case END:;
	}
	Instruct.reset();
}

void Parser::parseALIGN(int bytes)
{	if (doPreprocessor())
		return;

	if (bytes < 0)
	{	auto val = ParseExpression();
		if (val.Type != V_INT)
			Fail("Expected integer constant after .align.");
		if (val.iValue > 64 || val.iValue < 0)
			Fail("Alignment value must be in the range [0, 64].");
		bytes = (int)val.iValue;
		if (bytes & (bytes-1))
			Fail("Alignment value must be a power of 2.");
	}
	int offset = 0;
	switch (NextToken())
	{default:
		Fail("Expected end of line or ,<offset>.");
	 case COMMA:
		{	auto val = ParseExpression();
			switch (val.Type)
			{default:
				Fail("Expected integer constant or label after .align.");
			 case V_LABEL:
			 case V_INT:
				offset = -val.iValue & 63;
			}
			if (NextToken() != END)
				Fail("Expected end of line after alignment directive.");
		}
	 case END:;
	}

	doALIGN(bytes, offset);
}

bool Parser::doALIGN(int bytes, int offset)
{	if (!bytes)
		return false;

	// bit alignment
	int align = (BitOffset+(offset<<3)) & 63 & ((bytes<<3)-1);
	if (align)
	{	BitOffset += 8*bytes - align;
		if (BitOffset >= 64) // cannot overflow
		{	++PC;
			BitOffset = 0;
		}
	}

	// Instruction level alignment
	bytes >>= 3;
	offset >>= 3;
	if (!bytes || !((PC+offset) & --bytes))
		return align != 0;
	// BitOffset is necessarily zero at this point.
	do
	{	StoreInstruction(0);
		++PC;
	} while ((PC+offset) & bytes);
	return true;
}

void Parser::beginLOCAL(int)
{
	if (doPreprocessor())
		return;

	if (NextToken() != END)
		Fail("Expected end of line.");

	Context.emplace_back(new fileContext(CTX_BLOCK, Context.back()->File, Context.back()->Line));
}

void Parser::endLOCAL(int)
{
	if (doPreprocessor())
		return;

	if (Context.back()->Type != CTX_BLOCK)
		Fail(".endloc without .local in the current file.");
	// keep line
	unsigned line = Context.back()->Line;
	Context.pop_back();
	Context.back()->Line = line;
}

void Parser::beginREP(int mode)
{
	if (doPreprocessor())
		return;

	auto name = mode ? ".foreach" : ".rep";
	AtMacro = &Macros[name];
	AtMacro->Definition = *Context.back();

	if (NextToken() != WORD)
		Fail("Expected loop variable name after %s.", name);

	AtMacro->Args.push_back(Token);
	if (NextToken() != COMMA)
		Fail(mode ? "Expected ', <parameters>' at .foreach." : "Expected ', <count>' at .rep.");

	{nextpar:
		const auto& expr = ParseExpression();
		if (!mode && (expr.Type != V_INT || (uint64_t)expr.iValue > 0x1000000))
			Fail("Second argument to .rep must be a non-negative integral number. Found %s", expr.toString().c_str());
		AtMacro->Args.push_back(expr.toString());

		switch (NextToken())
		{default:
			Fail("Expected ',' or end of line.");
		 case COMMA:
			if (!mode)
				Fail("Expected end of line.");
			goto nextpar;
		 case END:;
		}
	}
}

void Parser::endREP(int mode)
{
	auto name = mode ? ".foreach" : ".rep";
	auto iter = Macros.find(name);
	if (AtMacro != &iter->second)
	{	if (doPreprocessor())
			return;
		Fail("%s without %s", Token.c_str(), name);
	}
	const macro m = *AtMacro;
	AtMacro = NULL;
	Macros.erase(iter);

	if (NextToken() != END)
		Fail("Expected end of line.");

	if (m.Args.size() < 2)
		return; // no loop count => 0

	// Setup invocation context
	saveContext ctx(*this, new fileContext(CTX_MACRO, m.Definition.File, m.Definition.Line));

	// loop
	size_t count;
	if (mode)
		count = m.Args.size()-1;
	else
		sscanf(m.Args[1].c_str(), "%zi", &count);
	auto& current = *Context.back();
	auto& value = current.Consts.emplace(m.Args.front(), constDef(exprValue((int64_t)0), current)).first->second.Value;
	for (size_t i = 0; i < count; ++i)
	{	// set argument
		if (mode)
		{	strncpy(Line, m.Args[i+1].c_str(), sizeof(Line));
			At = Line;
			value = ParseExpression();
		} else
			value.iValue = i;
		// Invoke body
		for (const string& line : m.Content)
		{	++Context.back()->Line;
			strncpy(Line, line.c_str(), sizeof(Line));
			ParseLine();
		}
	}
}

void Parser::beginBACK(int)
{
	if (doPreprocessor())
		return;

	if (Back)
		Fail("Cannot nest .back directives.");
	exprValue param = ParseExpression();
	if (param.Type != V_INT)
		Fail("Expected integer constant after .back.");
	if (param.iValue < 0)
		Fail(".back expects positive integer argument.");
	if (param.iValue > 10)
		Fail("Cannot move instructions more than 10 slots back.");
	if ((unsigned)param.iValue > PC)
		Fail("Cannot move instructions back before the start of the code.");
	if (NextToken() != END)
		Fail("Expected end of line, found '%s'.", Token.c_str());
	Back = (unsigned)param.iValue;
	size_t pos = PC -= Back;
	// Load last instruction before .back to provide combine support
	if (pos)
		Instruct.decode(Instructions[pos-1]);

	if (Pass2)
		while (++pos < PC + Back)
			if (InstFlags[pos] & IF_BRANCH_TARGET)
				Msg(WARNING, ".back crosses branch target at address 0x%zx. Code might not work.", pos*8);
}

void Parser::endBACK(int)
{
	if (doPreprocessor())
		return;

	/* avoids .back 0  if (!Back)
		Fail(".endb without .back.");*/
	if (NextToken() != END)
		Fail("Expected end of line, found '%s'.", Token.c_str());
	PC += Back;
	Back = 0;
	// Restore last instruction to provide combine support
	if (PC)
		Instruct.decode(Instructions[PC-1]);
}

void Parser::parseCLONE(int)
{
	if (doPreprocessor())
		return;

	exprValue param1 = ParseExpression();
	if (param1.Type != V_LABEL)
		Fail("The first argument to .clone must by a label. Found %s.", type2string(param1.Type));
	param1.iValue >>= 3; // offset in instructions rather than bytes
	if (NextToken() != COMMA)
		Fail("Expected ', <count>' at .clone.");
	exprValue param2 = ParseExpression();
	if (param2.Type != V_INT || (uint64_t)param2.iValue > 3)
		Fail("Expected integer constant in the range [0,3].");
	// Fast exit without any further checks.
	if (param2.iValue == 0)
		return;

	FlagsSize(PC + (unsigned)param2.iValue);
	param2.iValue += param1.iValue; // end offset rather than count
	if (Pass2 && (unsigned)param2.iValue >= Instructions.size())
		Fail("Cannot clone behind the end of the code.");

	if (doALIGN(8, 0))
		Msg(WARNING, "Used padding to enforce 64 bit alignment of GPU instruction.");

	auto src = (unsigned)param1.iValue;
	do
	{	InstFlags[PC] |= InstFlags[src] & ~IF_BRANCH_TARGET;
		if ((Instructions[src] & 0xF000000000000000ULL) == 0xF000000000000000ULL)
			Msg(WARNING, "You should not clone branch instructions. (#%u)", src - (unsigned)param1.iValue);
		StoreInstruction(Instructions[src]);
		++PC;
		++src;
	} while (src < (unsigned)param2.iValue);
	// Restore last instruction to provide combine support
	Instruct.decode(Instructions[PC-1]);
}

void Parser::parseSET(int flags)
{
	if (doPreprocessor())
		return;

	if (NextToken() != WORD)
		Fail("Directive .set requires identifier.");
	string name = Token;
	switch (NextToken())
	{default:
		Fail("Directive .set requires ', <value>' or '(<arguments>) <value>'. Found %s.", Token.c_str());
	 case BRACE1:
		{	function func(*Context.back());
		 next:
			if (NextToken() != WORD)
				Fail("Function argument name expected. Found '%s'.", Token.c_str());
			func.Args.push_back(Token);
			switch (NextToken())
			{default:
				Fail("Expected ',' or ')' after function argument.");
			 case NUM:
				Fail("Function arguments must not start with a digit.");
			 case COMMA:
				goto next;
			 case END:
				Fail("Unexpected end of function argument definition");
			 case BRACE2:
				break;
			}

			// Anything after ')' is function body and evaluated delayed
			At += strspn(At, " \t\r\n,");
			func.DefLine = Line;
			func.Start = At;

			const auto& ret = Functions.emplace(name, func);
			if (!ret.second)
			{	Msg(INFO, "Redefinition of function %s.\n"
				      "Previous definition at %s (%u).",
				  Token.c_str(), fName(ret.first->second.Definition.File), ret.first->second.Definition.Line);
				ret.first->second = func;
			}
			break;
		}
	 case COMMA:
		{	exprValue expr = ParseExpression();
			if (NextToken() != END)
				Fail("Syntax error: unexpected %s.", Token.c_str());

			auto& consts = (flags & C_LOCAL ? Context.back() : Context.front())->Consts;
			auto r = consts.emplace(name, constDef(expr, *Context.back()));
			if (!r.second)
			{	if (flags & C_CONST)
					// redefinition not allowed
					Fail("Identifier %s has already been defined at %s (%u).",
						name.c_str(), fName(r.first->second.Definition.File), r.first->second.Definition.Line);
				r.first->second.Value = expr;
			}
		}
	}
}

void Parser::parseUNSET(int flags)
{
	if (doPreprocessor())
		return;

	if (NextToken() != WORD)
		Fail("Directive .unset requires identifier.");
	string Name = Token;
	if (NextToken() != END)
		Fail("Syntax error: unexpected %s.", Token.c_str());

	auto& consts = (flags & C_LOCAL ? Context.back() : Context.front())->Consts;
	auto r = consts.find(Name);
	if (r == consts.end())
		return Msg(WARNING, "Cannot unset %s because it has not yet been definied in the required context.", Name.c_str());
	consts.erase(r);
}

bool Parser::doCondition()
{
	const exprValue& param = ParseExpression();
	if (param.Type != V_INT)
		Fail("Conditional expression must be a integer constant, found '%s'.", param.toString().c_str());
	if (NextToken() != END)
		Fail("Expected end of line, found '%s'.", Token.c_str());
	return param.iValue != 0;
}

void Parser::parseIF(int)
{
	if (doPreprocessor(PP_MACRO))
		return;

	AtIf.emplace_back(*Context.back(), isDisabled() ? 4 : doCondition());
}

void Parser::parseIFSET(int)
{
	if (doPreprocessor(PP_MACRO))
		return;

	if (NextToken() != WORD)
		Fail("Expected identifier after .ifset, found '%s'.", Token.c_str());

	int state = 4;
	if (!isDisabled())
	{	state = 0;
		for (auto i = Context.end(); i != Context.begin(); )
		{	auto c = (*--i)->Consts.find(Token);
			if (c != (*i)->Consts.end())
			{	state = 1;
				break;
			}
		}
	}

	if (NextToken() != END)
		Fail("Expected end of line, found '%s'.", Token.c_str());

	AtIf.emplace_back(*Context.back(), state);
}

void Parser::parseELSEIF(int)
{
	if (doPreprocessor(PP_MACRO))
		return;

	if (!AtIf.size())
		Fail(".elseif without .if");

	if (AtIf.back().State == IS_FALSE)
		AtIf.back().State = doCondition();
	else
		AtIf.back().State |= IS_ELSE;
}

void Parser::parseELSE(int)
{
	if (doPreprocessor(PP_MACRO))
		return;

	if (!AtIf.size())
		Fail(".else without .if");
	if (NextToken() != END)
		Msg(ERROR, "Expected end of line. .else has no arguments.");

	// Hack for .else
	// State transition from IS_FALSE to IF_TRUE and from IF_TRUE to IS_FALSE|IS_ELSE
	++AtIf.back().State;
}

void Parser::parseENDIF(int)
{
	if (doPreprocessor(PP_MACRO))
		return;

	if (NextToken() != END)
		Msg(ERROR, "Expected end of line. .endif has no arguments.");
	if (!AtIf.size())
		Fail(".endif without .if.");
	if (AtIf.back().File != Context.back()->File)
		Fail(".endif without .if in the same file.");

	AtIf.pop_back();
}

void Parser::parseASSERT(int)
{
	if (doPreprocessor())
		return;

	if (!doCondition())
		Msg(ERROR, "Assertion failed.");
}

void Parser::beginMACRO(int flags)
{
	if (doPreprocessor(PP_IF))
		return;

	if (AtMacro)
		Fail("Cannot nest macro definitions.\n"
		     "  In definition of macro starting at %s (%u).",
		     fName(AtMacro->Definition.File), AtMacro->Definition.Line);
	if (NextToken() != WORD)
		Fail("Expected macro name.");
	AtMacro = &(flags & M_FUNC ? MacroFuncs : Macros)[Token];
	if (!!AtMacro->Definition)
	{	Msg(INFO, "Redefinition of macro %s.\n"
		          "  Previous definition at %s (%u).",
		  Token.c_str(), fName(AtMacro->Definition.File), AtMacro->Definition.Line);
		// redefine
		AtMacro->Args.clear();
		AtMacro->Content.clear();
	}
	AtMacro->Definition = *Context.back();
	AtMacro->Flags = (macroFlags)flags;

	int brace = 0;
	while (true)
		switch (NextToken())
		{case BRACE1:
			if (!AtMacro->Args.size())
			{	brace = 1;
				goto comma;
			}
		 default:
			Fail("Expected ',' before (next) macro argument.");
		 case NUM:
			Fail("Macro arguments must not be numbers.");
		 case COMMA:
			if (brace == 2)
				Fail("Expected end of line after closing brace.");
		 comma:
			// Macro argument
			if (NextToken() != WORD)
				Fail("Macro argument name expected. Found '%s'.", Token.c_str());
			AtMacro->Args.push_back(Token);
			break;
		 case BRACE2:
			if (brace != 1)
				Fail("Unexpected closing brace.");
			brace = 2;
			break;
		 case END:
			if (brace == 1)
				Fail("Closing brace is missing");
			return;
		}
}

void Parser::endMACRO(int flags)
{
	if (doPreprocessor(PP_IF))
		return;

	if (!AtMacro)
		Fail(".%s outside a macro definition.", Token.c_str());
	if (AtMacro->Flags != flags)
		Fail("Cannot close this macro with .%s. Expected .end%c.", Token.c_str(), flags & M_FUNC ? 'f' : 'm');
	AtMacro = NULL;
	if (NextToken() != END)
		Msg(ERROR, "Expected end of line.");
}

void Parser::doMACRO(macros_t::const_iterator m)
{
	// Fetch macro arguments
	const auto& argnames = m->second.Args;
	vector<exprValue> args;
	if (argnames.size())
	{	args.reserve(argnames.size());
		while (true)
		{	args.emplace_back(ParseExpression());
			switch (NextToken())
			{default:
				Fail("internal error");
			 case COMMA:
				if (args.size() == argnames.size())
					Fail("Too much arguments for macro %s.", m->first.c_str());
				continue;
			 case END:
				if (args.size() != argnames.size())
					Fail("Too few arguments for macro %s.", m->first.c_str());
			}
			break;
		}
	} else if (NextToken() != END)
		Fail("The macro %s does not take arguments.", m->first.c_str());

	// Setup invocation context
	saveContext ctx(*this, new fileContext(CTX_MACRO, m->second.Definition.File, m->second.Definition.Line));

	// setup args inside new context to avoid interaction with argument values that are also functions.
	auto& current = *Context.back();
	current.Consts.reserve(current.Consts.size() + argnames.size());
	size_t n = 0;
	for (auto arg : argnames)
		current.Consts.emplace(arg, constDef(args[n++], current));

	// Invoke macro
	for (const string& line : m->second.Content)
	{	++Context.back()->Line;
		strncpy(Line, line.c_str(), sizeof(Line));
		ParseLine();
	}
}

exprValue Parser::doFUNCMACRO(macros_t::const_iterator m)
{
	if (NextToken() != BRACE1)
		Fail("Expected '(' after function name.");

	// Fetch macro arguments
	const auto& argnames = m->second.Args;
	vector<exprValue> args;
	args.reserve(argnames.size());
	if (argnames.size() == 0)
	{	// no arguments
		if (NextToken() != BRACE2)
			Fail("Expected ')' because function %s has no arguments.", m->first.c_str());
	} else
	{next:
		args.push_back(ParseExpression());
		switch (NextToken())
		{case BRACE2:
			// End of argument list. Are we complete?
			if (args.size() != argnames.size())
				Fail("Too few arguments for function %s. Expected %zu, found %zu.", m->first.c_str(), argnames.size(), args.size());
			break;
		 default:
			Fail("Unexpected '%s' in argument list of function %s.", Token.c_str(), m->first.c_str());
		 case COMMA:
			// next argument
			if (args.size() == argnames.size())
				Fail("Too much arguments for function %s. Expected %zu.", m->first.c_str(), argnames.size());
			goto next;
		}
	}

	// Setup invocation context
	saveLineContext ctx(*this, new fileContext(CTX_MACRO, m->second.Definition.File, m->second.Definition.Line));

	// setup args inside new context to avoid interaction with argument values that are also functions.
	auto& current = *Context.back();
	current.Consts.reserve(current.Consts.size() + argnames.size());
	size_t n = 0;
	for (auto arg : argnames)
		current.Consts.emplace(arg, constDef(args[n++], current));

	// Invoke macro
	exprValue ret;
	for (const string& line : m->second.Content)
	{	++Context.back()->Line;
		strncpy(Line, line.c_str(), sizeof(Line));
		At = Line;
		switch (NextToken())
		{case DOT:
			// directives
			ParseDirective();
		 case END:
			break;

		 case COLON:
		 label:
			Msg(ERROR, "Label definition not allowed in functional macro %s.", m->first.c_str());
			break;

		 default:
			if (doPreprocessor())
				break;
			// read-ahead to see if the next token is a colon in which case
			// this is a label.
			if (*At == ':')
				goto label;
			if (ret.Type != V_NONE)
			{	Msg(ERROR, "Only one expression allowed per functional macro.");
				break;
			}
			At -= Token.size();
			ret = ParseExpression();
		}
	}
	if (ret.Type == V_NONE)
		Fail("Failed to return a value in functional macro %s.", m->first.c_str());
	return ret;
}

exprValue Parser::doFUNC(funcs_t::const_iterator f)
{
	if (NextToken() != BRACE1)
		Fail("Expected '(' after function name.");

	// Fetch macro arguments
	const auto& argnames = f->second.Args;
	vector<exprValue> args;
	args.reserve(argnames.size());
	if (argnames.size() == 0)
	{	// no arguments
		if (NextToken() != BRACE2)
			Fail("Expected ')' because function %s has no arguments.", f->first.c_str());
	} else
	{next:
		args.push_back(ParseExpression());
		switch (NextToken())
		{case BRACE2:
			// End of argument list. Are we complete?
			if (args.size() != argnames.size())
				Fail("Too few arguments for function %s. Expected %zu, found %zu.", f->first.c_str(), argnames.size(), args.size());
			break;
		 default:
			Fail("Unexpected '%s' in argument list of function %s.", Token.c_str(), f->first.c_str());
		 case COMMA:
			// next argument
			if (args.size() == argnames.size())
				Fail("Too much arguments for function %s. Expected %zu.", f->first.c_str(), argnames.size());
			goto next;
		}
	}

	// Setup invocation context
	saveLineContext ctx(*this, new fileContext(CTX_FUNCTION, f->second.Definition.File, f->second.Definition.Line));
	strncpy(Line, f->second.DefLine.c_str(), sizeof Line);
	At = f->second.Start;
	// setup args inside new context to avoid interaction with argument values that are also functions.
	auto& current = *Context.back();
	current.Consts.reserve(current.Consts.size() + argnames.size());
	unsigned n = 0;
	for (auto arg : argnames)
		current.Consts.emplace(arg, constDef(args[n++], current));

	const exprValue&& val = ParseExpression();
	if (NextToken() != END)
		Fail("Function %s evaluated to an incomplete expression.", f->first.c_str());
	return val;
}

void Parser::doSEGMENT(int flags)
{
	if (doPreprocessor())
		return;

	if (NextToken() != END)
		Msg(ERROR, "End of line expected.");

	// Search segment entry at PC
	auto ptr = upper_bound(Segments.begin()+1, Segments.end(), PC,
		[](decltype(PC) pc, const segment& seg) { return pc < seg.Start; } );
	auto cur = ptr - 1;

	// Segment attributes happen to match => no action
	if (cur->Flags == flags)
		return;
	// otherwise ensure distinct entry at PC
	if (cur->Start != PC)
		Segments.insert(ptr, segment(PC, (uint8_t)flags));
	else
		cur->Flags = flags;
}

void Parser::doINCLUDE(int)
{
	if (doPreprocessor())
		return;

	At += strspn(At, " \t\r\n");
	{	// remove trailing blanks
		char* cp = At + strlen(At);
		while (strchr(" \t\r\n", *--cp))
			*cp = 0;
	}
	size_t len = strlen(At);
	if ((*At != '"' || At[len-1] != '"') && (*At != '<' || At[len-1] != '>'))
		Fail("Syntax error. Expected \"file-name\" or <file-name> after .include, found '%s'.", At);
	Token.assign(At+1, len-2);

	// find file
	struct stat buffer;
	string file;
	if (*At == '<')
	{	// check include paths first
		for (string path : IncludePaths)
		{	file = path + Token;
			if (stat(file.c_str(), &buffer) == 0)
				goto got_it;
		}
	}
	file = relpath(SourceFiles[Context.back()->File].Name, Token);
	if (stat(file.c_str(), &buffer) != 0)
		Fail("Cannot locate included file '%s'.", Token.c_str());

 got_it:
	if (Pass2)
	{	const auto& p1file = SourceFiles[FilesCount];
		if (p1file.Name != file)
			Fail("Inconsistent include invocation during pass 2.");
		++FilesCount;
	} else
	{	SourceFiles.emplace_back(file, *Context.back());
		FilesCount = SourceFiles.size();
	}
	saveContext ctx(*this, new fileContext(CTX_INCLUDE, FilesCount-1, 0));
	ParseFile();
}

void Parser::ParseDirective()
{
	if (NextToken() != WORD)
		Fail("Expected assembler directive after '.'. Found '%s'.", Token.c_str());

	const opEntry<8>* op = binary_search(directiveMap, Token.c_str());
	if (!op)
		Fail("Invalid assembler directive: %s", Token.c_str());

	(this->*op->Func)(op->Arg);
}

bool Parser::doPreprocessor(preprocType type)
{
	if (AtMacro && (type & PP_MACRO))
	{	AtMacro->Content.push_back(Line);
		return true;
	}
	return (type & PP_IF) && isDisabled();
}

void Parser::ParseLine()
{
	At = Line;
	bool trycombine = false;
	bool isinst = false;
	size_t pos = PC;
	FlagsSize(pos + 1);

 next:
	switch (NextToken())
	{case DOT:
		if (isinst)
			goto def;
		// directives
		ParseDirective();
		return;

	 case END:
		*Line = 0;
		doPreprocessor(PP_MACRO);
		return;

	 case COLON:
		if (doPreprocessor())
			return;
		if (isinst)
			goto def;

		parseLabel();
		goto next;

	 case SEMI:
		if (doPreprocessor())
			return;
		if (pos && (InstFlags[pos-1] & IF_CMB_ALLOWED) && (InstFlags[pos] & IF_BRANCH_TARGET) == 0)
			trycombine = true;
		isinst = true;
		goto next;

	 def:
	 default:
		if (!AtMacro || (AtMacro->Flags & M_FUNC) == 0)
			Fail("Syntax error. Unexpected '%s'.", Token.c_str());
	 case WORD:
		if (doPreprocessor())
			return;

		// read-ahead to see if the next token is a colon in which case
		// this is a label.
		if (*At == ':')
		{	// Search for '::name' labels
			if (*(At+1) == ':')
			{ ++At;
				defineLabel(true);
			}else{
				defineLabel();
			}
			InstFlags[pos] |= IF_BRANCH_TARGET;
			++At;
			goto next;
		}

		// Try macro
		macros_t::const_iterator m = Macros.find(Token);
		if (m != Macros.end())
		{	doMACRO(m);
			return;
		}

		if (doALIGN(8, 0))
			Msg(WARNING, "Used padding to enforce 64 bit alignment of GPU instruction.");

		if (trycombine)
		{	char* atbak = At;
			bool succbak = Success;
			string tokenbak = Token;
			try
			{	// Try to parse into existing instruction.
				ParseInstruction();
				// Do not combine TMU instructions
				if ( Instruct.Sig < Inst::S_LDI
					&& ( ((0xfff09e0000000000ULL & (1ULL << Instruct.WAddrA)) != 0)
					+ ((0xfff09e0000000000ULL & (1ULL << Instruct.WAddrM)) != 0)
					+ ((0x0008060000000000ULL & (1ULL << Instruct.RAddrA)) != 0)
					+ (Instruct.Sig != Inst::S_SMI && (0x0008060000000000ULL & (1ULL << Instruct.RAddrB)) != 0)
					+ ( Instruct.Sig == Inst::S_LDTMU0 || Instruct.Sig == Inst::S_LDTMU1
						|| Instruct.Sig == Inst::S_LOADCV || Instruct.Sig == Inst::S_LOADAM
						|| (Instruct.Sig == Inst::S_LDI && (Instruct.LdMode & Inst::L_SEMA)) )
					+ (Instruct.WAddrA == 45 || Instruct.WAddrA == 46 || Instruct.WAddrM == 45 || Instruct.WAddrM == 46 || Instruct.Sig == Inst::S_LOADC || Instruct.Sig == Inst::S_LDCEND) ) > 1 )
					throw string();
				// Combine succeeded
				Instructions[pos-1] = Instruct.encode();
				return;
			} catch (const string& msg)
			{	// Combine failed => try new instruction.
				At = atbak;
				Success = succbak;
				Token = tokenbak;
			}
		}
		// new instruction
		Instruct.reset();

		ParseInstruction();
		StoreInstruction(Instruct.encode());
		if (Pass2)
		{	LineForInstruction.emplace_back(Line);
			auto &l = LineForInstruction.back();
			if (l.back() == '\n')
				l.pop_back();
			// Truncate '#'- Comments
			l.erase(l.begin() + min(l.find("#"), l.size()), l.end());
			// Trim
			l.erase(l.begin(), l.begin() + l.find_first_not_of("\t ") );
			l.erase(l.begin() + l.find_last_not_of("\t ") + 1, l.end());
		}

		++PC;
		return;
	}
}

void Parser::ParseFile()
{
	LineForInstruction.clear();
	FILE* f = fopen(fName(Context.back()->File), "r");
	if (!f)
		Fail("Failed to open file %s.", fName(Context.back()->File));
	auto ifs = AtIf.size();
	try
	{	while (fgets(Line, sizeof(Line), f))
		{	++Context.back()->Line;
			try
			{	ParseLine();
			} catch (const string& msg)
			{	// recover from errors
				Success = false;
				fputs(msgpfx[ERROR], stderr);
				fputs(msg.c_str(), stderr);
				fputc('\n', stderr);
			}

			if (AtMacro && AtMacro->Definition.Line == 0)
				Fail("!!!");
		}
		fclose(f);
	} catch (...)
	{	fclose(f);
		throw;
	}
	if (ifs < AtIf.size())
		Fail("Unterminated .if at line %u.", AtIf[ifs].Line);
}
void Parser::ParseFile(const string& file)
{	if (Pass2)
		throw string("Cannot add another file after pass 2 has been entered.");
	SourceFiles.emplace_back(file);
	FilesCount = SourceFiles.size();
	saveContext ctx(*this, new fileContext(CTX_FILE, FilesCount-1, 0));
	try
	{	ParseFile();
	} catch (const string& msg)
	{	// recover from errors
		Success = false;
		fputs(msgpfx[ERROR], stderr);
		fputs(msg.c_str(), stderr);
		fputc('\n', stderr);
	}
}

void Parser::ResetPass()
{	AtMacro = NULL;
	AtIf.clear();
	Context.clear();
	Context.emplace_back(new fileContext(CTX_ROOT, 0, 0));
	FilesCount = 0;
	Functions.clear();
	Macros.clear();
	LabelsByName.clear();
	GlobalsByName.clear();
	LabelCount = 0;
	InstFlags.clear();
	PC = 0;
	Instruct.reset();
	BitOffset = 0;
	Segments.resize(1);
	Segments[0].Start = 0;
	Segments[0].Flags = SF_None;
}

void Parser::EnsurePass2()
{
	if (Pass2 || !Success)
		return;

	// enter pass 2
	Pass2 = true;
	ResetPass();

	// Check all labels
	for (auto& label : Labels)
	{	if (!label.Definition)
			Msg(ERROR, "Label '%s' is undefined. Referenced from %s (%u).\n",
				label.Name.c_str(), fName(label.Reference.File), label.Reference.Line);
		if (!label.Reference)
			Msg(INFO, "Label '%s' defined at %s (%u) is not used.\n",
				label.Name.c_str(), fName(label.Definition.File), label.Definition.Line);
		// prepare for next pass
		label.Definition.Line = 0;
	}

	//for (auto& file : SourceFiles)
	while (FilesCount < SourceFiles.size())
	{	const auto& file = SourceFiles[FilesCount];
		if (!!file.Parent)
			Fail("Inconsistent include files during pass 2.");
		saveContext ctx(*this, new fileContext(CTX_FILE, FilesCount, 0));
		++FilesCount;
		ParseFile();
	}

	// Optimize instructions identify code segments automatically
	unsigned pc = 0;
	Inst optimized;
	auto sp = Segments.begin();
	auto np = sp + 1;
	bool autocode = false;
	for (auto& inst : Instructions)
	{	// next segment?
		if (np != Segments.end() && np->Start <= pc)
		{	sp = np++;
			autocode = false;
		}
		if ((InstFlags[pc++] & IF_DATA) == 0)
		{	// instruction
			optimized.decode(inst);
			optimized.optimize();
			inst = optimized.encode();
			// auto code?
			if (sp->Flags == SF_None)
			{	if (sp->Start != pc)
				{	// not current pc => insert new segment
					sp = Segments.emplace(np, pc, SF_Code);
					np = sp + 1;
				} else
					// exact match => just change the flags
					sp->Flags = SF_Code;
				autocode = true;
			}
		} else if (autocode)
		{	// autocode implies that sp->Start != pc && last Flags = SF_None
			// => insert an entry at pc
			sp = Segments.insert(np, segment(pc, SF_None));
			np = sp + 1;
			autocode = false;
		}
	}
}

void Parser::Reset()
{ ResetPass();
	Labels.clear();
	Pass2 = false;
	SourceFiles.clear();
}

Parser::lnames_t&  Parser::getLabelsForIntruction(unsigned int PC, bool exported) const
{
	lnames_t *ret = new lnames_t();
	unsigned int i=0;
	for (auto& label : Labels){
		if (label.Value - 4*PC < 4 && (!exported || label.Exported))
		{
			ret->emplace(label.Name, i);
		}
		++i;
	}
	return *ret;
}

Parser::Parser()
{	Reset();
}
