#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingInvocation.h"
#include "AvidScriptComponent.h"
#include "AvidScriptEditorBindingDescriptorGenerator.h"
#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "AvidScriptEditorCSharpWorkspaceService.h"
#include "AvidScriptEditorResultPresentation.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{

uint64 MakeAvidScriptBindingRuntimeF32Cell(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
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

	bool bAcceptPrepare = false;
	int32 PrepareCallCount = 0;
	FAvidScriptObjectRegistry* LastRegistry = nullptr;
	FAvidScriptObjectHandle LastHandle;
	UObject* LastTarget = nullptr;
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
		*FString::Printf(TEXT("%s authorization ceiling contains 117 generated bindings"), *BuildLabel),
		AuthorizationImports->Num(),
		117);

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
		*FString::Printf(TEXT("%s runtime package contains five reachable bindings"), *BuildLabel),
		OutManifest.BindingPackage->GetVmPackage().Imports.Num(),
		5);
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
		*FString::Printf(TEXT("%s authorization ceiling contains 117 gameplay bindings"), *BuildLabel),
		AuthorizationImports->Num(),
		117);

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
		*FString::Printf(TEXT("%s runtime package contains three reachable bindings"), *BuildLabel),
		Manifest.BindingPackage->GetVmPackage().Imports.Num(),
		3);
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

	const FString TamperedJson = DescriptorJson.Replace(
		TEXT("\"cpp_type\": \"float\""),
		TEXT("\"cpp_type\": \"int32\""),
		ESearchCase::CaseSensitive);
	TestFalse(TEXT("Scalar cpp_type metadata was changed"), TamperedJson == DescriptorJson);

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
		TEXT("Readable Actor property generates a schema v4 package"),
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
	TestEqual(TEXT("Property package exposes one cached import"), Package->GetVmPackage().Imports.Num(), 1);

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
		TEXT("Generated C# manifest records a five-binding runtime profile"),
		static_cast<int32>((*BindingPackageObject)->GetIntegerField(TEXT("profile_import_count"))),
		5);
	TestEqual(
		TEXT("Generated C# manifest records five used binding stable identities"),
		static_cast<int32>((*BindingPackageObject)->GetIntegerField(TEXT("used_import_count"))),
		5);
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

#endif // WITH_DEV_AUTOMATION_TESTS
