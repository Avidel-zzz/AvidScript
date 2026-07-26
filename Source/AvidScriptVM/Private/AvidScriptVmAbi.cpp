#include "AvidScriptVmBackend.h"

namespace
{
constexpr int32 AvidScriptMaximumAbiParameterCount = 64;

bool TryParseAvidScriptVmValueKind(TCHAR Token, EAvidScriptVmValueKind& OutKind)
{
	switch (Token)
	{
	case TEXT('i'):
		OutKind = EAvidScriptVmValueKind::I32;
		return true;
	case TEXT('I'):
		OutKind = EAvidScriptVmValueKind::I64;
		return true;
	case TEXT('f'):
		OutKind = EAvidScriptVmValueKind::F32;
		return true;
	case TEXT('F'):
		OutKind = EAvidScriptVmValueKind::F64;
		return true;
	default:
		return false;
	}
}
} // namespace

bool ParseAvidScriptVmAbiSignature(
	const FString& CompactSignature,
	FAvidScriptVmAbiSignature& OutSignature,
	FString& OutError)
{
	OutSignature = FAvidScriptVmAbiSignature();
	OutError.Reset();
	if (CompactSignature.Len() < 2 || CompactSignature[0] != TEXT('('))
	{
		OutError = TEXT("Compact WASM ABI signatures must begin with '('.");
		return false;
	}

	const int32 CloseIndex = CompactSignature.Find(TEXT(")"), ESearchCase::CaseSensitive);
	if (CloseIndex == INDEX_NONE || (CloseIndex != CompactSignature.Len() - 1 && CloseIndex != CompactSignature.Len() - 2))
	{
		OutError = TEXT("Compact WASM ABI signatures require one closing ')' and at most one result.");
		return false;
	}
	if (CloseIndex - 1 > AvidScriptMaximumAbiParameterCount)
	{
		OutError = TEXT("Compact WASM ABI signatures support at most 64 parameters.");
		return false;
	}

	OutSignature.Parameters.Reserve(CloseIndex - 1);
	for (int32 Index = 1; Index < CloseIndex; ++Index)
	{
		EAvidScriptVmValueKind Kind = EAvidScriptVmValueKind::I32;
		if (!TryParseAvidScriptVmValueKind(CompactSignature[Index], Kind))
		{
			OutSignature = FAvidScriptVmAbiSignature();
			OutError = FString::Printf(
				TEXT("Compact WASM ABI signature has an unsupported parameter token at index %d."),
				Index);
			return false;
		}
		OutSignature.Parameters.Add(Kind);
	}

	if (CloseIndex + 1 < CompactSignature.Len())
	{
		if (!TryParseAvidScriptVmValueKind(CompactSignature[CloseIndex + 1], OutSignature.Result))
		{
			OutSignature = FAvidScriptVmAbiSignature();
			OutError = TEXT("Compact WASM ABI signature has an unsupported result token.");
			return false;
		}
		OutSignature.bHasResult = true;
	}
	return true;
}
