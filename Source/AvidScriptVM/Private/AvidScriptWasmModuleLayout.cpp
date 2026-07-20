#include "AvidScriptWasmModuleLayout.h"

#include "Containers/StringConv.h"

namespace
{
constexpr uint32 MaxWasmLayoutItems = 65536;
constexpr uint32 MaxWasmLayoutNameBytes = 1024 * 1024;

class FAvidScriptWasmLayoutReader
{
public:
	explicit FAvidScriptWasmLayoutReader(TConstArrayView<uint8> InBytes)
		: Bytes(InBytes)
	{
	}

	bool IsAtEnd() const
	{
		return Offset == Bytes.Num();
	}

	int32 Remaining() const
	{
		return Bytes.Num() - Offset;
	}

	bool ReadByte(uint8& OutValue)
	{
		if (Offset >= Bytes.Num())
		{
			return false;
		}
		OutValue = Bytes[Offset++];
		return true;
	}

	bool ReadU32Leb(uint32& OutValue)
	{
		uint64 Value = 0;
		for (uint32 ByteIndex = 0; ByteIndex < 5; ++ByteIndex)
		{
			uint8 Byte = 0;
			if (!ReadByte(Byte))
			{
				return false;
			}
			Value |= static_cast<uint64>(Byte & 0x7f) << (ByteIndex * 7);
			if ((Byte & 0x80) == 0)
			{
				if (Value > MAX_uint32 || (ByteIndex == 4 && (Byte & 0xf0) != 0))
				{
					return false;
				}
				OutValue = static_cast<uint32>(Value);
				return true;
			}
		}
		return false;
	}

	bool ReadU64Leb(uint64& OutValue)
	{
		uint64 Value = 0;
		for (uint32 ByteIndex = 0; ByteIndex < 10; ++ByteIndex)
		{
			uint8 Byte = 0;
			if (!ReadByte(Byte))
			{
				return false;
			}
			if (ByteIndex == 9 && (Byte & 0xfe) != 0)
			{
				return false;
			}
			Value |= static_cast<uint64>(Byte & 0x7f) << (ByteIndex * 7);
			if ((Byte & 0x80) == 0)
			{
				OutValue = Value;
				return true;
			}
		}
		return false;
	}

	bool ReadName(FString& OutName)
	{
		uint32 ByteCount = 0;
		if (!ReadU32Leb(ByteCount)
			|| ByteCount == 0
			|| ByteCount > MaxWasmLayoutNameBytes
			|| static_cast<uint64>(ByteCount) > static_cast<uint64>(Remaining()))
		{
			return false;
		}

		for (uint32 ByteIndex = 0; ByteIndex < ByteCount; ++ByteIndex)
		{
			if (Bytes[Offset + static_cast<int32>(ByteIndex)] == 0)
			{
				return false;
			}
		}
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset),
			static_cast<int32>(ByteCount));
		if (Converted.Length() <= 0)
		{
			return false;
		}
		OutName = FString(Converted.Length(), Converted.Get());
		Offset += static_cast<int32>(ByteCount);
		return true;
	}

	bool ReadSubReader(uint32 ByteCount, FAvidScriptWasmLayoutReader& OutReader)
	{
		if (static_cast<uint64>(ByteCount) > static_cast<uint64>(Remaining()))
		{
			return false;
		}
		OutReader = FAvidScriptWasmLayoutReader(
			MakeArrayView(Bytes.GetData() + Offset, static_cast<int32>(ByteCount)));
		Offset += static_cast<int32>(ByteCount);
		return true;
	}

private:
	TConstArrayView<uint8> Bytes;
	int32 Offset = 0;
};

