#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptBindingLatent.h"
#include "AvidScriptComponent.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorCSharpBindingEmitterTestTypes.h"
#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "AvidScriptEditorCSharpWorkspaceService.h"
#include "AvidScriptEditorResultPresentation.h"
#include "AvidScriptHash.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/LatentActionManager.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"

namespace
{

class FAvidScriptEditorLatentTestHost final
	: public IAvidScriptBindingLatentHost
{
public:
	explicit FAvidScriptEditorLatentTestHost(UWorld& InWorld)
		: World(&InWorld)
		, CallbackTarget(
			NewObject<UAvidScriptEditorLatentCallbackTestObject>(
				GetTransientPackage()))
	{
	}

	bool BeginLatent(
		const int32 CallbackId,
		FAvidScriptBindingLatentReservation& OutReservation) override
	{
		OutReservation = {};
		if (CallbackId < 0 || !CallbackTarget.IsValid() || bReserved)
		{
			return false;
		}
		bReserved = true;
		OutReservation.Token = 9001;
		OutReservation.CallbackTarget = CallbackTarget.Get();
		OutReservation.ExecutionFunction = GET_FUNCTION_NAME_CHECKED(
			UAvidScriptEditorLatentCallbackTestObject,
			OnLatentCompleted);
		OutReservation.UUID = 1;
		OutReservation.Linkage = 0;
		return true;
	}

	bool CommitLatent(const int64 Token) override
	{
		bObservedRegisteredAction = Token == 9001
			&& World != nullptr
			&& World->GetLatentActionManager().GetNumActionsForObject(
				CallbackTarget.Get()) == 1;
		bCommitted = bAllowCommit && bObservedRegisteredAction;
		return bCommitted;
	}

	bool AbortLatent(const int64 Token) override
	{
		if (Token != 9001 || World == nullptr || !CallbackTarget.IsValid())
		{
			return false;
		}
		++AbortCount;
		World->GetLatentActionManager().RemoveActionsForObject(
			CallbackTarget.Get());
		return true;
	}

	UAvidScriptEditorLatentCallbackTestObject* GetCallbackTarget() const
	{
		return CallbackTarget.Get();
	}

	bool bReserved = false;
	bool bAllowCommit = true;
	bool bObservedRegisteredAction = false;
	bool bCommitted = false;
	int32 AbortCount = 0;

private:
	UWorld* World = nullptr;
	TStrongObjectPtr<UAvidScriptEditorLatentCallbackTestObject> CallbackTarget;
};

uint64 MakeAvidScriptBindingRuntimeF32Cell(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

void AppendAvidScriptPropertyBenchmarkU32Leb(TArray<uint8>& Bytes, uint32 Value)
{
	do
	{
		uint8 Byte = Value & 0x7f;
		Value >>= 7;
		if (Value != 0)
		{
			Byte |= 0x80;
		}
		Bytes.Add(Byte);
	}
	while (Value != 0);
}

void AppendAvidScriptPropertyBenchmarkI32Leb(TArray<uint8>& Bytes, int32 Value)
{
	bool bHasMore = true;
	while (bHasMore)
	{
		uint8 Byte = static_cast<uint8>(Value) & 0x7f;
		Value >>= 7;
		const bool bSignBitSet = (Byte & 0x40) != 0;
		bHasMore = !((Value == 0 && !bSignBitSet)
			|| (Value == -1 && bSignBitSet));
		if (bHasMore)
		{
			Byte |= 0x80;
		}
		Bytes.Add(Byte);
	}
}

void AppendAvidScriptPropertyBenchmarkString(TArray<uint8>& Bytes, const FString& Value)
{
	FTCHARToUTF8 Utf8(*Value);
	AppendAvidScriptPropertyBenchmarkU32Leb(Bytes, Utf8.Length());
	Bytes.Append(
		reinterpret_cast<const uint8*>(Utf8.Get()),
		Utf8.Length());
}

void AppendAvidScriptPropertyBenchmarkSection(
	TArray<uint8>& Module,
	const uint8 SectionId,
	const TConstArrayView<uint8> Payload)
{
	Module.Add(SectionId);
	AppendAvidScriptPropertyBenchmarkU32Leb(Module, Payload.Num());
	Module.Append(Payload.GetData(), Payload.Num());
}

TArray<uint8> BuildAvidScriptPropertyBenchmarkModule(
	const FAvidScriptBindingHostImportModel& HostImport,
	const FAvidScriptObjectHandle& OwnerHandle,
	const bool bWriteOnBeginPlay = false,
	const bool bTrapAfterBeginPlayWrite = false,
	const float BeginPlayValue = 1.0f)
{
	TArray<uint8> Module = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };

	TArray<uint8> Types;
	AppendAvidScriptPropertyBenchmarkU32Leb(Types, 3);
	Types.Add(0x60);
	AppendAvidScriptPropertyBenchmarkU32Leb(Types, 3);
	Types.Append({ 0x7f, 0x7f, 0x7d });
	AppendAvidScriptPropertyBenchmarkU32Leb(Types, 1);
	Types.Add(0x7f);
	Types.Add(0x60);
	AppendAvidScriptPropertyBenchmarkU32Leb(Types, 0);
	AppendAvidScriptPropertyBenchmarkU32Leb(Types, 0);
	Types.Add(0x60);
	AppendAvidScriptPropertyBenchmarkU32Leb(Types, 1);
	Types.Add(0x7d);
	AppendAvidScriptPropertyBenchmarkU32Leb(Types, 0);
	AppendAvidScriptPropertyBenchmarkSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendAvidScriptPropertyBenchmarkU32Leb(Imports, 1);
	AppendAvidScriptPropertyBenchmarkString(Imports, HostImport.Module);
	AppendAvidScriptPropertyBenchmarkString(Imports, HostImport.Name);
	Imports.Add(0x00);
	AppendAvidScriptPropertyBenchmarkU32Leb(Imports, 0);
	AppendAvidScriptPropertyBenchmarkSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendAvidScriptPropertyBenchmarkU32Leb(Functions, 2);
	AppendAvidScriptPropertyBenchmarkU32Leb(Functions, 1);
	AppendAvidScriptPropertyBenchmarkU32Leb(Functions, 2);
	AppendAvidScriptPropertyBenchmarkSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendAvidScriptPropertyBenchmarkU32Leb(Exports, 2);
	AppendAvidScriptPropertyBenchmarkString(Exports, TEXT("avid_on_begin_play"));
	Exports.Add(0x00);
	AppendAvidScriptPropertyBenchmarkU32Leb(Exports, 1);
	AppendAvidScriptPropertyBenchmarkString(Exports, TEXT("avid_on_tick"));
	Exports.Add(0x00);
	AppendAvidScriptPropertyBenchmarkU32Leb(Exports, 2);
	AppendAvidScriptPropertyBenchmarkSection(Module, 7, Exports);

	TArray<uint8> BeginPlayBody;
	AppendAvidScriptPropertyBenchmarkU32Leb(BeginPlayBody, 0);
	if (bWriteOnBeginPlay)
	{
		BeginPlayBody.Add(0x41);
		AppendAvidScriptPropertyBenchmarkI32Leb(
			BeginPlayBody,
			static_cast<int32>(OwnerHandle.Slot));
		BeginPlayBody.Add(0x41);
		AppendAvidScriptPropertyBenchmarkI32Leb(
			BeginPlayBody,
			static_cast<int32>(OwnerHandle.Generation));
		BeginPlayBody.Add(0x43);
		uint32 BeginPlayValueBits = 0;
		FMemory::Memcpy(&BeginPlayValueBits, &BeginPlayValue, sizeof(BeginPlayValueBits));
		BeginPlayBody.Append(
			reinterpret_cast<const uint8*>(&BeginPlayValueBits),
			sizeof(BeginPlayValueBits));
		BeginPlayBody.Append({ 0x10, 0x00, 0x1a });
	}
	if (bTrapAfterBeginPlayWrite)
	{
		BeginPlayBody.Add(0x00);
	}
	BeginPlayBody.Add(0x0b);
	TArray<uint8> TickBody;
	AppendAvidScriptPropertyBenchmarkU32Leb(TickBody, 0);
	TickBody.Add(0x41);
	AppendAvidScriptPropertyBenchmarkI32Leb(
		TickBody,
		static_cast<int32>(OwnerHandle.Slot));
	TickBody.Add(0x41);
	AppendAvidScriptPropertyBenchmarkI32Leb(
		TickBody,
		static_cast<int32>(OwnerHandle.Generation));
	TickBody.Append({ 0x20, 0x00, 0x10, 0x00, 0x1a, 0x0b });
	TArray<uint8> Code;
	AppendAvidScriptPropertyBenchmarkU32Leb(Code, 2);
	AppendAvidScriptPropertyBenchmarkU32Leb(Code, BeginPlayBody.Num());
	Code.Append(BeginPlayBody);
	AppendAvidScriptPropertyBenchmarkU32Leb(Code, TickBody.Num());
	Code.Append(TickBody);
	AppendAvidScriptPropertyBenchmarkSection(Module, 10, Code);
	return Module;
}

TArray<uint8> BuildAvidScriptPreparedI32PropertyModule(
	const FAvidScriptBindingHostImportModel& GetterImport,
	const FAvidScriptBindingHostImportModel& SetterImport,
	const FAvidScriptObjectHandle& OwnerHandle)
{
	TArray<uint8> Module = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00
	};
	TArray<uint8> Types;
	AppendAvidScriptPropertyBenchmarkU32Leb(Types, 3);
	Types.Append({
		0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f,
		0x60, 0x00, 0x00,
		0x60, 0x01, 0x7d, 0x00
	});
	AppendAvidScriptPropertyBenchmarkSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendAvidScriptPropertyBenchmarkU32Leb(Imports, 2);
	AppendAvidScriptPropertyBenchmarkString(Imports, GetterImport.Module);
	AppendAvidScriptPropertyBenchmarkString(Imports, GetterImport.Name);
	Imports.Add(0x00);
	AppendAvidScriptPropertyBenchmarkU32Leb(Imports, 0);
	AppendAvidScriptPropertyBenchmarkString(Imports, SetterImport.Module);
	AppendAvidScriptPropertyBenchmarkString(Imports, SetterImport.Name);
	Imports.Add(0x00);
	AppendAvidScriptPropertyBenchmarkU32Leb(Imports, 0);
	AppendAvidScriptPropertyBenchmarkSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendAvidScriptPropertyBenchmarkU32Leb(Functions, 2);
	AppendAvidScriptPropertyBenchmarkU32Leb(Functions, 1);
	AppendAvidScriptPropertyBenchmarkU32Leb(Functions, 2);
	AppendAvidScriptPropertyBenchmarkSection(Module, 3, Functions);

	TArray<uint8> Memory = { 0x01, 0x00, 0x01 };
	AppendAvidScriptPropertyBenchmarkSection(Module, 5, Memory);

	TArray<uint8> Exports;
	AppendAvidScriptPropertyBenchmarkU32Leb(Exports, 3);
	AppendAvidScriptPropertyBenchmarkString(Exports, TEXT("memory"));
	Exports.Add(0x02);
	AppendAvidScriptPropertyBenchmarkU32Leb(Exports, 0);
	AppendAvidScriptPropertyBenchmarkString(
		Exports,
		TEXT("avid_on_begin_play"));
	Exports.Add(0x00);
	AppendAvidScriptPropertyBenchmarkU32Leb(Exports, 2);
	AppendAvidScriptPropertyBenchmarkString(Exports, TEXT("avid_on_tick"));
	Exports.Add(0x00);
	AppendAvidScriptPropertyBenchmarkU32Leb(Exports, 3);
	AppendAvidScriptPropertyBenchmarkSection(Module, 7, Exports);

	const auto AppendOwner = [&OwnerHandle](TArray<uint8>& Body)
	{
		Body.Add(0x41);
		AppendAvidScriptPropertyBenchmarkI32Leb(
			Body,
			static_cast<int32>(OwnerHandle.Slot));
		Body.Add(0x41);
		AppendAvidScriptPropertyBenchmarkI32Leb(
			Body,
			static_cast<int32>(OwnerHandle.Generation));
	};
	TArray<uint8> BeginPlayBody;
	AppendAvidScriptPropertyBenchmarkU32Leb(BeginPlayBody, 0);
	AppendOwner(BeginPlayBody);
	BeginPlayBody.Add(0x41);
	AppendAvidScriptPropertyBenchmarkI32Leb(BeginPlayBody, 41);
	BeginPlayBody.Append({ 0x10, 0x01, 0x1a, 0x0b });

	TArray<uint8> TickBody;
	AppendAvidScriptPropertyBenchmarkU32Leb(TickBody, 0);
	AppendOwner(TickBody);
	TickBody.Add(0x41);
	AppendAvidScriptPropertyBenchmarkI32Leb(TickBody, 32);
	TickBody.Append({ 0x10, 0x00, 0x1a });
	AppendOwner(TickBody);
	TickBody.Add(0x41);
	AppendAvidScriptPropertyBenchmarkI32Leb(TickBody, 32);
	TickBody.Append({ 0x28, 0x02, 0x00 });
	TickBody.Add(0x41);
	AppendAvidScriptPropertyBenchmarkI32Leb(TickBody, 1);
	TickBody.Append({ 0x6a, 0x10, 0x01, 0x1a, 0x0b });

	TArray<uint8> Code;
	AppendAvidScriptPropertyBenchmarkU32Leb(Code, 2);
	AppendAvidScriptPropertyBenchmarkU32Leb(
		Code,
		BeginPlayBody.Num());
	Code.Append(BeginPlayBody);
	AppendAvidScriptPropertyBenchmarkU32Leb(Code, TickBody.Num());
	Code.Append(TickBody);
	AppendAvidScriptPropertyBenchmarkSection(Module, 10, Code);
	return Module;
}

FAvidScriptWasmReloadManifest MakeAvidScriptPropertySessionManifest(
	const FString& ModuleId,
	const TSharedPtr<const FAvidScriptBindingPackage>& Package,
	const FAvidScriptBindingHostImportModel& HostImport)
{
	FAvidScriptWasmReloadManifest Manifest =
		FAvidScriptWasmReloadManifest::MakeSmoke(ModuleId);
	Manifest.Language = TEXT("CSharp");
	Manifest.WasmFile = TEXT("Saved/AvidScript/") + ModuleId + TEXT(".wasm");
	Manifest.WasmSha256 = FString::ChrN(64, TEXT('a'));
	Manifest.RequiredImports = {
		FAvidScriptWasmRequiredImport{ HostImport.Module, HostImport.Name }
	};
	Manifest.BindingPackageName = Package->GetPackageName();
	Manifest.BindingPackageHash = Package->GetPackageHash();
	Manifest.BindingPackageManifestFile =
		TEXT("Saved/AvidScript/") + ModuleId + TEXT(".bindings.json");
	Manifest.BindingPackageManifestSha256 = FString::ChrN(64, TEXT('b'));
	Manifest.BindingDescriptorFile =
		TEXT("Saved/AvidScript/") + ModuleId + TEXT(".descriptor.json");
	Manifest.BindingDescriptorSha256 = FString::ChrN(64, TEXT('c'));
	Manifest.DebugMapFile = TEXT("Saved/AvidScript/") + ModuleId + TEXT(".debug.json");
	Manifest.DebugMapSha256 = FString::ChrN(64, TEXT('d'));
	Manifest.BindingPackage = Package;
	return Manifest;
}

double CalculateAvidScriptPropertyBenchmarkPercentile(
	TArray<double> Samples,
	const double Quantile)
{
	Samples.Sort();
	if (Samples.IsEmpty())
	{
		return 0.0;
	}
	const int32 Index = FMath::Clamp(
		FMath::FloorToInt(Quantile * static_cast<double>(Samples.Num() - 1)),
		0,
		Samples.Num() - 1);
	return Samples[Index];
}

FORCENOINLINE void SetAvidScriptPropertyBenchmarkNative(
	AActor& Actor,
	const float Value)
{
	Actor.CustomTimeDilation = Value;
}

bool RehashAvidScriptBindingRuntimeDescriptor(FString& InOutJson)
{
	FAvidScriptBindingPackageModel Package;
	FString ErrorCategory;
	FString ErrorSource;
	TSharedPtr<FJsonObject> Root;
	if (!FAvidScriptBindingDescriptorParser::Parse(
			InOutJson,
			Package,
			ErrorCategory,
			ErrorSource)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(InOutJson), Root)
		|| !Root.IsValid())
	{
		return false;
	}

	Root->SetStringField(
		TEXT("package_hash"),
		FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package));
	InOutJson.Empty();
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&InOutJson);
	return FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
}

bool SaveAvidScriptBindingRuntimeJsonWithHash(
	const TSharedRef<FJsonObject>& Object,
	const FString& Path,
	FString& OutSha256)
{
	OutSha256.Reset();
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Object, Writer)
		|| !FFileHelper::SaveStringToFile(
			Json,
			*Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		return false;
	}
	OutSha256 = FAvidScriptHash::Sha256Hex(Bytes);
	return !OutSha256.IsEmpty();
}

class FAvidScriptBindingRuntimeTestGuestMemory final : public IAvidScriptVmGuestMemory
{
public:
	explicit FAvidScriptBindingRuntimeTestGuestMemory(const int32 Size)
	{
		Bytes.SetNumZeroed(Size);
	}

	bool ReadBytes(
		const uint32 GuestAddress,
		TArrayView<uint8> OutBytes,
		FString& OutError) override
	{
		if (!IsRangeValid(GuestAddress, OutBytes.Num()))
		{
			OutError = TEXT("test guest read is out of bounds");
			return false;
		}
		FMemory::Memcpy(OutBytes.GetData(), Bytes.GetData() + GuestAddress, OutBytes.Num());
		return true;
	}

	bool WriteBytes(
		const uint32 GuestAddress,
		TConstArrayView<uint8> InBytes,
		FString& OutError) override
	{
		if (!IsRangeValid(GuestAddress, InBytes.Num()))
		{
			OutError = TEXT("test guest write is out of bounds");
			return false;
		}
		FMemory::Memcpy(Bytes.GetData() + GuestAddress, InBytes.GetData(), InBytes.Num());
		return true;
	}

	bool BorrowMutableBytes(
		const uint32 GuestAddress,
		const uint32 ByteCount,
		const uint32 Alignment,
		TArrayView<uint8>& OutBytes,
		FString& OutError) override
	{
		OutBytes = TArrayView<uint8>();
		OutError.Reset();
		if (Alignment == 0
			|| !FMath::IsPowerOfTwo(Alignment)
			|| GuestAddress % Alignment != 0
			|| !IsRangeValid(GuestAddress, ByteCount))
		{
			OutError = TEXT("test guest mutable borrow alignment or range is invalid");
			return false;
		}
		OutBytes = MakeArrayView(Bytes).Slice(
			static_cast<int32>(GuestAddress),
			static_cast<int32>(ByteCount));
		return true;
	}

	template <typename ValueType>
	ValueType ReadValue(const uint32 GuestAddress) const
	{
		ValueType Value{};
		if (IsRangeValid(GuestAddress, sizeof(ValueType)))
		{
			FMemory::Memcpy(&Value, Bytes.GetData() + GuestAddress, sizeof(ValueType));
		}
		return Value;
	}

	template <typename ValueType>
	void WriteValue(const uint32 GuestAddress, const ValueType& Value)
	{
		if (IsRangeValid(GuestAddress, sizeof(ValueType)))
		{
			FMemory::Memcpy(Bytes.GetData() + GuestAddress, &Value, sizeof(ValueType));
		}
	}

private:
	bool IsRangeValid(const uint32 GuestAddress, const uint64 Size) const
	{
		return GuestAddress <= static_cast<uint64>(Bytes.Num())
			&& Size <= static_cast<uint64>(Bytes.Num()) - GuestAddress;
	}

	TArray<uint8> Bytes;
};

bool WriteAvidScriptBindingRuntimeUtf8String(
	FAvidScriptBindingRuntimeTestGuestMemory& GuestMemory,
	const uint32 GuestAddress,
	const TConstArrayView<uint8> Payload,
	const uint8 Terminator = 0)
{
	GuestMemory.WriteValue<int32>(GuestAddress, Payload.Num());
	FString Error;
	return GuestMemory.WriteBytes(GuestAddress + sizeof(int32), Payload, Error)
		&& GuestMemory.WriteBytes(
			GuestAddress + sizeof(int32) + Payload.Num(),
			MakeArrayView(&Terminator, 1),
			Error);
}

int32 FindAvidScriptBindingRuntimeBytes(
	const TConstArrayView<uint8> Bytes,
	const TConstArrayView<uint8> Sequence)
{
	if (Sequence.IsEmpty() || Sequence.Num() > Bytes.Num())
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index <= Bytes.Num() - Sequence.Num(); ++Index)
	{
		if (FMemory::Memcmp(Bytes.GetData() + Index, Sequence.GetData(), Sequence.Num()) == 0)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool ReadAvidScriptBindingRuntimeU32Leb(
	TConstArrayView<uint8> Bytes,
	int32 Limit,
	int32& InOutOffset,
	uint32& OutValue)
{
	OutValue = 0;
	for (uint32 Shift = 0; Shift <= 28; Shift += 7)
	{
		if (InOutOffset < 0 || InOutOffset >= Limit || InOutOffset >= Bytes.Num())
		{
			return false;
		}
		const uint8 Byte = Bytes[InOutOffset++];
		if (Shift == 28 && (Byte & 0xf0) != 0)
		{
			return false;
		}
		OutValue |= static_cast<uint32>(Byte & 0x7f) << Shift;
		if ((Byte & 0x80) == 0)
		{
			return true;
		}
	}
	return false;
}

bool PatchAvidScriptBindingRuntimeFunctionToTrap(
	TArray<uint8>& Bytecode,
	uint32 ImportedFunctionCount,
	uint32 FunctionIndex)
{
	static constexpr uint8 ExpectedHeader[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	if (Bytecode.Num() < UE_ARRAY_COUNT(ExpectedHeader)
		|| FMemory::Memcmp(Bytecode.GetData(), ExpectedHeader, UE_ARRAY_COUNT(ExpectedHeader)) != 0
		|| FunctionIndex < ImportedFunctionCount)
	{
		return false;
	}

	const uint32 TargetDefinedOrdinal = FunctionIndex - ImportedFunctionCount;
	int32 SectionOffset = UE_ARRAY_COUNT(ExpectedHeader);
	while (SectionOffset < Bytecode.Num())
	{
		const uint8 SectionId = Bytecode[SectionOffset++];
		uint32 SectionSize = 0;
		if (!ReadAvidScriptBindingRuntimeU32Leb(Bytecode, Bytecode.Num(), SectionOffset, SectionSize)
			|| SectionSize > static_cast<uint32>(Bytecode.Num() - SectionOffset))
		{
			return false;
		}
		const int32 SectionEnd = SectionOffset + static_cast<int32>(SectionSize);
		if (SectionId != 10)
		{
			SectionOffset = SectionEnd;
			continue;
		}

		int32 BodyOffset = SectionOffset;
		uint32 BodyCount = 0;
		if (!ReadAvidScriptBindingRuntimeU32Leb(Bytecode, SectionEnd, BodyOffset, BodyCount)
			|| TargetDefinedOrdinal >= BodyCount)
		{
			return false;
		}
		for (uint32 BodyOrdinal = 0; BodyOrdinal < BodyCount; ++BodyOrdinal)
		{
			uint32 BodySize = 0;
			if (!ReadAvidScriptBindingRuntimeU32Leb(Bytecode, SectionEnd, BodyOffset, BodySize)
				|| BodySize > static_cast<uint32>(SectionEnd - BodyOffset))
			{
				return false;
			}
			const int32 BodyEnd = BodyOffset + static_cast<int32>(BodySize);
			if (BodyOrdinal != TargetDefinedOrdinal)
			{
				BodyOffset = BodyEnd;
				continue;
			}

			int32 InstructionOffset = BodyOffset;
			uint32 LocalGroupCount = 0;
			if (!ReadAvidScriptBindingRuntimeU32Leb(Bytecode, BodyEnd, InstructionOffset, LocalGroupCount))
			{
				return false;
			}
			for (uint32 LocalGroupIndex = 0; LocalGroupIndex < LocalGroupCount; ++LocalGroupIndex)
			{
				uint32 LocalCount = 0;
				if (!ReadAvidScriptBindingRuntimeU32Leb(Bytecode, BodyEnd, InstructionOffset, LocalCount)
					|| InstructionOffset >= BodyEnd)
				{
					return false;
				}
				++InstructionOffset;
			}
			if (InstructionOffset >= BodyEnd)
			{
				return false;
			}
			Bytecode[InstructionOffset] = 0x00;
			return true;
		}
		return false;
	}
	return false;
}

bool LoadAvidScriptBindingRuntimeDebugFunction(
	const FString& DebugMapPath,
	const FString& DisplayNameFragment,
	uint32& OutFunctionIndex,
	FString& OutDisplayName,
	FString& OutSourceFile,
	int32& OutLine,
	int32& OutColumn)
{
	OutFunctionIndex = MAX_uint32;
	OutDisplayName.Reset();
	OutSourceFile.Reset();
	OutLine = 0;
	OutColumn = 0;

	FString Json;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Json, *DebugMapPath)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root)
		|| !Root.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* SourceObject = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
	if (!Root->TryGetObjectField(TEXT("source"), SourceObject)
		|| SourceObject == nullptr
		|| !SourceObject->IsValid()
		|| !(*SourceObject)->TryGetStringField(TEXT("id"), OutSourceFile)
		|| !Root->TryGetArrayField(TEXT("functions"), Functions)
		|| Functions == nullptr)
	{
		return false;
	}

	int32 MatchCount = 0;
	for (const TSharedPtr<FJsonValue>& FunctionValue : *Functions)
	{
		const TSharedPtr<FJsonObject> Function = FunctionValue.IsValid() ? FunctionValue->AsObject() : nullptr;
		const TSharedPtr<FJsonObject>* Span = nullptr;
		double FunctionIndex = 0.0;
		double Line = 0.0;
		double Column = 0.0;
		FString DisplayName;
		if (!Function.IsValid()
			|| !Function->TryGetStringField(TEXT("display_name"), DisplayName)
			|| !DisplayName.Contains(DisplayNameFragment, ESearchCase::CaseSensitive)
			|| !Function->TryGetNumberField(TEXT("wasm_function_index"), FunctionIndex)
			|| FunctionIndex < 0.0
			|| FunctionIndex > static_cast<double>(MAX_uint32)
			|| FunctionIndex != FMath::TruncToDouble(FunctionIndex)
			|| !Function->TryGetObjectField(TEXT("span"), Span)
			|| Span == nullptr
			|| !Span->IsValid()
			|| !(*Span)->TryGetNumberField(TEXT("line"), Line)
			|| !(*Span)->TryGetNumberField(TEXT("column"), Column)
			|| Line < 0.0
			|| Column < 0.0
			|| Line > static_cast<double>(MAX_int32 - 1)
			|| Column > static_cast<double>(MAX_int32 - 1)
			|| Line != FMath::TruncToDouble(Line)
			|| Column != FMath::TruncToDouble(Column))
		{
			continue;
		}
		++MatchCount;
		OutFunctionIndex = static_cast<uint32>(FunctionIndex);
		OutDisplayName = MoveTemp(DisplayName);
		OutLine = static_cast<int32>(Line) + 1;
		OutColumn = static_cast<int32>(Column) + 1;
	}
	return MatchCount == 1 && !OutSourceFile.IsEmpty();
}

bool CreateAvidScriptBindingRuntimeIntegrationWorld(
	UWorld*& OutWorld,
	bool bInitializeForPlay = true)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptBindingRuntimeIntegrationWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	if (bInitializeForPlay)
	{
		OutWorld->InitializeActorsForPlay(FURL());
	}
	return true;
}

void DestroyAvidScriptBindingRuntimeIntegrationWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}

	if (World->HasBegunPlay())
	{
		World->EndPlay(EEndPlayReason::Quit);
	}
	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}
	World->DestroyWorld(false);
	World = nullptr;
}

AActor* SpawnAvidScriptBindingRuntimeIntegrationActor(UWorld& World)
{
	AActor* Actor = World.SpawnActor<AActor>();
	if (Actor == nullptr)
	{
		return nullptr;
	}

	USceneComponent* RootComponent = NewObject<USceneComponent>(Actor, TEXT("BindingRuntimeRoot"));
	if (RootComponent == nullptr)
	{
		return nullptr;
	}
	Actor->SetRootComponent(RootComponent);
	RootComponent->RegisterComponent();
	return Actor;
}

bool LoadAvidScriptBindingRuntimeFixture(TArray<uint8>& OutBytecode)
{
	const FString FixturePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Tests/Fixtures/WasmBackend/P42_4_ReflectedSetActorScale.wasm")));
	return FFileHelper::LoadFileToArray(OutBytecode, *FixturePath);
}

bool LoadAvidScriptBindingRuntimeTrapFixture(TArray<uint8>& OutBytecode)
{
	if (!LoadAvidScriptBindingRuntimeFixture(OutBytecode))
	{
		return false;
	}

	const TArray<uint8> CodeHeader = { 0x0a, 0x47, 0x01, 0x45 };
	const TArray<uint8> CallTail = { 0x10, 0x02, 0x21, 0x05, 0x0f };
	const int32 CodeHeaderIndex = FindAvidScriptBindingRuntimeBytes(OutBytecode, CodeHeader);
	const int32 CallTailIndex = FindAvidScriptBindingRuntimeBytes(OutBytecode, CallTail);
	if (CodeHeaderIndex == INDEX_NONE || CallTailIndex == INDEX_NONE)
	{
		return false;
	}

	// This checked fixture has one small code body, so both encoded sizes remain one-byte LEB128 values.
	++OutBytecode[CodeHeaderIndex + 1];
	++OutBytecode[CodeHeaderIndex + 3];
	OutBytecode.Insert(0x00, CallTailIndex + 4);
	return true;
}

class FAvidScriptBindingRuntimeRecordingJournal final : public IAvidScriptBindingHostEffectJournal
{
public:
	explicit FAvidScriptBindingRuntimeRecordingJournal(const bool bInAcceptPrepare)
		: bAcceptPrepare(bInAcceptPrepare)
	{
	}

	bool PrepareEffect(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& Handle,
		UObject& Target,
		const EAvidScriptBindingReloadEffect Effect,
		FAvidScriptBindingHostEffectPrepareResult& OutResult) override
	{
		++PrepareCallCount;
		LastRegistry = &Registry;
		LastHandle = Handle;
		LastTarget = &Target;
		LastEffect = Effect;
		OutResult = FAvidScriptBindingHostEffectPrepareResult();
		OutResult.bSucceeded = bAcceptPrepare;
		if (!bAcceptPrepare)
		{
			OutResult.ErrorCategory = TEXT("test_host_effect_rejected");
			OutResult.ErrorSource = Target.GetPathName();
			OutResult.ErrorDetails = TEXT("The test journal rejected the candidate write.");
		}
		return bAcceptPrepare;
	}

	bool PrepareReflectedProperty(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& Handle,
		UObject& Target,
		FProperty& Property,
		FAvidScriptBindingHostEffectPrepareResult& OutResult) override
	{
		++ReflectedPropertyPrepareCallCount;
		LastRegistry = &Registry;
		LastHandle = Handle;
		LastTarget = &Target;
		LastProperty = &Property;
		LastEffect = EAvidScriptBindingReloadEffect::ReflectedProperty;
		OutResult = FAvidScriptBindingHostEffectPrepareResult();
		OutResult.bSucceeded = bAcceptPrepare;
		if (!bAcceptPrepare)
		{
			OutResult.ErrorCategory = TEXT("test_host_effect_rejected");
			OutResult.ErrorSource = Target.GetPathName();
			OutResult.ErrorDetails = TEXT("The test journal rejected the candidate property write.");
		}
		return bAcceptPrepare;
	}

