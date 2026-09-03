#pragma once

#include "CoreMinimal.h"

namespace AvidScriptGameplayEventFixture
{
inline void AppendU32Leb(TArray<uint8>& Bytes, uint32 Value)
{
	do
	{
		uint8 Byte = static_cast<uint8>(Value & 0x7f);
		Value >>= 7;
		if (Value != 0)
		{
			Byte |= 0x80;
		}
		Bytes.Add(Byte);
	} while (Value != 0);
}

inline void AppendString(TArray<uint8>& Bytes, const char* Text)
{
	const int32 Length = static_cast<int32>(FCStringAnsi::Strlen(Text));
	AppendU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Text[Index]));
	}
}

inline void AppendSection(TArray<uint8>& Module, uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

inline void AppendF32(TArray<uint8>& Bytes, float Value)
{
	uint8 RawBytes[sizeof(float)] = {};
	FMemory::Memcpy(RawBytes, &Value, sizeof(float));
	Bytes.Append(RawBytes, UE_ARRAY_COUNT(RawBytes));
}

inline TArray<uint8> Build(bool bTrap, float XOffset = 0.0f, bool bTrapBeginPlay = false)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	const uint8 TypeBytes[] = {
		0x04,
		0x60, 0x05, 0x7f, 0x7f, 0x7d, 0x7d, 0x7d, 0x01, 0x7f,
		0x60, 0x00, 0x00,
		0x60, 0x01, 0x7d, 0x00,
		0x60, 0x08, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7d, 0x7d, 0x7d, 0x00
	};
	Types.Append(TypeBytes, UE_ARRAY_COUNT(TypeBytes));
	AppendSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendU32Leb(Imports, 1);
	AppendString(Imports, "avidscript");
	AppendString(Imports, "actor_set_location");
	Imports.Add(0x00);
	AppendU32Leb(Imports, 0);
	AppendSection(Module, 2, Imports);

	TArray<uint8> Functions;
	const uint8 FunctionBytes[] = { 0x03, 0x01, 0x02, 0x03 };
	Functions.Append(FunctionBytes, UE_ARRAY_COUNT(FunctionBytes));
	AppendSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendU32Leb(Exports, 3);
	AppendString(Exports, "avid_on_begin_play");
	Exports.Add(0x00);
	Exports.Add(0x01);
	AppendString(Exports, "avid_on_tick");
	Exports.Add(0x00);
	Exports.Add(0x02);
	AppendString(Exports, "avid_on_gameplay_event");
	Exports.Add(0x00);
	Exports.Add(0x03);
	AppendSection(Module, 7, Exports);

	TArray<uint8> Code;
	AppendU32Leb(Code, 3);
	const uint8 EmptyBody[] = { 0x00, 0x0b };
	const uint8 TrapBody[] = { 0x00, 0x00, 0x0b };
	AppendU32Leb(Code, bTrapBeginPlay ? UE_ARRAY_COUNT(TrapBody) : UE_ARRAY_COUNT(EmptyBody));
	if (bTrapBeginPlay)
	{
		Code.Append(TrapBody, UE_ARRAY_COUNT(TrapBody));
	}
	else
	{
		Code.Append(EmptyBody, UE_ARRAY_COUNT(EmptyBody));
	}
	AppendU32Leb(Code, UE_ARRAY_COUNT(EmptyBody));
	Code.Append(EmptyBody, UE_ARRAY_COUNT(EmptyBody));

	TArray<uint8> EventBody;
	EventBody.Add(0x00);
	if (bTrap)
	{
		EventBody.Add(0x00);
	}
	else
	{
		EventBody.Add(0x20);
		EventBody.Add(0x03);
		EventBody.Add(0x20);
		EventBody.Add(0x04);
		EventBody.Add(0x20);
		EventBody.Add(0x05);
		if (!FMath::IsNearlyZero(XOffset))
		{
			EventBody.Add(0x43);
			AppendF32(EventBody, XOffset);
			EventBody.Add(0x92);
		}
		EventBody.Add(0x20);
		EventBody.Add(0x06);
		EventBody.Add(0x20);
		EventBody.Add(0x07);
		EventBody.Add(0x10);
		EventBody.Add(0x00);
		EventBody.Add(0x1a);
	}
	EventBody.Add(0x0b);
	AppendU32Leb(Code, static_cast<uint32>(EventBody.Num()));
	Code.Append(EventBody);
	AppendSection(Module, 10, Code);
	return Module;
}
} // namespace AvidScriptGameplayEventFixture