bool SkipWasmLimits(FAvidScriptWasmLayoutReader& Reader)
{
	uint32 Flags = 0;
	if (!Reader.ReadU32Leb(Flags) || (Flags & ~0x07u) != 0)
	{
		return false;
	}

	const bool bMemory64 = (Flags & 0x04u) != 0;
	if (bMemory64)
	{
		uint64 Minimum = 0;
		if (!Reader.ReadU64Leb(Minimum))
		{
			return false;
		}
		if ((Flags & 0x01u) != 0)
		{
			uint64 Maximum = 0;
			return Reader.ReadU64Leb(Maximum);
		}
		return true;
	}

	uint32 Minimum = 0;
	if (!Reader.ReadU32Leb(Minimum))
	{
		return false;
	}
	if ((Flags & 0x01u) != 0)
	{
		uint32 Maximum = 0;
		return Reader.ReadU32Leb(Maximum);
	}
	return true;
}

bool SkipWasmImportDescriptor(FAvidScriptWasmLayoutReader& Reader, uint8 Kind)
{
	switch (Kind)
	{
	case 0:
	{
		uint32 TypeIndex = 0;
		return Reader.ReadU32Leb(TypeIndex);
	}
	case 1:
	{
		uint8 ReferenceType = 0;
		return Reader.ReadByte(ReferenceType) && SkipWasmLimits(Reader);
	}
	case 2:
		return SkipWasmLimits(Reader);
	case 3:
	{
		uint8 ValueType = 0;
		uint8 Mutability = 0;
		return Reader.ReadByte(ValueType) && Reader.ReadByte(Mutability) && Mutability <= 1;
	}
	case 4:
	{
		uint8 Attribute = 0;
		uint32 TypeIndex = 0;
		return Reader.ReadByte(Attribute) && Attribute == 0 && Reader.ReadU32Leb(TypeIndex);
	}
	default:
		return false;
	}
}

bool ParseWasmImportSection(
	FAvidScriptWasmLayoutReader& Reader,
	uint32& OutImportedFunctionCount)
{
	uint32 ImportCount = 0;
	if (!Reader.ReadU32Leb(ImportCount) || ImportCount > MaxWasmLayoutItems)
	{
		return false;
	}

	for (uint32 ImportIndex = 0; ImportIndex < ImportCount; ++ImportIndex)
	{
		FString ModuleName;
		FString ImportName;
		uint8 Kind = 0;
		if (!Reader.ReadName(ModuleName)
			|| !Reader.ReadName(ImportName)
			|| !Reader.ReadByte(Kind)
			|| !SkipWasmImportDescriptor(Reader, Kind))
		{
			return false;
		}
		if (Kind == 0)
		{
			if (OutImportedFunctionCount == MAX_uint32)
			{
				return false;
			}
			++OutImportedFunctionCount;
		}
	}
	return Reader.IsAtEnd();
}

bool ParseWasmFunctionSection(
	FAvidScriptWasmLayoutReader& Reader,
	uint32& OutDefinedFunctionCount)
{
	if (!Reader.ReadU32Leb(OutDefinedFunctionCount)
		|| OutDefinedFunctionCount > MaxWasmLayoutItems)
	{
		return false;
	}
	for (uint32 FunctionIndex = 0; FunctionIndex < OutDefinedFunctionCount; ++FunctionIndex)
	{
		uint32 TypeIndex = 0;
		if (!Reader.ReadU32Leb(TypeIndex))
		{
			return false;
		}
	}
	return Reader.IsAtEnd();
}

bool ParseWasmExportSection(
	FAvidScriptWasmLayoutReader& Reader,
	TArray<FAvidScriptWasmFunctionExport>& OutFunctionExports)
{
	uint32 ExportCount = 0;
	if (!Reader.ReadU32Leb(ExportCount) || ExportCount > MaxWasmLayoutItems)
	{
		return false;
	}

	TSet<FString> ExportNames;
	for (uint32 ExportOrdinal = 0; ExportOrdinal < ExportCount; ++ExportOrdinal)
	{
		FString ExportName;
		uint8 Kind = 0;
		uint32 Index = 0;
		if (!Reader.ReadName(ExportName)
			|| !Reader.ReadByte(Kind)
			|| !Reader.ReadU32Leb(Index)
			|| ExportNames.Contains(ExportName))
		{
			return false;
		}
		ExportNames.Add(ExportName);
		if (Kind == 0)
		{
			FAvidScriptWasmFunctionExport& FunctionExport = OutFunctionExports.AddDefaulted_GetRef();
			FunctionExport.Name = MoveTemp(ExportName);
			FunctionExport.FunctionIndex = Index;
		}
	}
	return Reader.IsAtEnd();
}
}