	bool bAcceptPrepare = false;
	int32 PrepareCallCount = 0;
	int32 ReflectedPropertyPrepareCallCount = 0;
	FAvidScriptObjectRegistry* LastRegistry = nullptr;
	FAvidScriptObjectHandle LastHandle;
	UObject* LastTarget = nullptr;
	FProperty* LastProperty = nullptr;
	EAvidScriptBindingReloadEffect LastEffect = EAvidScriptBindingReloadEffect::Unsupported;
};

bool GenerateAvidScriptBindingRuntimePackage(
	TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
	FAvidScriptBindingPackageLoadResult& OutLoadResult,
	FString& OutDescriptorJson)
{
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	return FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(
			OutDescriptorJson,
			GenerateResult)
		&& FAvidScriptBindingPackage::LoadDescriptor(
			OutDescriptorJson,
			OutPackage,
			OutLoadResult);
}

bool BuildAvidScriptGeneratedBindingLifecycle(
	const FString& SemanticCacheRoot,
	FAvidScriptEditorCSharpBuildResult& OutBuildResult)
{
	FAvidScriptEditorCSharpBuildConfig Config;
	Config.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	Config.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	Config.SourcePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Samples/CSharp/GeneratedBindingLifecycle/GeneratedBindingLifecycleScript.cs")));
	Config.ModuleId = TEXT("csharp_generated_binding_lifecycle");
	Config.ArtifactStem = TEXT("generated_binding_lifecycle");
	Config.OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest/GeneratedBindingLifecycle")));
	Config.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
		Config.OutputRoot,
		Config.ArtifactStem);
	Config.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
		Config.OutputRoot,
		Config.ArtifactStem);
	Config.SemanticCacheRoot = SemanticCacheRoot;
	return FAvidScriptEditorCSharpBuildService::BuildProfile(Config, OutBuildResult);
}

bool BuildAvidScriptPlayablePickup(
	const FString& SourcePath,
	const FString& OutputRoot,
	const FString& SemanticCacheRoot,
	const FString& ModuleId,
	const FString& ArtifactStem,
	FAvidScriptEditorCSharpBuildResult& OutBuildResult)
{
	FAvidScriptEditorCSharpBuildConfig Config;
	Config.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	Config.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	Config.SourcePath = SourcePath;
	Config.ModuleId = ModuleId;
	Config.ArtifactStem = ArtifactStem;
	Config.OutputRoot = OutputRoot;
	Config.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
		Config.OutputRoot,
		Config.ArtifactStem);
	Config.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
		Config.OutputRoot,
		Config.ArtifactStem);
	Config.SemanticCacheRoot = SemanticCacheRoot;
	return FAvidScriptEditorCSharpBuildService::BuildProfile(Config, OutBuildResult);
}

bool AcceptAvidScriptGeneratedBindingLifecycleBuild(
	FAutomationTestBase& Test,
	const FString& BuildLabel,
	const FAvidScriptEditorCSharpBuildResult& BuildResult,
	FAvidScriptWasmReloadManifest& OutManifest,
	TArray<uint8>& OutBytecode)
{
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s authorization binding package exists"), *BuildLabel),
			FPaths::FileExists(BuildResult.AuthorizationBindingPackagePath))
		|| !Test.TestTrue(
			*FString::Printf(TEXT("%s runtime binding package exists"), *BuildLabel),
			FPaths::FileExists(BuildResult.BindingPackagePath)))
	{
		return false;
	}
	Test.TestNotEqual(
		*FString::Printf(TEXT("%s separates authorization and runtime packages"), *BuildLabel),
		BuildResult.AuthorizationBindingPackagePath,
		BuildResult.BindingPackagePath);

	FString AuthorizationPackageJson;
	TSharedPtr<FJsonObject> AuthorizationPackageObject;
	const TArray<TSharedPtr<FJsonValue>>* AuthorizationImports = nullptr;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s authorization package can be read"), *BuildLabel),
			FFileHelper::LoadFileToString(
				AuthorizationPackageJson,
				*BuildResult.AuthorizationBindingPackagePath))
		|| !Test.TestTrue(
			*FString::Printf(TEXT("%s authorization package parses"), *BuildLabel),
			FJsonSerializer::Deserialize(
				TJsonReaderFactory<>::Create(AuthorizationPackageJson),
				AuthorizationPackageObject))
		|| !Test.TestTrue(
			*FString::Printf(TEXT("%s authorization package exposes required imports"), *BuildLabel),
			AuthorizationPackageObject.IsValid()
				&& AuthorizationPackageObject->TryGetArrayField(
					TEXT("required_imports"),
					AuthorizationImports))
		|| AuthorizationImports == nullptr)
	{
		return false;
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s authorization ceiling contains 373 generated bindings and 17 shared imports"), *BuildLabel),
		AuthorizationImports->Num(),
		390);

	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s manifest, WASM, and runtime package load"), *BuildLabel),
			FAvidScriptWasmReloadManifestLoader::LoadFromFile(
				BuildResult.ManifestPath,
				OutManifest,
				OutBytecode,
				ManifestLoadResult)))
	{
		Test.AddError(ManifestLoadResult.ErrorMessage);
		return false;
	}
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s owns an immutable runtime package"), *BuildLabel),
			OutManifest.BindingPackage.IsValid()))
	{
		return false;
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s runtime package contains five reachable bindings and object-type support"), *BuildLabel),
		OutManifest.BindingPackage->GetVmPackage().Imports.Num(),
		6);
	int32 RequiredDynamicImportCount = 0;
	for (const FAvidScriptWasmRequiredImport& Import : OutManifest.RequiredImports)
	{
		if (Import.ModuleName == TEXT("avidscript")
			&& Import.ImportName.StartsWith(TEXT("avid_ue_"), ESearchCase::CaseSensitive))
		{
			++RequiredDynamicImportCount;
		}
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s WASM requires five reachable reflected imports"), *BuildLabel),
		RequiredDynamicImportCount,
		5);

	UWorld* World = nullptr;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s integration world is created"), *BuildLabel),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};

	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!Test.TestNotNull(
			*FString::Printf(TEXT("%s lifecycle actor spawns"), *BuildLabel),
			Actor))
	{
		return false;
	}
	Actor->SetActorScale3D(FVector(1.0, 1.0, 1.0));
	Actor->SetActorLocation(FVector(0.5, 0.0, 0.0));
	Actor->CustomTimeDilation = 1.25f;
	Test.TestTrue(
		*FString::Printf(TEXT("%s root component has the scripted world location"), *BuildLabel),
		Actor->GetRootComponent()->GetComponentLocation().Equals(FVector(0.5, 0.0, 0.0), 0.001));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	Test.TestTrue(
		*FString::Printf(TEXT("%s lifecycle owner registers"), *BuildLabel),
		RegisterResult.bSucceeded);

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.OwnerHandle = ActorHandle;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptRuntimeSession Session;
	Session.SetHostContext(HostContext);
	FAvidScriptWasmReloadResult ReloadResult;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s BeginPlay activates through Runtime Session and WAMR"), *BuildLabel),
			Session.LoadInitialModule(
				OutBytecode.GetData(),
				OutBytecode.Num(),
				OutManifest,
				ReloadResult)))
	{
		Test.AddError(ReloadResult.ErrorMessage);
		return false;
	}
	Test.TestTrue(
		*FString::Printf(TEXT("%s BeginPlay composes scalar and object properties with a component call"), *BuildLabel),
		Actor->GetActorScale3D().Equals(FVector(3.0, 3.0, 4.0), 0.001));

	FAvidScriptWasmSmokeResult TickResult;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s Tick executes through the live scheduler"), *BuildLabel),
			Session.TickLive(0.5f, TickResult)))
	{
		Test.AddError(TickResult.ErrorMessage);
		return false;
	}
	Test.TestTrue(
		*FString::Printf(TEXT("%s Tick reads and writes through generated bindings"), *BuildLabel),
		Actor->GetActorScale3D().Equals(FVector(3.5, 3.0, 4.0), 0.001));
	Test.TestEqual(
		*FString::Printf(TEXT("%s scheduler records one Tick"), *BuildLabel),
		Session.GetLiveTickCallCount(),
		1);

	FAvidScriptWasmReloadManifest CommitManifest = OutManifest;
	CommitManifest.ModuleId += TEXT("_transaction_commit");
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s C# candidate reload commits"), *BuildLabel),
			Session.ReloadModule(
				OutBytecode.GetData(),
				OutBytecode.Num(),
				CommitManifest,
				ReloadResult)))
	{
		Test.AddError(ReloadResult.ErrorMessage);
		return false;
	}
	Test.TestTrue(
		*FString::Printf(TEXT("%s candidate opens a host effect transaction"), *BuildLabel),
		ReloadResult.bHostEffectTransactionAttempted);
	Test.TestTrue(
		*FString::Printf(TEXT("%s candidate commits its host effect transaction"), *BuildLabel),
		ReloadResult.bHostEffectTransactionCommitted);
	Test.TestEqual(
		*FString::Printf(TEXT("%s candidate captures one reflected Actor transform"), *BuildLabel),
		ReloadResult.HostEffectCapturedObjectCount,
		1);
	Test.TestTrue(
		*FString::Printf(TEXT("%s committed C# BeginPlay scale remains applied"), *BuildLabel),
		Actor->GetActorScale3D().Equals(FVector(3.0, 3.0, 4.0), 0.001));

	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s committed C# runtime ticks"), *BuildLabel),
			Session.TickLive(0.25f, TickResult)))
	{
		Test.AddError(TickResult.ErrorMessage);
		return false;
	}
	const FVector ScaleBeforeRejectedCandidate = Actor->GetActorScale3D();
	Test.TestTrue(
		*FString::Printf(TEXT("%s committed C# Tick retains live reflected writes"), *BuildLabel),
		ScaleBeforeRejectedCandidate.Equals(FVector(3.25, 3.0, 4.0), 0.001));

	TArray<uint8> TrapBytecode;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s dynamic write-then-trap fixture loads"), *BuildLabel),
			LoadAvidScriptBindingRuntimeTrapFixture(TrapBytecode)))
	{
		return false;
	}
	FAvidScriptWasmReloadManifest TrapManifest = OutManifest;
	TrapManifest.ModuleId += TEXT("_transaction_trap");
	TrapManifest.RequiredExports = { TEXT("avid_on_begin_play") };
	TrapManifest.RequiredImports = {
		{ TEXT("env"), TEXT("owner_get_slot") },
		{ TEXT("env"), TEXT("owner_get_generation") },
		{ TEXT("avidscript"), TEXT("avid_ue_e493dae7c6aae6c7") }
	};
	Test.TestFalse(
		*FString::Printf(TEXT("%s reflected write-then-trap candidate is rejected"), *BuildLabel),
		Session.ReloadModule(
			TrapBytecode.GetData(),
			TrapBytecode.Num(),
			TrapManifest,
			ReloadResult));
	Test.TestTrue(
		*FString::Printf(TEXT("%s rejected candidate attempts rollback"), *BuildLabel),
		ReloadResult.bHostEffectRollbackAttempted);
	Test.TestTrue(
		*FString::Printf(TEXT("%s rejected candidate restores reflected host effects"), *BuildLabel),
		ReloadResult.bHostEffectRollbackSucceeded);
	Test.TestEqual(
		*FString::Printf(TEXT("%s rejected candidate restores one Actor transform"), *BuildLabel),
		ReloadResult.HostEffectRestoredObjectCount,
		1);
	Test.TestTrue(
		*FString::Printf(TEXT("%s rejected candidate preserves the committed scale"), *BuildLabel),
		Actor->GetActorScale3D().Equals(ScaleBeforeRejectedCandidate, 0.001));
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s old C# runtime ticks after candidate rollback"), *BuildLabel),
			Session.TickLive(0.25f, TickResult)))
	{
		Test.AddError(TickResult.ErrorMessage);
		return false;
	}
	Test.TestTrue(
		*FString::Printf(TEXT("%s old C# Tick continues reflected gameplay after rollback"), *BuildLabel),
		Actor->GetActorScale3D().Equals(FVector(3.5, 3.0, 4.0), 0.001));

	AActor* RootlessActor = World->SpawnActor<AActor>();
	if (!Test.TestNotNull(
		*FString::Printf(TEXT("%s rootless lifecycle actor spawns"), *BuildLabel),
		RootlessActor))
	{
		return false;
	}
	Test.TestNull(
		*FString::Printf(TEXT("%s negative fixture has no root component"), *BuildLabel),
		RootlessActor->GetRootComponent());
	RootlessActor->SetActorScale3D(FVector(1.0, 1.0, 1.0));

	FAvidScriptObjectRegistry RootlessRegistry;
	FAvidScriptObjectHandleResult RootlessRegisterResult;
	const FAvidScriptObjectHandle RootlessActorHandle =
		RootlessRegistry.RegisterObject(RootlessActor, RootlessRegisterResult);
	Test.TestTrue(
		*FString::Printf(TEXT("%s rootless lifecycle owner registers"), *BuildLabel),
		RootlessRegisterResult.bSucceeded);

	FAvidScriptWasmHostContext RootlessHostContext;
	RootlessHostContext.ObjectRegistry = &RootlessRegistry;
	RootlessHostContext.OwnerHandle = RootlessActorHandle;
	RootlessHostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptRuntimeSession RootlessSession;
	RootlessSession.SetHostContext(RootlessHostContext);
	FAvidScriptWasmReloadResult RootlessReloadResult;
	Test.TestFalse(
		*FString::Printf(TEXT("%s null object property receiver rejects BeginPlay"), *BuildLabel),
		RootlessSession.LoadInitialModule(
			OutBytecode.GetData(),
			OutBytecode.Num(),
			OutManifest,
			RootlessReloadResult));
	Test.TestEqual(
		*FString::Printf(TEXT("%s null receiver surfaces a host import failure"), *BuildLabel),
		RootlessReloadResult.ErrorCategory,
		FString(TEXT("host_import_failed")));
	Test.TestTrue(
		*FString::Printf(TEXT("%s null receiver identifies the generated import"), *BuildLabel),
		RootlessReloadResult.RuntimeResult.ImportName.StartsWith(
			TEXT("avid_ue_"),
			ESearchCase::CaseSensitive));
	Test.TestTrue(
		*FString::Printf(TEXT("%s null receiver preserves the object handle diagnostic"), *BuildLabel),
		RootlessReloadResult.ErrorMessage.Contains(
			TEXT("invalid UObject handle"),
			ESearchCase::CaseSensitive));
	Test.TestTrue(
		*FString::Printf(TEXT("%s failed object chain does not continue with default values"), *BuildLabel),
		RootlessActor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.001));
	return true;
}

