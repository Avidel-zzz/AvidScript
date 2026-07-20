#include "AvidScriptWamrCallStack.h"

#ifndef AVIDSCRIPT_WITH_WAMR
#define AVIDSCRIPT_WITH_WAMR 0
#endif

#if AVIDSCRIPT_WITH_WAMR
extern "C"
{
#include "wasm_export.h"
}
#endif

namespace
{
constexpr int32 MaxCallStackTextLength = 64 * 1024;
constexpr int32 MaxCallStackLineLength = 256;
constexpr int32 MaxCallStackFrames = 128;

bool TryParseDecimal(const FString& Text, uint32& OutValue)
{
	if (Text.IsEmpty())
	{
		return false;
	}

	uint64 Value = 0;
	for (const TCHAR Character : Text)
	{
		if (Character < TEXT('0') || Character > TEXT('9'))
		{
			return false;
		}
		Value = Value * 10 + static_cast<uint32>(Character - TEXT('0'));
		if (Value > MAX_uint32)
		{
			return false;
		}
	}
	OutValue = static_cast<uint32>(Value);
	return true;
}

bool TryParseHex(const FString& Text, uint32& OutValue)
{
	if (Text.IsEmpty() || Text.Len() > 8)
	{
		return false;
	}

	uint32 Value = 0;
	for (const TCHAR Character : Text)
	{
		uint32 Digit = 0;
		if (Character >= TEXT('0') && Character <= TEXT('9'))
		{
			Digit = static_cast<uint32>(Character - TEXT('0'));
		}
		else if (Character >= TEXT('a') && Character <= TEXT('f'))
		{
			Digit = static_cast<uint32>(Character - TEXT('a')) + 10;
		}
		else if (Character >= TEXT('A') && Character <= TEXT('F'))
		{
			Digit = static_cast<uint32>(Character - TEXT('A')) + 10;
		}
		else
		{
			return false;
		}
		Value = (Value << 4) | Digit;
	}
	OutValue = Value;
	return true;
}
}

bool ParseAvidScriptWamrCallStack(
	const FString& Text,
	TArray<FAvidScriptVmStackFrame>& OutFrames)
{
	OutFrames.Reset();
	if (Text.Len() > MaxCallStackTextLength)
	{
		return false;
	}

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	uint32 ExpectedOrdinal = 0;
	for (FString& Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty())
		{
			continue;
		}
		if (Line.Len() > MaxCallStackLineLength || OutFrames.Num() >= MaxCallStackFrames)
		{
			OutFrames.Reset();
			return false;
		}

		const int32 OffsetMarker = Line.Find(TEXT(": 0x"), ESearchCase::CaseSensitive);
		const int32 TokenMarker = Line.Find(TEXT(" - "), ESearchCase::CaseSensitive);
		if (!Line.StartsWith(TEXT("#"), ESearchCase::CaseSensitive)
			|| OffsetMarker <= 1
			|| TokenMarker <= OffsetMarker + 4)
		{
			OutFrames.Reset();
			return false;
		}

		uint32 Ordinal = 0;
		uint32 FunctionOffset = 0;
		const FString OrdinalText = Line.Mid(1, OffsetMarker - 1);
		const FString OffsetText = Line.Mid(OffsetMarker + 4, TokenMarker - OffsetMarker - 4);
		const FString RawToken = Line.Mid(TokenMarker + 3);
		if (!TryParseDecimal(OrdinalText, Ordinal)
			|| Ordinal != ExpectedOrdinal
			|| !TryParseHex(OffsetText, FunctionOffset)
			|| RawToken.IsEmpty())
		{
			OutFrames.Reset();
			return false;
		}

		FAvidScriptVmStackFrame& Frame = OutFrames.AddDefaulted_GetRef();
		Frame.FunctionOffset = FunctionOffset;
		Frame.RawFunctionToken = RawToken;
		if (RawToken.StartsWith(TEXT("$f"), ESearchCase::CaseSensitive))
		{
			if (!TryParseDecimal(RawToken.Mid(2), Frame.FunctionIndex))
			{
				OutFrames.Reset();
				return false;
			}
		}
		++ExpectedOrdinal;
	}
	return !OutFrames.IsEmpty();
}

void CaptureAvidScriptWamrCallStack(
	void* ExecEnv,
	TArray<FAvidScriptVmStackFrame>& OutFrames)
{
	OutFrames.Reset();
#if AVIDSCRIPT_WITH_WAMR
	if (ExecEnv == nullptr)
	{
		return;
	}

	wasm_exec_env_t WamrExecEnv = static_cast<wasm_exec_env_t>(ExecEnv);
	const uint32 BufferSize = wasm_runtime_get_call_stack_buf_size(WamrExecEnv);
	if (BufferSize <= 1 || BufferSize > static_cast<uint32>(MaxCallStackTextLength + 1))
	{
		return;
	}

	TArray<ANSICHAR> Buffer;
	Buffer.SetNumUninitialized(static_cast<int32>(BufferSize));
	const uint32 DumpedSize = wasm_runtime_dump_call_stack_to_buf(
		WamrExecEnv,
		Buffer.GetData(),
		BufferSize);
	if (DumpedSize <= 1 || DumpedSize > BufferSize || Buffer[DumpedSize - 1] != '\0')
	{
		return;
	}

	FUTF8ToTCHAR Converted(Buffer.GetData(), static_cast<int32>(DumpedSize - 1));
	const FString StackText(Converted.Length(), Converted.Get());
	ParseAvidScriptWamrCallStack(StackText, OutFrames);
#endif
}
