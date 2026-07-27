#include "DataBridge/AvidScriptCommandBuffer.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingInvocation.h"
#include "AvidScriptGeneratedBindingRegistry.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmRuntime.h"
#include "Tests/AvidScriptObjectRegistryTestTypes.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
void WriteU16(TArray<uint8>& Bytes, const int32 Offset, const uint16 Value)
{
	Bytes[Offset] = static_cast<uint8>(Value);
	Bytes[Offset + 1] = static_cast<uint8>(Value >> 8);
}

void WriteU32(TArray<uint8>& Bytes, const int32 Offset, const uint32 Value)
{
	Bytes[Offset] = static_cast<uint8>(Value);
	Bytes[Offset + 1] = static_cast<uint8>(Value >> 8);
	Bytes[Offset + 2] = static_cast<uint8>(Value >> 16);
	Bytes[Offset + 3] = static_cast<uint8>(Value >> 24);
}

void WriteU64(TArray<uint8>& Bytes, const int32 Offset, const uint64 Value)
{
	WriteU32(Bytes, Offset, static_cast<uint32>(Value));
	WriteU32(Bytes, Offset + 4, static_cast<uint32>(Value >> 32));
}

TArray<uint8> MakeSetI32Buffer(
	const uint64 Epoch,
	const uint32 BindingOrdinal,
	const uint32 SelfSlot,
	const uint32 SelfGeneration,
	const TConstArrayView<int32> Values)
{
	TArray<uint8> Bytes;
	Bytes.SetNumZeroed(
		AvidScriptDataBridgeAbi::HeaderBytes
		+ Values.Num() * AvidScriptDataBridgeAbi::CommandRecordBytes);
	WriteU32(Bytes, 0, AvidScriptDataBridgeAbi::CommandBufferMagic);
	WriteU16(Bytes, 4, AvidScriptDataBridgeAbi::CommandBufferSchemaVersion);
	WriteU16(Bytes, 6, static_cast<uint16>(Values.Num()));
	WriteU32(Bytes, 8, Bytes.Num());
	WriteU64(Bytes, 16, Epoch);

	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		const int32 CommandOffset = AvidScriptDataBridgeAbi::HeaderBytes
			+ Index * AvidScriptDataBridgeAbi::CommandRecordBytes;
		WriteU16(Bytes, CommandOffset, static_cast<uint16>(EAvidScriptCommandOpcode::SetI32));
		WriteU32(Bytes, CommandOffset + 4, AvidScriptDataBridgeAbi::CommandRecordBytes);
		WriteU32(Bytes, CommandOffset + 8, BindingOrdinal);
		WriteU32(Bytes, CommandOffset + 12, SelfSlot);
		WriteU32(Bytes, CommandOffset + 16, SelfGeneration);
		WriteU32(Bytes, CommandOffset + 20, static_cast<uint32>(Values[Index]));
	}
	return Bytes;
}

int32 GRejectedDataLaneValue = MAX_int32;