bool AcceptAvidScriptProjectGameplayWorkspaceBuild(
	FAutomationTestBase& Test,
	const FString& BuildLabel,
	const FAvidScriptEditorCSharpWorkspaceResult& WorkspaceResult,
	const FAvidScriptEditorCSharpBuildResult& BuildResult)
{
	FString AuthorizationPackageJson;
	TSharedPtr<FJsonObject> AuthorizationPackageObject;
	const TArray<TSharedPtr<FJsonValue>>* AuthorizationImports = nullptr;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s authorization package can be read"), *BuildLabel),
			FFileHelper::LoadFileToString(
				AuthorizationPackageJson,
				*BuildResult.AuthorizationBindingPackagePath))
		|| !Test.TestTrue(
			*FString::Printf(TEXT("%s authorization package parses"), *BuildLabel),
			FJsonSerializer::Deserialize(
				TJsonReaderFactory<>::Create(AuthorizationPackageJson),
				AuthorizationPackageObject))
		|| !Test.TestTrue(
			*FString::Printf(TEXT("%s authorization package exposes required imports"), *BuildLabel),
			AuthorizationPackageObject.IsValid()
				&& AuthorizationPackageObject->TryGetArrayField(
					TEXT("required_imports"),
					AuthorizationImports))
		|| AuthorizationImports == nullptr)
	{
		return false;
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s authorization ceiling contains 373 gameplay bindings and 17 shared imports"), *BuildLabel),
		AuthorizationImports->Num(),
		390);

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s manifest, WASM, and runtime package load"), *BuildLabel),
			FAvidScriptWasmReloadManifestLoader::LoadFromFile(
				BuildResult.ManifestPath,
				Manifest,
				Bytecode,
				ManifestLoadResult)))
	{
		Test.AddError(ManifestLoadResult.ErrorMessage);
		return false;
	}
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s owns a runtime binding package"), *BuildLabel),
			Manifest.BindingPackage.IsValid()))
	{
		return false;
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s runtime package contains three reachable bindings and object-type support"), *BuildLabel),
		Manifest.BindingPackage->GetVmPackage().Imports.Num(),
		4);
	int32 DynamicImportCount = 0;
	for (const FAvidScriptWasmRequiredImport& Import : Manifest.RequiredImports)
	{
		if (Import.ModuleName == TEXT("avidscript")
			&& Import.ImportName.StartsWith(TEXT("avid_ue_"), ESearchCase::CaseSensitive))
		{
			++DynamicImportCount;
		}
	}
	Test.TestEqual(
		*FString::Printf(TEXT("%s WASM requires three reflected imports"), *BuildLabel),
		DynamicImportCount,
		3);

	FString ManifestJson;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s manifest can be read for provenance checks"), *BuildLabel),
			FFileHelper::LoadFileToString(ManifestJson, *BuildResult.ManifestPath)))
	{
		return false;
	}
	Test.TestFalse(
		*FString::Printf(TEXT("%s manifest excludes generated facade path"), *BuildLabel),
		ManifestJson.Contains(WorkspaceResult.FacadePath, ESearchCase::CaseSensitive));

	UWorld* World = nullptr;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s component lifecycle world is created"), *BuildLabel),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World, false)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};

	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	World->SetBegunPlay(true);

	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!Test.TestNotNull(
			*FString::Printf(TEXT("%s gameplay actor spawns"), *BuildLabel),
			Actor))
	{
		return false;
	}
	Actor->SetActorScale3D(FVector(2.0, 2.0, 2.0));
	Actor->SetActorRotation(FRotator(0.0, 10.0, 0.0));

	FAvidScriptEditorComponentBindingResult BindingResult;
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s report binds through ComponentBindingService"), *BuildLabel),
			FAvidScriptEditorComponentBindingService::ApplyCSharpReportToActor(
				BuildResult.ReportPath,
				Actor,
				BindingResult)))
	{
		Test.AddError(BindingResult.ErrorMessage);
		return false;
	}
	if (!Test.TestNotNull(
			*FString::Printf(TEXT("%s binding creates an AvidScript component"), *BuildLabel),
			BindingResult.Component))
	{
		return false;
	}

	const FAvidScriptComponentRuntimeStats StatsAfterBeginPlay =
		BindingResult.Component->GetRuntimeStats();
	if (!Test.TestTrue(
			*FString::Printf(TEXT("%s component loads the C# WASM runtime"), *BuildLabel),
			StatsAfterBeginPlay.bRuntimeLoaded))
	{
		Test.AddError(StatsAfterBeginPlay.LastErrorMessage);
		return false;
	}
	Test.TestTrue(
		*FString::Printf(TEXT("%s component calls C# BeginPlay"), *BuildLabel),
		StatsAfterBeginPlay.bBeginPlayCalled);
	Test.TestTrue(
		*FString::Printf(TEXT("%s BeginPlay resets Actor scale"), *BuildLabel),
		Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.001));

	BindingResult.Component->TickComponent(0.5f, LEVELTICK_All, nullptr);
	const FAvidScriptComponentRuntimeStats StatsAfterTick =
		BindingResult.Component->GetRuntimeStats();
	Test.TestEqual(
		*FString::Printf(TEXT("%s component records one Tick"), *BuildLabel),
		StatsAfterTick.TickCallCount,
		1);
	Test.TestTrue(
		*FString::Printf(TEXT("%s Tick rotates Actor yaw by 45 degrees"), *BuildLabel),
		FMath::IsNearlyEqual(Actor->GetActorRotation().Yaw, 55.0, 0.01));
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeScalarMetadataFailureTest,
	"AvidScript.Editor.BindingRuntime.ScalarMetadataFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeScalarMetadataFailureTest::RunTest(const FString& Parameters)
{
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
		TEXT("Default descriptor generates for scalar metadata validation"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateDefault(
			DescriptorJson,
			GenerateResult)))
	{
		return false;
	}

	FString TamperedJson = DescriptorJson.Replace(
		TEXT("\"cpp_type\": \"float\""),
		TEXT("\"cpp_type\": \"int32\""),
		ESearchCase::CaseSensitive);
	TestFalse(TEXT("Scalar cpp_type metadata was changed"), TamperedJson == DescriptorJson);
	TestTrue(
		TEXT("Scalar metadata tamper is rehashed to exercise the reflected contract gate"),
		RehashAvidScriptBindingRuntimeDescriptor(TamperedJson));

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	TestFalse(
		TEXT("Runtime package rejects same-width scalar cpp_type tampering"),
		FAvidScriptBindingPackage::LoadDescriptor(TamperedJson, Package, LoadResult));
	TestEqual(
		TEXT("Scalar metadata failure is attributed to the reflected return contract"),
		LoadResult.ErrorCategory,
		FString(TEXT("binding_return_contract_mismatch")));
	TestFalse(TEXT("Failed scalar package is not published"), Package.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeFNameInputTest,
	"AvidScript.Editor.BindingRuntime.FNameInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeFNameInputTest::RunTest(const FString& Parameters)
{
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
		TEXT("ActorHasTag descriptor generates for runtime FName validation"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.engine.fname.runtime"),
			{ { TEXT("/Script/Engine.Actor"), TEXT("ActorHasTag") } },
			DescriptorJson,
			GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ") + GenerateResult.ErrorMessage);
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
		TEXT("Runtime loads the exact FName input package"),
		FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, Package, LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}
	TestEqual(TEXT("FName runtime package exposes its reflected and object-type imports"), Package->GetVmPackage().Imports.Num(), 2);
	if (Package->GetVmPackage().Imports.Num() != 2)
	{
		return false;
	}
	TestTrue(
		TEXT("FName runtime package exposes object-type support"),
		Package->GetVmPackage().Imports.ContainsByPredicate(
			[](const FAvidScriptVmDynamicImport& Import)
			{
				return Import.ImportName == TEXT("avid_object_type_is_a");
			}));

	UWorld* World = nullptr;
	if (!TestTrue(
		TEXT("FName runtime integration world is created"),
		CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};

	AAvidScriptBindingRuntimeProcessEventTestActor* Actor =
		World->SpawnActor<AAvidScriptBindingRuntimeProcessEventTestActor>();
	if (!TestNotNull(TEXT("FName runtime test actor spawns"), Actor))
	{
		return false;
	}
	Actor->Tags.Add(FName(TEXT("Player")));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	if (!TestTrue(TEXT("FName runtime actor registers"), RegisterResult.bSucceeded))
	{
		return false;
	}

	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.OwnerHandle = ActorHandle;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	const uint32 BindingOrdinal = Package->GetVmPackage().Imports[0].Ordinal;
	const auto Dispatch = [&Package, &Context, &Scratch, ActorHandle, BindingOrdinal](
		FAvidScriptBindingRuntimeTestGuestMemory* GuestMemory,
		const uint64 NameAddress,
		const uint32 ReturnAddress,
		FAvidScriptDynamicHostCallResult& OutResult)
	{
		const uint64 Arguments[] = {
			ActorHandle.Slot,
			ActorHandle.Generation,
			NameAddress,
			ReturnAddress
		};
		FAvidScriptDynamicHostCall Call;
		Call.BindingOrdinal = BindingOrdinal;
		Call.Arguments = MakeArrayView(Arguments);
		Call.GuestMemory = GuestMemory;
		return Package->Dispatch(Call, Context, Scratch, OutResult);
	};
	const auto TestRejected = [this, Actor, &Dispatch](
		const TCHAR* Label,
		FAvidScriptBindingRuntimeTestGuestMemory* GuestMemory,
		const uint64 NameAddress,
		const uint32 ReturnAddress,
		const TCHAR* ExpectedCategory)
	{
		static constexpr int32 ReturnSentinel = 0x13572468;
		if (GuestMemory != nullptr)
		{
			GuestMemory->WriteValue<int32>(ReturnAddress, ReturnSentinel);
		}
		Actor->ProcessEventCallCount = 0;
		FAvidScriptDynamicHostCallResult Result;
		TestFalse(Label, Dispatch(GuestMemory, NameAddress, ReturnAddress, Result));
		TestTrue(
			*FString::Printf(TEXT("%s reports %s"), Label, ExpectedCategory),
			Result.Details.Contains(ExpectedCategory, ESearchCase::CaseSensitive));
		TestEqual(
			*FString::Printf(TEXT("%s is rejected before ProcessEvent"), Label),
			Actor->ProcessEventCallCount,
			0);
		if (GuestMemory != nullptr)
		{
			TestEqual(
				*FString::Printf(TEXT("%s leaves return storage untouched"), Label),
				GuestMemory->ReadValue<int32>(ReturnAddress),
				ReturnSentinel);
		}
	};

	static constexpr uint32 NameAddress = 16;
	static constexpr uint32 ReturnAddress = 64;
	const uint8 PlayerUtf8[] = { 'P', 'l', 'a', 'y', 'e', 'r' };
	FAvidScriptBindingRuntimeTestGuestMemory ValidMemory(128);
	TestTrue(
		TEXT("Valid Player string is written into guest memory"),
		WriteAvidScriptBindingRuntimeUtf8String(ValidMemory, NameAddress, MakeArrayView(PlayerUtf8)));
	ValidMemory.WriteValue<int32>(ReturnAddress, 0);
	Actor->ProcessEventCallCount = 0;
	FAvidScriptDynamicHostCallResult ValidResult;
	TestTrue(
		TEXT("ActorHasTag dispatch accepts a valid FName input"),
		Dispatch(&ValidMemory, NameAddress, ReturnAddress, ValidResult));
	TestTrue(TEXT("ActorHasTag dispatch reports success"), ValidResult.bSucceeded);
	TestEqual(TEXT("ActorHasTag writes true into guest memory"), ValidMemory.ReadValue<int32>(ReturnAddress), 1);
	TestEqual(TEXT("Valid ActorHasTag reaches ProcessEvent once"), Actor->ProcessEventCallCount, 1);

	const TArray<uint8> EmptyPayload;
	FAvidScriptBindingRuntimeTestGuestMemory EmptyMemory(128);
	TestTrue(
		TEXT("Empty FName fixture is written"),
		WriteAvidScriptBindingRuntimeUtf8String(EmptyMemory, NameAddress, EmptyPayload));
	EmptyMemory.WriteValue<int32>(ReturnAddress, -1);
	Actor->ProcessEventCallCount = 0;
	FAvidScriptDynamicHostCallResult EmptyResult;
	TestTrue(
		TEXT("Empty FName maps to NAME_None and reaches ActorHasTag"),
		Dispatch(&EmptyMemory, NameAddress, ReturnAddress, EmptyResult));
	TestEqual(TEXT("ActorHasTag reports false for NAME_None"), EmptyMemory.ReadValue<int32>(ReturnAddress), 0);
	TestEqual(TEXT("Empty FName reaches ProcessEvent once"), Actor->ProcessEventCallCount, 1);

	const TCHAR ChineseTag[] = { static_cast<TCHAR>(0x73a9), static_cast<TCHAR>(0x5bb6), 0 };
	Actor->Tags.Add(FName(ChineseTag));
	const uint8 ChineseUtf8[] = { 0xe7, 0x8e, 0xa9, 0xe5, 0xae, 0xb6 };
	FAvidScriptBindingRuntimeTestGuestMemory ChineseMemory(128);
	TestTrue(
		TEXT("Valid multibyte FName fixture is written"),
		WriteAvidScriptBindingRuntimeUtf8String(ChineseMemory, NameAddress, MakeArrayView(ChineseUtf8)));
	ChineseMemory.WriteValue<int32>(ReturnAddress, 0);
	Actor->ProcessEventCallCount = 0;
	FAvidScriptDynamicHostCallResult ChineseResult;
	TestTrue(
		TEXT("Valid multibyte UTF-8 reaches ActorHasTag"),
		Dispatch(&ChineseMemory, NameAddress, ReturnAddress, ChineseResult));
	TestEqual(TEXT("Multibyte FName retains exact tag identity"), ChineseMemory.ReadValue<int32>(ReturnAddress), 1);
	TestEqual(TEXT("Multibyte FName reaches ProcessEvent once"), Actor->ProcessEventCallCount, 1);

	const TCHAR SupplementaryTag[] = { static_cast<TCHAR>(0xd83d), static_cast<TCHAR>(0xde00), 0 };
	Actor->Tags.Add(FName(SupplementaryTag));
	const uint8 SupplementaryUtf8[] = { 0xf0, 0x9f, 0x98, 0x80 };
	FAvidScriptBindingRuntimeTestGuestMemory SupplementaryMemory(128);
	TestTrue(
		TEXT("Valid four-byte Unicode FName fixture is written"),
		WriteAvidScriptBindingRuntimeUtf8String(
			SupplementaryMemory,
			NameAddress,
			MakeArrayView(SupplementaryUtf8)));
	SupplementaryMemory.WriteValue<int32>(ReturnAddress, 0);
	Actor->ProcessEventCallCount = 0;
	FAvidScriptDynamicHostCallResult SupplementaryResult;
	TestTrue(
		TEXT("Valid four-byte Unicode reaches ActorHasTag"),
		Dispatch(&SupplementaryMemory, NameAddress, ReturnAddress, SupplementaryResult));
	TestEqual(
		TEXT("Four-byte Unicode FName retains exact tag identity"),
		SupplementaryMemory.ReadValue<int32>(ReturnAddress),
		1);
	TestEqual(TEXT("Four-byte Unicode reaches ProcessEvent once"), Actor->ProcessEventCallCount, 1);

	const TCHAR ReplacementTag[] = { static_cast<TCHAR>(0xfffd), 0 };
	Actor->Tags.Add(FName(ReplacementTag));
	const uint8 ReplacementUtf8[] = { 0xef, 0xbf, 0xbd };
	FAvidScriptBindingRuntimeTestGuestMemory ReplacementMemory(128);
	TestTrue(
		TEXT("Valid U+FFFD FName fixture is written"),
		WriteAvidScriptBindingRuntimeUtf8String(ReplacementMemory, NameAddress, MakeArrayView(ReplacementUtf8)));
	ReplacementMemory.WriteValue<int32>(ReturnAddress, 0);
	Actor->ProcessEventCallCount = 0;
	FAvidScriptDynamicHostCallResult ReplacementResult;
	TestTrue(
		TEXT("Valid U+FFFD is not confused with invalid UTF-8 replacement"),
		Dispatch(&ReplacementMemory, NameAddress, ReturnAddress, ReplacementResult));
	TestEqual(TEXT("Valid U+FFFD retains exact tag identity"), ReplacementMemory.ReadValue<int32>(ReturnAddress), 1);
	TestEqual(TEXT("Valid U+FFFD reaches ProcessEvent once"), Actor->ProcessEventCallCount, 1);

	TArray<uint8> MaximumPayload;
	MaximumPayload.Init(static_cast<uint8>('M'), NAME_SIZE - 1);
	FAvidScriptBindingRuntimeTestGuestMemory MaximumMemory(NAME_SIZE + 128);
	TestTrue(
		TEXT("Maximum-length FName fixture is written"),
		WriteAvidScriptBindingRuntimeUtf8String(MaximumMemory, NameAddress, MaximumPayload));
	MaximumMemory.WriteValue<int32>(NAME_SIZE + 64, -1);
	Actor->ProcessEventCallCount = 0;
	FAvidScriptDynamicHostCallResult MaximumResult;
	TestTrue(
		TEXT("A NAME_SIZE minus one ASCII FName is accepted"),
		Dispatch(&MaximumMemory, NameAddress, NAME_SIZE + 64, MaximumResult));
	TestEqual(TEXT("Unknown maximum-length tag reports false"), MaximumMemory.ReadValue<int32>(NAME_SIZE + 64), 0);
	TestEqual(TEXT("Maximum-length FName reaches ProcessEvent once"), Actor->ProcessEventCallCount, 1);

	TestRejected(
		TEXT("FName input without guest memory fails closed"),
		nullptr,
		NameAddress,
		ReturnAddress,
		TEXT("binding_frame_mismatch"));
	TestRejected(
		TEXT("FName address with non-zero high bits fails closed"),
		&ValidMemory,
		(1ull << 32) | NameAddress,
		ReturnAddress,
		TEXT("binding_argument_invalid"));

	FAvidScriptBindingRuntimeTestGuestMemory NegativeLengthMemory(128);
	NegativeLengthMemory.WriteValue<int32>(NameAddress, -1);
	TestRejected(
		TEXT("Negative FName byte length fails closed"),
		&NegativeLengthMemory,
		NameAddress,
		ReturnAddress,
		TEXT("binding_argument_invalid"));

	TArray<uint8> OverlongPayload;
	OverlongPayload.Init(static_cast<uint8>('A'), NAME_SIZE);
	FAvidScriptBindingRuntimeTestGuestMemory OverlongMemory(NAME_SIZE + 128);
	TestTrue(
		TEXT("Overlong FName fixture is written"),
		WriteAvidScriptBindingRuntimeUtf8String(OverlongMemory, NameAddress, OverlongPayload));
	TestRejected(
		TEXT("A NAME_SIZE ASCII FName fails closed"),
		&OverlongMemory,
		NameAddress,
		NAME_SIZE + 64,
		TEXT("binding_argument_invalid"));

	FAvidScriptBindingRuntimeTestGuestMemory ExcessByteLengthMemory(128);
	ExcessByteLengthMemory.WriteValue<uint32>(NameAddress, NAME_SIZE * 4 + 1);
	TestRejected(
		TEXT("FName byte length above the UTF-8 ceiling fails before payload read"),
		&ExcessByteLengthMemory,
		NameAddress,
		ReturnAddress,
		TEXT("binding_argument_invalid"));

	FAvidScriptBindingRuntimeTestGuestMemory OutOfBoundsMemory(24);
	OutOfBoundsMemory.WriteValue<int32>(NameAddress, 6);
	TestRejected(
		TEXT("Out-of-bounds FName payload fails closed"),
		&OutOfBoundsMemory,
		NameAddress,
		20,
		TEXT("binding_argument_invalid"));

	const uint8 EmbeddedNullUtf8[] = { 'P', 'l', 0, 'y', 'e', 'r' };
	FAvidScriptBindingRuntimeTestGuestMemory EmbeddedNullMemory(128);
	TestTrue(
		TEXT("Embedded NUL FName fixture is written"),
		WriteAvidScriptBindingRuntimeUtf8String(EmbeddedNullMemory, NameAddress, MakeArrayView(EmbeddedNullUtf8)));
	TestRejected(
		TEXT("Embedded NUL FName payload fails closed"),
		&EmbeddedNullMemory,
		NameAddress,
		ReturnAddress,
		TEXT("binding_argument_invalid"));

	FAvidScriptBindingRuntimeTestGuestMemory MissingTerminatorMemory(128);
	TestTrue(
		TEXT("Missing terminator FName fixture is written"),
		WriteAvidScriptBindingRuntimeUtf8String(
			MissingTerminatorMemory,
			NameAddress,
			MakeArrayView(PlayerUtf8),
			1));
	TestRejected(
		TEXT("FName payload without a zero terminator fails closed"),
		&MissingTerminatorMemory,
		NameAddress,
		ReturnAddress,
		TEXT("binding_argument_invalid"));

	const TArray<TArray<uint8>> InvalidUtf8Payloads = {
		{ 0xc3, 0x28 },
		{ 0xe2, 0x82 },
		{ 0xc0, 0xaf },
		{ 0xed, 0xa0, 0x80 },
		{ 0xf4, 0x90, 0x80, 0x80 }
	};
	for (int32 Index = 0; Index < InvalidUtf8Payloads.Num(); ++Index)
	{
		FAvidScriptBindingRuntimeTestGuestMemory InvalidUtf8Memory(128);
		TestTrue(
			*FString::Printf(TEXT("Invalid UTF-8 FName fixture %d is written"), Index),
			WriteAvidScriptBindingRuntimeUtf8String(
				InvalidUtf8Memory,
				NameAddress,
				InvalidUtf8Payloads[Index]));
		const FString Label = FString::Printf(TEXT("Invalid UTF-8 FName payload %d fails closed"), Index);
		TestRejected(
			*Label,
			&InvalidUtf8Memory,
			NameAddress,
			ReturnAddress,
			TEXT("binding_argument_invalid"));
	}

	FString TamperedDescriptorJson = DescriptorJson.Replace(
		TEXT("\"kind\": \"name_utf8\""),
		TEXT("\"kind\": \"name_utf16\""),
		ESearchCase::CaseSensitive);
	TestFalse(TEXT("FName kind metadata was changed"), TamperedDescriptorJson == DescriptorJson);
	TestTrue(
		TEXT("FName kind tamper is rehashed to exercise the reflected contract gate"),
		RehashAvidScriptBindingRuntimeDescriptor(TamperedDescriptorJson));
	TSharedPtr<const FAvidScriptBindingPackage> TamperedPackage;
	FAvidScriptBindingPackageLoadResult TamperedLoadResult;
	TestFalse(
		TEXT("Runtime rejects an FName descriptor kind mismatch"),
		FAvidScriptBindingPackage::LoadDescriptor(
			TamperedDescriptorJson,
			TamperedPackage,
			TamperedLoadResult));
	TestTrue(
		TEXT("FName descriptor mismatch has a stable contract category"),
		TamperedLoadResult.ErrorCategory == TEXT("binding_property_contract_mismatch"));
	TestFalse(TEXT("Rejected FName descriptor publishes no package"), TamperedPackage.IsValid());

	FString TamperedSizeDescriptorJson = DescriptorJson.Replace(
		TEXT("\"size\": 4"),
		TEXT("\"size\": 8"),
		ESearchCase::CaseSensitive);
	TestFalse(TEXT("FName declared size metadata was changed"), TamperedSizeDescriptorJson == DescriptorJson);
	TestTrue(
		TEXT("FName size tamper is rehashed to exercise the reflected contract gate"),
		RehashAvidScriptBindingRuntimeDescriptor(TamperedSizeDescriptorJson));
	TSharedPtr<const FAvidScriptBindingPackage> TamperedSizePackage;
	FAvidScriptBindingPackageLoadResult TamperedSizeLoadResult;
	TestFalse(
		TEXT("Runtime exact tuple gate rejects an FName size mismatch"),
		FAvidScriptBindingPackage::LoadDescriptor(
			TamperedSizeDescriptorJson,
			TamperedSizePackage,
			TamperedSizeLoadResult));
	TestEqual(
		TEXT("FName size mismatch reaches the runtime property contract gate"),
		TamperedSizeLoadResult.ErrorCategory,
		FString(TEXT("binding_property_contract_mismatch")));
	TestFalse(TEXT("Rejected FName size descriptor publishes no package"), TamperedSizePackage.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeReflectedPropertyGetTest,
	"AvidScript.Editor.BindingRuntime.ReflectedPropertyGet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeReflectedPropertyGetTest::RunTest(const FString& Parameters)
{
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ TEXT("/Script/Engine.Actor"), TEXT("CustomTimeDilation") }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
		TEXT("Readable Actor property generates a schema v6 package"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
			TEXT("avidscript.engine.property_runtime"),
			{},
			Properties,
			DescriptorJson,
			GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ") + GenerateResult.ErrorMessage);
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
		TEXT("Runtime loads the reflected property package"),
		FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, Package, LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}
	TestEqual(TEXT("Property package exposes its reflected and object-type imports"), Package->GetVmPackage().Imports.Num(), 2);
	TestTrue(
		TEXT("Property runtime package exposes object-type support"),
		Package->GetVmPackage().Imports.ContainsByPredicate(
			[](const FAvidScriptVmDynamicImport& Import)
			{
				return Import.ImportName == TEXT("avid_object_type_is_a");
			}));

	UWorld* World = nullptr;
	if (!TestTrue(
		TEXT("Property runtime integration world is created"),
		CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};

	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!TestNotNull(TEXT("Property runtime integration actor spawns"), Actor))
	{
		return false;
	}
	Actor->CustomTimeDilation = 1.75f;

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	if (!TestTrue(TEXT("Property owner registers in the object registry"), RegisterResult.bSucceeded))
	{
		return false;
	}

	static constexpr uint32 ReturnAddress = 16;
	FAvidScriptBindingRuntimeTestGuestMemory GuestMemory(64);
	const uint64 Arguments[] = { ActorHandle.Slot, ActorHandle.Generation, ReturnAddress };
	FAvidScriptDynamicHostCall Call;
	Call.BindingOrdinal = Package->GetVmPackage().Imports[0].Ordinal;
	Call.Arguments = MakeArrayView(Arguments);
	Call.GuestMemory = &GuestMemory;
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.OwnerHandle = ActorHandle;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	FAvidScriptDynamicHostCallResult DispatchResult;
	TestTrue(
		TEXT("Cached FProperty getter writes CustomTimeDilation into guest memory"),
		Package->Dispatch(Call, Context, Scratch, DispatchResult));
	TestTrue(TEXT("Property dispatch reports success"), DispatchResult.bSucceeded);
	TestEqual(TEXT("Property dispatch returns the host success code"), DispatchResult.ReturnValue, 1);
	TestTrue(
		TEXT("Guest memory receives the reflected float value"),
		FMath::IsNearlyEqual(GuestMemory.ReadValue<float>(ReturnAddress), 1.75f));

	GuestMemory.WriteValue<float>(ReturnAddress, -10.0f);
	const uint64 StaleArguments[] = { ActorHandle.Slot, ActorHandle.Generation + 1, ReturnAddress };
	Call.Arguments = MakeArrayView(StaleArguments);
	TestFalse(
		TEXT("A stale object generation cannot read a reflected property"),
		Package->Dispatch(Call, Context, Scratch, DispatchResult));
	TestEqual(
		TEXT("Rejected handle leaves guest memory untouched"),
		GuestMemory.ReadValue<float>(ReturnAddress),
		-10.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeReflectedPropertySetTest,
	"AvidScript.Editor.BindingRuntime.ReflectedPropertySet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeReflectedPropertySetTest::RunTest(const FString& Parameters)
{
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ TEXT("/Script/Engine.Actor"), TEXT("CustomTimeDilation"), true }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Writable Actor property generates a schema v8 package"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
				TEXT("avidscript.engine.property_write_runtime"),
				{},
				Properties,
				DescriptorJson,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
			TEXT("Writable property descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Descriptor,
				ParseCategory,
				ParseSource)))
	{
		return false;
	}
	const FAvidScriptBindingFunctionModel* Setter =
		Descriptor.Bindings.FindByPredicate(
			[](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.BindingKind == TEXT("property_set");
			});
	if (!TestNotNull(TEXT("Writable descriptor contains a setter"), Setter))
	{
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
			TEXT("Runtime builds an immutable writable property plan"),
			FAvidScriptBindingPackage::LoadDescriptor(
				DescriptorJson,
				Package,
				LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("Writable property runtime world is created"),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};
	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!TestNotNull(TEXT("Writable property actor spawns"), Actor))
	{
		return false;
	}
	Actor->CustomTimeDilation = 1.0f;

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle =
		Registry.RegisterObject(Actor, RegisterResult);
	const uint64 Arguments[] = {
		ActorHandle.Slot,
		ActorHandle.Generation,
		MakeAvidScriptBindingRuntimeF32Cell(2.5f)
	};
	FAvidScriptDynamicHostCall Call;
	Call.BindingOrdinal = Setter->Ordinal;
	Call.Arguments = MakeArrayView(Arguments);
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.OwnerHandle = ActorHandle;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	FAvidScriptDynamicHostCallResult DispatchResult;
	TestFalse(
		TEXT("Read-only host context rejects reflected property writes"),
		Package->Dispatch(Call, Context, Scratch, DispatchResult));
	TestTrue(
		TEXT("Rejected write leaves the Actor property unchanged"),
		FMath::IsNearlyEqual(Actor->CustomTimeDilation, 1.0f));

	Context.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TestTrue(
		TEXT("Writable host context dispatches the cached property setter"),
		Package->Dispatch(Call, Context, Scratch, DispatchResult));
	TestTrue(
		TEXT("Direct reflected property setter mutates the Actor"),
		FMath::IsNearlyEqual(Actor->CustomTimeDilation, 2.5f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeReflectedVectorPropertySetTest,
	"AvidScript.Editor.BindingRuntime.ReflectedVectorPropertySet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeReflectedVectorPropertySetTest::RunTest(const FString& Parameters)
{
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ TEXT("/Script/Engine.MovementComponent"), TEXT("Velocity"), true }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
		TEXT("FVector property setter descriptor generates"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
			TEXT("avidscript.editor.vector_property_set"),
			{},
			Properties,
			DescriptorJson,
			GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ") + GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
		TEXT("FVector property setter descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			Descriptor,
			ParseCategory,
			ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}
	const FAvidScriptBindingFunctionModel* Setter = Descriptor.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.BindingKind == TEXT("property_set");
		});
	if (!TestNotNull(TEXT("FVector property setter resolves"), Setter)
		|| !TestEqual(
			TEXT("FVector property setter uses the packed struct ABI"),
			Setter->HostImport.Signature,
			FString(TEXT("(iifff)i"))))
	{
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
		TEXT("FVector property setter package loads"),
		FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, Package, LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
		TEXT("FVector property setter world is created"),
		CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};
	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	UProjectileMovementComponent* Movement = Actor == nullptr
		? nullptr
		: NewObject<UProjectileMovementComponent>(Actor);
	if (!TestNotNull(TEXT("FVector property setter actor spawns"), Actor)
		|| !TestNotNull(TEXT("Concrete movement component is created"), Movement))
	{
		return false;
	}
	Actor->AddInstanceComponent(Movement);
	Movement->RegisterComponent();

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle MovementHandle = Registry.RegisterObject(
		Movement,
		RegisterResult);
	if (!TestTrue(TEXT("Movement component registers"), RegisterResult.bSucceeded))
	{
		return false;
	}
	const FVector ExpectedVelocity(120.0, -35.0, 8.5);
	const uint64 Arguments[] = {
		MovementHandle.Slot,
		MovementHandle.Generation,
		MakeAvidScriptBindingRuntimeF32Cell(ExpectedVelocity.X),
		MakeAvidScriptBindingRuntimeF32Cell(ExpectedVelocity.Y),
		MakeAvidScriptBindingRuntimeF32Cell(ExpectedVelocity.Z)
	};
	FAvidScriptDynamicHostCall Call;
	Call.BindingOrdinal = Setter->Ordinal;
	Call.Arguments = MakeArrayView(Arguments);
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.OwnerHandle = MovementHandle;
	Context.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	FAvidScriptDynamicHostCallResult DispatchResult;
	TestTrue(
		TEXT("Cached FVector property setter dispatches"),
		Package->Dispatch(Call, Context, Scratch, DispatchResult));
	TestTrue(
		TEXT("Cached FVector property setter mutates the concrete component"),
		Movement->Velocity.Equals(ExpectedVelocity, 0.001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeNetworkRpcTest,
	"AvidScript.Editor.BindingRuntime.NetworkRpcAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeNetworkRpcTest::RunTest(
	const FString& Parameters)
{
	const FString OwnerPath =
		AAvidScriptBindingRuntimeNetworkTestActor::StaticClass()->GetPathName();
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Runtime RPC descriptor generates"),
			FAvidScriptEditorBindingDescriptorGenerator::Generate(
				TEXT("avidscript.editor.network_rpc_runtime"),
				{
					{ OwnerPath, TEXT("ServerSubmitValue") },
					{ OwnerPath, TEXT("ClientApplyValue") },
					{ OwnerPath, TEXT("MulticastAnnounceValue") }
				},
				DescriptorJson,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ")
			+ GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
			TEXT("Runtime RPC descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Descriptor,
				ParseCategory,
				ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}
	const auto FindOrdinal = [&Descriptor](const TCHAR* Name)
	{
		const FAvidScriptBindingFunctionModel* Binding =
			Descriptor.Bindings.FindByPredicate(
				[Name](const FAvidScriptBindingFunctionModel& Candidate)
				{
					return Candidate.UeFunction == Name;
				});
		return Binding == nullptr ? INDEX_NONE : Binding->Ordinal;
	};
	const int32 ServerOrdinal = FindOrdinal(TEXT("ServerSubmitValue"));
	const int32 ClientOrdinal = FindOrdinal(TEXT("ClientApplyValue"));
	const int32 MulticastOrdinal = FindOrdinal(TEXT("MulticastAnnounceValue"));
	if (!TestTrue(
			TEXT("Runtime RPC ordinals are present"),
			ServerOrdinal != INDEX_NONE
				&& ClientOrdinal != INDEX_NONE
				&& MulticastOrdinal != INDEX_NONE))
	{
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
			TEXT("Runtime caches immutable RPC invocation plans"),
			FAvidScriptBindingPackage::LoadDescriptor(
				DescriptorJson,
				Package,
				LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ")
			+ LoadResult.ErrorDetails);
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("RPC integration world is created"),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};
	AAvidScriptBindingRuntimeNetworkTestActor* Actor =
		World->SpawnActor<AAvidScriptBindingRuntimeNetworkTestActor>();
	if (!TestNotNull(TEXT("RPC integration actor spawns"), Actor))
	{
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle =
		Registry.RegisterObject(Actor, RegisterResult);
	if (!TestTrue(TEXT("RPC integration actor registers"), RegisterResult.bSucceeded))
	{
		return false;
	}
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.OwnerHandle = ActorHandle;
	Context.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	const auto Dispatch = [&](const int32 Ordinal, const int32 Value,
		FAvidScriptDynamicHostCallResult& OutResult)
	{
		const uint64 Arguments[] = {
			ActorHandle.Slot,
			ActorHandle.Generation,
			static_cast<uint64>(static_cast<uint32>(Value))
		};
		FAvidScriptDynamicHostCall Call;
		Call.BindingOrdinal = Ordinal;
		Call.Arguments = MakeArrayView(Arguments);
		return Package->Dispatch(Call, Context, Scratch, OutResult);
	};

	FAvidScriptDynamicHostCallResult DispatchResult;
	TestTrue(
		TEXT("Authority executes Server RPC through UE callspace"),
		Dispatch(ServerOrdinal, 11, DispatchResult));
	TestEqual(TEXT("Server implementation receives value"), Actor->LastServerValue, 11);
	TestTrue(
		TEXT("Authority executes Client RPC through UE callspace"),
		Dispatch(ClientOrdinal, 22, DispatchResult));
	TestEqual(TEXT("Client implementation receives value"), Actor->LastClientValue, 22);
	TestTrue(
		TEXT("Authority executes multicast RPC through UE callspace"),
		Dispatch(MulticastOrdinal, 33, DispatchResult));
	TestEqual(
		TEXT("Multicast implementation receives value"),
		Actor->LastMulticastValue,
		33);

	FAvidScriptBindingRuntimeRecordingJournal CandidateJournal(true);
	Context.HostEffectJournal = &CandidateJournal;
	Actor->ProcessEventCallCount = 0;
	TestFalse(
		TEXT("Candidate reload rejects irreversible RPC"),
		Dispatch(ServerOrdinal, 44, DispatchResult));
	TestTrue(
		TEXT("Candidate RPC reports the network-specific category"),
		DispatchResult.Details.Contains(
			TEXT("binding_network_reload_effect_unsupported")));
	TestEqual(
		TEXT("Candidate RPC is rejected before ProcessEvent"),
		Actor->ProcessEventCallCount,
		0);
	Context.HostEffectJournal = nullptr;

	Actor->SetRole(ROLE_SimulatedProxy);
	Actor->ProcessEventCallCount = 0;
	TestFalse(
		TEXT("Non-authority cannot initiate Client RPC"),
		Dispatch(ClientOrdinal, 55, DispatchResult));
	TestTrue(
		TEXT("Client direction failure is explicit"),
		DispatchResult.Details.Contains(
			TEXT("binding_network_authority_denied")));
	TestFalse(
		TEXT("Non-authority cannot initiate multicast RPC"),
		Dispatch(MulticastOrdinal, 66, DispatchResult));
	TestFalse(
		TEXT("Standalone non-authority Server RPC is absorbed"),
		Dispatch(ServerOrdinal, 77, DispatchResult));
	TestTrue(
		TEXT("Absorbed Server RPC has a stable category"),
		DispatchResult.Details.Contains(
			TEXT("binding_network_call_absorbed")));
	TestEqual(
		TEXT("Every denied RPC fails before ProcessEvent"),
		Actor->ProcessEventCallCount,
		0);
	Actor->SetRole(ROLE_Authority);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeReplicatedPropertyTest,
	"AvidScript.Editor.BindingRuntime.ReplicatedPropertyAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeReplicatedPropertyTest::RunTest(
	const FString& Parameters)
{
	const FString OwnerPath =
		AAvidScriptBindingRuntimeNetworkTestActor::StaticClass()->GetPathName();
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ OwnerPath, TEXT("ReplicatedScore"), true },
		{ OwnerPath, TEXT("ReplicatedRoutedValue"), true }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Runtime replicated property descriptor generates"),
			FAvidScriptEditorBindingDescriptorGenerator::
				GenerateWithReadableProperties(
					TEXT("avidscript.editor.replicated_property_runtime"),
					{},
					Properties,
					DescriptorJson,
					GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ")
			+ GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
			TEXT("Runtime replicated property descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Descriptor,
				ParseCategory,
				ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}
	const auto FindSetterOrdinal = [&Descriptor](const TCHAR* Member)
	{
		const FAvidScriptBindingFunctionModel* Binding =
			Descriptor.Bindings.FindByPredicate(
				[Member](const FAvidScriptBindingFunctionModel& Candidate)
				{
					return Candidate.UeMember == Member
						&& Candidate.BindingKind == TEXT("property_set");
				});
		return Binding == nullptr ? INDEX_NONE : Binding->Ordinal;
	};
	const int32 ScoreSetterOrdinal =
		FindSetterOrdinal(TEXT("ReplicatedScore"));
	const int32 RoutedSetterOrdinal =
		FindSetterOrdinal(TEXT("ReplicatedRoutedValue"));
	if (!TestTrue(
			TEXT("Replicated property setter ordinals are present"),
			ScoreSetterOrdinal != INDEX_NONE
				&& RoutedSetterOrdinal != INDEX_NONE))
	{
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
			TEXT("Runtime caches replicated property plans"),
			FAvidScriptBindingPackage::LoadDescriptor(
				DescriptorJson,
				Package,
				LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ")
			+ LoadResult.ErrorDetails);
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("Replicated property runtime world is created"),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};
	AAvidScriptBindingRuntimeNetworkTestActor* Actor =
		World->SpawnActor<AAvidScriptBindingRuntimeNetworkTestActor>();
	if (!TestNotNull(TEXT("Replicated property actor spawns"), Actor))
	{
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle =
		Registry.RegisterObject(Actor, RegisterResult);
	if (!TestTrue(
			TEXT("Replicated property actor registers"),
			RegisterResult.bSucceeded))
	{
		return false;
	}
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.OwnerHandle = ActorHandle;
	Context.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	const auto DispatchSetter = [&](const int32 Ordinal, const int32 Value,
		FAvidScriptDynamicHostCallResult& OutResult)
	{
		const uint64 Arguments[] = {
			ActorHandle.Slot,
			ActorHandle.Generation,
			static_cast<uint64>(static_cast<uint32>(Value))
		};
		FAvidScriptDynamicHostCall Call;
		Call.BindingOrdinal = Ordinal;
		Call.Arguments = MakeArrayView(Arguments);
		return Package->Dispatch(Call, Context, Scratch, OutResult);
	};

	FAvidScriptDynamicHostCallResult DispatchResult;
	TestTrue(
		TEXT("Authority writes direct replicated property"),
		DispatchSetter(ScoreSetterOrdinal, 11, DispatchResult));
	TestEqual(TEXT("Direct replicated value is applied"), Actor->ReplicatedScore, 11);
	TestEqual(
		TEXT("Authority write does not forge RepNotify"),
		Actor->RepNotifyCallCount,
		0);
	TestTrue(
		TEXT("Authority writes replicated BlueprintSetter property"),
		DispatchSetter(RoutedSetterOrdinal, 20, DispatchResult));
	TestEqual(
		TEXT("Replicated BlueprintSetter transformation is preserved"),
		Actor->ReplicatedRoutedValue,
		21);
	TestEqual(
		TEXT("Replicated BlueprintSetter executes exactly once"),
		Actor->ReplicatedSetterCallCount,
		1);

	FAvidScriptBindingRuntimeRecordingJournal CandidateJournal(true);
	Context.HostEffectJournal = &CandidateJournal;
	TestFalse(
		TEXT("Candidate reload rejects replicated writes"),
		DispatchSetter(ScoreSetterOrdinal, 30, DispatchResult));
	TestTrue(
		TEXT("Candidate rejection uses replication category"),
		DispatchResult.Details.Contains(
			TEXT("binding_property_replication_reload_effect_unsupported")));
	TestEqual(
		TEXT("Candidate rejection occurs before mutation"),
		Actor->ReplicatedScore,
		11);
	Context.HostEffectJournal = nullptr;

	Actor->SetRole(ROLE_SimulatedProxy);
	TestFalse(
		TEXT("Non-authority cannot write direct replicated property"),
		DispatchSetter(ScoreSetterOrdinal, 40, DispatchResult));
	TestTrue(
		TEXT("Non-authority rejection uses authority category"),
		DispatchResult.Details.Contains(
			TEXT("binding_property_replication_authority_denied")));
	TestFalse(
		TEXT("Non-authority cannot invoke replicated BlueprintSetter"),
		DispatchSetter(RoutedSetterOrdinal, 50, DispatchResult));
	TestEqual(
		TEXT("Denied writes preserve direct value"),
		Actor->ReplicatedScore,
		11);
	TestEqual(
		TEXT("Denied writes preserve routed value"),
		Actor->ReplicatedRoutedValue,
		21);
	TestEqual(
		TEXT("Denied routed write never enters ProcessEvent"),
		Actor->ReplicatedSetterCallCount,
		1);
	Actor->SetRole(ROLE_Authority);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeBlueprintSetterPropertyTest,
	"AvidScript.Editor.BindingRuntime.BlueprintSetterProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeBlueprintSetterPropertyTest::RunTest(const FString& Parameters)
{
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{
			TEXT("/Script/AvidScriptEditor.AvidScriptBindingRuntimeProcessEventTestActor"),
			TEXT("RoutedValue"),
			true
		}
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("BlueprintSetter property generates a schema v8 package"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
				TEXT("avidscript.editor.blueprint_setter_runtime"),
				{},
				Properties,
				DescriptorJson,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ") + GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
			TEXT("BlueprintSetter descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Descriptor,
				ParseCategory,
				ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}
	const FAvidScriptBindingFunctionModel* Setter = Descriptor.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.BindingKind == TEXT("property_set");
		});
	if (!TestNotNull(TEXT("BlueprintSetter descriptor contains a setter"), Setter))
	{
		return false;
	}
	TestEqual(
		TEXT("BlueprintSetter write policy is explicit"),
		Setter->WritePolicy,
		FString(TEXT("blueprint_setter")));
	TestEqual(
		TEXT("BlueprintSetter dispatch mode is cached"),
		Setter->DispatchMode,
		FString(TEXT("cached_blueprint_setter")));

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
			TEXT("Runtime caches the BlueprintSetter invocation plan"),
			FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, Package, LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("BlueprintSetter runtime world is created"),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};
	AAvidScriptBindingRuntimeProcessEventTestActor* Actor =
		World->SpawnActor<AAvidScriptBindingRuntimeProcessEventTestActor>();
	if (!TestNotNull(TEXT("BlueprintSetter runtime actor spawns"), Actor))
	{
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	if (!TestTrue(TEXT("BlueprintSetter runtime actor registers"), RegisterResult.bSucceeded))
	{
		return false;
	}
	const uint64 Arguments[] = {
		ActorHandle.Slot,
		ActorHandle.Generation,
		MakeAvidScriptBindingRuntimeF32Cell(2.5f)
	};
	FAvidScriptDynamicHostCall Call;
	Call.BindingOrdinal = Setter->Ordinal;
	Call.Arguments = MakeArrayView(Arguments);
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.OwnerHandle = ActorHandle;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	FAvidScriptDynamicHostCallResult DispatchResult;

	Actor->ProcessEventCallCount = 0;
	Actor->BlueprintSetterCallCount = 0;
	TestFalse(
		TEXT("Read-only context rejects BlueprintSetter dispatch"),
		Package->Dispatch(Call, Context, Scratch, DispatchResult));
	TestEqual(TEXT("Rejected BlueprintSetter does not reach ProcessEvent"), Actor->ProcessEventCallCount, 0);
	TestEqual(TEXT("Rejected BlueprintSetter does not call the setter"), Actor->BlueprintSetterCallCount, 0);

	Context.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TestTrue(
		TEXT("Writable context dispatches the cached BlueprintSetter"),
		Package->Dispatch(Call, Context, Scratch, DispatchResult));
	TestEqual(TEXT("BlueprintSetter reaches ProcessEvent once"), Actor->ProcessEventCallCount, 1);
	TestEqual(TEXT("BlueprintSetter native implementation runs once"), Actor->BlueprintSetterCallCount, 1);
	TestTrue(
		TEXT("BlueprintSetter semantics, not raw property copy, determine the stored value"),
		FMath::IsNearlyEqual(Actor->RoutedValue, 3.5f));

	FAvidScriptBindingRuntimeRecordingJournal CandidateJournal(true);
	Context.HostEffectJournal = &CandidateJournal;
	Actor->RoutedValue = 7.0f;
	Actor->ProcessEventCallCount = 0;
	Actor->BlueprintSetterCallCount = 0;
	TestFalse(
		TEXT("Candidate context rejects a BlueprintSetter before ProcessEvent"),
		Package->Dispatch(Call, Context, Scratch, DispatchResult));
	TestEqual(
		TEXT("Rejected BlueprintSetter candidate does not capture a partial property snapshot"),
		CandidateJournal.ReflectedPropertyPrepareCallCount,
		0);
	TestEqual(TEXT("Rejected candidate does not reach ProcessEvent"), Actor->ProcessEventCallCount, 0);
	TestEqual(TEXT("Rejected candidate does not call the BlueprintSetter"), Actor->BlueprintSetterCallCount, 0);
	TestTrue(
		TEXT("Rejected candidate leaves the property unchanged"),
		FMath::IsNearlyEqual(Actor->RoutedValue, 7.0f));
	TestTrue(
		TEXT("Rejected candidate reports the stable unsupported reload category"),
		DispatchResult.Details.Contains(TEXT("binding_reload_effect_unsupported")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeReflectedPropertyReloadTest,
	"AvidScript.Editor.BindingRuntime.ReflectedPropertyCandidateReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeReflectedPropertyReloadTest::RunTest(const FString& Parameters)
{
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ TEXT("/Script/Engine.Actor"), TEXT("CustomTimeDilation"), true }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Reload property descriptor generates"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
				TEXT("avidscript.editor.property_reload"),
				{},
				Properties,
				DescriptorJson,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ") + GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			Descriptor,
			ParseCategory,
			ParseSource))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}
	const FAvidScriptBindingFunctionModel* Setter = Descriptor.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.BindingKind == TEXT("property_set");
		});
	if (!TestNotNull(TEXT("Reload property setter resolves"), Setter))
	{
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
			TEXT("Reload property package loads"),
			FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, Package, LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("Reload property world is created"),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};
	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!TestNotNull(TEXT("Reload property actor spawns"), Actor))
	{
		return false;
	}
	Actor->CustomTimeDilation = 1.0f;

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	if (!TestTrue(TEXT("Reload property actor registers"), RegisterResult.bSucceeded))
	{
		return false;
	}
	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.OwnerHandle = ActorHandle;
	HostContext.World = World;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	const TArray<uint8> InitialModule = BuildAvidScriptPropertyBenchmarkModule(
		Setter->HostImport,
		ActorHandle);
	const TArray<uint8> SuccessfulCandidate = BuildAvidScriptPropertyBenchmarkModule(
		Setter->HostImport,
		ActorHandle,
		true,
		false,
		2.0f);
	const TArray<uint8> TrappingCandidate = BuildAvidScriptPropertyBenchmarkModule(
		Setter->HostImport,
		ActorHandle,
		true,
		true,
		3.0f);

	FAvidScriptRuntimeSession Session;
	Session.SetHostContext(HostContext);
	FAvidScriptWasmReloadResult ReloadResult;
	TestTrue(
		TEXT("Initial property runtime starts"),
		Session.LoadInitialModule(
			InitialModule.GetData(),
			InitialModule.Num(),
			MakeAvidScriptPropertySessionManifest(
				TEXT("property_reload_live"),
				Package,
				Setter->HostImport),
			ReloadResult));
	TestTrue(
		TEXT("Initial runtime leaves the property unchanged"),
		FMath::IsNearlyEqual(Actor->CustomTimeDilation, 1.0f));

	TestTrue(
		TEXT("Successful property candidate applies"),
		Session.ReloadModule(
			SuccessfulCandidate.GetData(),
			SuccessfulCandidate.Num(),
			MakeAvidScriptPropertySessionManifest(
				TEXT("property_reload_committed"),
				Package,
				Setter->HostImport),
			ReloadResult));
	TestTrue(TEXT("Successful candidate opens a host-effect transaction"), ReloadResult.bHostEffectTransactionAttempted);
	TestTrue(TEXT("Successful candidate commits the property transaction"), ReloadResult.bHostEffectTransactionCommitted);
	TestFalse(TEXT("Successful candidate does not roll back"), ReloadResult.bHostEffectRollbackAttempted);
	TestEqual(TEXT("Successful candidate captures the property once"), ReloadResult.HostEffectCapturedObjectCount, 1);
	TestTrue(
		TEXT("Committed candidate property value remains active"),
		FMath::IsNearlyEqual(Actor->CustomTimeDilation, 2.0f));

	TestFalse(
		TEXT("Trapping property candidate is rejected"),
		Session.ReloadModule(
			TrappingCandidate.GetData(),
			TrappingCandidate.Num(),
			MakeAvidScriptPropertySessionManifest(
				TEXT("property_reload_trap"),
				Package,
				Setter->HostImport),
			ReloadResult));
	TestTrue(TEXT("Rejected candidate preserves the previous runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestFalse(TEXT("Rejected candidate does not commit"), ReloadResult.bHostEffectTransactionCommitted);
	TestTrue(TEXT("Rejected candidate attempts property rollback"), ReloadResult.bHostEffectRollbackAttempted);
	TestTrue(TEXT("Rejected candidate property rollback succeeds"), ReloadResult.bHostEffectRollbackSucceeded);
	TestEqual(TEXT("Rejected candidate captures one property snapshot"), ReloadResult.HostEffectCapturedObjectCount, 1);
	TestEqual(TEXT("Rejected candidate restores one property snapshot"), ReloadResult.HostEffectRestoredObjectCount, 1);
	TestTrue(
		TEXT("Rejected candidate restores the committed property value"),
		FMath::IsNearlyEqual(Actor->CustomTimeDilation, 2.0f));
	TestEqual(
		TEXT("Committed runtime remains active after candidate trap"),
		Session.GetSnapshot().ModuleId,
		FString(TEXT("property_reload_committed")));

	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("Previous runtime continues ticking after rollback"), Session.Tick(0.75f, TickResult));
	TestTrue(
		TEXT("Previous runtime setter remains executable after rollback"),
		FMath::IsNearlyEqual(Actor->CustomTimeDilation, 0.75f));
	FAvidScriptWasmSmokeResult StopResult;
	Session.StopAndUnload(StopResult);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeBidirectionalPropertiesSampleTest,
	"AvidScript.Editor.BindingRuntime.BidirectionalPropertiesSample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeBidirectionalPropertiesSampleTest::RunTest(const FString& Parameters)
{
	FString ProfilePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Samples/CSharp/BidirectionalProperties/BidirectionalProperties.csharp-profile.json")));
	FPaths::NormalizeFilename(ProfilePath);
	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	if (!TestTrue(
		TEXT("Bidirectional property sample profile loads"),
		FAvidScriptEditorCSharpProfileService::LoadProfile(
			ProfilePath,
			ProfileResult)))
	{
		AddError(ProfileResult.ErrorCategory + TEXT(": ") + ProfileResult.ErrorMessage);
		return false;
	}

	FString OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest/Tests/P52/BidirectionalProperties")));
	FString SemanticCacheRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P52/BidirectionalProperties/CSharpSemanticCache/v1")));
	FPaths::NormalizeFilename(OutputRoot);
	FPaths::NormalizeFilename(SemanticCacheRoot);
	IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);
	IFileManager::Get().DeleteDirectory(*SemanticCacheRoot, false, true);
	FAvidScriptEditorCSharpBuildRequest BuildRequest =
		FAvidScriptEditorCSharpProfileService::MakeBuildRequest(ProfileResult);
	BuildRequest.Config.OutputRoot = OutputRoot;
	BuildRequest.Config.ReportPath =
		FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
			OutputRoot,
			BuildRequest.Config.ArtifactStem);
	BuildRequest.Config.ManifestPath =
		FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
			OutputRoot,
			BuildRequest.Config.ArtifactStem);
	BuildRequest.Config.SemanticCacheRoot = SemanticCacheRoot;

	FAvidScriptEditorCSharpBuildResult BuildResult;
	if (!TestTrue(
		TEXT("Bidirectional property sample builds through the production profile pipeline"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(
			BuildRequest,
			BuildResult)))
	{
		AddError(BuildResult.ErrorMessage + TEXT("\n") + BuildResult.Stderr);
		return false;
	}
	TestEqual(TEXT("Sample performs bootstrap and final builds"), BuildResult.BuildInvocationCount, 2);
	TestTrue(TEXT("Sample publishes a runtime binding package"), FPaths::FileExists(BuildResult.BindingPackagePath));
	TestTrue(TEXT("Sample publishes a WASM manifest"), FPaths::FileExists(BuildResult.ManifestPath));

	FString RuntimePackageManifestJson;
	TSharedPtr<FJsonObject> RuntimePackageManifestObject;
	const TSharedPtr<FJsonObject>* RuntimePackageFilesObject = nullptr;
	FString RuntimeDescriptorFile;
	if (!TestTrue(
		TEXT("Sample runtime binding package manifest can be read"),
		FFileHelper::LoadFileToString(
			RuntimePackageManifestJson,
			*BuildResult.BindingPackagePath))
		|| !TestTrue(
			TEXT("Sample runtime binding package manifest parses"),
			FJsonSerializer::Deserialize(
				TJsonReaderFactory<>::Create(RuntimePackageManifestJson),
				RuntimePackageManifestObject))
		|| !TestTrue(
			TEXT("Sample runtime binding package manifest exposes files"),
			RuntimePackageManifestObject.IsValid()
				&& RuntimePackageManifestObject->TryGetObjectField(
					TEXT("files"),
					RuntimePackageFilesObject))
		|| RuntimePackageFilesObject == nullptr
		|| !TestTrue(
			TEXT("Sample runtime binding package manifest names its descriptor"),
			(*RuntimePackageFilesObject)->TryGetStringField(
				TEXT("descriptor"),
				RuntimeDescriptorFile)
				&& !RuntimeDescriptorFile.IsEmpty()))
	{
		return false;
	}

	const FString RuntimeDescriptorPath = FPaths::Combine(
		FPaths::GetPath(BuildResult.BindingPackagePath),
		RuntimeDescriptorFile);
	FString RuntimeDescriptorJson;
	FAvidScriptBindingPackageModel RuntimePackageModel;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
		TEXT("Sample runtime binding descriptor can be read"),
		FFileHelper::LoadFileToString(RuntimeDescriptorJson, *RuntimeDescriptorPath))
		|| !TestTrue(
			TEXT("Sample runtime binding descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				RuntimeDescriptorJson,
				RuntimePackageModel,
				ParseCategory,
				ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}
	int32 RuntimeSetterCount = 0;
	int32 RuntimeGetterCount = 0;
	for (const FAvidScriptBindingFunctionModel& Binding : RuntimePackageModel.Bindings)
	{
		if (Binding.UeMember != TEXT("CustomTimeDilation"))
		{
			continue;
		}
		RuntimeSetterCount += Binding.BindingKind == TEXT("property_set") ? 1 : 0;
		RuntimeGetterCount += Binding.BindingKind == TEXT("property_get") ? 1 : 0;
	}
	TestEqual(TEXT("Setter-only source retains the authorized setter"), RuntimeSetterCount, 1);
	TestEqual(TEXT("Setter-only source does not retain the unused getter"), RuntimeGetterCount, 0);

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestResult;
	if (!TestTrue(
		TEXT("Sample manifest and WASM load with their generated runtime slice"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			BuildResult.ManifestPath,
			Manifest,
			Bytecode,
			ManifestResult)))
	{
		AddError(ManifestResult.ErrorCategory + TEXT(": ") + ManifestResult.ErrorMessage);
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
		TEXT("Bidirectional property sample world is created"),
		CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};
	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!TestNotNull(TEXT("Bidirectional property sample actor spawns"), Actor))
	{
		return false;
	}
	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	if (!TestTrue(TEXT("Bidirectional property sample actor registers"), RegisterResult.bSucceeded))
	{
		return false;
	}

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.OwnerHandle = ActorHandle;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	FAvidScriptWasmRuntimeInstance Runtime;
	Runtime.SetHostContext(HostContext);
	FAvidScriptWasmSmokeResult WasmResult;
	if (!TestTrue(
		TEXT("Bidirectional property sample WASM links"),
		Runtime.LoadModule(
			Bytecode.GetData(),
			Bytecode.Num(),
			Manifest.ModuleId,
			Manifest.BindingPackage,
			WasmResult))
		|| !TestTrue(TEXT("Bidirectional property sample BeginPlay executes"), Runtime.BeginPlay(WasmResult)))
	{
		AddError(WasmResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("BeginPlay writes the scalar reflected property"),
		FMath::IsNearlyEqual(Actor->CustomTimeDilation, 1.0f));
	TestTrue(
		TEXT("BeginPlay writes FVector scale through the generated facade"),
		Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.001));
	TestTrue(TEXT("Bidirectional property sample Tick executes"), Runtime.Tick(0.5f, WasmResult));
	TestTrue(
		TEXT("Tick advances the scalar reflected property"),
		FMath::IsNearlyEqual(Actor->CustomTimeDilation, 1.025f));
	TestTrue(
		TEXT("Tick reads and writes FVector scale"),
		Actor->GetActorScale3D().Equals(FVector(1.25, 1.0, 1.0), 0.001));
	TestTrue(TEXT("Bidirectional property sample EndPlay executes"), Runtime.EndPlay(WasmResult));
	TestTrue(
		TEXT("EndPlay restores the scalar reflected property"),
		FMath::IsNearlyEqual(Actor->CustomTimeDilation, 1.0f));
	TestTrue(
		TEXT("EndPlay restores FVector scale"),
		Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.001));
	Runtime.Unload();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeReflectedPropertyBenchmarkTest,
	"AvidScript.Performance.ReflectedPropertySetter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeReflectedPropertyBenchmarkTest::RunTest(const FString& Parameters)
{
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ TEXT("/Script/Engine.Actor"), TEXT("CustomTimeDilation"), true }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Property benchmark descriptor generates"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateWithReadableProperties(
				TEXT("avidscript.performance.property_setter"),
				{},
				Properties,
				DescriptorJson,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ") + GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			Descriptor,
			ParseCategory,
			ParseSource))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}
	const FAvidScriptBindingFunctionModel* Setter = Descriptor.Bindings.FindByPredicate(
		[](const FAvidScriptBindingFunctionModel& Binding)
		{
			return Binding.BindingKind == TEXT("property_set");
		});
	if (!TestNotNull(TEXT("Property benchmark setter resolves"), Setter))
	{
		return false;
	}
	if (!TestEqual(
			TEXT("Property benchmark setter has the expected WASM ABI"),
			Setter->HostImport.Signature,
			FString(TEXT("(iif)i"))))
	{
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
			TEXT("Property benchmark package loads"),
			FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, Package, LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("Property benchmark world is created"),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};
	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!TestNotNull(TEXT("Property benchmark actor spawns"), Actor))
	{
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	if (!TestTrue(TEXT("Property benchmark actor registers"), RegisterResult.bSucceeded))
	{
		return false;
	}
	FAvidScriptBindingInvocationContext InvocationContext;
	InvocationContext.ObjectRegistry = &Registry;
	InvocationContext.OwnerHandle = ActorHandle;
	InvocationContext.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	uint64 Arguments[] = {
		ActorHandle.Slot,
		ActorHandle.Generation,
		MakeAvidScriptBindingRuntimeF32Cell(1.0f)
	};
	FAvidScriptDynamicHostCall Call;
	Call.BindingOrdinal = Setter->Ordinal;
	Call.Arguments = MakeArrayView(Arguments);
	FAvidScriptDynamicHostCallResult DispatchResult;

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.OwnerHandle = ActorHandle;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	FAvidScriptWasmRuntimeInstance Runtime;
	Runtime.SetHostContext(HostContext);
	const TArray<uint8> Bytecode = BuildAvidScriptPropertyBenchmarkModule(
		Setter->HostImport,
		ActorHandle);
	FAvidScriptWasmSmokeResult WasmResult;
	if (!TestTrue(
			TEXT("Property benchmark WASM module loads with the generated package"),
			Runtime.LoadModule(
				Bytecode.GetData(),
				Bytecode.Num(),
				TEXT("phase52_property_setter_benchmark"),
				Package,
				WasmResult))
		|| !TestTrue(TEXT("Property benchmark BeginPlay succeeds"), Runtime.BeginPlay(WasmResult)))
	{
		AddError(WasmResult.ErrorMessage);
		return false;
	}

	static constexpr int32 WarmupCount = 3;
	static constexpr int32 SampleCount = 20;
	static constexpr int32 IterationsPerSample = 512;
	TArray<double> NativeSamples;
	TArray<double> BindingSamples;
	TArray<double> WasmSamples;
	NativeSamples.Reserve(SampleCount);
	BindingSamples.Reserve(SampleCount);
	WasmSamples.Reserve(SampleCount);
	const FAvidScriptBindingPackageInstrumentation WarmInstrumentation =
		Package->GetInstrumentation();
	const int32 HostImportsBeforeTicks = WasmResult.HostImportCallCount;
	double ExpectedChecksum = 0.0;
	double NativeChecksum = 0.0;
	double BindingChecksum = 0.0;
	double WasmChecksum = 0.0;

	for (int32 RunIndex = 0; RunIndex < WarmupCount + SampleCount; ++RunIndex)
	{
		const double NativeStart = FPlatformTime::Seconds();
		for (int32 Iteration = 0; Iteration < IterationsPerSample; ++Iteration)
		{
			SetAvidScriptPropertyBenchmarkNative(
				*Actor,
				1.0f + static_cast<float>((RunIndex + Iteration) & 7) * 0.01f);
		}
		const double NativeMs = (FPlatformTime::Seconds() - NativeStart)
			* 1000.0 / IterationsPerSample;
		NativeChecksum += Actor->CustomTimeDilation;

		const double BindingStart = FPlatformTime::Seconds();
		for (int32 Iteration = 0; Iteration < IterationsPerSample; ++Iteration)
		{
			const float Value = 1.0f
				+ static_cast<float>((RunIndex + Iteration) & 7) * 0.01f;
			Arguments[2] = MakeAvidScriptBindingRuntimeF32Cell(Value);
			if (!Package->Dispatch(Call, InvocationContext, Scratch, DispatchResult))
			{
				AddError(DispatchResult.Details);
				return false;
			}
		}
		const double BindingMs = (FPlatformTime::Seconds() - BindingStart)
			* 1000.0 / IterationsPerSample;
		BindingChecksum += Actor->CustomTimeDilation;

		const double WasmStart = FPlatformTime::Seconds();
		for (int32 Iteration = 0; Iteration < IterationsPerSample; ++Iteration)
		{
			const float Value = 1.0f
				+ static_cast<float>((RunIndex + Iteration) & 7) * 0.01f;
			if (!Runtime.Tick(Value, WasmResult))
			{
				AddError(WasmResult.ErrorMessage);
				return false;
			}
		}
		const double WasmMs = (FPlatformTime::Seconds() - WasmStart)
			* 1000.0 / IterationsPerSample;
		WasmChecksum += Actor->CustomTimeDilation;
		ExpectedChecksum += 1.0
			+ static_cast<double>((RunIndex + IterationsPerSample - 1) & 7) * 0.01;

		if (RunIndex >= WarmupCount)
		{
			NativeSamples.Add(NativeMs);
			BindingSamples.Add(BindingMs);
			WasmSamples.Add(WasmMs);
		}
	}

	const FAvidScriptBindingPackageInstrumentation FinalInstrumentation =
		Package->GetInstrumentation();
	const int32 ExpectedCrossings =
		(WarmupCount + SampleCount) * IterationsPerSample;
	TestEqual(
		TEXT("Warm setter performs no additional class loads"),
		FinalInstrumentation.ClassLoadCount,
		WarmInstrumentation.ClassLoadCount);
	TestEqual(
		TEXT("Warm setter performs no additional reflected-name lookups"),
		FinalInstrumentation.ReflectedNameLookupCount,
		WarmInstrumentation.ReflectedNameLookupCount);
	TestEqual(
		TEXT("Every WAMR setter performs exactly one host crossing"),
		WasmResult.HostImportCallCount - HostImportsBeforeTicks,
		ExpectedCrossings);
	TestEqual(TEXT("Native benchmark records all samples"), NativeSamples.Num(), SampleCount);
	TestEqual(TEXT("Binding benchmark records all samples"), BindingSamples.Num(), SampleCount);
	TestEqual(TEXT("WAMR benchmark records all samples"), WasmSamples.Num(), SampleCount);
	TestTrue(
		TEXT("Native setter checksum proves every sample reached its expected final write"),
		FMath::IsNearlyEqual(NativeChecksum, ExpectedChecksum, 0.0001));
	TestTrue(
		TEXT("Binding setter checksum matches the native write sequence"),
		FMath::IsNearlyEqual(BindingChecksum, ExpectedChecksum, 0.0001));
	TestTrue(
		TEXT("WAMR setter checksum matches the native write sequence"),
		FMath::IsNearlyEqual(WasmChecksum, ExpectedChecksum, 0.0001));

	const double NativeP50 = CalculateAvidScriptPropertyBenchmarkPercentile(NativeSamples, 0.50);
	const double NativeP95 = CalculateAvidScriptPropertyBenchmarkPercentile(NativeSamples, 0.95);
	const double BindingP50 = CalculateAvidScriptPropertyBenchmarkPercentile(BindingSamples, 0.50);
	const double BindingP95 = CalculateAvidScriptPropertyBenchmarkPercentile(BindingSamples, 0.95);
	const double WasmP50 = CalculateAvidScriptPropertyBenchmarkPercentile(WasmSamples, 0.50);
	const double WasmP95 = CalculateAvidScriptPropertyBenchmarkPercentile(WasmSamples, 0.95);
	TestTrue(TEXT("Native setter P50 is sampled"), NativeP50 > 0.0);
	TestTrue(TEXT("Binding setter P50 is sampled"), BindingP50 > 0.0);
	TestTrue(TEXT("WAMR setter P50 is sampled"), WasmP50 > 0.0);
	AddInfo(FString::Printf(
		TEXT("phase52_property_setter_benchmark | samples=%d | iterations=%d | native_p50_ms=%.9f | native_p95_ms=%.9f | binding_p50_ms=%.9f | binding_p95_ms=%.9f | wamr_p50_ms=%.9f | wamr_p95_ms=%.9f | checksum=%.6f | crossings=%d | warm_class_loads=0 | warm_reflected_name_lookups=0 | snapshot_captures=0"),
		SampleCount,
		IterationsPerSample,
		NativeP50,
		NativeP95,
		BindingP50,
		BindingP95,
		WasmP50,
		WasmP95,
		ExpectedChecksum,
		ExpectedCrossings));
	Runtime.Unload();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeReflectedSetActorScaleTest,
	"AvidScript.Editor.BindingRuntime.ReflectedSetActorScaleLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeReflectedSetActorScaleTest::RunTest(const FString& Parameters)
{
	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult PackageResult;
	FString DescriptorJson;
	if (!TestTrue(
		TEXT("Default reflection descriptor resolves into a cached runtime package"),
		GenerateAvidScriptBindingRuntimePackage(Package, PackageResult, DescriptorJson)))
	{
		AddError(PackageResult.ErrorCategory + TEXT(": ") + PackageResult.ErrorDetails);
		return false;
	}
	TestEqual(TEXT("Cached package contains the default eight bindings"), PackageResult.BindingCount, 8);

	TArray<uint8> Bytecode;
	if (!TestTrue(
		TEXT("Generated reflected binding WASM fixture loads"),
		LoadAvidScriptBindingRuntimeFixture(Bytecode)))
	{
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
		TEXT("Binding runtime integration world is created"),
		CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};

	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!TestNotNull(TEXT("Binding runtime integration actor spawns"), Actor))
	{
		return false;
	}
	const FVector InitialScale(1.0, 1.0, 1.0);
	const FVector TargetScale(2.0, 3.0, 4.0);
	Actor->SetActorScale3D(InitialScale);
	USceneComponent* RootComponent = Actor->GetRootComponent();
	if (!TestNotNull(TEXT("Binding runtime integration actor retains its root component"), RootComponent))
	{
		return false;
	}
	TestEqual(TEXT("Binding runtime integration actor has authority"), Actor->GetLocalRole(), ROLE_Authority);

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RootRegisterResult;
	const FAvidScriptObjectHandle RootHandle = Registry.RegisterObject(RootComponent, RootRegisterResult);
	TestTrue(TEXT("Root component reserves the first registry slot"), RootRegisterResult.bSucceeded);
	TestEqual(TEXT("Root component uses slot one"), RootHandle.Slot, 1u);

	FAvidScriptObjectHandleResult ActorRegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, ActorRegisterResult);
	TestTrue(TEXT("Owner actor registers"), ActorRegisterResult.bSucceeded);
	TestEqual(TEXT("Owner actor is deliberately not hardcoded to slot one"), ActorHandle.Slot, 2u);

	const FAvidScriptVmDynamicImport* SetScaleImport = Package->GetVmPackage().Imports.FindByPredicate(
		[](const FAvidScriptVmDynamicImport& Import)
		{
			return Import.ImportName == TEXT("avid_ue_e493dae7c6aae6c7");
		});
	if (!TestNotNull(TEXT("Cached package exposes the reflected SetActorScale3D import"), SetScaleImport))
	{
		return false;
	}
	const uint64 DirectArguments[] = {
		ActorHandle.Slot,
		ActorHandle.Generation,
		MakeAvidScriptBindingRuntimeF32Cell(2.0f),
		MakeAvidScriptBindingRuntimeF32Cell(3.0f),
		MakeAvidScriptBindingRuntimeF32Cell(4.0f)
	};
	FAvidScriptDynamicHostCall DirectCall;
	DirectCall.BindingOrdinal = SetScaleImport->Ordinal;
	DirectCall.Arguments = MakeArrayView(DirectArguments);
	FAvidScriptBindingInvocationContext DirectContext;
	DirectContext.ObjectRegistry = &Registry;
	DirectContext.OwnerHandle = ActorHandle;
	DirectContext.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TArray<uint8> DirectScratch;
	DirectScratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	FAvidScriptDynamicHostCallResult DirectResult;
	TestTrue(
		TEXT("Cached reflected package directly dispatches SetActorScale3D"),
		Package->Dispatch(DirectCall, DirectContext, DirectScratch, DirectResult));
	TestTrue(
		TEXT("Direct cached ProcessEvent applies FVector scale"),
		Actor->GetActorScale3D().Equals(TargetScale, 0.001));
	Actor->SetActorScale3D(InitialScale);

	FAvidScriptBindingRuntimeRecordingJournal RejectingJournal(false);
	DirectContext.HostEffectJournal = &RejectingJournal;
	TestFalse(
		TEXT("Candidate journal rejection prevents reflected SetActorScale3D"),
		Package->Dispatch(DirectCall, DirectContext, DirectScratch, DirectResult));
	TestEqual(TEXT("Candidate journal receives one prepare call"), RejectingJournal.PrepareCallCount, 1);
	TestEqual(TEXT("Candidate journal receives the invocation registry"), RejectingJournal.LastRegistry, &Registry);
	TestEqual(TEXT("Candidate journal receives the Actor handle"), RejectingJournal.LastHandle, ActorHandle);
	TestEqual(TEXT("Candidate journal receives the Actor target"), RejectingJournal.LastTarget, static_cast<UObject*>(Actor));
	TestEqual(
		TEXT("Candidate journal receives the generated Actor transform effect"),
		RejectingJournal.LastEffect,
		EAvidScriptBindingReloadEffect::ActorTransform);
	TestTrue(
		TEXT("Candidate journal rejection preserves Actor scale before ProcessEvent"),
		Actor->GetActorScale3D().Equals(InitialScale, 0.001));
	TestTrue(
		TEXT("Candidate journal failure keeps its stable category"),
		DirectResult.Details.Contains(TEXT("test_host_effect_rejected"), ESearchCase::CaseSensitive));

	FString UnsupportedDescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult UnsupportedGenerateResult;
	TSharedPtr<const FAvidScriptBindingPackage> UnsupportedPackage;
	FAvidScriptBindingPackageLoadResult UnsupportedLoadResult;
	if (!TestTrue(
			TEXT("Unsupported SetVisibility descriptor generates"),
			FAvidScriptEditorBindingDescriptorGenerator::Generate(
				TEXT("avidscript.test.reload_unsupported"),
				{ { TEXT("/Script/Engine.SceneComponent"), TEXT("SetVisibility") } },
				UnsupportedDescriptorJson,
				UnsupportedGenerateResult))
		|| !TestTrue(
			TEXT("Unsupported SetVisibility package loads"),
			FAvidScriptBindingPackage::LoadDescriptor(
				UnsupportedDescriptorJson,
				UnsupportedPackage,
				UnsupportedLoadResult)))
	{
		AddError(UnsupportedGenerateResult.ErrorMessage + TEXT("\n") + UnsupportedLoadResult.ErrorDetails);
		return false;
	}
	const FAvidScriptVmDynamicImport* SetVisibilityImport = UnsupportedPackage->GetVmPackage().Imports.GetData();
	if (!TestNotNull(TEXT("Unsupported package exposes SetVisibility"), SetVisibilityImport))
	{
		return false;
	}
	const uint64 VisibilityArguments[] = {
		RootHandle.Slot,
		RootHandle.Generation,
		0,
		0
	};
	FAvidScriptDynamicHostCall VisibilityCall;
	VisibilityCall.BindingOrdinal = SetVisibilityImport->Ordinal;
	VisibilityCall.Arguments = MakeArrayView(VisibilityArguments);
	TArray<uint8> VisibilityScratch;
	VisibilityScratch.SetNumUninitialized(UnsupportedPackage->GetRequiredScratchSize());
	FAvidScriptDynamicHostCallResult VisibilityResult;
	FAvidScriptBindingRuntimeRecordingJournal PermissiveJournal(true);
	FAvidScriptBindingInvocationContext VisibilityContext = DirectContext;
	VisibilityContext.OwnerHandle = RootHandle;
	VisibilityContext.HostEffectJournal = &PermissiveJournal;
	RootComponent->SetVisibility(true);
	TestFalse(
		TEXT("Candidate rejects an unsupported reflected mutation before ProcessEvent"),
		UnsupportedPackage->Dispatch(
			VisibilityCall,
			VisibilityContext,
			VisibilityScratch,
			VisibilityResult));
	TestTrue(TEXT("Rejected unsupported mutation keeps the component visible"), RootComponent->IsVisible());
	TestTrue(
		TEXT("Unsupported mutation reports a stable category"),
		VisibilityResult.Details.Contains(TEXT("binding_reload_effect_unsupported"), ESearchCase::CaseSensitive));
	VisibilityContext.HostEffectJournal = nullptr;
	TestTrue(
		TEXT("Live context preserves existing SetVisibility behavior"),
		UnsupportedPackage->Dispatch(
			VisibilityCall,
			VisibilityContext,
			VisibilityScratch,
			VisibilityResult));
	TestFalse(TEXT("Live SetVisibility reaches ProcessEvent"), RootComponent->IsVisible());

	FAvidScriptWasmHostContext ReadOnlyContext;
	ReadOnlyContext.ObjectRegistry = &Registry;
	ReadOnlyContext.OwnerHandle = ActorHandle;
	ReadOnlyContext.ActorWritePolicy = EAvidScriptActorWritePolicy::ReadOnly;

	FAvidScriptWasmRuntimeInstance ReadOnlyRuntime;
	ReadOnlyRuntime.SetHostContext(ReadOnlyContext);
	FAvidScriptWasmSmokeResult RuntimeResult;
	TestTrue(
		TEXT("Read-only runtime links the reflected binding package"),
		ReadOnlyRuntime.LoadModule(
			Bytecode.GetData(),
			Bytecode.Num(),
			TEXT("p42_4_reflected_set_actor_scale_read_only"),
			Package,
			RuntimeResult));
	TestFalse(
		TEXT("Read-only runtime rejects the reflected SetActorScale3D call"),
		ReadOnlyRuntime.BeginPlay(RuntimeResult));
	TestTrue(
		TEXT("Denied reflected write keeps the actor scale unchanged"),
		Actor->GetActorScale3D().Equals(InitialScale, 0.001));
	ReadOnlyRuntime.Unload();

	FAvidScriptWasmHostContext WritableContext = ReadOnlyContext;
	WritableContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	FAvidScriptWasmRuntimeInstance WritableRuntime;
	WritableRuntime.SetHostContext(WritableContext);
	TestTrue(
		TEXT("Writable runtime links the same immutable reflected binding package"),
		WritableRuntime.LoadModule(
			Bytecode.GetData(),
			Bytecode.Num(),
			TEXT("p42_4_reflected_set_actor_scale_writable"),
			Package,
			RuntimeResult));
	TestTrue(
		TEXT("BeginPlay executes the generated dynamic reflected import"),
		WritableRuntime.BeginPlay(RuntimeResult));
	const FVector AppliedScale = Actor->GetActorScale3D();
	TestTrue(
		*FString::Printf(
			TEXT("Reflected ProcessEvent applies FVector scale from WASM | actual=(%.6f, %.6f, %.6f)"),
			AppliedScale.X,
			AppliedScale.Y,
			AppliedScale.Z),
		AppliedScale.Equals(TargetScale, 0.001));
	TestEqual(TEXT("Dynamic reflected import reports success"), RuntimeResult.LastHostImportResult, 1);
	TestTrue(
		TEXT("Lifecycle call observed owner imports and the dynamic reflected import"),
		RuntimeResult.HostImportCallCount >= 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeGeneratedCSharpLifecycleTest,
	"AvidScript.Editor.BindingRuntime.GeneratedCSharpLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeGeneratedCSharpLifecycleTest::RunTest(const FString& Parameters)
{
	FString SemanticCacheRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P43_5/GeneratedBindingLifecycle/CSharpSemanticCache/v1")));
	FPaths::NormalizeFilename(SemanticCacheRoot);
	IFileManager::Get().DeleteDirectory(*SemanticCacheRoot, false, true);

	FAvidScriptEditorCSharpBuildResult ColdBuildResult;
	if (!TestTrue(
		TEXT("Cold custom C# lifecycle builds and publishes semantic cache state"),
		BuildAvidScriptGeneratedBindingLifecycle(SemanticCacheRoot, ColdBuildResult)))
	{
		AddError(ColdBuildResult.ErrorMessage + TEXT("\n") + ColdBuildResult.Stderr);
		return false;
	}
	TestEqual(TEXT("Cold lifecycle performs bootstrap and final builds"), ColdBuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Cold lifecycle invokes the C# frontend once"), ColdBuildResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Cold lifecycle invokes C# semantic analysis once"), ColdBuildResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Cold lifecycle invokes Guest IR twice"), ColdBuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Cold lifecycle invokes WASM backend twice"), ColdBuildResult.WasmBackendInvocationCount, 2);
	TestEqual(TEXT("Cold lifecycle records semantic cache miss"), ColdBuildResult.SemanticCacheLookup, FString(TEXT("miss")));
	TestTrue(TEXT("Cold lifecycle publishes semantic cache entry"), ColdBuildResult.bSemanticCachePublished);

	FAvidScriptWasmReloadManifest ColdManifest;
	TArray<uint8> ColdBytecode;
	if (!AcceptAvidScriptGeneratedBindingLifecycleBuild(
			*this,
			TEXT("Cold lifecycle"),
			ColdBuildResult,
			ColdManifest,
			ColdBytecode))
	{
		return false;
	}

	FAvidScriptEditorCSharpBuildResult BuildResult;
	if (!TestTrue(
		TEXT("Warm custom C# lifecycle reuses semantic cache and remains loadable"),
		BuildAvidScriptGeneratedBindingLifecycle(SemanticCacheRoot, BuildResult)))
	{
		AddError(BuildResult.ErrorMessage + TEXT("\n") + BuildResult.Stderr);
		return false;
	}
	TestEqual(TEXT("Generated lifecycle performs bootstrap and final builds"), BuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Warm lifecycle skips the C# frontend"), BuildResult.FrontendInvocationCount, 0);
	TestEqual(TEXT("Warm lifecycle skips C# semantic analysis"), BuildResult.SemanticInvocationCount, 0);
	TestEqual(TEXT("Warm lifecycle still invokes Guest IR twice"), BuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Warm lifecycle still invokes WASM backend twice"), BuildResult.WasmBackendInvocationCount, 2);
	TestEqual(TEXT("Warm lifecycle records semantic cache hit"), BuildResult.SemanticCacheLookup, FString(TEXT("hit")));
	TestFalse(TEXT("Warm lifecycle does not republish semantic cache entry"), BuildResult.bSemanticCachePublished);

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	if (!AcceptAvidScriptGeneratedBindingLifecycleBuild(
			*this,
			TEXT("Warm lifecycle"),
			BuildResult,
			Manifest,
			Bytecode))
	{
		return false;
	}

	FString ManifestJson;
	TSharedPtr<FJsonObject> ManifestObject;
	const TSharedPtr<FJsonObject>* BindingPackageObject = nullptr;
	TestTrue(
		TEXT("Generated C# manifest can be read for tamper validation"),
		FFileHelper::LoadFileToString(ManifestJson, *BuildResult.ManifestPath));
	const TSharedRef<TJsonReader<>> ManifestReader = TJsonReaderFactory<>::Create(ManifestJson);
	TestTrue(
		TEXT("Generated C# manifest parses for tamper validation"),
		FJsonSerializer::Deserialize(ManifestReader, ManifestObject));
	if (!TestTrue(
		TEXT("Generated C# manifest exposes binding package metadata"),
		ManifestObject.IsValid()
			&& ManifestObject->TryGetObjectField(TEXT("binding_package"), BindingPackageObject))
		|| BindingPackageObject == nullptr
		|| !BindingPackageObject->IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("Generated C# manifest records five bindings, object-type support, and packed owner access"),
		static_cast<int32>((*BindingPackageObject)->GetIntegerField(TEXT("profile_import_count"))),
		7);
	TestEqual(
		TEXT("Generated C# manifest records five reflected bindings and packed owner access"),
		static_cast<int32>((*BindingPackageObject)->GetIntegerField(TEXT("used_import_count"))),
		6);
	(*BindingPackageObject)->SetStringField(
		TEXT("descriptor_sha256"),
		TEXT("0000000000000000000000000000000000000000000000000000000000000000"));
	FString TamperedManifestJson;
	const TSharedRef<TJsonWriter<>> ManifestWriter = TJsonWriterFactory<>::Create(&TamperedManifestJson);
	TestTrue(
		TEXT("Tampered C# manifest serializes"),
		FJsonSerializer::Serialize(ManifestObject.ToSharedRef(), ManifestWriter));
	const FString TamperedManifestPath = FPaths::Combine(
		FPaths::GetPath(BuildResult.ManifestPath),
		TEXT("generated_binding_lifecycle.tampered.avidscript.json"));
	TestTrue(
		TEXT("Tampered C# manifest writes"),
		FFileHelper::SaveStringToFile(TamperedManifestJson, *TamperedManifestPath));
	FAvidScriptWasmReloadManifest TamperedManifest;
	TArray<uint8> TamperedBytecode;
	FAvidScriptWasmReloadManifestLoadResult TamperedLoadResult;
	TestFalse(
		TEXT("Runtime transaction rejects a tampered binding descriptor hash"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			TamperedManifestPath,
			TamperedManifest,
			TamperedBytecode,
			TamperedLoadResult));
	TestEqual(
		TEXT("Tampered binding descriptor hash has a stable category"),
		TamperedLoadResult.ErrorCategory,
		FString(TEXT("binding_package_hash_mismatch")));

	FString OriginalPackageJson;
	TSharedPtr<FJsonObject> OriginalPackageObject;
	if (!TestTrue(
			TEXT("Runtime binding package can be read for capability tamper validation"),
			FFileHelper::LoadFileToString(OriginalPackageJson, *BuildResult.BindingPackagePath))
		|| !TestTrue(
			TEXT("Runtime binding package parses for capability tamper validation"),
			FJsonSerializer::Deserialize(
				TJsonReaderFactory<>::Create(OriginalPackageJson),
				OriginalPackageObject))
		|| !TestTrue(
			TEXT("Runtime binding package is an object for capability tamper validation"),
			OriginalPackageObject.IsValid()))
	{
		return false;
	}

	TArray<FString> CapabilityTamperPaths;
	ON_SCOPE_EXIT
	{
		for (const FString& Path : CapabilityTamperPaths)
		{
			IFileManager::Get().Delete(*Path, false, true, true);
		}
	};
	const auto RejectMutatedPackage = [
		this,
		&ManifestJson,
		&BuildResult,
		&CapabilityTamperPaths](
		const TSharedRef<FJsonObject>& PackageObject,
		const TCHAR* Suffix,
		const TCHAR* Label,
		const TCHAR* ExpectedCategory,
		const bool bHidePackedOwnerInScriptManifest)
	{
		const FString PackagePath = FPaths::Combine(
			FPaths::GetPath(BuildResult.BindingPackagePath),
			FString::Printf(TEXT("package.%s.json"), Suffix));
		const FString ScriptManifestPath = FPaths::Combine(
			FPaths::GetPath(BuildResult.ManifestPath),
			FString::Printf(TEXT("generated_binding_lifecycle.%s.avidscript.json"), Suffix));
		CapabilityTamperPaths.Add(PackagePath);
		CapabilityTamperPaths.Add(ScriptManifestPath);

		FString PackageSha256;
		if (!TestTrue(
			*FString::Printf(TEXT("%s package writes"), Label),
			SaveAvidScriptBindingRuntimeJsonWithHash(
				PackageObject,
				PackagePath,
				PackageSha256)))
		{
			return false;
		}

		TSharedPtr<FJsonObject> ScriptManifestObject;
		const TSharedRef<TJsonReader<>> ScriptManifestReader =
			TJsonReaderFactory<>::Create(ManifestJson);
		const TSharedPtr<FJsonObject>* ScriptBindingPackage = nullptr;
		if (!TestTrue(
				*FString::Printf(TEXT("%s script manifest reparses"), Label),
				FJsonSerializer::Deserialize(
					ScriptManifestReader,
					ScriptManifestObject))
			|| !TestTrue(
				*FString::Printf(TEXT("%s script manifest retains package metadata"), Label),
				ScriptManifestObject.IsValid()
					&& ScriptManifestObject->TryGetObjectField(
						TEXT("binding_package"),
						ScriptBindingPackage)
					&& ScriptBindingPackage != nullptr
					&& ScriptBindingPackage->IsValid()))
		{
			return false;
		}
		(*ScriptBindingPackage)->SetStringField(TEXT("manifest_file"), PackagePath);
		(*ScriptBindingPackage)->SetStringField(TEXT("manifest_sha256"), PackageSha256);
		if (bHidePackedOwnerInScriptManifest)
		{
			TArray<TSharedPtr<FJsonValue>> ScriptRequiredImports =
				ScriptManifestObject->GetArrayField(TEXT("required_imports"));
			int32 HiddenOwnerCount = 0;
			for (const TSharedPtr<FJsonValue>& Value : ScriptRequiredImports)
			{
				const TSharedPtr<FJsonObject> Import =
					Value.IsValid() ? Value->AsObject() : nullptr;
				FString ModuleName;
				FString ImportName;
				if (Import.IsValid()
					&& Import->TryGetStringField(TEXT("module"), ModuleName)
					&& Import->TryGetStringField(TEXT("name"), ImportName)
					&& ModuleName == TEXT("avidscript")
					&& ImportName == TEXT("avid_owner_get_handle"))
				{
					Import->SetStringField(TEXT("module"), TEXT("env"));
					Import->SetStringField(TEXT("name"), TEXT("hidden_owner_import"));
					++HiddenOwnerCount;
				}
			}
			TestEqual(
				TEXT("Tampered script manifest hides exactly one real WASM owner import"),
				HiddenOwnerCount,
				1);
			ScriptManifestObject->SetArrayField(
				TEXT("required_imports"),
				MoveTemp(ScriptRequiredImports));
		}

		FString IgnoredScriptManifestSha256;
		if (!TestTrue(
			*FString::Printf(TEXT("%s script manifest writes"), Label),
			SaveAvidScriptBindingRuntimeJsonWithHash(
				ScriptManifestObject.ToSharedRef(),
				ScriptManifestPath,
				IgnoredScriptManifestSha256)))
		{
			return false;
		}

		FAvidScriptWasmReloadManifest RejectedManifest;
		TArray<uint8> RejectedBytecode;
		FAvidScriptWasmReloadManifestLoadResult RejectedLoadResult;
		TestFalse(
			*FString::Printf(TEXT("%s is rejected"), Label),
			FAvidScriptWasmReloadManifestLoader::LoadFromFile(
				ScriptManifestPath,
				RejectedManifest,
				RejectedBytecode,
				RejectedLoadResult));
		TestEqual(
			*FString::Printf(TEXT("%s has a stable category"), Label),
			RejectedLoadResult.ErrorCategory,
			FString(ExpectedCategory));
		return true;
	};

	TSharedPtr<FJsonObject> OwnerOmittedPackage;
	TestTrue(
		TEXT("Owner-omitted package reparses"),
		FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(OriginalPackageJson),
			OwnerOmittedPackage));
	if (OwnerOmittedPackage.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> RequiredImports =
			OwnerOmittedPackage->GetArrayField(TEXT("required_imports"));
		const int32 RemovedOwnerCount = RequiredImports.RemoveAll(
			[](const TSharedPtr<FJsonValue>& Value)
			{
				const TSharedPtr<FJsonObject> Import =
					Value.IsValid() ? Value->AsObject() : nullptr;
				FString ImportName;
				return Import.IsValid()
					&& Import->TryGetStringField(TEXT("name"), ImportName)
					&& ImportName == TEXT("avid_owner_get_handle");
			});
		TestEqual(TEXT("Owner-omitted package removes exactly one capability"), RemovedOwnerCount, 1);
		OwnerOmittedPackage->SetArrayField(TEXT("required_imports"), MoveTemp(RequiredImports));
		RejectMutatedPackage(
			OwnerOmittedPackage.ToSharedRef(),
			TEXT("owner_omitted"),
			TEXT("Package that omits a script-required packed owner capability"),
			TEXT("binding_package_import_mismatch"),
			false);
		RejectMutatedPackage(
			OwnerOmittedPackage.ToSharedRef(),
			TEXT("owner_hidden"),
			TEXT("Script manifest that hides a real WASM packed owner import"),
			TEXT("manifest_wasm_import_mismatch"),
			true);
	}

	TSharedPtr<FJsonObject> FractionalOrdinalPackage;
	TestTrue(
		TEXT("Fractional-ordinal package reparses"),
		FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(OriginalPackageJson),
			FractionalOrdinalPackage));
	if (FractionalOrdinalPackage.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> RequiredImports =
			FractionalOrdinalPackage->GetArrayField(TEXT("required_imports"));
		bool bMutatedDynamicOrdinal = false;
		for (const TSharedPtr<FJsonValue>& Value : RequiredImports)
		{
			const TSharedPtr<FJsonObject> Import =
				Value.IsValid() ? Value->AsObject() : nullptr;
			FString ImportName;
			if (Import.IsValid()
				&& Import->TryGetStringField(TEXT("name"), ImportName)
				&& ImportName.StartsWith(TEXT("avid_ue_"), ESearchCase::CaseSensitive))
			{
				Import->SetNumberField(TEXT("ordinal"), 0.4);
				bMutatedDynamicOrdinal = true;
				break;
			}
		}
		TestTrue(TEXT("Fractional-ordinal package mutates one dynamic import"), bMutatedDynamicOrdinal);
		FractionalOrdinalPackage->SetArrayField(TEXT("required_imports"), MoveTemp(RequiredImports));
		RejectMutatedPackage(
			FractionalOrdinalPackage.ToSharedRef(),
			TEXT("fractional_ordinal"),
			TEXT("Package with a fractional dynamic import ordinal"),
			TEXT("binding_package_invalid"),
			false);
	}

	TSharedPtr<FJsonObject> UnauthorizedImportManifest;
	TestTrue(
		TEXT("Unauthorized-import script manifest reparses"),
		FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(ManifestJson),
			UnauthorizedImportManifest));
	if (UnauthorizedImportManifest.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> RequiredImports =
			UnauthorizedImportManifest->GetArrayField(TEXT("required_imports"));
		FString OriginalDynamicImport;
		const FString UnauthorizedDynamicImport =
			TEXT("avid_ue_ffffffffffffffff");
		for (const TSharedPtr<FJsonValue>& Value : RequiredImports)
		{
			const TSharedPtr<FJsonObject> Import =
				Value.IsValid() ? Value->AsObject() : nullptr;
			FString ModuleName;
			FString ImportName;
			if (Import.IsValid()
				&& Import->TryGetStringField(TEXT("module"), ModuleName)
				&& Import->TryGetStringField(TEXT("name"), ImportName)
				&& ModuleName == TEXT("avidscript")
				&& ImportName.StartsWith(TEXT("avid_ue_"), ESearchCase::CaseSensitive))
			{
				OriginalDynamicImport = ImportName;
				Import->SetStringField(TEXT("name"), UnauthorizedDynamicImport);
				break;
			}
		}
		TestFalse(
			TEXT("Unauthorized-import fixture finds one real dynamic import"),
			OriginalDynamicImport.IsEmpty());
		TestEqual(
			TEXT("Unauthorized dynamic import preserves WASM section width"),
			UnauthorizedDynamicImport.Len(),
			OriginalDynamicImport.Len());
		UnauthorizedImportManifest->SetArrayField(
			TEXT("required_imports"),
			MoveTemp(RequiredImports));

		TArray<uint8> MutatedWasm = Bytecode;
		FTCHARToUTF8 OriginalImportUtf8(*OriginalDynamicImport);
		FTCHARToUTF8 UnauthorizedImportUtf8(*UnauthorizedDynamicImport);
		int32 ReplacedImportCount = 0;
		for (int32 Offset = 0;
			Offset + OriginalImportUtf8.Length() <= MutatedWasm.Num();
			++Offset)
		{
			if (FMemory::Memcmp(
				MutatedWasm.GetData() + Offset,
				OriginalImportUtf8.Get(),
				OriginalImportUtf8.Length()) == 0)
			{
				FMemory::Memcpy(
					MutatedWasm.GetData() + Offset,
					UnauthorizedImportUtf8.Get(),
					UnauthorizedImportUtf8.Length());
				++ReplacedImportCount;
			}
		}
		TestEqual(
			TEXT("Unauthorized-import fixture replaces exactly one WASM import identity"),
			ReplacedImportCount,
			1);

		const FString MutatedWasmPath = FPaths::Combine(
			FPaths::GetPath(BuildResult.ManifestPath),
			TEXT("generated_binding_lifecycle.unauthorized_import.wasm"));
		const FString UnauthorizedManifestPath = FPaths::Combine(
			FPaths::GetPath(BuildResult.ManifestPath),
			TEXT("generated_binding_lifecycle.unauthorized_import.avidscript.json"));
		CapabilityTamperPaths.Add(MutatedWasmPath);
		CapabilityTamperPaths.Add(UnauthorizedManifestPath);
		TestTrue(
			TEXT("Unauthorized-import WASM writes"),
			FFileHelper::SaveArrayToFile(MutatedWasm, *MutatedWasmPath));

		const TSharedPtr<FJsonObject>* WasmObject = nullptr;
		if (TestTrue(
			TEXT("Unauthorized-import manifest retains WASM metadata"),
			UnauthorizedImportManifest->TryGetObjectField(
				TEXT("wasm"),
				WasmObject)
				&& WasmObject != nullptr
				&& WasmObject->IsValid()))
		{
			(*WasmObject)->SetStringField(TEXT("file"), MutatedWasmPath);
			(*WasmObject)->SetStringField(
				TEXT("sha256"),
				FAvidScriptHash::Sha256Hex(MutatedWasm));
		}

		FString IgnoredManifestSha256;
		TestTrue(
			TEXT("Unauthorized-import script manifest writes"),
			SaveAvidScriptBindingRuntimeJsonWithHash(
				UnauthorizedImportManifest.ToSharedRef(),
				UnauthorizedManifestPath,
				IgnoredManifestSha256));

		FAvidScriptWasmReloadManifest RejectedManifest;
		TArray<uint8> RejectedBytecode;
		FAvidScriptWasmReloadManifestLoadResult RejectedLoadResult;
		TestFalse(
			TEXT("Actual WASM dynamic import must be authorized by the current package"),
			FAvidScriptWasmReloadManifestLoader::LoadFromFile(
				UnauthorizedManifestPath,
				RejectedManifest,
				RejectedBytecode,
				RejectedLoadResult));
		TestEqual(
			TEXT("Unauthorized dynamic import has a stable category"),
			RejectedLoadResult.ErrorCategory,
			FString(TEXT("binding_package_import_mismatch")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimePlayablePickupTest,
	"AvidScript.Editor.BindingRuntime.PlayablePickup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimePlayablePickupTest::RunTest(const FString& Parameters)
{
	const FString SourcePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Samples/CSharp/PlayablePickup/PlayablePickupScript.cs")));
	const FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P48_6/PlayablePickup")));
	const FString OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest/PlayablePickup")));
	const FString SemanticCacheRoot = FPaths::Combine(TestRoot, TEXT("CSharpSemanticCache/v1"));
	IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);

	FAvidScriptEditorCSharpBuildResult BuildResult;
	if (!TestTrue(
		TEXT("Playable pickup builds through the EngineGameplay C# profile"),
		BuildAvidScriptPlayablePickup(
			SourcePath,
			OutputRoot,
			SemanticCacheRoot,
			TEXT("csharp_playable_pickup"),
			TEXT("playable_pickup"),
			BuildResult)))
	{
		AddError(
			BuildResult.ErrorMessage
			+ TEXT("\nstdout:\n") + BuildResult.Stdout
			+ TEXT("\nstderr:\n") + BuildResult.Stderr);
		return false;
	}
	TestEqual(TEXT("Playable pickup performs bootstrap and final builds"), BuildResult.BuildInvocationCount, 2);
	TestTrue(TEXT("Playable pickup publishes a manifest"), FPaths::FileExists(BuildResult.ManifestPath));

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!TestTrue(
		TEXT("Playable pickup manifest, WASM, and runtime package load"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			BuildResult.ManifestPath,
			Manifest,
			Bytecode,
			ManifestLoadResult)))
	{
		AddError(ManifestLoadResult.ErrorMessage);
		return false;
	}
	if (!TestTrue(TEXT("Playable pickup owns a reflected runtime package"), Manifest.BindingPackage.IsValid()))
	{
		return false;
	}
	TestEqual(
		TEXT("Playable pickup runtime package contains five reflected bindings and object-type support"),
		Manifest.BindingPackage->GetVmPackage().Imports.Num(),
		6);
	int32 ReflectedImportCount = 0;
	int32 TimerImportCount = 0;
	for (const FAvidScriptWasmRequiredImport& Import : Manifest.RequiredImports)
	{
		if (Import.ModuleName == TEXT("avidscript")
			&& Import.ImportName.StartsWith(TEXT("avid_ue_"), ESearchCase::CaseSensitive))
		{
			++ReflectedImportCount;
		}
		if (Import.ModuleName == TEXT("env") && Import.ImportName == TEXT("timer_set_once"))
		{
			++TimerImportCount;
		}
	}
	TestEqual(TEXT("Playable pickup requires five dynamic UE imports"), ReflectedImportCount, 5);
	TestEqual(TEXT("Playable pickup reaches the timer service once"), TimerImportCount, 1);

	UWorld* World = nullptr;
	if (!TestTrue(
		TEXT("Playable pickup integration world is created"),
		CreateAvidScriptBindingRuntimeIntegrationWorld(World, false)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	World->SetBegunPlay(true);

	AActor* PickupActor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	AActor* NonPlayerActor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	AActor* PlayerActor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!TestNotNull(TEXT("Playable pickup Actor spawns"), PickupActor)
		|| !TestNotNull(TEXT("Non-player overlap Actor spawns"), NonPlayerActor)
		|| !TestNotNull(TEXT("Player overlap Actor spawns"), PlayerActor))
	{
		return false;
	}
	PickupActor->SetActorRotation(FRotator(0.0, 10.0, 0.0));
	PickupActor->SetActorHiddenInGame(false);
	PickupActor->SetActorEnableCollision(true);
	PlayerActor->Tags.Add(TEXT("Player"));

	FAvidScriptObjectRegistry MetricsRegistry;
	FAvidScriptObjectHandleResult PickupRegisterResult;
	FAvidScriptObjectHandleResult PlayerRegisterResult;
	const FAvidScriptObjectHandle PickupHandle = MetricsRegistry.RegisterObject(
		PickupActor,
		PickupRegisterResult);
	const FAvidScriptObjectHandle PlayerHandle = MetricsRegistry.RegisterObject(
		PlayerActor,
		PlayerRegisterResult);
	if (!TestTrue(TEXT("Performance pickup Actor registers"), PickupRegisterResult.bSucceeded)
		|| !TestTrue(TEXT("Performance player Actor registers"), PlayerRegisterResult.bSucceeded))
	{
		return false;
	}
	FAvidScriptWasmHostContext MetricsHostContext;
	MetricsHostContext.ObjectRegistry = &MetricsRegistry;
	MetricsHostContext.OwnerHandle = PickupHandle;
	MetricsHostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	FAvidScriptRuntimeSession MetricsSession;
	MetricsSession.SetHostContext(MetricsHostContext);
	FAvidScriptWasmReloadResult MetricsLoadResult;
	if (!TestTrue(
		TEXT("Performance session loads the playable pickup"),
		MetricsSession.LoadInitialModule(
			Bytecode.GetData(),
			Bytecode.Num(),
			Manifest,
			MetricsLoadResult)))
	{
		AddError(MetricsLoadResult.ErrorMessage);
		return false;
	}
	const int32 HostImportsAfterBeginPlay = MetricsLoadResult.RuntimeResult.HostImportCallCount;
	FAvidScriptWasmSmokeResult MetricsTickResult;
	if (!TestTrue(TEXT("Performance session ticks"), MetricsSession.TickLive(0.5f, MetricsTickResult)))
	{
		AddError(MetricsTickResult.ErrorMessage);
		return false;
	}
	TestEqual(
		TEXT("One playable pickup Tick uses four host crossings with packed owner access"),
		MetricsTickResult.HostImportCallCount - HostImportsAfterBeginPlay,
		4);
	FAvidScriptGameplayEvent MetricsOverlapEvent;
	MetricsOverlapEvent.Type = EAvidScriptGameplayEventType::BeginOverlap;
	MetricsOverlapEvent.ObjectHandle = PlayerHandle;
	MetricsOverlapEvent.VectorValue = FVector3f(PlayerActor->GetActorLocation());
	FAvidScriptWasmSmokeResult MetricsOverlapResult;
	if (!TestTrue(
		TEXT("Performance session dispatches a successful player overlap"),
		MetricsSession.DispatchGameplayEventLive(MetricsOverlapEvent, MetricsOverlapResult)))
	{
		AddError(MetricsOverlapResult.ErrorMessage);
		return false;
	}
	TestEqual(
		TEXT("One successful pickup overlap uses six host crossings with packed owner access"),
		MetricsOverlapResult.HostImportCallCount - MetricsTickResult.HostImportCallCount,
		6);
	MetricsSession.UnloadLive();
	PickupActor->SetActorRotation(FRotator(0.0, 10.0, 0.0));
	PickupActor->SetActorHiddenInGame(false);
	PickupActor->SetActorEnableCollision(true);

	FAvidScriptEditorComponentBindingResult BindingResult;
	if (!TestTrue(
		TEXT("Playable pickup build report binds to the Actor"),
		FAvidScriptEditorComponentBindingService::ApplyCSharpReportToActor(
			BuildResult.ReportPath,
			PickupActor,
			BindingResult)))
	{
		AddError(BindingResult.ErrorMessage);
		return false;
	}
	UAvidScriptComponent* Component = BindingResult.Component;
	if (!TestNotNull(TEXT("Playable pickup binding creates an AvidScript component"), Component))
	{
		return false;
	}
	const FAvidScriptComponentRuntimeStats BeginPlayStats = Component->GetRuntimeStats();
	TestTrue(TEXT("Playable pickup loads the generated WASM runtime"), BeginPlayStats.bRuntimeLoaded);
	TestTrue(TEXT("Playable pickup enters C# BeginPlay"), BeginPlayStats.bBeginPlayCalled);
	TestFalse(TEXT("Initial BeginPlay preserves configured pickup visibility"), PickupActor->IsHidden());
	TestTrue(TEXT("Initial BeginPlay preserves configured pickup collision"), PickupActor->GetActorEnableCollision());

	Component->TickComponent(0.5f, LEVELTICK_All, nullptr);
	TestTrue(
		TEXT("C# Tick rotates the pickup by 45 degrees"),
		FMath::IsNearlyEqual(PickupActor->GetActorRotation().Yaw, 55.0, 0.01));

	PickupActor->OnActorBeginOverlap.Broadcast(PickupActor, NonPlayerActor);
	TestFalse(TEXT("Actor without Player tag cannot collect the pickup"), PickupActor->IsHidden());
	TestTrue(TEXT("Rejected overlap preserves pickup collision"), PickupActor->GetActorEnableCollision());

	PickupActor->OnActorBeginOverlap.Broadcast(PickupActor, PlayerActor);
	TestTrue(TEXT("Player overlap hides the pickup"), PickupActor->IsHidden());
	TestFalse(TEXT("Player overlap disables pickup collision"), PickupActor->GetActorEnableCollision());

	FAvidScriptEditorCSharpBuildResult SuccessfulReloadBuildResult;
	if (!TestTrue(
		TEXT("Collected pickup builds a successful hot-reload candidate"),
		BuildAvidScriptPlayablePickup(
			SourcePath,
			FPaths::Combine(TestRoot, TEXT("SuccessfulReload")),
			FPaths::Combine(TestRoot, TEXT("SuccessfulReloadSemanticCache/v1")),
			TEXT("csharp_playable_pickup"),
			TEXT("playable_pickup_reload"),
			SuccessfulReloadBuildResult)))
	{
		AddError(
			SuccessfulReloadBuildResult.ErrorMessage
			+ TEXT("\nstdout:\n") + SuccessfulReloadBuildResult.Stdout
			+ TEXT("\nstderr:\n") + SuccessfulReloadBuildResult.Stderr);
		return false;
	}
	FAvidScriptEditorComponentBindingResult SuccessfulReloadResult;
	if (!TestTrue(
		TEXT("Collected pickup applies a successful hot reload"),
		FAvidScriptEditorComponentBindingService::ApplyCSharpReportToActor(
			SuccessfulReloadBuildResult.ReportPath,
			PickupActor,
			SuccessfulReloadResult)))
	{
		AddError(SuccessfulReloadResult.ErrorMessage);
		return false;
	}
	TestTrue(TEXT("Collected pickup hot reload attempts a live transaction"), SuccessfulReloadResult.bReloadAttempted);
	TestTrue(TEXT("Collected pickup hot reload commits the candidate"), SuccessfulReloadResult.bReloadApplied);
	TestEqual(TEXT("Collected pickup hot reload reuses the component"), SuccessfulReloadResult.Component, Component);
	TestTrue(TEXT("Collected pickup hot reload attempts state migration"), SuccessfulReloadResult.RuntimeResult.bStateMigrationAttempted);
	TestTrue(TEXT("Collected pickup hot reload applies persisted state"), SuccessfulReloadResult.RuntimeResult.bStateMigrationApplied);
	TestEqual(TEXT("Collected pickup hot reload migrates one state slot"), SuccessfulReloadResult.RuntimeResult.StateMigrationMigratedSlotCount, 1);
	TestEqual(TEXT("Reloaded BeginPlay schedules exactly one timer"), SuccessfulReloadResult.RuntimeResult.RuntimeResult.HostImportCallCount, 1);
	TestEqual(TEXT("Reloaded BeginPlay preserves the timer callback id"), SuccessfulReloadResult.RuntimeResult.RuntimeResult.LastHostImportInput, 7);
	TestTrue(TEXT("Reloaded BeginPlay receives a timer handle"), SuccessfulReloadResult.RuntimeResult.RuntimeResult.LastHostImportResult > 0);
	TestTrue(TEXT("Successful reload preserves collected visibility"), PickupActor->IsHidden());
	TestFalse(TEXT("Successful reload preserves collected collision state"), PickupActor->GetActorEnableCollision());
	TestEqual(TEXT("Component records one successful pickup reload"), Component->GetRuntimeStats().SuccessfulReloadCount, 1);

	Component->TickComponent(3.1f, LEVELTICK_All, nullptr);
	const FAvidScriptComponentRuntimeStats TimerStats = Component->GetRuntimeStats();
	TestFalse(TEXT("Timer callback makes the pickup visible again"), PickupActor->IsHidden());
	TestTrue(TEXT("Timer callback restores pickup collision"), PickupActor->GetActorEnableCollision());
	TestEqual(TEXT("Pickup fires one timer callback"), TimerStats.TimerCallbackCount, 1);
	TestEqual(TEXT("Pickup timer preserves callback id"), TimerStats.LastTimerCallbackId, 7);

	const FString BadSourcePath = FPaths::Combine(TestRoot, TEXT("BadPlayablePickupScript.cs"));
	TestTrue(
		TEXT("Invalid hot-reload source writes"),
		FFileHelper::SaveStringToFile(
			TEXT("namespace AvidScript; public static class BadPlayablePickupScript { this is invalid }"),
			*BadSourcePath));
	FAvidScriptEditorCSharpBuildResult BadBuildResult;
	TestFalse(
		TEXT("Invalid C# hot-reload candidate fails its build"),
		BuildAvidScriptPlayablePickup(
			BadSourcePath,
			FPaths::Combine(TestRoot, TEXT("BadBuild")),
			FPaths::Combine(TestRoot, TEXT("BadSemanticCache/v1")),
			TEXT("csharp_playable_pickup_bad"),
			TEXT("playable_pickup_bad"),
			BadBuildResult));

	const FString InvalidManifestPath = FPaths::Combine(TestRoot, TEXT("playable_pickup_bad.avidscript.json"));
	TestTrue(
		TEXT("Malformed watcher artifact writes"),
		FFileHelper::SaveStringToFile(TEXT("{"), *InvalidManifestPath));
	const FString CommittedManifestPath = Component->GetScriptManifestPath();
	const FString CommittedModuleId = Component->GetRuntimeStats().ModuleId;
	const float YawBeforeRejectedReload = PickupActor->GetActorRotation().Yaw;
	FAvidScriptEditorComponentBindingRequest ReloadRequest;
	ReloadRequest.Actor = PickupActor;
	ReloadRequest.ManifestPath = InvalidManifestPath;
	FAvidScriptEditorComponentBindingResult RejectedReloadResult;
	TestFalse(
		TEXT("Malformed hot-reload manifest is rejected"),
		FAvidScriptEditorComponentBindingService::ApplyManifestToActor(
			ReloadRequest,
			RejectedReloadResult));
	TestEqual(
		TEXT("Rejected pickup reload has the transactional status"),
		RejectedReloadResult.Status,
		EAvidScriptEditorComponentBindingStatus::ReloadRejected);
	TestTrue(
		TEXT("Rejected pickup reload preserves the live runtime"),
		RejectedReloadResult.RuntimeResult.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("Rejected pickup reload restores its manifest path"), Component->GetScriptManifestPath(), CommittedManifestPath);
	TestEqual(TEXT("Rejected pickup reload keeps the active module"), Component->GetRuntimeStats().ModuleId, CommittedModuleId);

	Component->TickComponent(0.25f, LEVELTICK_All, nullptr);
	const float YawAfterRejectedReload = PickupActor->GetActorRotation().Yaw;
	TestTrue(
		TEXT("Old pickup WASM keeps ticking after rejected hot reload"),
		FMath::IsNearlyEqual(
			FMath::FindDeltaAngleDegrees(YawBeforeRejectedReload, YawAfterRejectedReload),
			22.5,
			0.01));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeProjectCSharpGameplayWorkspaceTest,
	"AvidScript.Editor.BindingRuntime.ProjectCSharpGameplayWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeProjectCSharpGameplayWorkspaceTest::RunTest(const FString& Parameters)
{
	FString TestSavedRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P44/GameplayWorkspace")));
	FString GeneratedRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("AvidScript/Tests/P44/GameplayWorkspace/CSharpWorkspace")));
	FString OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest/Tests/P44/GameplayWorkspace")));
	FPaths::NormalizeFilename(TestSavedRoot);
	FPaths::NormalizeFilename(GeneratedRoot);
	FPaths::NormalizeFilename(OutputRoot);
	IFileManager::Get().DeleteDirectory(*TestSavedRoot, false, true);
	IFileManager::Get().DeleteDirectory(*GeneratedRoot, false, true);
	IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);

	FAvidScriptEditorCSharpWorkspaceConfig WorkspaceConfig;
	WorkspaceConfig.WorkspaceRoot = FPaths::Combine(TestSavedRoot, TEXT("Workspace"));
	WorkspaceConfig.GeneratedRoot = GeneratedRoot;
	WorkspaceConfig.BindingPackageRoot = FPaths::Combine(GeneratedRoot, TEXT("BindingPackages"));
	WorkspaceConfig.OutputRoot = OutputRoot;
	FAvidScriptEditorCSharpWorkspaceResult WorkspaceResult;
	if (!TestTrue(
			TEXT("Project C# gameplay workspace is created in isolation"),
			FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(
				WorkspaceConfig,
				WorkspaceResult)))
	{
		AddError(WorkspaceResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Project C# gameplay workspace creates four user files"), WorkspaceResult.CreatedUserFileCount, 4);

	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	if (!TestTrue(
			TEXT("Generated project C# gameplay profile loads"),
			FAvidScriptEditorCSharpProfileService::LoadProfile(
				WorkspaceResult.ProfilePath,
				ProfileResult)))
	{
		AddError(ProfileResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("Profile owns the workspace source"), ProfileResult.BuildConfig.SourcePath, WorkspaceResult.SourcePath);
	TestEqual(TEXT("Profile owns the workspace project"), ProfileResult.BuildConfig.ProjectPath, WorkspaceResult.ProjectPath);
	ProfileResult.BuildConfig.SemanticCacheRoot = FPaths::Combine(
		TestSavedRoot,
		TEXT("CSharpSemanticCache/v1"));

	FAvidScriptEditorCSharpBuildResult ColdBuildResult;
	if (!TestTrue(
			TEXT("Cold project C# gameplay build succeeds"),
			FAvidScriptEditorCSharpBuildService::BuildProfile(
				ProfileResult.BuildConfig,
				ColdBuildResult)))
	{
		AddError(ColdBuildResult.ErrorMessage + TEXT("\n") + ColdBuildResult.Stderr);
		return false;
	}
	TestEqual(TEXT("Cold gameplay build performs two build passes"), ColdBuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Cold gameplay build invokes Frontend once"), ColdBuildResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Cold gameplay build invokes Semantic once"), ColdBuildResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Cold gameplay build invokes Guest IR twice"), ColdBuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Cold gameplay build invokes WASM twice"), ColdBuildResult.WasmBackendInvocationCount, 2);
	TestEqual(TEXT("Cold gameplay build records cache miss"), ColdBuildResult.SemanticCacheLookup, FString(TEXT("miss")));
	if (!AcceptAvidScriptProjectGameplayWorkspaceBuild(
			*this,
			TEXT("Cold gameplay workspace"),
			WorkspaceResult,
			ColdBuildResult))
	{
		return false;
	}

	FAvidScriptEditorCSharpBuildResult WarmBuildResult;
	if (!TestTrue(
			TEXT("Warm project C# gameplay build succeeds"),
			FAvidScriptEditorCSharpBuildService::BuildProfile(
				ProfileResult.BuildConfig,
				WarmBuildResult)))
	{
		AddError(WarmBuildResult.ErrorMessage + TEXT("\n") + WarmBuildResult.Stderr);
		return false;
	}
	TestEqual(TEXT("Warm gameplay build performs two build passes"), WarmBuildResult.BuildInvocationCount, 2);
	TestEqual(TEXT("Warm gameplay build skips Frontend"), WarmBuildResult.FrontendInvocationCount, 0);
	TestEqual(TEXT("Warm gameplay build skips Semantic"), WarmBuildResult.SemanticInvocationCount, 0);
	TestEqual(TEXT("Warm gameplay build still invokes Guest IR twice"), WarmBuildResult.GuestIrInvocationCount, 2);
	TestEqual(TEXT("Warm gameplay build still invokes WASM twice"), WarmBuildResult.WasmBackendInvocationCount, 2);
	TestEqual(TEXT("Warm gameplay build records cache hit"), WarmBuildResult.SemanticCacheLookup, FString(TEXT("hit")));
	return AcceptAvidScriptProjectGameplayWorkspaceBuild(
		*this,
		TEXT("Warm gameplay workspace"),
		WorkspaceResult,
		WarmBuildResult);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptGeneratedCSharpDiagnosticsTest,
	"AvidScript.Editor.BindingRuntime.GeneratedCSharpDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptGeneratedCSharpDiagnosticsTest::RunTest(const FString& Parameters)
{
	const FString SemanticCacheRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptSemanticCache/P45_6_GeneratedDiagnostics")));
	FAvidScriptEditorCSharpBuildResult BuildResult;
	if (!TestTrue(
		TEXT("real C# gameplay script builds with debug artifacts"),
		BuildAvidScriptGeneratedBindingLifecycle(SemanticCacheRoot, BuildResult)))
	{
		AddError(BuildResult.ErrorMessage + TEXT("\n") + BuildResult.Stderr);
		return false;
	}

	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult ManifestLoadResult;
	if (!TestTrue(
		TEXT("real C# manifest validates its WASM, bindings, and debug map"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			BuildResult.ManifestPath,
			Manifest,
			Bytecode,
			ManifestLoadResult)))
	{
		AddError(ManifestLoadResult.ErrorMessage);
		return false;
	}
	TestTrue(TEXT("real C# manifest owns an immutable debug map"), Manifest.DebugMap.IsValid());
	TestTrue(TEXT("real C# debug map artifact exists"), FPaths::FileExists(Manifest.DebugMapFile));

	uint32 HelperFunctionIndex = MAX_uint32;
	FString HelperDisplayName;
	FString DebugSourceFile;
	int32 HelperLine = 0;
	int32 HelperColumn = 0;
	if (!TestTrue(
		TEXT("real C# debug map identifies the shared SetScale helper"),
		LoadAvidScriptBindingRuntimeDebugFunction(
			Manifest.DebugMapFile,
			TEXT("SetScale"),
			HelperFunctionIndex,
			HelperDisplayName,
			DebugSourceFile,
			HelperLine,
			HelperColumn)))
	{
		return false;
	}
	TestTrue(TEXT("debug source identity is project relative"), FPaths::IsRelative(DebugSourceFile));
	TestTrue(
		TEXT("helper index starts after every imported function"),
		HelperFunctionIndex >= static_cast<uint32>(Manifest.RequiredImports.Num()));

	TArray<uint8> TrapBytecode = Bytecode;
	if (!TestTrue(
		TEXT("trap candidate replaces one helper opcode without changing the module index space"),
		PatchAvidScriptBindingRuntimeFunctionToTrap(
			TrapBytecode,
			static_cast<uint32>(Manifest.RequiredImports.Num()),
			HelperFunctionIndex)))
	{
		return false;
	}
	TestEqual(TEXT("trap patch preserves WASM byte size"), TrapBytecode.Num(), Bytecode.Num());

	UWorld* World = nullptr;
	if (!TestTrue(
		TEXT("generated diagnostics integration world is created"),
		CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};

	AActor* Actor = SpawnAvidScriptBindingRuntimeIntegrationActor(*World);
	if (!TestNotNull(TEXT("generated diagnostics Actor spawns"), Actor))
	{
		return false;
	}
	Actor->SetActorScale3D(FVector(1.0, 1.0, 1.0));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	if (!TestTrue(TEXT("generated diagnostics Actor registers"), RegisterResult.bSucceeded))
	{
		return false;
	}

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.OwnerHandle = ActorHandle;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptRuntimeSession Session;
	Session.SetHostContext(HostContext);
	FAvidScriptWasmReloadResult ReloadResult;
	if (!TestTrue(
		TEXT("healthy generated C# runtime enters BeginPlay"),
		Session.LoadInitialModule(Bytecode.GetData(), Bytecode.Num(), Manifest, ReloadResult)))
	{
		AddError(ReloadResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("healthy C# BeginPlay executes the shared helper"),
		Actor->GetActorScale3D().Equals(FVector(2.0, 3.0, 4.0), 0.001));

	FAvidScriptWasmSmokeResult TickResult;
	if (!TestTrue(TEXT("healthy generated C# runtime ticks"), Session.TickLive(0.25f, TickResult)))
	{
		AddError(TickResult.ErrorMessage);
		return false;
	}
	const FVector ScaleBeforeRejectedCandidate = Actor->GetActorScale3D();
	TestTrue(
		TEXT("healthy C# Tick executes the shared helper"),
		ScaleBeforeRejectedCandidate.Equals(FVector(2.25, 3.0, 4.0), 0.001));

	FAvidScriptWasmReloadManifest TrapManifest = Manifest;
	TrapManifest.ModuleId += TEXT("_diagnostic_trap");
	TestFalse(
		TEXT("generated C# helper trap rejects the reload candidate"),
		Session.ReloadModule(
			TrapBytecode.GetData(),
			TrapBytecode.Num(),
			TrapManifest,
			ReloadResult));
	TestEqual(TEXT("candidate reports a VM trap"), ReloadResult.ErrorCategory, FString(TEXT("trap")));
	TestTrue(
		TEXT("rejected helper trap does not alter live Actor state"),
		Actor->GetActorScale3D().Equals(ScaleBeforeRejectedCandidate, 0.001));

	const TArray<FAvidScriptWasmDiagnosticFrame>& DiagnosticFrames =
		ReloadResult.RuntimeResult.DiagnosticFrames;
	if (!TestTrue(TEXT("candidate trap returns diagnostic frames"), !DiagnosticFrames.IsEmpty()))
	{
		return false;
	}
	const FAvidScriptWasmDiagnosticFrame& TopFrame = DiagnosticFrames[0];
	TestTrue(TEXT("top trap frame is source mapped"), TopFrame.bSourceMapped);
	TestEqual(TEXT("top trap frame identifies the C# helper"), TopFrame.FunctionName, HelperDisplayName);
	TestEqual(TEXT("top trap frame preserves project-relative source"), TopFrame.SourceFile, DebugSourceFile);
	TestEqual(TEXT("top trap frame exposes one-based source line"), TopFrame.Line, HelperLine);
	TestEqual(TEXT("top trap frame exposes one-based source column"), TopFrame.Column, HelperColumn);
	TestEqual(TEXT("top trap frame preserves function index"), TopFrame.FunctionIndex, HelperFunctionIndex);

	FAvidScriptEditorCommandLaunchResult LaunchResult;
	LaunchResult.bSucceeded = false;
	LaunchResult.SourcePath = BuildResult.SourcePath;
	LaunchResult.ManifestPath = BuildResult.ManifestPath;
	LaunchResult.Summary = ReloadResult.ErrorMessage;
	LaunchResult.CommandResult.ErrorCategory = ReloadResult.ErrorCategory;
	LaunchResult.CommandResult.ErrorMessage = ReloadResult.ErrorMessage;
	LaunchResult.CommandResult.NextAction = ReloadResult.NextAction;
	LaunchResult.CommandResult.ReloadApplyResult.RuntimeResult = ReloadResult;
	const FAvidScriptEditorCommandPresentation Presentation =
		FAvidScriptEditorResultPresenter::MakePresentation(LaunchResult);
	const FString ExpectedCSharpFrame = FString::Printf(
		TEXT("at %s (%s:%d:%d)"),
		*HelperDisplayName,
		*DebugSourceFile,
		HelperLine,
		HelperColumn);
	TestTrue(
		TEXT("Editor presentation renders mapped C# method and source position"),
		Presentation.Details.Contains(ExpectedCSharpFrame));
	TestTrue(
		TEXT("Editor presentation preserves raw WASM function evidence"),
		Presentation.Details.Contains(FString::Printf(
			TEXT("wasm frame: function=%u offset=0x"),
			HelperFunctionIndex)));

	if (!TestTrue(
		TEXT("old generated C# runtime ticks after candidate rejection"),
		Session.TickLive(0.25f, TickResult)))
	{
		AddError(TickResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("old generated C# gameplay continues after candidate rejection"),
		Actor->GetActorScale3D().Equals(FVector(2.5, 3.0, 4.0), 0.001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeTypedThunkTest,
	"AvidScript.Editor.BindingRuntime.TypedThunk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeTypedThunkTest::RunTest(const FString& Parameters)
{
	const FString TestClassPath =
		TEXT("/Script/AvidScriptBindings.AvidScriptBindingsTestObject");
	const TArray<FAvidScriptReflectedFunctionSelection> Selections = {
		{ TestClassPath, TEXT("FastPathAddInt32") },
		{ TestClassPath, TEXT("FastPathMaxInt32") },
		{ TestClassPath, TEXT("ReflectionFallbackAddFloat") }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
		TEXT("Typed thunk test descriptor generates from three real UFUNCTIONs"),
		FAvidScriptEditorBindingDescriptorGenerator::Generate(
			TEXT("avidscript.test.typed_thunk"),
			Selections,
			DescriptorJson,
			GenerateResult)))
	{
		AddError(GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
		TEXT("Typed thunk test descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			Descriptor,
			ParseCategory,
			ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
		TEXT("Typed thunk test package loads"),
		FAvidScriptBindingPackage::LoadDescriptor(
			DescriptorJson,
			Package,
			LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}

	const FAvidScriptBindingPackageInstrumentation LoadInstrumentation =
		Package->GetInstrumentation();
	TestEqual(
		TEXT("Two different int32 UFUNCTIONs bind typed thunk plans"),
		LoadInstrumentation.TypedThunkPlanCount,
		2ull);
	TestEqual(
		TEXT("Unsupported float shape retains one reflection fallback plan"),
		LoadInstrumentation.ReflectionFallbackPlanCount,
		1ull);

	const auto FindBinding =
		[&Descriptor](const TCHAR* FunctionName)
		{
			return Descriptor.Bindings.FindByPredicate(
				[FunctionName](const FAvidScriptBindingFunctionModel& Binding)
				{
					return Binding.UeFunction == FunctionName;
				});
		};
	const FAvidScriptBindingFunctionModel* AddBinding =
		FindBinding(TEXT("FastPathAddInt32"));
	const FAvidScriptBindingFunctionModel* MaxBinding =
		FindBinding(TEXT("FastPathMaxInt32"));
	const FAvidScriptBindingFunctionModel* FloatBinding =
		FindBinding(TEXT("ReflectionFallbackAddFloat"));
	if (!TestNotNull(TEXT("Add binding is present"), AddBinding)
		|| !TestNotNull(TEXT("Max binding is present"), MaxBinding)
		|| !TestNotNull(TEXT("Float fallback binding is present"), FloatBinding))
	{
		return false;
	}

	EAvidScriptBindingFastPathKind AddFastPath =
		EAvidScriptBindingFastPathKind::None;
	EAvidScriptBindingFastPathKind MaxFastPath =
		EAvidScriptBindingFastPathKind::None;
	EAvidScriptBindingFastPathKind FloatFastPath =
		EAvidScriptBindingFastPathKind::ScalarI32PairToI32;
	TestTrue(
		TEXT("Add ordinal exposes fast path diagnostics"),
		Package->TryGetFastPathKind(AddBinding->Ordinal, AddFastPath));
	TestTrue(
		TEXT("Max ordinal exposes fast path diagnostics"),
		Package->TryGetFastPathKind(MaxBinding->Ordinal, MaxFastPath));
	TestTrue(
		TEXT("Float ordinal exposes fallback diagnostics"),
		Package->TryGetFastPathKind(FloatBinding->Ordinal, FloatFastPath));
	TestEqual(
		TEXT("Different int32 functions share the same shape thunk"),
		AddFastPath,
		MaxFastPath);
	TestEqual(
		TEXT("Int32 pair uses the scalar pair thunk"),
		AddFastPath,
		EAvidScriptBindingFastPathKind::ScalarI32PairToI32);
	TestEqual(
		TEXT("Float shape remains on reflection fallback"),
		FloatFastPath,
		EAvidScriptBindingFastPathKind::None);

	UClass* TestClass = LoadObject<UClass>(nullptr, *TestClassPath);
	if (!TestNotNull(TEXT("Typed thunk test class loads"), TestClass))
	{
		return false;
	}
	UObject* Target = NewObject<UObject>(GetTransientPackage(), TestClass);
	if (!TestNotNull(TEXT("Typed thunk test target is created"), Target))
	{
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle Handle =
		Registry.RegisterObject(Target, RegisterResult);
	if (!TestTrue(TEXT("Typed thunk target registers"), RegisterResult.bSucceeded))
	{
		return false;
	}

	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.OwnerHandle = Handle;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	FAvidScriptBindingRuntimeTestGuestMemory GuestMemory(128);
	const auto Dispatch =
		[&](const uint32 Ordinal, TConstArrayView<uint64> Arguments)
		{
			FAvidScriptDynamicHostCall Call;
			Call.BindingOrdinal = Ordinal;
			Call.Arguments = Arguments;
			Call.GuestMemory = &GuestMemory;
			FAvidScriptDynamicHostCallResult Result;
			return Package->Dispatch(
				Call,
				Context,
				Scratch,
				Result);
		};

	constexpr uint32 AddResultAddress = 64;
	const uint64 AddArguments[] = {
		Handle.Slot,
		Handle.Generation,
		19,
		23,
		AddResultAddress
	};
	TestTrue(
		TEXT("Add executes through the typed thunk"),
		Dispatch(AddBinding->Ordinal, MakeArrayView(AddArguments)));
	TestEqual(
		TEXT("Add typed thunk returns the expected value"),
		GuestMemory.ReadValue<int32>(AddResultAddress),
		42);

	constexpr uint32 MaxResultAddress = 68;
	const uint64 MaxArguments[] = {
		Handle.Slot,
		Handle.Generation,
		17,
		29,
		MaxResultAddress
	};
	TestTrue(
		TEXT("Max executes through the shared typed thunk"),
		Dispatch(MaxBinding->Ordinal, MakeArrayView(MaxArguments)));
	TestEqual(
		TEXT("Max typed thunk returns the expected value"),
		GuestMemory.ReadValue<int32>(MaxResultAddress),
		29);

	constexpr uint32 FloatResultAddress = 72;
	const uint64 FloatArguments[] = {
		Handle.Slot,
		Handle.Generation,
		MakeAvidScriptBindingRuntimeF32Cell(1.25f),
		MakeAvidScriptBindingRuntimeF32Cell(2.5f),
		FloatResultAddress
	};
	TestTrue(
		TEXT("Unsupported float shape executes through reflection fallback"),
		Dispatch(FloatBinding->Ordinal, MakeArrayView(FloatArguments)));
	TestEqual(
		TEXT("Reflection fallback preserves float semantics"),
		GuestMemory.ReadValue<float>(FloatResultAddress),
		3.75f);

	constexpr uint32 StaleResultAddress = 76;
	const int32 StaleSentinel = 0x12345678;
	GuestMemory.WriteValue(StaleResultAddress, StaleSentinel);
	const uint64 StaleArguments[] = {
		Handle.Slot,
		Handle.Generation + 1,
		10,
		20,
		StaleResultAddress
	};
	TestFalse(
		TEXT("Stale generation is rejected before typed thunk execution"),
		Dispatch(AddBinding->Ordinal, MakeArrayView(StaleArguments)));
	TestEqual(
		TEXT("Rejected stale generation does not write Guest Memory"),
		GuestMemory.ReadValue<int32>(StaleResultAddress),
		StaleSentinel);

	const FAvidScriptBindingPackageInstrumentation WarmInstrumentation =
		Package->GetInstrumentation();
	TestEqual(
		TEXT("Warm typed thunk dispatch performs no class loads"),
		WarmInstrumentation.ClassLoadCount,
		LoadInstrumentation.ClassLoadCount);
	TestEqual(
		TEXT("Warm typed thunk dispatch performs no reflected name lookups"),
		WarmInstrumentation.ReflectedNameLookupCount,
		LoadInstrumentation.ReflectedNameLookupCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingRuntimeQualifiedNativeDirectTest,
	"AvidScript.Editor.BindingRuntime.QualifiedNativeDirect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingRuntimeQualifiedNativeDirectTest::RunTest(
	const FString& Parameters)
{
	const FString TestClassPath =
		TEXT("/Script/AvidScriptBindings.AvidScriptBindingsTestObject");
	const TArray<FAvidScriptReflectedFunctionSelection> Selections = {
		{ TestClassPath, TEXT("FastPathAddInt32") },
		{ TestClassPath, TEXT("FastPathMaxInt32") },
		{ TestClassPath, TEXT("ReflectionFallbackAddFloat") }
	};
	const auto ParseDescriptor = [this](
		const FString& Label,
		const FString& DescriptorJson,
		FAvidScriptBindingPackageModel& OutDescriptor)
	{
		FString ErrorCategory;
		FString ErrorSource;
		if (!TestTrue(
				*FString::Printf(TEXT("%s descriptor parses"), *Label),
				FAvidScriptBindingDescriptorParser::Parse(
					DescriptorJson,
					OutDescriptor,
					ErrorCategory,
					ErrorSource)))
		{
			AddError(ErrorCategory + TEXT(": ") + ErrorSource);
			return false;
		}
		return true;
	};
	const auto LoadPackage = [this](
		const FString& Label,
		const FString& DescriptorJson,
		TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
		FAvidScriptBindingPackageLoadResult& OutLoadResult)
	{
		if (!TestTrue(
				*FString::Printf(TEXT("%s package loads"), *Label),
				FAvidScriptBindingPackage::LoadDescriptor(
					DescriptorJson,
					OutPackage,
					OutLoadResult)))
		{
			AddError(
				OutLoadResult.ErrorCategory
				+ TEXT(": ")
				+ OutLoadResult.ErrorDetails);
			return false;
		}
		return true;
	};

	FString LegacyDescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult LegacyGenerateResult;
	if (!TestTrue(
			TEXT("Legacy descriptor generates without native-direct authorization"),
			FAvidScriptEditorBindingDescriptorGenerator::Generate(
				TEXT("avidscript.test.qualified_native_direct.legacy"),
				Selections,
				LegacyDescriptorJson,
				LegacyGenerateResult)))
	{
		AddError(LegacyGenerateResult.ErrorMessage);
		return false;
	}
	FAvidScriptBindingPackageModel LegacyDescriptor;
	if (!ParseDescriptor(
			TEXT("Legacy qualified-native-direct control"),
			LegacyDescriptorJson,
			LegacyDescriptor))
	{
		return false;
	}
	TSharedPtr<const FAvidScriptBindingPackage> LegacyPackage;
	FAvidScriptBindingPackageLoadResult LegacyLoadResult;
	if (!LoadPackage(
			TEXT("Legacy qualified-native-direct control"),
			LegacyDescriptorJson,
			LegacyPackage,
			LegacyLoadResult))
	{
		return false;
	}
	const FAvidScriptBindingPackageInstrumentation LegacyInstrumentation =
		LegacyPackage->GetInstrumentation();
	TestEqual(
		TEXT("Legacy package has no qualified native-direct plans"),
		LegacyInstrumentation.QualifiedNativeDirectPlanCount,
		0ull);
	TestEqual(
		TEXT("Legacy package keeps every dynamic plan semantic"),
		LegacyInstrumentation.SemanticOnlyPlanCount,
		static_cast<uint64>(LegacyLoadResult.BindingCount));
	TestEqual(
		TEXT("Legacy mode counters partition every dynamic plan"),
		LegacyInstrumentation.QualifiedNativeDirectPlanCount
			+ LegacyInstrumentation.SemanticOnlyPlanCount,
		static_cast<uint64>(LegacyLoadResult.BindingCount));
	for (const FAvidScriptBindingFunctionModel& Binding : LegacyDescriptor.Bindings)
	{
		EAvidScriptBindingInvocationMode Mode =
			EAvidScriptBindingInvocationMode::QualifiedNativeDirect;
		TestTrue(
			*FString::Printf(
				TEXT("Legacy ordinal %d exposes an invocation mode"),
				Binding.Ordinal),
			LegacyPackage->TryGetInvocationMode(Binding.Ordinal, Mode));
		TestEqual(
			*FString::Printf(
				TEXT("Legacy ordinal %d remains semantic"),
				Binding.Ordinal),
			Mode,
			EAvidScriptBindingInvocationMode::SemanticProcessEvent);
	}

	FAvidScriptBindingSelectionProfile DirectProfile;
	DirectProfile.PackageName =
		TEXT("avidscript.test.qualified_native_direct");
	FAvidScriptReflectedClassSelection DirectClass;
	DirectClass.OwnerClassPath = TestClassPath;
	DirectClass.IncludeFunctions = {
		TEXT("FastPathAddInt32"),
		TEXT("FastPathMaxInt32"),
		TEXT("ReflectionFallbackAddFloat")
	};
	DirectClass.NativeDirectFunctions = DirectClass.IncludeFunctions;
	DirectProfile.Classes.Add(MoveTemp(DirectClass));

	FString DirectDescriptorJson;
	FAvidScriptBindingSelectionResolveResult DirectSelectionResult;
	FAvidScriptBindingDescriptorGenerateResult DirectGenerateResult;
	if (!TestTrue(
			TEXT("Qualified descriptor generates with explicit authorization"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
				DirectProfile,
				DirectDescriptorJson,
				DirectSelectionResult,
				DirectGenerateResult)))
	{
		AddError(DirectGenerateResult.ErrorMessage);
		return false;
	}
	FAvidScriptBindingPackageModel DirectDescriptor;
	if (!ParseDescriptor(
			TEXT("Qualified native-direct"),
			DirectDescriptorJson,
			DirectDescriptor))
	{
		return false;
	}
	TSharedPtr<const FAvidScriptBindingPackage> DirectPackage;
	FAvidScriptBindingPackageLoadResult DirectLoadResult;
	if (!LoadPackage(
			TEXT("Qualified native-direct"),
			DirectDescriptorJson,
			DirectPackage,
			DirectLoadResult))
	{
		return false;
	}

	const auto FindBinding =
		[&DirectDescriptor](const TCHAR* FunctionName)
		{
			return DirectDescriptor.Bindings.FindByPredicate(
				[FunctionName](const FAvidScriptBindingFunctionModel& Binding)
				{
					return Binding.UeFunction == FunctionName;
				});
		};
	const FAvidScriptBindingFunctionModel* AddBinding =
		FindBinding(TEXT("FastPathAddInt32"));
	const FAvidScriptBindingFunctionModel* MaxBinding =
		FindBinding(TEXT("FastPathMaxInt32"));
	const FAvidScriptBindingFunctionModel* FloatBinding =
		FindBinding(TEXT("ReflectionFallbackAddFloat"));
	if (!TestNotNull(TEXT("Qualified Add binding is present"), AddBinding)
		|| !TestNotNull(TEXT("Qualified Max binding is present"), MaxBinding)
		|| !TestNotNull(TEXT("Authorized float fallback binding is present"), FloatBinding))
	{
		return false;
	}

	const FAvidScriptBindingPackageInstrumentation DirectInstrumentation =
		DirectPackage->GetInstrumentation();
	TestEqual(
		TEXT("Two authorized int32 functions qualify for native-direct"),
		DirectInstrumentation.QualifiedNativeDirectPlanCount,
		2ull);
	TestEqual(
		TEXT("Authorized unsupported float shape remains semantic-only"),
		DirectInstrumentation.SemanticOnlyPlanCount,
		1ull);
	TestEqual(
		TEXT("Qualified mode counters partition every dynamic plan"),
		DirectInstrumentation.QualifiedNativeDirectPlanCount
			+ DirectInstrumentation.SemanticOnlyPlanCount,
		static_cast<uint64>(DirectLoadResult.BindingCount));
	TestEqual(
		TEXT("Two int32 plans qualify for adaptive prepared reflection"),
		DirectInstrumentation.AdaptivePreparedNativePlanCount,
		2ull);
	TestEqual(
		TEXT("Unsupported float remains an adaptive strict fallback"),
		DirectInstrumentation.AdaptiveStrictFallbackPlanCount,
		1ull);

	EAvidScriptBindingInvocationMode AddMode =
		EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	EAvidScriptBindingInvocationMode MaxMode =
		EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	EAvidScriptBindingInvocationMode FloatMode =
		EAvidScriptBindingInvocationMode::QualifiedNativeDirect;
	TestTrue(
		TEXT("Qualified Add ordinal exposes its invocation mode"),
		DirectPackage->TryGetInvocationMode(AddBinding->Ordinal, AddMode));
	TestTrue(
		TEXT("Qualified Max ordinal exposes its invocation mode"),
		DirectPackage->TryGetInvocationMode(MaxBinding->Ordinal, MaxMode));
	TestTrue(
		TEXT("Authorized float ordinal exposes its fallback mode"),
		DirectPackage->TryGetInvocationMode(FloatBinding->Ordinal, FloatMode));
	TestEqual(
		TEXT("Add qualifies for native-direct"),
		AddMode,
		EAvidScriptBindingInvocationMode::QualifiedNativeDirect);
	TestEqual(
		TEXT("Max qualifies for native-direct"),
		MaxMode,
		EAvidScriptBindingInvocationMode::QualifiedNativeDirect);
	TestEqual(
		TEXT("Unsupported float shape falls back to semantic ProcessEvent"),
		FloatMode,
		EAvidScriptBindingInvocationMode::SemanticProcessEvent);
	EAvidScriptBindingInvocationMode AdaptiveAddMode =
		EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	EAvidScriptBindingInvocationMode AdaptiveFloatMode =
		EAvidScriptBindingInvocationMode::AdaptivePreparedNative;
	TestTrue(
		TEXT("Adaptive Add ordinal exposes its policy-specific mode"),
		DirectPackage->TryGetInvocationMode(
			AddBinding->Ordinal,
			EAvidScriptBindingInvocationPolicy::AdaptiveSemantic,
			AdaptiveAddMode));
	TestTrue(
		TEXT("Adaptive float ordinal exposes its strict fallback mode"),
		DirectPackage->TryGetInvocationMode(
			FloatBinding->Ordinal,
			EAvidScriptBindingInvocationPolicy::AdaptiveSemantic,
			AdaptiveFloatMode));
	TestEqual(
		TEXT("Adaptive Add uses prepared native reflection"),
		AdaptiveAddMode,
		EAvidScriptBindingInvocationMode::AdaptivePreparedNative);
	TestEqual(
		TEXT("Adaptive unsupported float remains ProcessEvent"),
		AdaptiveFloatMode,
		EAvidScriptBindingInvocationMode::SemanticProcessEvent);

	EAvidScriptBindingInvocationMode InvalidMode =
		EAvidScriptBindingInvocationMode::QualifiedNativeDirect;
	TestFalse(
		TEXT("Invalid ordinal has no invocation mode"),
		DirectPackage->TryGetInvocationMode(MAX_uint32, InvalidMode));
	TestEqual(
		TEXT("Invalid ordinal resets invocation mode to semantic"),
		InvalidMode,
		EAvidScriptBindingInvocationMode::SemanticProcessEvent);

	FAvidScriptBindingInvocationContext DefaultInvocationContext;
	TestEqual(
		TEXT("Default binding invocation policy remains semantic"),
		DefaultInvocationContext.InvocationPolicy,
		EAvidScriptBindingInvocationPolicy::SemanticProcessEvent);
	FAvidScriptWasmHostContext DefaultHostContext;
	TestEqual(
		TEXT("Default WASM host context requests semantic ProcessEvent"),
		DefaultHostContext.BindingInvocationPolicy,
		EAvidScriptBindingInvocationPolicy::SemanticProcessEvent);
	FAvidScriptRuntimeSession DefaultPolicySession;
	DefaultPolicySession.SetHostContext(DefaultHostContext);
	TestEqual(
		TEXT("Session SetHostContext preserves the default semantic policy"),
		DefaultPolicySession.GetTestSnapshot().HostContext.BindingInvocationPolicy,
		EAvidScriptBindingInvocationPolicy::SemanticProcessEvent);

	UClass* TestClass = LoadObject<UClass>(nullptr, *TestClassPath);
	if (!TestNotNull(TEXT("Qualified native-direct test class loads"), TestClass))
	{
		return false;
	}
	uint32 ResolvedAddOrdinal = MAX_uint32;
	TestTrue(
		TEXT("Function identity resolves to one immutable package ordinal"),
		DirectPackage->TryFindFunctionOrdinal(
			*TestClass,
			TEXT("FastPathAddInt32"),
			ResolvedAddOrdinal));
	TestEqual(
		TEXT("Resolved function ordinal matches the generated binding"),
		ResolvedAddOrdinal,
		AddBinding->Ordinal);
	UObject* Target = NewObject<UObject>(GetTransientPackage(), TestClass);
	if (!TestNotNull(TEXT("Qualified native-direct target is created"), Target))
	{
		return false;
	}
	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle Handle =
		Registry.RegisterObject(Target, RegisterResult);
	if (!TestTrue(
			TEXT("Qualified native-direct target registers"),
			RegisterResult.bSucceeded))
	{
		return false;
	}

	FAvidScriptBindingInvocationContext SemanticContext;
	SemanticContext.ObjectRegistry = &Registry;
	SemanticContext.OwnerHandle = Handle;
	FAvidScriptBindingInvocationInstrumentation InvocationInstrumentation;
	SemanticContext.InvocationInstrumentation = &InvocationInstrumentation;
	FAvidScriptBindingInvocationContext DirectContext = SemanticContext;
	DirectContext.InvocationPolicy =
		EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect;
	FAvidScriptBindingInvocationContext AdaptiveContext =
		SemanticContext;
	AdaptiveContext.InvocationPolicy =
		EAvidScriptBindingInvocationPolicy::AdaptiveSemantic;
	TArray<FAvidScriptPreparedReflectionBinding>
		PreparedReflectionBindings;
	FString PreparedReflectionError;
	TestTrue(
		TEXT("Package publishes prepared reflection bindings"),
		DirectPackage->BuildPreparedReflectionBindings(
			PreparedReflectionBindings,
			PreparedReflectionError));
	TestEqual(
		TEXT("Package publishes both int32 pair call-sites"),
		PreparedReflectionBindings.Num(),
		2);
	const FAvidScriptPreparedReflectionBinding* PreparedAdd =
		PreparedReflectionBindings.FindByPredicate(
			[AddBinding](
				const FAvidScriptPreparedReflectionBinding& Binding)
			{
				return Binding.BindingOrdinal
					== AddBinding->Ordinal;
			});
	if (!TestNotNull(
			TEXT("Prepared Add call-site is present"),
			PreparedAdd))
	{
		return false;
	}
	TestNotNull(
		TEXT("Prepared Add carries immutable plan identity"),
		PreparedAdd->ImmutablePlanIdentity);
	FAvidScriptPreparedReflectionBinding ForgedPreparedAdd =
		*PreparedAdd;
	ForgedPreparedAdd.ImmutablePlanIdentity = nullptr;
	int32 ForgedPreparedValue = 0;
	FString ForgedPreparedErrorCategory;
	FString ForgedPreparedErrorDetails;
	TestFalse(
		TEXT("Prepared Add rejects a forged immutable plan identity"),
		DirectPackage->InvokePreparedReflectionI32Pair(
			ForgedPreparedAdd,
			*Target,
			19,
			23,
			AdaptiveContext,
			ForgedPreparedValue,
			ForgedPreparedErrorCategory,
			ForgedPreparedErrorDetails));
	TestEqual(
		TEXT("Forged prepared identity reports stable category"),
		ForgedPreparedErrorCategory,
		FString(TEXT("binding_prepared_identity_mismatch")));
	int32 PreparedAdaptiveValue = 0;
	FString PreparedErrorCategory;
	FString PreparedErrorDetails;
	TestTrue(
		TEXT("Prepared Add invokes adaptive native reflection"),
		DirectPackage->InvokePreparedReflectionI32Pair(
			*PreparedAdd,
			*Target,
			19,
			23,
			AdaptiveContext,
			PreparedAdaptiveValue,
			PreparedErrorCategory,
			PreparedErrorDetails));
	TestEqual(
		TEXT("Prepared adaptive Add returns 42"),
		PreparedAdaptiveValue,
		42);
	int32 PreparedSemanticValue = 0;
	TestTrue(
		TEXT("Prepared Add preserves strict ProcessEvent policy"),
		DirectPackage->InvokePreparedReflectionI32Pair(
			*PreparedAdd,
			*Target,
			19,
			23,
			SemanticContext,
			PreparedSemanticValue,
			PreparedErrorCategory,
			PreparedErrorDetails));
	TestEqual(
		TEXT("Prepared strict Add returns 42"),
		PreparedSemanticValue,
		42);
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(DirectPackage->GetRequiredScratchSize());
	FAvidScriptBindingRuntimeTestGuestMemory GuestMemory(128);
	const auto Dispatch =
		[&](const uint32 Ordinal,
			const TConstArrayView<uint64> Arguments,
			const FAvidScriptBindingInvocationContext& Context,
			FAvidScriptDynamicHostCallResult& OutResult)
		{
			FAvidScriptDynamicHostCall Call;
			Call.BindingOrdinal = Ordinal;
			Call.Arguments = Arguments;
			Call.GuestMemory = &GuestMemory;
			return DirectPackage->Dispatch(
				Call,
				Context,
				Scratch,
				OutResult);
		};

	constexpr uint32 SemanticAddAddress = 64;
	constexpr uint32 DirectAddAddress = 68;
	const uint64 SemanticAddArguments[] = {
		Handle.Slot,
		Handle.Generation,
		19,
		23,
		SemanticAddAddress
	};
	const uint64 DirectAddArguments[] = {
		Handle.Slot,
		Handle.Generation,
		19,
		23,
		DirectAddAddress
	};
	FAvidScriptDynamicHostCallResult SemanticAddResult;
	FAvidScriptDynamicHostCallResult DirectAddResult;
	TestTrue(
		TEXT("Qualified Add executes with semantic ProcessEvent policy"),
		Dispatch(
			AddBinding->Ordinal,
			MakeArrayView(SemanticAddArguments),
			SemanticContext,
			SemanticAddResult));
	TestTrue(
		TEXT("Qualified Add executes with native-direct policy"),
		Dispatch(
			AddBinding->Ordinal,
			MakeArrayView(DirectAddArguments),
			DirectContext,
			DirectAddResult));
	TestEqual(
		TEXT("Semantic Add returns 42"),
		GuestMemory.ReadValue<int32>(SemanticAddAddress),
		42);
	TestEqual(
		TEXT("Native-direct Add returns 42"),
		GuestMemory.ReadValue<int32>(DirectAddAddress),
		42);
	TestEqual(
		TEXT("Add policies expose the same host-call return"),
		DirectAddResult.ReturnValue,
		SemanticAddResult.ReturnValue);

	constexpr uint32 AdaptiveAddAddress = 96;
	const uint64 AdaptiveAddArguments[] = {
		Handle.Slot,
		Handle.Generation,
		19,
		23,
		AdaptiveAddAddress
	};
	FAvidScriptDynamicHostCallResult AdaptiveAddResult;
	TestTrue(
		TEXT("Adaptive Add executes through prepared native reflection"),
		Dispatch(
			AddBinding->Ordinal,
			MakeArrayView(AdaptiveAddArguments),
			AdaptiveContext,
			AdaptiveAddResult));
	TestEqual(
		TEXT("Adaptive Add returns 42"),
		GuestMemory.ReadValue<int32>(AdaptiveAddAddress),
		42);

	constexpr uint32 SemanticMaxAddress = 72;
	constexpr uint32 DirectMaxAddress = 76;
	const uint64 SemanticMaxArguments[] = {
		Handle.Slot,
		Handle.Generation,
		17,
		29,
		SemanticMaxAddress
	};
	const uint64 DirectMaxArguments[] = {
		Handle.Slot,
		Handle.Generation,
		17,
		29,
		DirectMaxAddress
	};
	FAvidScriptDynamicHostCallResult SemanticMaxResult;
	FAvidScriptDynamicHostCallResult DirectMaxResult;
	TestTrue(
		TEXT("Qualified Max executes with semantic ProcessEvent policy"),
		Dispatch(
			MaxBinding->Ordinal,
			MakeArrayView(SemanticMaxArguments),
			SemanticContext,
			SemanticMaxResult));
	TestTrue(
		TEXT("Qualified Max executes with native-direct policy"),
		Dispatch(
			MaxBinding->Ordinal,
			MakeArrayView(DirectMaxArguments),
			DirectContext,
			DirectMaxResult));
	TestEqual(
		TEXT("Semantic Max returns 29"),
		GuestMemory.ReadValue<int32>(SemanticMaxAddress),
		29);
	TestEqual(
		TEXT("Native-direct Max returns 29"),
		GuestMemory.ReadValue<int32>(DirectMaxAddress),
		29);
	TestEqual(
		TEXT("Max policies expose the same host-call return"),
		DirectMaxResult.ReturnValue,
		SemanticMaxResult.ReturnValue);

	constexpr uint32 FloatResultAddress = 80;
	const uint64 FloatArguments[] = {
		Handle.Slot,
		Handle.Generation,
		MakeAvidScriptBindingRuntimeF32Cell(1.25f),
		MakeAvidScriptBindingRuntimeF32Cell(2.5f),
		FloatResultAddress
	};
	FAvidScriptDynamicHostCallResult FloatResult;
	TestTrue(
		TEXT("Requested direct policy falls back for an unsupported float shape"),
		Dispatch(
			FloatBinding->Ordinal,
			MakeArrayView(FloatArguments),
			DirectContext,
			FloatResult));
	TestEqual(
		TEXT("Unsupported direct-authorized float preserves semantic return"),
		GuestMemory.ReadValue<float>(FloatResultAddress),
		3.75f);

	constexpr uint32 GuardedDirectAddress = 88;
	const int32 GuardedDirectSentinel = 0x13572468;
	GuestMemory.WriteValue(GuardedDirectAddress, GuardedDirectSentinel);
	const uint64 GuardedDirectArguments[] = {
		Handle.Slot,
		Handle.Generation,
		19,
		23,
		GuardedDirectAddress
	};
	{
		TGuardValue<bool> DebuggingGuard(
			GIntraFrameDebuggingGameThread,
			true);
		FAvidScriptDynamicHostCallResult GuardedDirectResult;
		TestFalse(
			TEXT("Native-direct fails closed during intra-frame debugging"),
			Dispatch(
				AddBinding->Ordinal,
				MakeArrayView(GuardedDirectArguments),
				DirectContext,
				GuardedDirectResult));
		TestTrue(
			TEXT("Debugging guard reports the native-direct failure category"),
			GuardedDirectResult.Details.Contains(
				TEXT("binding_native_direct_debugging_active")));
	}
	TestEqual(
		TEXT("Debugging guard does not write Guest Memory"),
		GuestMemory.ReadValue<int32>(GuardedDirectAddress),
		GuardedDirectSentinel);

	constexpr uint32 AdaptiveFallbackAddress = 100;
	UClass* DerivedTestClass = LoadObject<UClass>(
		nullptr,
		TEXT("/Script/AvidScriptBindings.AvidScriptBindingsDerivedTestObject"));
	UObject* DerivedTarget = DerivedTestClass == nullptr
		? nullptr
		: NewObject<UObject>(GetTransientPackage(), DerivedTestClass);
	if (!TestNotNull(
			TEXT("Adaptive fallback derived target is created"),
			DerivedTarget))
	{
		return false;
	}
	FAvidScriptObjectHandleResult DerivedRegisterResult;
	const FAvidScriptObjectHandle DerivedHandle = Registry.RegisterObject(
		DerivedTarget,
		DerivedRegisterResult);
	if (!TestTrue(
			TEXT("Adaptive fallback derived target registers"),
			DerivedRegisterResult.bSucceeded))
	{
		return false;
	}
	FAvidScriptBindingInvocationContext DerivedAdaptiveContext =
		AdaptiveContext;
	DerivedAdaptiveContext.OwnerHandle = DerivedHandle;
	const uint64 AdaptiveFallbackArguments[] = {
		DerivedHandle.Slot,
		DerivedHandle.Generation,
		31,
		11,
		AdaptiveFallbackAddress
	};
	FAvidScriptDynamicHostCallResult AdaptiveFallbackResult;
	TestTrue(
		TEXT("Adaptive exact-class guard falls back to ProcessEvent"),
		Dispatch(
			AddBinding->Ordinal,
			MakeArrayView(AdaptiveFallbackArguments),
			DerivedAdaptiveContext,
			AdaptiveFallbackResult));
	TestEqual(
		TEXT("Adaptive exact-class fallback preserves the exact result"),
		GuestMemory.ReadValue<int32>(AdaptiveFallbackAddress),
		42);

	constexpr uint32 StaleResultAddress = 92;
	const int32 StaleSentinel = 0x24681357;
	GuestMemory.WriteValue(StaleResultAddress, StaleSentinel);
	const uint64 StaleArguments[] = {
		Handle.Slot,
		Handle.Generation + 1,
		10,
		20,
		StaleResultAddress
	};
	FAvidScriptDynamicHostCallResult StaleResult;
	TestFalse(
		TEXT("Native-direct rejects a stale handle generation"),
		Dispatch(
			AddBinding->Ordinal,
			MakeArrayView(StaleArguments),
			DirectContext,
			StaleResult));
	TestEqual(
		TEXT("Rejected native-direct stale handle does not write Guest Memory"),
		GuestMemory.ReadValue<int32>(StaleResultAddress),
		StaleSentinel);
	TestEqual(
		TEXT("Instrumentation records successful semantic dispatches"),
		InvocationInstrumentation.SemanticProcessEventCount,
		5ull);
	TestEqual(
		TEXT("Instrumentation records adaptive prepared native hits"),
		InvocationInstrumentation.AdaptivePreparedNativeHitCount,
		2ull);
	TestEqual(
		TEXT("Instrumentation records adaptive ProcessEvent fallback"),
		InvocationInstrumentation.AdaptiveProcessEventFallbackCount,
		1ull);
	TestEqual(
		TEXT("Instrumentation records adaptive runtime guard rejection"),
		InvocationInstrumentation.AdaptiveGuardRejectCount,
		1ull);
	TestEqual(
		TEXT("Instrumentation records actual qualified native-direct dispatches"),
		InvocationInstrumentation.QualifiedNativeDirectCount,
		2ull);
	TestEqual(
		TEXT("Instrumentation records requested direct semantic fallback"),
		InvocationInstrumentation.RequestedNativeDirectFallbackCount,
		1ull);

	AddInfo(
		TEXT("Direct authorization for Actor functions accepts bypassing "
			"AActor::ProcessEvent S3 guards; equal observable returns here "
			"do not claim full semantic equivalence."));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPreparedReflectionBroadShapeTest,
	"AvidScript.Editor.BindingRuntime.PreparedReflectionBroadShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPreparedReflectionBroadShapeTest::RunTest(
	const FString& Parameters)
{
	const FString TestClassPath =
		TEXT("/Script/AvidScriptBindings.AvidScriptBindingsTestObject");
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName =
		TEXT("avidscript.test.prepared_reflection_broad_shape");
	FAvidScriptReflectedClassSelection ClassSelection;
	ClassSelection.OwnerClassPath = TestClassPath;
	ClassSelection.IncludeFunctions = {
		TEXT("FastPathVectorValue"),
		TEXT("FastPathObjectRoundtrip")
	};
	ClassSelection.NativeDirectFunctions =
		ClassSelection.IncludeFunctions;
	Profile.Classes.Add(MoveTemp(ClassSelection));

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Broad prepared reflection descriptor generates"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
				Profile,
				DescriptorJson,
				SelectionResult,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
			TEXT("Broad prepared reflection descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Descriptor,
				ParseCategory,
				ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
			TEXT("Broad prepared reflection package loads"),
			FAvidScriptBindingPackage::LoadDescriptor(
				DescriptorJson,
				Package,
				LoadResult)))
	{
		AddError(
			LoadResult.ErrorCategory
			+ TEXT(": ")
			+ LoadResult.ErrorDetails);
		return false;
	}

	TArray<FAvidScriptPreparedReflectionBinding> PreparedBindings;
	FString PreparedError;
	if (!TestTrue(
			TEXT("Broad package publishes prepared reflection call cells"),
			Package->BuildPreparedReflectionBindings(
				PreparedBindings,
				PreparedError)))
	{
		AddError(PreparedError);
		return false;
	}
	if (!TestEqual(
			TEXT("Broad package publishes vector and object call cells"),
			PreparedBindings.Num(),
			2))
	{
		return false;
	}

	const auto FindModel = [&Descriptor](const TCHAR* FunctionName)
	{
		return Descriptor.Bindings.FindByPredicate(
			[FunctionName](
				const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.UeFunction == FunctionName;
			});
	};
	const FAvidScriptBindingFunctionModel* VectorModel =
		FindModel(TEXT("FastPathVectorValue"));
	const FAvidScriptBindingFunctionModel* ObjectModel =
		FindModel(TEXT("FastPathObjectRoundtrip"));
	if (!TestNotNull(TEXT("Vector binding model is present"), VectorModel)
		|| !TestNotNull(TEXT("Object binding model is present"), ObjectModel))
	{
		return false;
	}
	const auto FindPrepared = [&PreparedBindings](const int32 Ordinal)
	{
		return PreparedBindings.FindByPredicate(
			[Ordinal](const FAvidScriptPreparedReflectionBinding& Binding)
			{
				return Binding.BindingOrdinal
					== static_cast<uint32>(Ordinal);
			});
	};
	const FAvidScriptPreparedReflectionBinding* VectorBinding =
		FindPrepared(VectorModel->Ordinal);
	const FAvidScriptPreparedReflectionBinding* ObjectBinding =
		FindPrepared(ObjectModel->Ordinal);
	if (!TestNotNull(TEXT("Prepared vector call cell is present"), VectorBinding)
		|| !TestNotNull(TEXT("Prepared object call cell is present"), ObjectBinding))
	{
		return false;
	}
	TestEqual(
		TEXT("Vector call cell uses expanded-f32 semantic ABI"),
		VectorBinding->TypedHostImport.Shape,
		EAvidScriptVmTypedHostShape::SelfF32TripleToGuestVector);
	TestEqual(
		TEXT("Object call cell uses stable object ABI"),
		ObjectBinding->TypedHostImport.Shape,
		EAvidScriptVmTypedHostShape::StableObjectRoundtrip);
	TestNotNull(TEXT("Vector call cell has a direct target"), VectorBinding->VectorCall);
	TestNotNull(TEXT("Object call cell has a direct target"), ObjectBinding->ObjectCall);

	UClass* TestClass = LoadObject<UClass>(nullptr, *TestClassPath);
	UObject* Target = TestClass == nullptr
		? nullptr
		: NewObject<UObject>(GetTransientPackage(), TestClass);
	if (!TestNotNull(TEXT("Broad prepared target is created"), Target))
	{
		return false;
	}
	const bool bVectorNativeGuardAllowed =
		VectorBinding->NativeGuard != nullptr
		&& VectorBinding->NativeGuard(
			VectorBinding->ImmutablePlanIdentity,
			*Target);
	if (!bVectorNativeGuardAllowed)
	{
		const UFunction* VectorFunction = TestClass->FindFunctionByName(
			TEXT("FastPathVectorValue"));
		if (VectorFunction != nullptr)
		{
			AddInfo(FString::Printf(
				TEXT("Vector native diagnostic: flags=0x%08x native=%d owner_native=%d "
					"num_parms=%d parms=%d properties=%d structure=%d alignment=%d "
					"script=%d first_init=%d post_construct=%d destructor=%d"),
				static_cast<uint32>(VectorFunction->FunctionFlags),
				VectorFunction->GetNativeFunc() != nullptr,
				TestClass->HasAllClassFlags(CLASS_Native),
				VectorFunction->NumParms,
				VectorFunction->ParmsSize,
				VectorFunction->PropertiesSize,
				VectorFunction->GetStructureSize(),
				VectorFunction->GetMinAlignment(),
				VectorFunction->Script.Num(),
				VectorFunction->FirstPropertyToInit != nullptr,
				VectorFunction->PostConstructLink != nullptr,
				VectorFunction->DestructorLink != nullptr));
		}
	}
	TestTrue(
		TEXT("Vector native guard accepts the exact reflected class"),
		bVectorNativeGuardAllowed);
	TestTrue(
		TEXT("Object native guard accepts the exact reflected class"),
		ObjectBinding->NativeGuard != nullptr
			&& ObjectBinding->NativeGuard(
				ObjectBinding->ImmutablePlanIdentity,
				*Target));

	FString ErrorCategory;
	FString ErrorDetails;
	FVector SemanticVector = FVector::ZeroVector;
	FVector NativeVector = FVector::ZeroVector;
	TestTrue(
		TEXT("Prepared vector ProcessEvent entry succeeds"),
		VectorBinding->VectorCall(
			VectorBinding->ImmutablePlanIdentity,
			*Target,
			FVector(1.0, 2.0, 3.0),
			false,
			SemanticVector,
			ErrorCategory,
			ErrorDetails));
	TestTrue(
		TEXT("Prepared vector native entry succeeds"),
		VectorBinding->VectorCall(
			VectorBinding->ImmutablePlanIdentity,
			*Target,
			FVector(1.0, 2.0, 3.0),
			true,
			NativeVector,
			ErrorCategory,
			ErrorDetails));
	TestTrue(
		TEXT("Prepared vector entries preserve the reflected result"),
		SemanticVector.Equals(FVector(3.0, 5.0, 7.0))
			&& NativeVector.Equals(SemanticVector));

	UObject* InputObject = NewObject<UObject>(GetTransientPackage(), TestClass);
	UObject* SemanticObject = nullptr;
	UObject* NativeObject = nullptr;
	TestTrue(
		TEXT("Prepared object ProcessEvent entry succeeds"),
		ObjectBinding->ObjectCall(
			ObjectBinding->ImmutablePlanIdentity,
			*Target,
			InputObject,
			false,
			SemanticObject,
			ErrorCategory,
			ErrorDetails));
	TestTrue(
		TEXT("Prepared object native entry succeeds"),
		ObjectBinding->ObjectCall(
			ObjectBinding->ImmutablePlanIdentity,
			*Target,
			InputObject,
			true,
			NativeObject,
			ErrorCategory,
			ErrorDetails));
	TestTrue(
		TEXT("Prepared object entries preserve stable identity"),
		SemanticObject == InputObject && NativeObject == InputObject);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPreparedReflectionPropertyRuntimeTest,
	"AvidScript.Editor.BindingRuntime.PreparedReflectionPropertyRuntime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPreparedReflectionPropertyRuntimeTest::RunTest(
	const FString& Parameters)
{
	const FString TestClassPath =
		TEXT("/Script/AvidScriptBindings.AvidScriptBindingsTestObject");
	const TArray<FAvidScriptReflectedPropertySelection> Properties = {
		{ TestClassPath, TEXT("FastPathInt32Property"), true }
	};
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Prepared int32 property descriptor generates"),
			FAvidScriptEditorBindingDescriptorGenerator::
				GenerateWithReadableProperties(
					TEXT("avidscript.test.prepared_reflection_property"),
					{},
					Properties,
					DescriptorJson,
					GenerateResult)))
	{
		AddError(
			GenerateResult.ErrorCategory
			+ TEXT(": ")
			+ GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
			TEXT("Prepared int32 property descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Descriptor,
				ParseCategory,
				ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}
	const FAvidScriptBindingFunctionModel* Getter =
		Descriptor.Bindings.FindByPredicate(
			[](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.BindingKind == TEXT("property_get");
			});
	const FAvidScriptBindingFunctionModel* Setter =
		Descriptor.Bindings.FindByPredicate(
			[](const FAvidScriptBindingFunctionModel& Binding)
			{
				return Binding.BindingKind == TEXT("property_set");
			});
	if (!TestNotNull(TEXT("Prepared int32 getter resolves"), Getter)
		|| !TestNotNull(TEXT("Prepared int32 setter resolves"), Setter))
	{
		return false;
	}
	TestEqual(
		TEXT("Prepared int32 getter freezes the combined property ABI"),
		Getter->HostImport.Signature,
		FString(TEXT("(iii)i")));
	TestEqual(
		TEXT("Prepared int32 setter freezes the combined property ABI"),
		Setter->HostImport.Signature,
		FString(TEXT("(iii)i")));

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
			TEXT("Prepared int32 property package loads"),
			FAvidScriptBindingPackage::LoadDescriptor(
				DescriptorJson,
				Package,
				LoadResult)))
	{
		AddError(
			LoadResult.ErrorCategory
			+ TEXT(": ")
			+ LoadResult.ErrorDetails);
		return false;
	}

	UClass* TestClass = LoadObject<UClass>(nullptr, *TestClassPath);
	TStrongObjectPtr<UObject> Target(
		TestClass == nullptr
			? nullptr
			: NewObject<UObject>(GetTransientPackage(), TestClass));
	FIntProperty* Property = TestClass == nullptr
		? nullptr
		: FindFProperty<FIntProperty>(
			TestClass,
			FName(TEXT("FastPathInt32Property")));
	if (!TestNotNull(TEXT("Prepared property target is created"), Target.Get())
		|| !TestNotNull(TEXT("Prepared int32 property resolves"), Property))
	{
		return false;
	}
	Property->SetPropertyValue_InContainer(Target.Get(), 0);

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle Handle = Registry.RegisterObject(
		Target.Get(),
		RegisterResult,
		false);
	if (!TestTrue(
			TEXT("Prepared property target registers"),
			RegisterResult.bSucceeded))
	{
		return false;
	}
	FAvidScriptBindingInvocationInstrumentation Instrumentation;
	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.OwnerHandle = Handle;
	HostContext.ActorWritePolicy =
		EAvidScriptActorWritePolicy::AllowWrites;
	HostContext.BindingInvocationPolicy =
		EAvidScriptBindingInvocationPolicy::AdaptiveSemantic;
	HostContext.BindingInvocationInstrumentation = &Instrumentation;

	const TArray<uint8> Bytecode =
		BuildAvidScriptPreparedI32PropertyModule(
			Getter->HostImport,
			Setter->HostImport,
			Handle);
	FAvidScriptVmBackendSelection BackendSelection;
	BackendSelection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	BackendSelection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	BackendSelection.ArtifactFormat =
		EAvidScriptVmArtifactFormat::WasmBytecode;
	BackendSelection.bAllowFallback = false;
	FAvidScriptWasmRuntimeInstance Runtime(BackendSelection);
	Runtime.SetHostContext(HostContext);
	FAvidScriptWasmSmokeResult Result;
	if (!TestTrue(
			TEXT("Prepared property WASM module loads"),
			Runtime.LoadModule(
				Bytecode.GetData(),
				Bytecode.Num(),
				TEXT("prepared_reflection_property_runtime"),
				Package,
				Result)))
	{
		AddError(Result.ErrorCategory + TEXT(": ") + Result.ErrorMessage);
		return false;
	}
	const TArray<FAvidScriptVmTypedHostImport>& PreparedImports =
		Runtime.GetPreparedTypedHostImportsForTesting();
	TestEqual(
		TEXT("Runtime publishes both prepared property call cells"),
		PreparedImports.Num(),
		2);
	TestTrue(
		TEXT("BeginPlay executes the prepared property setter"),
		Runtime.BeginPlay(Result));
	TestEqual(
		TEXT("Prepared setter writes the reflected property"),
		Property->GetPropertyValue_InContainer(Target.Get()),
		41);
	TestTrue(
		TEXT("Tick executes prepared property getter and setter"),
		Runtime.Tick(0.016f, Result));
	TestEqual(
		TEXT("Prepared getter guest layout feeds the next property write"),
		Property->GetPropertyValue_InContainer(Target.Get()),
		42);
	TestEqual(
		TEXT("Prepared property calls record adaptive direct hits"),
		Instrumentation.AdaptivePreparedNativeHitCount,
		3ull);
	TestEqual(
		TEXT("Prepared property calls avoid semantic fallback"),
		Instrumentation.AdaptiveProcessEventFallbackCount,
		0ull);
	TestEqual(
		TEXT("Prepared property calls avoid semantic dispatch accounting"),
		Instrumentation.SemanticProcessEventCount,
		0ull);
	Runtime.Unload();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBindingLatentProcessEventTest,
	"AvidScript.Editor.BindingRuntime.LatentProcessEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBindingLatentProcessEventTest::RunTest(
	const FString& Parameters)
{
	FString DescriptorJson;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Delay latent descriptor generates"),
			FAvidScriptEditorBindingDescriptorGenerator::Generate(
				TEXT("avidscript.test.latent_process_event"),
				{
					{ TEXT("/Script/Engine.KismetSystemLibrary"), TEXT("Delay") }
				},
				DescriptorJson,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ") + GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			Descriptor,
			ParseCategory,
			ParseSource)
		|| !FAvidScriptBindingPackage::LoadDescriptor(
			DescriptorJson,
			Package,
			LoadResult)
		|| Descriptor.Bindings.Num() != 1)
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource
			+ TEXT(" ") + LoadResult.ErrorCategory + TEXT(": ")
			+ LoadResult.ErrorDetails);
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("Latent integration world is created"),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};

	FAvidScriptEditorLatentTestHost LatentHost(*World);
	FAvidScriptBindingInvocationContext InvocationContext;
	InvocationContext.World = World;
	InvocationContext.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	InvocationContext.LatentHost = &LatentHost;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	uint64 Arguments[] = {
		MakeAvidScriptBindingRuntimeF32Cell(0.01f),
		77
	};
	FAvidScriptDynamicHostCall Call;
	Call.BindingOrdinal = Descriptor.Bindings[0].Ordinal;
	Call.Arguments = MakeArrayView(Arguments);
	FAvidScriptDynamicHostCallResult Result;
	if (!TestTrue(
			TEXT("Generic latent ProcessEvent reserves and commits"),
			Package->Dispatch(
				Call,
				InvocationContext,
				Scratch,
				Result)))
	{
		AddError(Result.Details);
		return false;
	}
	TestTrue(TEXT("Latent host observed a committed action"), LatentHost.bCommitted);
	TestEqual(TEXT("Latent import returns the reserved i64 token"), Result.ReturnValueI64, 9001ll);
	TestEqual(TEXT("Successful registration does not abort"), LatentHost.AbortCount, 0);
	TestEqual(
		TEXT("Delay action is owned by the per-token callback target"),
		World->GetLatentActionManager().GetNumActionsForObject(
			LatentHost.GetCallbackTarget()),
		1);

	++GFrameCounter;
	World->Tick(ELevelTick::LEVELTICK_All, 0.02f);
	TestEqual(
		TEXT("Real latent manager resumes the reserved callback target"),
		LatentHost.GetCallbackTarget()->CompletionCount,
		1);
	TestEqual(
		TEXT("Real latent manager preserves the reserved linkage"),
		LatentHost.GetCallbackTarget()->LastLinkage,
		0);

	FAvidScriptEditorLatentTestHost RejectingHost(*World);
	RejectingHost.bAllowCommit = false;
	InvocationContext.LatentHost = &RejectingHost;
	FAvidScriptDynamicHostCallResult RejectedResult;
	TestFalse(
		TEXT("Rejected latent commit fails the host call"),
		Package->Dispatch(
			Call,
			InvocationContext,
			Scratch,
			RejectedResult));
	TestTrue(
		TEXT("Rejected host observed the action before refusing commit"),
		RejectingHost.bObservedRegisteredAction);
	TestEqual(
		TEXT("Rejected latent commit aborts exactly once"),
		RejectingHost.AbortCount,
		1);
	TestEqual(
		TEXT("Abort schedules the uncommitted latent action for removal"),
		World->GetLatentActionManager().GetNumActionsForObject(
			RejectingHost.GetCallbackTarget()),
		1);
	++GFrameCounter;
	World->Tick(ELevelTick::LEVELTICK_All, 0.02f);
	TestEqual(
		TEXT("Next latent-manager pass removes the aborted action"),
		World->GetLatentActionManager().GetNumActionsForObject(
			RejectingHost.GetCallbackTarget()),
		0);
	TestEqual(
		TEXT("Aborted latent action never invokes its callback"),
		RejectingHost.GetCallbackTarget()->CompletionCount,
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBlueprintDeclaredFunctionIntegrationTest,
	"AvidScript.Editor.BindingRuntime.BlueprintDeclaredFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBlueprintDeclaredFunctionIntegrationTest::RunTest(
	const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FName BlueprintName(*FString::Printf(
		TEXT("AvidScriptBlueprintCallable_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UBlueprint> Blueprint(
		FKismetEditorUtilities::CreateBlueprint(
			AAvidScriptBindingRuntimeProcessEventTestActor::StaticClass(),
			GetTransientPackage(),
			BlueprintName,
			BPTYPE_Normal,
			TEXT("AvidScriptBlueprintDeclaredFunctionTest")));
	if (!TestNotNull(TEXT("Transient callable Blueprint is created"), Blueprint.Get()))
	{
		return false;
	}

	const FName FunctionName(TEXT("ScriptOnlyPulse"));
	UEdGraph* const FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint.Get(),
		FunctionName,
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph(
		Blueprint.Get(),
		FunctionGraph,
		true,
		static_cast<UFunction*>(nullptr));
	TArray<UK2Node_FunctionEntry*> Entries;
	FunctionGraph->GetNodesOfClass(Entries);
	if (!TestEqual(TEXT("Declared function has one entry"), Entries.Num(), 1))
	{
		return false;
	}
	FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*FunctionGraph);
	UK2Node_CallFunction* const CallNode = NodeCreator.CreateNode();
	CallNode->FunctionReference.SetExternalMember(
		GET_FUNCTION_NAME_CHECKED(
			AAvidScriptBindingRuntimeProcessEventTestActor,
			RecordBlueprintDeclaredCall),
		AAvidScriptBindingRuntimeProcessEventTestActor::StaticClass());
	NodeCreator.Finalize();
	const UEdGraphSchema_K2* const Schema = CastChecked<UEdGraphSchema_K2>(
		FunctionGraph->GetSchema());
	if (!TestTrue(
			TEXT("Declared function entry connects to its Blueprint body"),
			Schema->TryCreateConnection(
				Entries[0]->FindPinChecked(UEdGraphSchema_K2::PN_Then),
				CallNode->FindPinChecked(UEdGraphSchema_K2::PN_Execute))))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	UClass* const BlueprintClass = Blueprint->GeneratedClass;
	UFunction* const BlueprintFunction = BlueprintClass == nullptr
		? nullptr
		: BlueprintClass->FindFunctionByName(
			FunctionName,
			EIncludeSuperFlag::ExcludeSuper);
	if (!TestNotNull(TEXT("Blueprint declared function compiles"), BlueprintFunction))
	{
		return false;
	}
	TestFalse(
		TEXT("Blueprint declared function is not native"),
		BlueprintFunction->HasAnyFunctionFlags(FUNC_Native));
	TestTrue(
		TEXT("Blueprint declared function owns bytecode"),
		!BlueprintFunction->Script.IsEmpty());

	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.test.blueprint_declared_function");
	Profile.SelfClassPath = BlueprintClass->GetPathName();
	Profile.ExplicitFunctions.Add({ BlueprintClass->GetPathName(), FunctionName });
	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Blueprint declared function generates through the Profile path"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
				Profile,
				DescriptorJson,
				SelectionResult,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ")
			+ GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
			TEXT("Blueprint declared function descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Descriptor,
				ParseCategory,
				ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}
	if (!TestEqual(TEXT("Blueprint provenance activates schema 21"), Descriptor.SchemaVersion, 21)
		|| !TestEqual(TEXT("One Blueprint function is described"), Descriptor.Bindings.Num(), 1))
	{
		return false;
	}
	const FAvidScriptBindingFunctionModel& Binding = Descriptor.Bindings[0];
	TestEqual(
		TEXT("Descriptor identifies a Blueprint owner"),
		Binding.ReflectedOwnerKind,
		FString(TEXT("blueprint")));
	TestEqual(
		TEXT("Descriptor freezes the Blueprint asset identity"),
		Binding.ReflectedOwnerAsset,
		Blueprint->GetPathName());
	TestEqual(
		TEXT("Descriptor freezes bytecode and signature provenance"),
		Binding.ReflectedFunctionFingerprint,
		FAvidScriptBindingDescriptorIdentity::MakeReflectedFunctionFingerprint(
			Binding.CanonicalIdentity,
			*BlueprintFunction));

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
			TEXT("Runtime accepts the live Blueprint provenance"),
			FAvidScriptBindingPackage::LoadDescriptor(
				DescriptorJson,
				Package,
				LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("Blueprint callable integration world is created"),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};
	AAvidScriptBindingRuntimeProcessEventTestActor* const Actor =
		Cast<AAvidScriptBindingRuntimeProcessEventTestActor>(
			World->SpawnActor<AActor>(BlueprintClass));
	if (!TestNotNull(TEXT("Blueprint callable actor spawns"), Actor))
	{
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(
		Actor,
		RegisterResult);
	if (!TestTrue(TEXT("Blueprint callable actor registers"), RegisterResult.bSucceeded))
	{
		return false;
	}
	const uint64 Arguments[] = { ActorHandle.Slot, ActorHandle.Generation };
	FAvidScriptDynamicHostCall Call;
	Call.BindingOrdinal = static_cast<uint32>(Binding.Ordinal);
	Call.Arguments = MakeArrayView(Arguments);
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.OwnerHandle = ActorHandle;
	Context.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(Package->GetRequiredScratchSize());
	FAvidScriptDynamicHostCallResult DispatchResult;
	const bool bDispatched = Package->Dispatch(
		Call,
		Context,
		Scratch,
		DispatchResult);
	if (!TestTrue(
		TEXT("Prepared ProcessEvent invokes Blueprint-owned bytecode"),
		bDispatched))
	{
		AddError(DispatchResult.Details);
	}
	TestTrue(TEXT("Blueprint dispatch reports success"), DispatchResult.bSucceeded);
	TestEqual(
		TEXT("Blueprint graph reaches its native observation point"),
		Actor->BlueprintDeclaredCallCount,
		1);
	TestTrue(
		TEXT("Blueprint call crosses ProcessEvent"),
		Actor->ProcessEventCallCount > 0);

	FGraphNodeCreator<UK2Node_CallFunction> SecondNodeCreator(*FunctionGraph);
	UK2Node_CallFunction* const SecondCallNode = SecondNodeCreator.CreateNode();
	SecondCallNode->FunctionReference.SetExternalMember(
		GET_FUNCTION_NAME_CHECKED(
			AAvidScriptBindingRuntimeProcessEventTestActor,
			RecordBlueprintDeclaredCall),
		AAvidScriptBindingRuntimeProcessEventTestActor::StaticClass());
	SecondNodeCreator.Finalize();
	if (!TestTrue(
			TEXT("Blueprint bytecode mutation extends the callable body"),
			Schema->TryCreateConnection(
				CallNode->FindPinChecked(UEdGraphSchema_K2::PN_Then),
				SecondCallNode->FindPinChecked(UEdGraphSchema_K2::PN_Execute))))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	TSharedPtr<const FAvidScriptBindingPackage> StalePackage;
	FAvidScriptBindingPackageLoadResult StaleLoadResult;
	TestFalse(
		TEXT("Runtime rejects a descriptor after Blueprint bytecode changes"),
		FAvidScriptBindingPackage::LoadDescriptor(
			DescriptorJson,
			StalePackage,
			StaleLoadResult));
	TestEqual(
		TEXT("Blueprint recompile rejection has a stable category"),
		StaleLoadResult.ErrorCategory,
		FString(TEXT("binding_reflection_provenance_mismatch")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorBlueprintDeclaredEventIntegrationTest,
	"AvidScript.Editor.BindingRuntime.BlueprintDeclaredEvent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorBlueprintDeclaredEventIntegrationTest::RunTest(
	const FString& Parameters)
{
	static_cast<void>(Parameters);
	const FName BlueprintName(*FString::Printf(
		TEXT("AvidScriptBlueprintEvent_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UBlueprint> Blueprint(
		FKismetEditorUtilities::CreateBlueprint(
			AAvidScriptBindingRuntimeProcessEventTestActor::StaticClass(),
			GetTransientPackage(),
			BlueprintName,
			BPTYPE_Normal,
			TEXT("AvidScriptBlueprintDeclaredEventTest")));
	if (!TestNotNull(TEXT("Transient event Blueprint is created"), Blueprint.Get()))
	{
		return false;
	}

	const FName FunctionName(TEXT("ScriptOnlyEvent"));
	UEdGraph* const FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint.Get(),
		FunctionName,
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph(
		Blueprint.Get(),
		FunctionGraph,
		true,
		static_cast<UFunction*>(nullptr));
	TArray<UK2Node_FunctionEntry*> Entries;
	FunctionGraph->GetNodesOfClass(Entries);
	if (!TestEqual(TEXT("Declared event has one entry"), Entries.Num(), 1))
	{
		return false;
	}
	FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*FunctionGraph);
	UK2Node_CallFunction* const CallNode = NodeCreator.CreateNode();
	CallNode->FunctionReference.SetExternalMember(
		GET_FUNCTION_NAME_CHECKED(
			AAvidScriptBindingRuntimeProcessEventTestActor,
			RecordBlueprintDeclaredCall),
		AAvidScriptBindingRuntimeProcessEventTestActor::StaticClass());
	NodeCreator.Finalize();
	const UEdGraphSchema_K2* const Schema = CastChecked<UEdGraphSchema_K2>(
		FunctionGraph->GetSchema());
	if (!TestTrue(
			TEXT("Declared event entry connects to its Blueprint body"),
			Schema->TryCreateConnection(
				Entries[0]->FindPinChecked(UEdGraphSchema_K2::PN_Then),
				CallNode->FindPinChecked(UEdGraphSchema_K2::PN_Execute))))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	UClass* const BlueprintClass = Blueprint->GeneratedClass;
	UFunction* const BlueprintFunction = BlueprintClass == nullptr
		? nullptr
		: BlueprintClass->FindFunctionByName(
			FunctionName,
			EIncludeSuperFlag::ExcludeSuper);
	if (!TestNotNull(TEXT("Blueprint declared event compiles"), BlueprintFunction))
	{
		return false;
	}
	TestFalse(
		TEXT("Blueprint declared event is not native"),
		BlueprintFunction->HasAnyFunctionFlags(FUNC_Native));
	TestTrue(
		TEXT("Blueprint declared event owns bytecode"),
		!BlueprintFunction->Script.IsEmpty());

	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.test.blueprint_declared_event");
	Profile.SelfClassPath = BlueprintClass->GetPathName();
	Profile.ExplicitDelegateEvents.Add({
		BlueprintClass->GetPathName(),
		FunctionName,
		TEXT("blueprint_event"),
		TEXT("before")
	});
	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
			TEXT("Blueprint declared event generates through the Profile path"),
			FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
				Profile,
				DescriptorJson,
				SelectionResult,
				GenerateResult)))
	{
		AddError(GenerateResult.ErrorCategory + TEXT(": ")
			+ GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
			TEXT("Blueprint declared event descriptor parses"),
			FAvidScriptBindingDescriptorParser::Parse(
				DescriptorJson,
				Descriptor,
				ParseCategory,
				ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}
	if (!TestEqual(TEXT("Blueprint event activates schema 22"), Descriptor.SchemaVersion, 22)
		|| !TestEqual(TEXT("One Blueprint event is described"), Descriptor.DelegateEvents.Num(), 1))
	{
		return false;
	}
	const FAvidScriptBindingDelegateEventModel& Event = Descriptor.DelegateEvents[0];
	TestEqual(
		TEXT("Descriptor identifies a Blueprint event"),
		Event.DelegateKind,
		FString(TEXT("blueprint_event")));
	TestEqual(
		TEXT("Descriptor freezes the Blueprint event asset"),
		Event.ReflectedOwnerAsset,
		Blueprint->GetPathName());
	TestEqual(
		TEXT("Descriptor freezes the Blueprint event bytecode"),
		Event.ReflectedFunctionFingerprint,
		FAvidScriptBindingDescriptorIdentity::MakeReflectedFunctionFingerprint(
			Event.CanonicalIdentity,
			*BlueprintFunction));

	FAvidScriptBindingSelectionProfile ConflictProfile = Profile;
	ConflictProfile.ExplicitFunctions.Add({
		BlueprintClass->GetPathName(),
		FunctionName
	});
	ConflictProfile.ExplicitDelegateEvents[0].HandlerMode = TEXT("replace");
	FString ConflictDescriptorJson;
	FAvidScriptBindingSelectionResolveResult ConflictSelectionResult;
	FAvidScriptBindingDescriptorGenerateResult ConflictGenerateResult;
	TestFalse(
		TEXT("Replace mode rejects the same outbound Blueprint callable"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			ConflictProfile,
			ConflictDescriptorJson,
			ConflictSelectionResult,
			ConflictGenerateResult));
	TestEqual(
		TEXT("Replace/call conflict has a stable category"),
		ConflictGenerateResult.ErrorCategory,
		FString(TEXT("blueprint_event_replace_invocation_conflict")));

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
			TEXT("Runtime accepts the live Blueprint event provenance"),
			FAvidScriptBindingPackage::LoadDescriptor(
				DescriptorJson,
				Package,
				LoadResult)))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails);
		return false;
	}
	TArray<FAvidScriptPreparedDelegateEvent> Handlers;
	FString HandlerError;
	if (!TestTrue(
			TEXT("Binding package prepares one Blueprint inbound handler"),
			Package->BuildPreparedInboundHandlers(Handlers, HandlerError))
		|| !TestEqual(TEXT("One Blueprint handler is prepared"), Handlers.Num(), 1))
	{
		AddError(HandlerError);
		return false;
	}
	TestEqual(
		TEXT("Prepared handler preserves Blueprint callback kind"),
		Handlers[0].CallbackKind,
		FString(TEXT("blueprint_event")));

	UWorld* World = nullptr;
	if (!TestTrue(
			TEXT("Blueprint event integration world is created"),
			CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};
	AAvidScriptBindingRuntimeProcessEventTestActor* const Actor =
		Cast<AAvidScriptBindingRuntimeProcessEventTestActor>(
			World->SpawnActor<AActor>(BlueprintClass));
	if (!TestNotNull(TEXT("Blueprint event actor spawns"), Actor))
	{
		return false;
	}

	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmReloadResult RuntimeLoadResult;
	if (!TestTrue(
			TEXT("Embedded runtime reaches Running for Blueprint event dispatch"),
			Session.LoadEmbeddedSmoke(RuntimeLoadResult)))
	{
		return false;
	}
	Handlers[0].ExportName = TEXT("avid_on_begin_play");
	if (!TestTrue(
			TEXT("Blueprint event export prepares against the live VM"),
			Session.GetLiveRuntimeForTesting()->PrepareDelegateEventExportsForTesting(
				Handlers,
				HandlerError))
		|| !TestEqual(TEXT("Live VM keeps the implemented event"), Handlers.Num(), 1))
	{
		AddError(HandlerError);
		return false;
	}
	if (!TestTrue(
			TEXT("Before handler prepares"),
			Session.PrepareInboundHandlersForTesting(
				Actor,
				Handlers,
				HandlerError))
		|| !TestTrue(
			TEXT("Before handler commits"),
			Session.CommitInboundHandlersForTesting(HandlerError)))
	{
		AddError(HandlerError);
		return false;
	}
	Actor->ProcessEvent(BlueprintFunction, nullptr);
	TestEqual(TEXT("Before mode dispatches one guest callback"), Session.GetLiveEventCallbackCount(), 1);
	TestEqual(TEXT("Before mode preserves the Blueprint body"), Actor->BlueprintDeclaredCallCount, 1);

	Handlers[0].HandlerMode = TEXT("after");
	if (!TestTrue(
			TEXT("After handler prepares"),
			Session.PrepareInboundHandlersForTesting(
				Actor,
				Handlers,
				HandlerError))
		|| !TestTrue(
			TEXT("After handler commits"),
			Session.CommitInboundHandlersForTesting(HandlerError)))
	{
		AddError(HandlerError);
		return false;
	}
	Actor->ProcessEvent(BlueprintFunction, nullptr);
	TestEqual(TEXT("After mode dispatches one guest callback"), Session.GetLiveEventCallbackCount(), 2);
	TestEqual(TEXT("After mode preserves the Blueprint body"), Actor->BlueprintDeclaredCallCount, 2);

	Handlers[0].HandlerMode = TEXT("replace");
	if (!TestTrue(
			TEXT("Replace handler prepares"),
			Session.PrepareInboundHandlersForTesting(
				Actor,
				Handlers,
				HandlerError))
		|| !TestTrue(
			TEXT("Replace handler commits"),
			Session.CommitInboundHandlersForTesting(HandlerError)))
	{
		AddError(HandlerError);
		return false;
	}
	Actor->ProcessEvent(BlueprintFunction, nullptr);
	TestEqual(TEXT("Replace mode dispatches one guest callback"), Session.GetLiveEventCallbackCount(), 3);
	TestEqual(TEXT("Replace mode suppresses the Blueprint body"), Actor->BlueprintDeclaredCallCount, 2);
	Session.UnbindInboundHandlersForTesting();
	Actor->ProcessEvent(BlueprintFunction, nullptr);
	TestEqual(TEXT("Unbind restores the Blueprint body"), Actor->BlueprintDeclaredCallCount, 3);
	FAvidScriptWasmSmokeResult StopResult;
	TestTrue(TEXT("Blueprint event session stops cleanly"), Session.StopAndUnload(StopResult));

	FGraphNodeCreator<UK2Node_CallFunction> SecondNodeCreator(*FunctionGraph);
	UK2Node_CallFunction* const SecondCallNode = SecondNodeCreator.CreateNode();
	SecondCallNode->FunctionReference.SetExternalMember(
		GET_FUNCTION_NAME_CHECKED(
			AAvidScriptBindingRuntimeProcessEventTestActor,
			RecordBlueprintDeclaredCall),
		AAvidScriptBindingRuntimeProcessEventTestActor::StaticClass());
	SecondNodeCreator.Finalize();
	if (!TestTrue(
			TEXT("Blueprint event bytecode mutation extends the body"),
			Schema->TryCreateConnection(
				CallNode->FindPinChecked(UEdGraphSchema_K2::PN_Then),
				SecondCallNode->FindPinChecked(UEdGraphSchema_K2::PN_Execute))))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	TSharedPtr<const FAvidScriptBindingPackage> StalePackage;
	FAvidScriptBindingPackageLoadResult StaleLoadResult;
	TestFalse(
		TEXT("Runtime rejects an event descriptor after Blueprint bytecode changes"),
		FAvidScriptBindingPackage::LoadDescriptor(
			DescriptorJson,
			StalePackage,
			StaleLoadResult));
	TestEqual(
		TEXT("Blueprint event recompile rejection has a stable category"),
		StaleLoadResult.ErrorCategory,
		FString(TEXT("binding_reflection_provenance_mismatch")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorDelegateActiveInvokeIntegrationTest,
	"AvidScript.Editor.BindingRuntime.DelegateActiveInvoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorDelegateActiveInvokeIntegrationTest::RunTest(
	const FString& Parameters)
{
	const FString OwnerPath =
		AAvidScriptEditorDelegateEventTestActor::StaticClass()->GetPathName();
	FAvidScriptBindingSelectionProfile Profile;
	Profile.PackageName = TEXT("avidscript.test.delegate_active_invoke");
	Profile.SelfClassPath = OwnerPath;
	Profile.ExplicitDelegateEvents.Add({
		OwnerPath,
		TEXT("OnScriptSignal"),
		TEXT("multicast"),
		TEXT("replace")
	});
	Profile.ExplicitDelegateEvents.Add({
		OwnerPath,
		TEXT("OnSinglecastSignal"),
		TEXT("singlecast"),
		TEXT("replace")
	});

	FString DescriptorJson;
	FAvidScriptBindingSelectionResolveResult SelectionResult;
	FAvidScriptBindingDescriptorGenerateResult GenerateResult;
	if (!TestTrue(
		TEXT("Active delegate descriptor generates from reflection"),
		FAvidScriptEditorBindingDescriptorGenerator::GenerateFromProfile(
			Profile,
			DescriptorJson,
			SelectionResult,
			GenerateResult)))
	{
		AddError(GenerateResult.ErrorMessage);
		return false;
	}

	FAvidScriptBindingPackageModel Descriptor;
	FString ParseCategory;
	FString ParseSource;
	if (!TestTrue(
		TEXT("Active delegate descriptor parses"),
		FAvidScriptBindingDescriptorParser::Parse(
			DescriptorJson,
			Descriptor,
			ParseCategory,
			ParseSource)))
	{
		AddError(ParseCategory + TEXT(": ") + ParseSource);
		return false;
	}

	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptBindingPackageLoadResult LoadResult;
	if (!TestTrue(
		TEXT("Active delegate package loads prepared plans"),
		FAvidScriptBindingPackage::LoadDescriptor(
			DescriptorJson,
			Package,
			LoadResult))
		|| !TestNotNull(TEXT("Active delegate package exists"), Package.Get()))
	{
		AddError(LoadResult.ErrorCategory + TEXT(": ")
			+ LoadResult.ErrorDetails);
		return false;
	}

	TArray<FAvidScriptPreparedDynamicBinding> InvokeBindings;
	FString BuildError;
	if (!TestTrue(
		TEXT("Package publishes both delegate invoke plans"),
		Package->BuildPreparedDynamicBindings(InvokeBindings, BuildError))
		|| !TestEqual(
			TEXT("Two delegate invoke plans are prepared"),
			InvokeBindings.Num(),
			2))
	{
		AddError(BuildError);
		return false;
	}

	const auto FindInvokeBinding = [&Descriptor, &InvokeBindings](
		const FString& Kind)
		-> const FAvidScriptPreparedDynamicBinding*
	{
		const FAvidScriptBindingDelegateEventModel* Event =
			Descriptor.DelegateEvents.FindByPredicate(
				[&Kind](const FAvidScriptBindingDelegateEventModel& Candidate)
				{
					return Candidate.DelegateKind == Kind;
				});
		FAvidScriptBindingDelegateInvokeSpec Spec;
		if (Event == nullptr
			|| !FAvidScriptBindingDescriptorIdentity::TryMakeDelegateInvokeSpec(
				*Event,
				0,
				Spec))
		{
			return nullptr;
		}
		return InvokeBindings.FindByPredicate(
			[&Spec](const FAvidScriptPreparedDynamicBinding& Binding)
			{
				return Binding.StableId == Spec.StableId;
			});
	};
	const FAvidScriptPreparedDynamicBinding* Singlecast =
		FindInvokeBinding(TEXT("singlecast"));
	const FAvidScriptPreparedDynamicBinding* Multicast =
		FindInvokeBinding(TEXT("multicast"));
	if (!TestNotNull(TEXT("Singlecast invoke binding resolves"), Singlecast)
		|| !TestNotNull(TEXT("Multicast broadcast binding resolves"), Multicast))
	{
		return false;
	}

	UWorld* World = nullptr;
	if (!TestTrue(
		TEXT("Delegate invocation world is created"),
		CreateAvidScriptBindingRuntimeIntegrationWorld(World)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroyAvidScriptBindingRuntimeIntegrationWorld(World);
	};
	AAvidScriptEditorDelegateEventTestActor* Receiver =
		World->SpawnActor<AAvidScriptEditorDelegateEventTestActor>();
	if (!TestNotNull(TEXT("Delegate invocation actor is spawned"), Receiver))
	{
		return false;
	}
	Receiver->OnSinglecastSignal.BindDynamic(
		Receiver,
		&AAvidScriptEditorDelegateEventTestActor::CaptureSinglecastSignal);
	Receiver->OnScriptSignal.AddDynamic(
		Receiver,
		&AAvidScriptEditorDelegateEventTestActor::CaptureScriptSignal);

	FAvidScriptBindingRuntimeTestGuestMemory GuestMemory(128);
	constexpr uint32 RefAddress = 16;
	constexpr uint32 OutAddress = 32;
	constexpr uint32 ReturnAddress = 48;
	GuestMemory.WriteValue<int32>(RefAddress, 5);
	FAvidScriptBindingInvocationContext InvocationContext;
	InvocationContext.World = World;
	InvocationContext.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(FMath::Max(1, Package->GetRequiredScratchSize()));
	FAvidScriptDynamicHostCallResult Result;
	const uint64 SinglecastArguments[] = {
		0,
		0,
		RefAddress,
		OutAddress,
		ReturnAddress
	};
	if (!TestTrue(
		TEXT("Prepared singlecast executes its bound native target"),
		Singlecast->Invoke(
			Singlecast->ImmutableInvocationCell,
			*Receiver,
			MakeArrayView(SinglecastArguments),
			&GuestMemory,
			InvocationContext,
			Scratch,
			Result)))
	{
		AddError(Result.Details);
		return false;
	}
	TestEqual(TEXT("Singlecast ref output commits"), GuestMemory.ReadValue<int32>(RefAddress), 8);
	TestEqual(TEXT("Singlecast out output commits"), GuestMemory.ReadValue<int32>(OutAddress), 16);
	TestEqual(TEXT("Singlecast return output commits"), GuestMemory.ReadValue<int32>(ReturnAddress), 18);
	TestEqual(TEXT("Singlecast target runs once"), Receiver->SinglecastInvocationCount, 1);

	uint64 MulticastArguments[] = {
		0,
		0,
		0,
		0,
		7,
		MakeAvidScriptBindingRuntimeF32Cell(1.5f)
	};
	if (!TestTrue(
		TEXT("Prepared multicast broadcasts to its bound native target"),
		Multicast->Invoke(
			Multicast->ImmutableInvocationCell,
			*Receiver,
			MakeArrayView(MulticastArguments),
			&GuestMemory,
			InvocationContext,
			Scratch,
			Result)))
	{
		AddError(Result.Details);
		return false;
	}
	TestEqual(TEXT("Multicast payload reaches the target"), Receiver->CapturedDelegateCount, 7);
	TestEqual(TEXT("Multicast float payload reaches the target"), Receiver->CapturedDelegateScale, 1.5f);

	static constexpr int32 WarmupCount = 3;
	static constexpr int32 SampleCount = 20;
	static constexpr int32 IterationsPerSample = 512;
	TArray<double> NativeSamples;
	TArray<double> PreparedSamples;
	NativeSamples.Reserve(SampleCount);
	PreparedSamples.Reserve(SampleCount);
	const FAvidScriptBindingPackageInstrumentation WarmInstrumentation =
		Package->GetInstrumentation();
	for (int32 RunIndex = 0;
		RunIndex < WarmupCount + SampleCount;
		++RunIndex)
	{
		const double NativeStart = FPlatformTime::Seconds();
		for (int32 Iteration = 0;
			Iteration < IterationsPerSample;
			++Iteration)
		{
			Receiver->OnScriptSignal.Broadcast(
				nullptr,
				RunIndex + Iteration,
				1.5f);
		}
		const double NativeMs =
			(FPlatformTime::Seconds() - NativeStart)
			* 1000.0 / IterationsPerSample;

		const double PreparedStart = FPlatformTime::Seconds();
		for (int32 Iteration = 0;
			Iteration < IterationsPerSample;
			++Iteration)
		{
			MulticastArguments[4] = RunIndex + Iteration;
			if (!Multicast->Invoke(
					Multicast->ImmutableInvocationCell,
					*Receiver,
					MakeArrayView(MulticastArguments),
					nullptr,
					InvocationContext,
					Scratch,
					Result))
			{
				AddError(Result.Details);
				return false;
			}
		}
		const double PreparedMs =
			(FPlatformTime::Seconds() - PreparedStart)
			* 1000.0 / IterationsPerSample;
		if (RunIndex >= WarmupCount)
		{
			NativeSamples.Add(NativeMs);
			PreparedSamples.Add(PreparedMs);
		}
	}
	const FAvidScriptBindingPackageInstrumentation FinalInstrumentation =
		Package->GetInstrumentation();
	const double NativeP50 =
		CalculateAvidScriptPropertyBenchmarkPercentile(NativeSamples, 0.50);
	const double NativeP95 =
		CalculateAvidScriptPropertyBenchmarkPercentile(NativeSamples, 0.95);
	const double PreparedP50 =
		CalculateAvidScriptPropertyBenchmarkPercentile(PreparedSamples, 0.50);
	const double PreparedP95 =
		CalculateAvidScriptPropertyBenchmarkPercentile(PreparedSamples, 0.95);
	TestTrue(TEXT("Native delegate P50 is sampled"), NativeP50 > 0.0);
	TestTrue(TEXT("Prepared delegate P50 is sampled"), PreparedP50 > 0.0);
	TestEqual(
		TEXT("Warm delegate calls perform no reflected-name lookup"),
		FinalInstrumentation.ReflectedNameLookupCount,
		WarmInstrumentation.ReflectedNameLookupCount);
	TestTrue(
		TEXT("Prepared delegate overhead remains bounded against UE dynamic broadcast"),
		PreparedP50 <= NativeP50 * 12.0);
	AddInfo(FString::Printf(
		TEXT("phase60_delegate_invoke_benchmark | samples=%d | iterations=%d | native_p50_ms=%.9f | native_p95_ms=%.9f | prepared_p50_ms=%.9f | prepared_p95_ms=%.9f | p50_ratio=%.6f | warm_reflected_name_lookups=0"),
		SampleCount,
		IterationsPerSample,
		NativeP50,
		NativeP95,
		PreparedP50,
		PreparedP95,
		PreparedP50 / NativeP50));

	Receiver->OnSinglecastSignal.Unbind();
	TestFalse(
		TEXT("Unbound singlecast fails closed"),
		Singlecast->Invoke(
			Singlecast->ImmutableInvocationCell,
			*Receiver,
			MakeArrayView(SinglecastArguments),
			&GuestMemory,
			InvocationContext,
			Scratch,
			Result));
	TestTrue(
		TEXT("Unbound singlecast reports a stable category"),
		Result.Details.Contains(TEXT("binding_delegate_unbound")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
