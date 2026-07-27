#include "DataBridge/AvidScriptCommandBuffer.h"

namespace
{
uint16 ReadU16(const uint8* Bytes)
{
	return static_cast<uint16>(Bytes[0])
		| (static_cast<uint16>(Bytes[1]) << 8);
}

uint32 ReadU32(const uint8* Bytes)
{
	return static_cast<uint32>(Bytes[0])
		| (static_cast<uint32>(Bytes[1]) << 8)
		| (static_cast<uint32>(Bytes[2]) << 16)
		| (static_cast<uint32>(Bytes[3]) << 24);
}

uint64 ReadU64(const uint8* Bytes)
{
	return static_cast<uint64>(ReadU32(Bytes))
		| (static_cast<uint64>(ReadU32(Bytes + 4)) << 32);
}

int32 ReadI32(const uint8* Bytes)
{
	const uint32 Bits = ReadU32(Bytes);
	int32 Value = 0;
	FMemory::Memcpy(&Value, &Bits, sizeof(Value));
	return Value;
}

bool Reject(
	const TCHAR* Category,
	const FString& Source,
	const FString& Details,
	FAvidScriptCommandBufferParseResult& OutResult)
{
	OutResult.ErrorCategory = Category;
	OutResult.ErrorSource = Source;
	OutResult.ErrorDetails = Details;
	return false;
}
}