bool InspectAvidScriptWasmModuleLayout(
	TConstArrayView<uint8> Bytecode,
	FAvidScriptWasmModuleLayout& OutLayout,
	FString& OutError)
{
	OutLayout = FAvidScriptWasmModuleLayout();
	OutError.Reset();
	if (Bytecode.Num() < 8
		|| Bytecode[0] != 0x00
		|| Bytecode[1] != 0x61
		|| Bytecode[2] != 0x73
		|| Bytecode[3] != 0x6d
		|| Bytecode[4] != 0x01
		|| Bytecode[5] != 0x00
		|| Bytecode[6] != 0x00
		|| Bytecode[7] != 0x00)
	{
		OutError = TEXT("invalid WASM magic or version");
		return false;
	}

	FAvidScriptWasmLayoutReader Reader(
		MakeArrayView(Bytecode.GetData() + 8, Bytecode.Num() - 8));
	bool bHasImportSection = false;
	bool bHasFunctionSection = false;
	bool bHasExportSection = false;
	while (!Reader.IsAtEnd())
	{
		uint8 SectionId = 0;
		uint32 SectionSize = 0;
		if (!Reader.ReadByte(SectionId) || !Reader.ReadU32Leb(SectionSize))
		{
			OutError = TEXT("truncated WASM section header");
			return false;
		}
		if (SectionId > 13)
		{
			OutError = TEXT("WASM section id is unsupported");
			return false;
		}

		FAvidScriptWasmLayoutReader SectionReader{TConstArrayView<uint8>()};
		if (!Reader.ReadSubReader(SectionSize, SectionReader))
		{
			OutError = TEXT("WASM section exceeds module bounds");
			return false;
		}

		bool bParsed = true;
		switch (SectionId)
		{
		case 2:
			if (bHasImportSection)
			{
				OutError = TEXT("WASM import section is duplicated");
				return false;
			}
			bHasImportSection = true;
			bParsed = ParseWasmImportSection(SectionReader, OutLayout.ImportedFunctionCount);
			break;
		case 3:
			if (bHasFunctionSection)
			{
				OutError = TEXT("WASM function section is duplicated");
				return false;
			}
			bHasFunctionSection = true;
			bParsed = ParseWasmFunctionSection(SectionReader, OutLayout.DefinedFunctionCount);
			break;
		case 7:
			if (bHasExportSection)
			{
				OutError = TEXT("WASM export section is duplicated");
				return false;
			}
			bHasExportSection = true;
			bParsed = ParseWasmExportSection(SectionReader, OutLayout.FunctionExports);
			break;
		default:
			break;
		}
		if (!bParsed)
		{
			OutError = FString::Printf(TEXT("invalid WASM section %u"), SectionId);
			return false;
		}
	}

	if (!bHasImportSection)
	{
		OutLayout.ImportedFunctionCount = 0;
	}
	if (!bHasFunctionSection)
	{
		OutLayout.DefinedFunctionCount = 0;
	}
	if (!bHasExportSection)
	{
		OutLayout.FunctionExports.Reset();
	}

	const uint64 FunctionIndexLimit =
		static_cast<uint64>(OutLayout.ImportedFunctionCount)
		+ static_cast<uint64>(OutLayout.DefinedFunctionCount);
	if (FunctionIndexLimit > MAX_uint32)
	{
		OutError = TEXT("WASM function index space overflows uint32");
		return false;
	}
	for (const FAvidScriptWasmFunctionExport& FunctionExport : OutLayout.FunctionExports)
	{
		if (static_cast<uint64>(FunctionExport.FunctionIndex) >= FunctionIndexLimit)
		{
			OutError = FString::Printf(
				TEXT("function export %s has out-of-range index %u"),
				*FunctionExport.Name,
				FunctionExport.FunctionIndex);
			return false;
		}
	}
	return true;
}