EAvidScriptVmTypedHostStatus DataLanePropertyCall(
	UObject& Receiver,
	const bool bWrite,
	int32& InOutValue)
{
	AAvidScriptActorBindingTestActor* Actor =
		Cast<AAvidScriptActorBindingTestActor>(&Receiver);
	if (Actor == nullptr)
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	if (!bWrite)
	{
		InOutValue = Actor->DataLaneI32;
		return EAvidScriptVmTypedHostStatus::Succeeded;
	}
	if (InOutValue == GRejectedDataLaneValue)
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	Actor->DataLaneI32 = InOutValue;
	return EAvidScriptVmTypedHostStatus::Succeeded;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptDataBridgeParserContractTest,
	"AvidScript.Architecture.DataBridge.ParserContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptDataBridgeParserContractTest::RunTest(const FString& Parameters)
{
	constexpr uint64 Epoch = 0x1020304050607080;
	const FAvidScriptDataBridgeBudget Budget;
	const int32 Value = -91;
	const TArray<uint8> ValidBytes = MakeSetI32Buffer(
		Epoch,
		37,
		11,
		5,
		MakeArrayView(&Value, 1));
	FAvidScriptParsedCommandBuffer Buffer;
	FAvidScriptCommandBufferParseResult Result;
	TestTrue(
		TEXT("A canonical SetI32 buffer parses"),
		FAvidScriptCommandBufferParser::Parse(ValidBytes, Epoch, Budget, Buffer, Result));
	TestEqual(TEXT("One command is retained"), Buffer.Commands.Num(), 1);
	if (Buffer.Commands.Num() == 1)
	{
		TestEqual(TEXT("Binding ordinal is little endian"), Buffer.Commands[0].BindingOrdinal, 37u);
		TestEqual(TEXT("Self slot is retained"), Buffer.Commands[0].SelfSlot, 11);
		TestEqual(TEXT("Self generation is retained"), Buffer.Commands[0].SelfGeneration, 5);
		TestEqual(TEXT("Signed scalar is retained"), Buffer.Commands[0].Arg0, -91);
	}

	TestFalse(
		TEXT("A prior callback cannot replay a valid buffer"),
		FAvidScriptCommandBufferParser::Parse(ValidBytes, Epoch + 1, Budget, Buffer, Result));
	TestEqual(
		TEXT("Replay rejection is stable"),
		Result.ErrorCategory,
		FString(TEXT("data_lane_stale_epoch")));

	TArray<uint8> UnknownOpcode = ValidBytes;
	WriteU16(UnknownOpcode, AvidScriptDataBridgeAbi::HeaderBytes, 999);
	TestFalse(
		TEXT("Unknown opcodes fail before apply"),
		FAvidScriptCommandBufferParser::Parse(UnknownOpcode, Epoch, Budget, Buffer, Result));
	TestEqual(
		TEXT("Unknown opcode rejection is stable"),
		Result.ErrorCategory,
		FString(TEXT("data_lane_opcode_unsupported")));

	FAvidScriptDataBridgeBudget TightBudget;
	TightBudget.MaxBytes = ValidBytes.Num() - 1;
	TestFalse(
		TEXT("The byte budget rejects an otherwise valid buffer"),
		FAvidScriptCommandBufferParser::Parse(ValidBytes, Epoch, TightBudget, Buffer, Result));
	TestEqual(
		TEXT("Budget rejection is stable"),
		Result.ErrorCategory,
		FString(TEXT("data_lane_byte_budget_exceeded")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptDataBridgeGeneratedPropertyApplyTest,
	"AvidScript.Architecture.DataBridge.GeneratedPropertyApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptDataBridgeGeneratedPropertyApplyTest::RunTest(
	const FString& Parameters)
{
	const FString PackageHash = FString::ChrN(64, TEXT('d'));
	const FString StableId = FString::ChrN(64, TEXT('e'));
	FAvidScriptGeneratedBindingRegistry& GeneratedRegistry =
		FAvidScriptGeneratedBindingRegistry::Get();
	GeneratedRegistry.UnregisterPackage(PackageHash);
	ON_SCOPE_EXIT
	{
		GRejectedDataLaneValue = MAX_int32;
		GeneratedRegistry.UnregisterPackage(PackageHash);
	};

	FAvidScriptGeneratedBindingEntry Entry;
	Entry.PackageHash = PackageHash;
	Entry.StableId = StableId;
	Entry.DescriptorIdentity = TEXT("test::data-lane-i32");
	Entry.Shape = EAvidScriptGeneratedBindingShape::PropertyI32GetSet;
	Entry.ReceiverMode = EAvidScriptGeneratedReceiverMode::SelfBound;
	Entry.PropertyI32Call = &DataLanePropertyCall;
	FString RegistryError;
	if (!TestTrue(
		TEXT("Data lane generated entry registers"),
		GeneratedRegistry.RegisterPackage(
			PackageHash,
			MakeArrayView(&Entry, 1),
			RegistryError)))
	{
		return false;
	}

	FProperty* Property = FindFProperty<FProperty>(
		AAvidScriptActorBindingTestActor::StaticClass(),
		GET_MEMBER_NAME_CHECKED(AAvidScriptActorBindingTestActor, DataLaneI32));
	if (!TestNotNull(TEXT("Data lane reflected int property resolves"), Property))
	{
		return false;
	}
	const TSharedPtr<const FAvidScriptBindingPackage> Package =
		FAvidScriptBindingPackage::MakeGeneratedPlanForTesting(
			PackageHash,
			StableId,
			Entry.DescriptorIdentity,
			Entry.Shape,
			AAvidScriptActorBindingTestActor::StaticClass(),
			Property,
			true,
			true,
			EAvidScriptBindingReloadEffect::ReflectedProperty);
	if (!TestTrue(TEXT("Data lane generated package is created"), Package.IsValid()))
	{
		return false;
	}

	TStrongObjectPtr<AAvidScriptActorBindingTestActor> Owner(
		NewObject<AAvidScriptActorBindingTestActor>());
	FAvidScriptObjectRegistry ObjectRegistry;
	FAvidScriptObjectHandleResult HandleResult;
	const FAvidScriptObjectHandle OwnerHandle = ObjectRegistry.RegisterObject(
		Owner.Get(),
		HandleResult,
		false);
	if (!TestTrue(TEXT("Data lane owner handle is valid"), OwnerHandle.IsValid()))
	{
		return false;
	}

	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmHostContext Context;
	Context.ObjectRegistry = &ObjectRegistry;
	Context.OwnerHandle = OwnerHandle;
	Context.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	Runtime.SetHostContext(Context);
	Runtime.SetBindingPackageForTesting(Package);
	Runtime.BeginTypedCallbackEpochForTesting();
	const uint64 Epoch = Runtime.GetActiveCallbackEpochForTesting();
	TestEqual(
		TEXT("Guest observes the active callback epoch"),
		Runtime.HandleDataLaneGetEpochImport(),
		static_cast<int64>(Epoch));
	const int32 Values[] = { 11, 22, 33 };
	const TArray<uint8> Buffer = MakeSetI32Buffer(
		Epoch,
		0,
		OwnerHandle.Slot,
		OwnerHandle.Generation,
		MakeArrayView(Values));
	TestEqual(
		TEXT("A three-command buffer applies as one submission"),
		Runtime.HandleDataLaneSubmitImport(Buffer),
		3);
	TestEqual(TEXT("The final source-order write wins"), Owner->DataLaneI32, 33);
	TestEqual(
		TEXT("Applied command metric is exact"),
		Runtime.GetDataBridgeMetrics().AppliedCommands,
		uint64(3));
	TestEqual(
		TEXT("One epoch read and one submit are counted as crossings"),
		Runtime.GetDataBridgeMetrics().BoundaryCrossings,
		uint64(2));

	Owner->DataLaneI32 = 7;
	GRejectedDataLaneValue = 22;
	TestEqual(
		TEXT("A generated rejection rejects the full batch"),
		Runtime.HandleDataLaneSubmitImport(Buffer),
		0);
	TestEqual(
		TEXT("A partial generated apply rolls back to the pre-batch value"),
		Owner->DataLaneI32,
		7);

	Runtime.EndTypedCallbackEpochForTesting();
	Runtime.BeginTypedCallbackEpochForTesting();
	GRejectedDataLaneValue = MAX_int32;
	TestEqual(
		TEXT("A prior callback buffer cannot be replayed"),
		Runtime.HandleDataLaneSubmitImport(Buffer),
		0);
	TestEqual(TEXT("Rejected replay leaves the object unchanged"), Owner->DataLaneI32, 7);
	Runtime.EndTypedCallbackEpochForTesting();
	Runtime.ClearHostContext();
	return true;
}

#endif