bool FAvidScriptCommandBufferParser::Parse(
	const TConstArrayView<uint8> Bytes,
	const uint64 ExpectedCallbackEpoch,
	const FAvidScriptDataBridgeBudget& Budget,
	FAvidScriptParsedCommandBuffer& OutBuffer,
	FAvidScriptCommandBufferParseResult& OutResult)
{
	OutBuffer = FAvidScriptParsedCommandBuffer();
	OutResult = FAvidScriptCommandBufferParseResult();

	if (Bytes.Num() < static_cast<int32>(AvidScriptDataBridgeAbi::HeaderBytes))
	{
		return Reject(
			TEXT("data_lane_truncated_header"),
			TEXT("header"),
			TEXT("The command buffer is smaller than the 24-byte header."),
			OutResult);
	}
	if (static_cast<uint64>(Bytes.Num()) > Budget.MaxBytes)
	{
		return Reject(
			TEXT("data_lane_byte_budget_exceeded"),
			TEXT("header.byte_count"),
			FString::Printf(
				TEXT("The command buffer contains %d bytes; the budget is %u."),
				Bytes.Num(),
				Budget.MaxBytes),
			OutResult);
	}

	const uint8* Header = Bytes.GetData();
	const uint32 Magic = ReadU32(Header);
	const uint16 SchemaVersion = ReadU16(Header + 4);
	const uint16 CommandCount = ReadU16(Header + 6);
	const uint32 DeclaredByteCount = ReadU32(Header + 8);
	const uint32 Reserved = ReadU32(Header + 12);
	const uint64 CallbackEpoch = ReadU64(Header + 16);
	if (Magic != AvidScriptDataBridgeAbi::CommandBufferMagic)
	{
		return Reject(
			TEXT("data_lane_magic_mismatch"),
			TEXT("header.magic"),
			TEXT("The command buffer magic does not match AVCB."),
			OutResult);
	}
	if (SchemaVersion != AvidScriptDataBridgeAbi::CommandBufferSchemaVersion)
	{
		return Reject(
			TEXT("data_lane_schema_unsupported"),
			TEXT("header.schema_version"),
			FString::Printf(TEXT("Unsupported command buffer schema %u."), SchemaVersion),
			OutResult);
	}
	if (Reserved != 0)
	{
		return Reject(
			TEXT("data_lane_reserved_nonzero"),
			TEXT("header.reserved"),
			TEXT("Reserved command buffer header bits must be zero."),
			OutResult);
	}
	if (CommandCount == 0 || CommandCount > Budget.MaxCommands)
	{
		return Reject(
			TEXT("data_lane_command_budget_invalid"),
			TEXT("header.command_count"),
			FString::Printf(
				TEXT("The command count %u is outside the allowed range 1..%u."),
				CommandCount,
				Budget.MaxCommands),
			OutResult);
	}

	const uint64 ExpectedByteCount = static_cast<uint64>(AvidScriptDataBridgeAbi::HeaderBytes)
		+ static_cast<uint64>(CommandCount) * AvidScriptDataBridgeAbi::CommandRecordBytes;
	if (ExpectedByteCount > MAX_uint32
		|| DeclaredByteCount != ExpectedByteCount
		|| static_cast<uint64>(Bytes.Num()) != ExpectedByteCount)
	{
		return Reject(
			TEXT("data_lane_byte_count_mismatch"),
			TEXT("header.byte_count"),
			FString::Printf(
				TEXT("Declared=%u, supplied=%d, expected=%llu."),
				DeclaredByteCount,
				Bytes.Num(),
				ExpectedByteCount),
			OutResult);
	}
	if (ExpectedCallbackEpoch == 0 || CallbackEpoch != ExpectedCallbackEpoch)
	{
		return Reject(
			TEXT("data_lane_stale_epoch"),
			TEXT("header.callback_epoch"),
			FString::Printf(
				TEXT("The buffer epoch %llu does not match the active callback epoch %llu."),
				CallbackEpoch,
				ExpectedCallbackEpoch),
			OutResult);
	}

	OutBuffer.CallbackEpoch = CallbackEpoch;
	OutBuffer.ByteCount = DeclaredByteCount;
	OutBuffer.Commands.Reserve(CommandCount);
	for (uint32 CommandIndex = 0; CommandIndex < CommandCount; ++CommandIndex)
	{
		const uint8* Record = Header
			+ AvidScriptDataBridgeAbi::HeaderBytes
			+ CommandIndex * AvidScriptDataBridgeAbi::CommandRecordBytes;
		const uint16 OpcodeValue = ReadU16(Record);
		const uint16 Flags = ReadU16(Record + 2);
		const uint32 RecordBytes = ReadU32(Record + 4);
		const uint32 BindingOrdinal = ReadU32(Record + 8);
		const uint32 CommandReserved = ReadU32(Record + 28);
		const int32 Arg1 = ReadI32(Record + 24);
		if (Flags != 0 || CommandReserved != 0)
		{
			return Reject(
				TEXT("data_lane_reserved_nonzero"),
				FString::Printf(TEXT("command[%u]"), CommandIndex),
				TEXT("Command flags and reserved bits must be zero."),
				OutResult);
		}
		if (RecordBytes != AvidScriptDataBridgeAbi::CommandRecordBytes)
		{
			return Reject(
				TEXT("data_lane_record_size_mismatch"),
				FString::Printf(TEXT("command[%u]"), CommandIndex),
				FString::Printf(TEXT("Unsupported command record size %u."), RecordBytes),
				OutResult);
		}
		if (OpcodeValue != static_cast<uint16>(EAvidScriptCommandOpcode::SetI32))
		{
			return Reject(
				TEXT("data_lane_opcode_unsupported"),
				FString::Printf(TEXT("command[%u]"), CommandIndex),
				FString::Printf(TEXT("Opcode %u is not enabled by schema 1."), OpcodeValue),
				OutResult);
		}
		if (BindingOrdinal == MAX_uint32)
		{
			return Reject(
				TEXT("data_lane_binding_invalid"),
				FString::Printf(TEXT("command[%u]"), CommandIndex),
				TEXT("The command uses the invalid binding ordinal."),
				OutResult);
		}
		if (Arg1 != 0)
		{
			return Reject(
				TEXT("data_lane_reserved_nonzero"),
				FString::Printf(TEXT("command[%u]"), CommandIndex),
				TEXT("SetI32 arg1 must be zero in schema 1."),
				OutResult);
		}

		FAvidScriptParsedCommand& Command = OutBuffer.Commands.AddDefaulted_GetRef();
		Command.Opcode = EAvidScriptCommandOpcode::SetI32;
		Command.BindingOrdinal = BindingOrdinal;
		Command.SelfSlot = ReadI32(Record + 12);
		Command.SelfGeneration = ReadI32(Record + 16);
		Command.Arg0 = ReadI32(Record + 20);
		Command.Arg1 = Arg1;
	}

	return true;
}
