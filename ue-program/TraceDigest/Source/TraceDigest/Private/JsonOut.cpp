// Copyright (c) ue-trace-mcp. Licensed under the MIT License.

#include "JsonOut.h"

namespace TraceDigest
{

static FString EscapeJson(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len() + 2);
	for (TCHAR C : In)
	{
		switch (C)
		{
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\b'): Out += TEXT("\\b");  break;
			case TEXT('\f'): Out += TEXT("\\f");  break;
			case TEXT('\n'): Out += TEXT("\\n");  break;
			case TEXT('\r'): Out += TEXT("\\r");  break;
			case TEXT('\t'): Out += TEXT("\\t");  break;
			default:
				if (C < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<int32>(C));
				}
				else
				{
					Out.AppendChar(C);
				}
		}
	}
	return Out;
}

void FJsonOut::Sep()
{
	if (bAfterKey)
	{
		bAfterKey = false;
		return;
	}
	if (NeedsComma.Num() > 0 && NeedsComma.Last())
	{
		Buffer.AppendChar(TEXT(','));
	}
	if (NeedsComma.Num() > 0)
	{
		NeedsComma.Last() = true;
	}
}

void FJsonOut::BeginObject() { Sep(); Buffer.AppendChar(TEXT('{')); NeedsComma.Push(false); }
void FJsonOut::EndObject()   { Buffer.AppendChar(TEXT('}')); NeedsComma.Pop(); }
void FJsonOut::BeginArray()  { Sep(); Buffer.AppendChar(TEXT('[')); NeedsComma.Push(false); }
void FJsonOut::EndArray()    { Buffer.AppendChar(TEXT(']')); NeedsComma.Pop(); }

void FJsonOut::Key(const TCHAR* K)
{
	Sep();
	Buffer += FString::Printf(TEXT("\"%s\":"), *EscapeJson(K));
	bAfterKey = true;
}

void FJsonOut::Str(const FString& V) { Sep(); Buffer += FString::Printf(TEXT("\"%s\""), *EscapeJson(V)); }

void FJsonOut::Num(double V)
{
	Sep();
	if (!FMath::IsFinite(V))
	{
		Buffer += TEXT("null");  // JSON has no NaN/Inf
	}
	else
	{
		// %g with enough precision to round-trip a double, no trailing zeros.
		Buffer += FString::Printf(TEXT("%.6g"), V);
	}
}

void FJsonOut::Int(int64 V)
{
	Sep();
	Buffer += FString::Printf(TEXT("%lld"), V);
}

void FJsonOut::Bool(bool V) { Sep(); Buffer += V ? TEXT("true") : TEXT("false"); }
void FJsonOut::Null()       { Sep(); Buffer += TEXT("null"); }

void FJsonOut::KeyStr (const TCHAR* K, const FString& V) { Key(K); Str(V);  }
void FJsonOut::KeyNum (const TCHAR* K, double V)         { Key(K); Num(V);  }
void FJsonOut::KeyInt (const TCHAR* K, int64 V)          { Key(K); Int(V);  }
void FJsonOut::KeyBool(const TCHAR* K, bool V)           { Key(K); Bool(V); }

FString FJsonOut::ToString() const { return Buffer; }

void FJsonOut::Reset()
{
	Buffer.Empty();
	NeedsComma.Reset();
	bAfterKey = false;
}

} // namespace TraceDigest
