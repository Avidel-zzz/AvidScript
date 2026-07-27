#pragma once

#include "CoreMinimal.h"

namespace AvidScriptVmResultFixture
{
enum class EValueKind : uint8
{
	I32 = 0x7f,
	I64 = 0x7e,
	F32 = 0x7d,
	F64 = 0x7c,
	ExternRef = 0x6f
};

struct FValue
{
	EValueKind Kind = EValueKind::I32;
	uint64 Bits = 0;
};

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

inline void AppendSignedLeb(
	TArray<uint8>& Bytes,
	int64 Value)
{
	bool bMore = true;
	while (bMore)
	{
		uint8 Byte = static_cast<uint8>(Value & 0x7f);
		Value >>= 7;
		const bool bSignBitSet = (Byte & 0x40) != 0;
		bMore = !((Value == 0 && !bSignBitSet) ||
			(Value == -1 && bSignBitSet));
		if (bMore)
		{
			Byte |= 0x80;
		}
		Bytes.Add(Byte);
	}
}

inline void AppendString(TArray<uint8>& Bytes, const ANSICHAR* Value)
{
	const int32 Length = FCStringAnsi::Strlen(Value);
	AppendU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Value[Index]));
	}
}

inline void AppendSection(
	TArray<uint8>& Module,
	const uint8 SectionId,
	const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

inline void AppendLittleEndian(
	TArray<uint8>& Bytes,
	const uint64 Bits,
	const int32 ByteCount)
{
	for (int32 ByteIndex = 0; ByteIndex < ByteCount; ++ByteIndex)
	{
		Bytes.Add(static_cast<uint8>(Bits >> (ByteIndex * 8)));
	}
}

inline void AppendConstant(TArray<uint8>& Body, const FValue& Value)
{
	switch (Value.Kind)
	{
	case EValueKind::I32:
		Body.Add(0x41);
		AppendSignedLeb(
			Body,
			static_cast<int32>(static_cast<uint32>(Value.Bits)));
		break;
	case EValueKind::I64:
		Body.Add(0x42);
		AppendSignedLeb(Body, static_cast<int64>(Value.Bits));
		break;
	case EValueKind::F32:
		Body.Add(0x43);
		AppendLittleEndian(Body, Value.Bits, sizeof(float));
		break;
	case EValueKind::F64:
		Body.Add(0x44);
		AppendLittleEndian(Body, Value.Bits, sizeof(double));
		break;
	case EValueKind::ExternRef:
		Body.Add(0xd0);
		Body.Add(static_cast<uint8>(EValueKind::ExternRef));
		break;
	}
}

inline TArray<uint8> Build(const TConstArrayView<FValue> Results)
{
	TArray<uint8> Module;
	const uint8 Header[] = {
		0x00, 0x61, 0x73, 0x6d,
		0x01, 0x00, 0x00, 0x00
	};
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendU32Leb(Types, 1);
	Types.Add(0x60);
	AppendU32Leb(Types, 0);
	AppendU32Leb(Types, static_cast<uint32>(Results.Num()));
	for (const FValue& Result : Results)
	{
		Types.Add(static_cast<uint8>(Result.Kind));
	}
	AppendSection(Module, 1, Types);

	TArray<uint8> Functions;
	AppendU32Leb(Functions, 1);
	AppendU32Leb(Functions, 0);
	AppendSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendU32Leb(Exports, 1);
	AppendString(Exports, "result_test");
	Exports.Add(0x00);
	AppendU32Leb(Exports, 0);
	AppendSection(Module, 7, Exports);

	TArray<uint8> Body;
	AppendU32Leb(Body, 0);
	for (const FValue& Result : Results)
	{
		AppendConstant(Body, Result);
	}
	Body.Add(0x0b);
	TArray<uint8> Code;
	AppendU32Leb(Code, 1);
	AppendU32Leb(Code, static_cast<uint32>(Body.Num()));
	Code.Append(Body);
	AppendSection(Module, 10, Code);
	return Module;
}

inline TArray<uint8> BuildSingle(
	const EValueKind Kind,
	const uint64 Bits)
{
	const FValue Result{ Kind, Bits };
	return Build(MakeArrayView(&Result, 1));
}
} // namespace AvidScriptVmResultFixture
