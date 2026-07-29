#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptBindingReloadEffect.h"
#include "AvidScriptGameplayEvent.h"
#include "AvidScriptObjectFactoryPolicy.h"
#include "AvidScriptObjectRegistryTestTypes.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"
#include "AvidScriptWasmRuntimePrivate.h"
#include "Session/AvidScriptRuntimeEventRouter.h"
#include "Session/AvidScriptRuntimeScheduler.h"
#include "StateMigration/AvidScriptRuntimeStateMigration.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Misc/EngineVersion.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
EAvidScriptVmTypedHostStatus GeneratedSessionPairCall(
	UObject& Receiver,
	const int32 Left,
	const int32 Right,
	int32& OutValue)
{
	static_cast<void>(Receiver);
	OutValue = Left + Right;
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

EAvidScriptVmTypedHostStatus GeneratedSessionPropertyCall(
	UObject& Receiver,
	const bool bWrite,
	int32& InOutValue)
{
	static_cast<void>(Receiver);
	static_cast<void>(bWrite);
	static_cast<void>(InOutValue);
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

class FGeneratedHostEffectJournal final
	: public IAvidScriptBindingHostEffectJournal
{
public:
	bool PrepareEffect(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& Handle,
		UObject& Target,
		const EAvidScriptBindingReloadEffect Effect,
		FAvidScriptBindingHostEffectPrepareResult& OutResult) override
	{
		static_cast<void>(Registry);
		static_cast<void>(Target);
		++EffectPrepareCount;
		LastHandle = Handle;
		LastEffect = Effect;
		OutResult.bSucceeded = true;
		return true;
	}

	bool PrepareReflectedProperty(
		FAvidScriptObjectRegistry& Registry,
		const FAvidScriptObjectHandle& Handle,
		UObject& Target,
		FProperty& Property,
		FAvidScriptBindingHostEffectPrepareResult& OutResult) override
	{
		static_cast<void>(Registry);
		static_cast<void>(Target);
		++PropertyPrepareCount;
		LastHandle = Handle;
		LastProperty = &Property;
		OutResult.bSucceeded = true;
		return true;
	}

	int32 EffectPrepareCount = 0;
	int32 PropertyPrepareCount = 0;
	FAvidScriptObjectHandle LastHandle;
	EAvidScriptBindingReloadEffect LastEffect =
		EAvidScriptBindingReloadEffect::None;
	FProperty* LastProperty = nullptr;
};

class FPreparedExportCacheTestBackend final
	: public IAvidScriptVmBackend
{
public:
	enum class EPrepareMode
	{
		Supported,
		Unsupported,
		Failed
	};

	const FAvidScriptVmBackendInfo& GetBackendInfo() const override
	{
		return BackendInfo;
	}

	bool Load(
		TArrayView<const uint8> Bytecode,
		const FString& ModuleId,
		const FAvidScriptVmLoadConfig& Config,
		FAvidScriptVmError& OutError) override
	{
		static_cast<void>(Bytecode);
		static_cast<void>(ModuleId);
		static_cast<void>(Config);
		OutError.Reset();
		return true;
	}

	bool ResolveExport(
		const FString& ExportName,
		FAvidScriptVmExportHandle& OutHandle,
		FAvidScriptVmError& OutError) override
	{
		static_cast<void>(ExportName);
		OutError.Reset();
		OutHandle = Handle;
		return true;
	}

	bool PrepareExportCall(
		const FAvidScriptVmExportHandle& InHandle,
		FAvidScriptVmPreparedExportCall& OutCall,
		FAvidScriptVmError& OutError) override
	{
		static_cast<void>(InHandle);
		OutCall = FAvidScriptVmPreparedExportCall();
		OutError.Reset();
		if (PrepareMode == EPrepareMode::Unsupported)
		{
			OutError.Category = TEXT("prepared_export_unsupported");
			OutError.Details = TEXT("test backend has no prepared path");
			return false;
		}
		if (PrepareMode == EPrepareMode::Failed)
		{
			OutError.Category = TEXT("stale_export");
			OutError.Details = TEXT("test backend rejected a stale handle");
			return false;
		}
		OutCall.Owner = this;
		OutCall.Target = this;
		OutCall.InvokeFunction = &InvokePrepared;
		OutCall.ParameterCellCount = PreparedParameterCellCount;
		return true;
	}

	bool Call(
		const FAvidScriptVmExportHandle& InHandle,
		const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult) override
	{
		static_cast<void>(InHandle);
		static_cast<void>(Frame);
		OutError.Reset();
		if (OutResult != nullptr)
		{
			*OutResult = FAvidScriptVmCallResult();
		}
		return true;
	}

	void Unload() override {}
	bool IsLoaded() const override { return true; }
	uint32 GetExportLookupCount() const override { return 0; }
	const FAvidScriptVmLoadMetrics& GetLoadMetrics() const override
	{
		return LoadMetrics;
	}

	static bool InvokePrepared(
		void* Owner,
		void* Target,
		const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult)
	{
		static_cast<void>(Owner);
		static_cast<void>(Target);
		static_cast<void>(Frame);
		OutError.Reset();
		if (OutResult != nullptr)
		{
			*OutResult = FAvidScriptVmCallResult();
		}
		return true;
	}

	EPrepareMode PrepareMode = EPrepareMode::Supported;
	uint32 PreparedParameterCellCount = 0;
	FAvidScriptVmExportHandle Handle{1, 1, 1};

private:
	FAvidScriptVmBackendInfo BackendInfo;
	FAvidScriptVmLoadMetrics LoadMetrics;
};

const uint8 GSessionCompatibleModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01,
	0x05, 0x03, 0x01, 0x00, 0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x07,
	0x02, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

const uint8 GSessionPackedOwnerImportModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x0c, 0x03, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x60, 0x00, 0x01, 0x7e,
	0x02, 0x24, 0x01, 0x0a, 0x61, 0x76, 0x69, 0x64,
	0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x15, 0x61,
	0x76, 0x69, 0x64, 0x5f, 0x6f, 0x77, 0x6e, 0x65,
	0x72, 0x5f, 0x67, 0x65, 0x74, 0x5f, 0x68, 0x61,
	0x6e, 0x64, 0x6c, 0x65, 0x00, 0x02,
	0x03, 0x03, 0x02, 0x00, 0x01,
	0x07, 0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64,
	0x5f, 0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69,
	0x6e, 0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x01,
	0x0c, 0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e,
	0x5f, 0x74, 0x69, 0x63, 0x6b, 0x00, 0x02,
	0x0a, 0x07, 0x02, 0x02, 0x00, 0x0b, 0x02, 0x00,
	0x0b
};

const uint8 GSessionBeginTrapModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x08,
	0x02, 0x03, 0x00, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

FAvidScriptWasmStateSlot MakeSessionStateSlot(
	const TCHAR* StableId,
	const TCHAR* Fingerprint,
	uint32 Offset,
	TArray<FString> Aliases = {})
{
	FAvidScriptWasmStateSlot Slot;
	Slot.StableId = StableId;
	Slot.Aliases = MoveTemp(Aliases);
	Slot.TypeFingerprint = Fingerprint;
	Slot.Offset = Offset;
	Slot.Size = 4;
	Slot.Alignment = 4;
	return Slot;
}

FAvidScriptBindingTypeModel MakeSessionOwnerType(
	const TCHAR* ClassPath,
	const TCHAR* CppType,
	const int32 ObjectTypeOrdinal,
	const FString& BaseTypeId)
{
	FAvidScriptBindingTypeModel Type;
	Type.CanonicalType = TEXT("object:") + FString(ClassPath);
	Type.StableId = FAvidScriptBindingDescriptorIdentity::MakeTypeStableId(Type.CanonicalType, {});
	Type.Kind = TEXT("object_handle");
	Type.CppType = CppType;
	Type.Size = 8;
	Type.Alignment = 4;
	Type.AbiTypes = { TEXT("i"), TEXT("i") };
	Type.ObjectTypeOrdinal = ObjectTypeOrdinal;
	Type.ClassPath = ClassPath;
	Type.BaseTypeId = BaseTypeId;
	return Type;
}

bool MakeTypedOwnerSessionPackage(
	const bool bExpectPawn,
	TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
	FString& OutError)
{
	FAvidScriptBindingPackageModel Model;
	Model.SchemaVersion = 6;
	Model.GeneratorVersion = TEXT("50.0.session_test");
	Model.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Model.Source = TEXT("ue_reflection");
	Model.PackageName = bExpectPawn
		? TEXT("avidscript.test.session_typed_pawn")
		: TEXT("avidscript.test.session_typed_actor");

	const FAvidScriptBindingTypeModel ObjectType = MakeSessionOwnerType(
		TEXT("/Script/CoreUObject.Object"),
		TEXT("UObject"),
		0,
		FString());
	const FAvidScriptBindingTypeModel ActorType = MakeSessionOwnerType(
		TEXT("/Script/Engine.Actor"),
		TEXT("AActor"),
		1,
		ObjectType.StableId);
	Model.Types = { ObjectType, ActorType };
	FAvidScriptBindingTypeModel ExpectedType = ActorType;
	if (bExpectPawn)
	{
		ExpectedType = MakeSessionOwnerType(
			TEXT("/Script/Engine.Pawn"),
			TEXT("APawn"),
			2,
			ActorType.StableId);
		Model.Types.Add(ExpectedType);
	}
	Model.SelfTypeId = ExpectedType.StableId;

	FAvidScriptBindingClassReferenceModel Reference;
	Reference.Ordinal = 0;
	Reference.ScriptName = bExpectPawn ? TEXT("PawnClass") : TEXT("ActorClass");
	Reference.ClassPath = ExpectedType.ClassPath;
	Reference.BaseClassPath = ExpectedType.ClassPath;
	Reference.LoadPolicy = TEXT("EditorLoad");
	Reference.ResultTypeId = ExpectedType.StableId;
	Reference.StableId = FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
		Reference.ClassPath,
		Reference.BaseClassPath,
		Reference.LoadPolicy);
	Model.ClassReferences.Add(Reference);
	Model.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Model);
	Model.PackageHash = FAvidScriptBindingDescriptorIdentity::MakePackageHash(Model);

	TArray<FString> TypeJsonEntries;
	for (const FAvidScriptBindingTypeModel& Type : Model.Types)
	{
		TypeJsonEntries.Add(FString::Printf(
			TEXT("{\"stable_id\":\"%s\",\"canonical_type\":\"%s\",\"kind\":\"object_handle\",\"cpp_type\":\"%s\",\"size\":8,\"alignment\":4,\"abi_types\":[\"i\",\"i\"],\"object_type_ordinal\":%d,\"class_path\":\"%s\",\"base_type_id\":\"%s\"}"),
			*Type.StableId,
			*Type.CanonicalType,
			*Type.CppType,
			Type.ObjectTypeOrdinal,
			*Type.ClassPath,
			*Type.BaseTypeId));
	}
	const FString DescriptorJson = FString::Printf(
		TEXT("{\"schema_version\":6,\"generator_version\":\"%s\",\"engine_version\":\"%s\",\"source\":\"ue_reflection\",\"package_name\":\"%s\",\"package_hash\":\"%s\",\"selection_hash\":\"%s\",\"self_type_id\":\"%s\",\"types\":[%s],\"class_references\":[{\"stable_id\":\"%s\",\"ordinal\":0,\"script_name\":\"%s\",\"class_path\":\"%s\",\"base_class_path\":\"%s\",\"load_policy\":\"EditorLoad\",\"result_type_id\":\"%s\"}],\"bindings\":[]}"),
		*Model.GeneratorVersion,
		*Model.EngineVersion,
		*Model.PackageName,
		*Model.PackageHash,
		*Model.SelectionHash,
		*Model.SelfTypeId,
		*FString::Join(TypeJsonEntries, TEXT(",")),
		*Reference.StableId,
		*Reference.ScriptName,
		*Reference.ClassPath,
		*Reference.BaseClassPath,
		*Reference.ResultTypeId);

	FAvidScriptBindingPackageLoadResult LoadResult;
	if (FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, OutPackage, LoadResult))
	{
		return true;
	}

	OutError = LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails;
	return false;
}

bool MakeLegacySessionPackage(
	TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
	FString& OutError)
{
	FAvidScriptBindingPackageModel Model;
	Model.SchemaVersion = 5;
	Model.GeneratorVersion = TEXT("50.0.session_test");
	Model.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Model.Source = TEXT("ue_reflection");
	Model.PackageName = TEXT("avidscript.test.session_legacy");

	FAvidScriptBindingClassReferenceModel Reference;
	Reference.Ordinal = 0;
	Reference.ScriptName = TEXT("ActorClass");
	Reference.ClassPath = TEXT("/Script/Engine.Actor");
	Reference.BaseClassPath = TEXT("/Script/Engine.Actor");
	Reference.LoadPolicy = TEXT("EditorLoad");
	Reference.StableId = FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
		Reference.ClassPath,
		Reference.BaseClassPath,
		Reference.LoadPolicy);
	Model.ClassReferences.Add(Reference);
	Model.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Model);
	Model.PackageHash = FAvidScriptBindingDescriptorIdentity::MakePackageHash(Model);

	const FString DescriptorJson = FString::Printf(
		TEXT("{\"schema_version\":5,\"generator_version\":\"%s\",\"engine_version\":\"%s\",\"source\":\"ue_reflection\",\"package_name\":\"%s\",\"package_hash\":\"%s\",\"selection_hash\":\"%s\",\"types\":[],\"class_references\":[{\"stable_id\":\"%s\",\"ordinal\":0,\"script_name\":\"ActorClass\",\"class_path\":\"/Script/Engine.Actor\",\"base_class_path\":\"/Script/Engine.Actor\",\"load_policy\":\"EditorLoad\"}],\"bindings\":[]}"),
		*Model.GeneratorVersion,
		*Model.EngineVersion,
		*Model.PackageName,
		*Model.PackageHash,
		*Model.SelectionHash,
		*Reference.StableId);

	FAvidScriptBindingPackageLoadResult LoadResult;
	if (FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, OutPackage, LoadResult))
	{
		return true;
	}

	OutError = LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails;
	return false;
}

FAvidScriptWasmReloadManifest MakeSessionManifest(
	const FString& ModuleId,
	const TSharedPtr<const FAvidScriptBindingPackage>& Package)
{
	FAvidScriptWasmReloadManifest Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(ModuleId);
	Manifest.Language = TEXT("CSharp");
	Manifest.WasmFile = TEXT("Saved/AvidScript/") + ModuleId + TEXT(".wasm");
	Manifest.WasmSha256 = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	Manifest.BindingPackageName = Package->GetPackageName();
	Manifest.BindingPackageHash = Package->GetPackageHash();
	Manifest.BindingPackageManifestFile = TEXT("Saved/AvidScript/") + ModuleId + TEXT(".bindings.json");
	Manifest.BindingPackageManifestSha256 = TEXT("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	Manifest.BindingDescriptorFile = TEXT("Saved/AvidScript/") + ModuleId + TEXT(".descriptor.json");
	Manifest.BindingDescriptorSha256 = TEXT("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
	Manifest.DebugMapFile = TEXT("Saved/AvidScript/") + ModuleId + TEXT(".debug.json");
	Manifest.DebugMapSha256 = TEXT("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
	Manifest.BindingPackage = Package;
	return Manifest;
}

bool CreateSessionOwnerWorld(UWorld*& OutWorld, AActor*& OutOwner)
{
	OutWorld = nullptr;
	OutOwner = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptTypedOwnerSessionWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	OutOwner = OutWorld->SpawnActor<AActor>();
	return OutOwner != nullptr;
}

void DestroySessionOwnerWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}
	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}
	World->DestroyWorld(false);
	World = nullptr;
}

void TestNoOwnerValidationTransaction(
	FAutomationTestBase& Test,
	const TCHAR* Prefix,
	const FAvidScriptWasmReloadResult& Result)
{
	Test.TestFalse(*FString::Printf(TEXT("%s does not report success"), Prefix), Result.bSucceeded);
	Test.TestFalse(*FString::Printf(TEXT("%s does not apply reload"), Prefix), Result.bReloadApplied);
	Test.TestFalse(*FString::Printf(TEXT("%s does not open host-effect transaction"), Prefix), Result.bHostEffectTransactionAttempted);
	Test.TestFalse(*FString::Printf(TEXT("%s does not commit host-effect transaction"), Prefix), Result.bHostEffectTransactionCommitted);
	Test.TestFalse(*FString::Printf(TEXT("%s does not attempt host-effect rollback"), Prefix), Result.bHostEffectRollbackAttempted);
	Test.TestFalse(*FString::Printf(TEXT("%s does not report host-effect rollback success"), Prefix), Result.bHostEffectRollbackSucceeded);
	Test.TestEqual(*FString::Printf(TEXT("%s captures zero host effects"), Prefix), Result.HostEffectCapturedObjectCount, 0);
	Test.TestEqual(*FString::Printf(TEXT("%s restores zero host effects"), Prefix), Result.HostEffectRestoredObjectCount, 0);
	Test.TestEqual(*FString::Printf(TEXT("%s fails zero host effects"), Prefix), Result.HostEffectFailedObjectCount, 0);
	Test.TestTrue(*FString::Printf(TEXT("%s has no host-effect source"), Prefix), Result.HostEffectErrorSource.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s does not attempt state migration"), Prefix), Result.bStateMigrationAttempted);
	Test.TestFalse(*FString::Printf(TEXT("%s does not apply state migration"), Prefix), Result.bStateMigrationApplied);
	Test.TestEqual(*FString::Printf(TEXT("%s migrates zero state slots"), Prefix), Result.StateMigrationMigratedSlotCount, 0);
	Test.TestEqual(*FString::Printf(TEXT("%s migrates zero state bytes"), Prefix), Result.StateMigrationMigratedByteCount, 0);
	Test.TestEqual(*FString::Printf(TEXT("%s skips zero state slots"), Prefix), Result.StateMigrationSkippedSlotCount, 0);
	Test.TestEqual(*FString::Printf(TEXT("%s aliases zero state slots"), Prefix), Result.StateMigrationAliasedSlotCount, 0);
	Test.TestTrue(*FString::Printf(TEXT("%s has no state migration stable id"), Prefix), Result.StateMigrationStableId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s candidate BeginPlay never ran"), Prefix), Result.RuntimeResult.bBeginPlayCalled);
}

void TestLiveManifestPreserved(
	FAutomationTestBase& Test,
	const FAvidScriptRuntimeSessionTestSnapshot& Before,
	const FAvidScriptRuntimeSessionTestSnapshot& After)
{
	Test.TestEqual(TEXT("Live manifest module id is preserved"), After.LiveManifest.ModuleId, Before.LiveManifest.ModuleId);
	Test.TestEqual(TEXT("Live manifest ABI is preserved"), After.LiveManifest.AbiVersion, Before.LiveManifest.AbiVersion);
	Test.TestEqual(TEXT("Live manifest language is preserved"), After.LiveManifest.Language, Before.LiveManifest.Language);
	Test.TestEqual(TEXT("Live manifest wasm file is preserved"), After.LiveManifest.WasmFile, Before.LiveManifest.WasmFile);
	Test.TestEqual(TEXT("Live manifest wasm hash is preserved"), After.LiveManifest.WasmSha256, Before.LiveManifest.WasmSha256);
	Test.TestTrue(TEXT("Live manifest exports are preserved"), After.LiveManifest.RequiredExports == Before.LiveManifest.RequiredExports);
	Test.TestEqual(TEXT("Live manifest import count is preserved"), After.LiveManifest.RequiredImports.Num(), Before.LiveManifest.RequiredImports.Num());
	const int32 ComparableImportCount = FMath::Min(
		After.LiveManifest.RequiredImports.Num(),
		Before.LiveManifest.RequiredImports.Num());
	for (int32 ImportIndex = 0; ImportIndex < ComparableImportCount; ++ImportIndex)
	{
		Test.TestEqual(
			*FString::Printf(TEXT("Live manifest import %d module is preserved"), ImportIndex),
			After.LiveManifest.RequiredImports[ImportIndex].ModuleName,
			Before.LiveManifest.RequiredImports[ImportIndex].ModuleName);
		Test.TestEqual(
			*FString::Printf(TEXT("Live manifest import %d name is preserved"), ImportIndex),
			After.LiveManifest.RequiredImports[ImportIndex].ImportName,
			Before.LiveManifest.RequiredImports[ImportIndex].ImportName);
	}
	Test.TestEqual(TEXT("Live manifest package name is preserved"), After.LiveManifest.BindingPackageName, Before.LiveManifest.BindingPackageName);
	Test.TestEqual(TEXT("Live manifest package hash is preserved"), After.LiveManifest.BindingPackageHash, Before.LiveManifest.BindingPackageHash);
	Test.TestEqual(TEXT("Live manifest package file is preserved"), After.LiveManifest.BindingPackageManifestFile, Before.LiveManifest.BindingPackageManifestFile);
	Test.TestEqual(TEXT("Live manifest package file hash is preserved"), After.LiveManifest.BindingPackageManifestSha256, Before.LiveManifest.BindingPackageManifestSha256);
	Test.TestEqual(TEXT("Live manifest descriptor file is preserved"), After.LiveManifest.BindingDescriptorFile, Before.LiveManifest.BindingDescriptorFile);
	Test.TestEqual(TEXT("Live manifest descriptor hash is preserved"), After.LiveManifest.BindingDescriptorSha256, Before.LiveManifest.BindingDescriptorSha256);
	Test.TestEqual(TEXT("Live manifest debug-map file is preserved"), After.LiveManifest.DebugMapFile, Before.LiveManifest.DebugMapFile);
	Test.TestEqual(TEXT("Live manifest debug-map hash is preserved"), After.LiveManifest.DebugMapSha256, Before.LiveManifest.DebugMapSha256);
	Test.TestEqual(TEXT("Live state migration strategy is preserved"), After.LiveManifest.StateMigration.Strategy, Before.LiveManifest.StateMigration.Strategy);
	Test.TestEqual(TEXT("Live state migration schema is preserved"), After.LiveManifest.StateMigration.SchemaVersion, Before.LiveManifest.StateMigration.SchemaVersion);
	Test.TestEqual(TEXT("Live state migration policy is preserved"), After.LiveManifest.StateMigration.Policy, Before.LiveManifest.StateMigration.Policy);
	Test.TestEqual(TEXT("Live state migration contract is preserved"), After.LiveManifest.StateMigration.ContractVersion, Before.LiveManifest.StateMigration.ContractVersion);
	Test.TestEqual(TEXT("Live state migration owner is preserved"), After.LiveManifest.StateMigration.OwnerTypeId, Before.LiveManifest.StateMigration.OwnerTypeId);
	Test.TestEqual(TEXT("Live state migration slot count is preserved"), After.LiveManifest.StateMigration.Slots.Num(), Before.LiveManifest.StateMigration.Slots.Num());
	Test.TestTrue(TEXT("Live binding package identity is preserved"), After.LiveManifest.BindingPackage.Get() == Before.LiveManifest.BindingPackage.Get());
	Test.TestTrue(TEXT("Live debug-map identity is preserved"), After.LiveManifest.DebugMap.Get() == Before.LiveManifest.DebugMap.Get());
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimePreparedExportCacheContractTest,
	"AvidScript.Runtime.Session.PreparedExportCacheContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimePreparedExportCacheContractTest::RunTest(
	const FString& Parameters)
{
	FPreparedExportCacheTestBackend Backend;
	FAvidScriptCachedVmExport CachedExport;
	FAvidScriptVmError Error;

	Backend.PrepareMode =
		FPreparedExportCacheTestBackend::EPrepareMode::Unsupported;
	TestTrue(
		TEXT("unsupported prepared calls fall back to the generic handle"),
		AvidScriptWasmRuntimePrivate::CacheResolvedVmExport(
			Backend,
			Backend.Handle,
			0,
			CachedExport,
			Error));
	TestTrue(
		TEXT("unsupported fallback retains the resolved handle"),
		CachedExport.Handle.IsValid());
	TestFalse(
		TEXT("unsupported fallback does not publish a prepared call"),
		CachedExport.PreparedCall.IsValid());
	TestTrue(
		TEXT("unsupported fallback consumes its diagnostic"),
		Error.Category.IsEmpty());

	Backend.PrepareMode =
		FPreparedExportCacheTestBackend::EPrepareMode::Failed;
	TestFalse(
		TEXT("non-unsupported prepare failures fail closed"),
		AvidScriptWasmRuntimePrivate::CacheResolvedVmExport(
			Backend,
			Backend.Handle,
			0,
			CachedExport,
			Error));
	TestEqual(
		TEXT("prepare failure category is propagated"),
		Error.Category,
		FString(TEXT("stale_export")));
	TestFalse(
		TEXT("failed preparation does not retain a generic handle"),
		CachedExport.Handle.IsValid());

	Backend.PrepareMode =
		FPreparedExportCacheTestBackend::EPrepareMode::Supported;
	Backend.PreparedParameterCellCount = 1;
	TestFalse(
		TEXT("prepared signature mismatch fails at cache time"),
		AvidScriptWasmRuntimePrivate::CacheResolvedVmExport(
			Backend,
			Backend.Handle,
			0,
			CachedExport,
			Error));
	TestEqual(
		TEXT("prepared signature mismatch uses invalid_export"),
		Error.Category,
		FString(TEXT("invalid_export")));

	Backend.PreparedParameterCellCount = 0;
	TestTrue(
		TEXT("matching prepared signature caches successfully"),
		AvidScriptWasmRuntimePrivate::CacheResolvedVmExport(
			Backend,
			Backend.Handle,
			0,
			CachedExport,
			Error));
	TestTrue(
		TEXT("matching prepared signature publishes a valid call"),
		CachedExport.PreparedCall.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeStateMigrationServiceTest,
	"AvidScript.Architecture.Session.StateMigrationService",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeStateMigrationServiceTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance PreviousRuntime;
	FAvidScriptWasmRuntimeInstance CandidateRuntime;
	FAvidScriptWasmSmokeResult RuntimeResult;
	TestTrue(TEXT("Previous state runtime loads"), PreviousRuntime.LoadModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		TEXT("state_service"),
		RuntimeResult));
	TestTrue(TEXT("Candidate state runtime loads"), CandidateRuntime.LoadModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		TEXT("state_service"),
		RuntimeResult));

	const TArray<uint8> ScoreBytes = { 0x00, 0x00, 0x60, 0x40 };
	const TArray<uint8> RemovedBytes = { 0x01, 0x02, 0x03, 0x04 };
	FString MemoryError;
	TestTrue(TEXT("Previous score writes"), PreviousRuntime.WriteStateBytes(16, ScoreBytes, MemoryError));
	TestTrue(TEXT("Previous removed state writes"), PreviousRuntime.WriteStateBytes(20, RemovedBytes, MemoryError));

	const TCHAR* FloatFingerprint = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	FAvidScriptWasmReloadManifest PreviousManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_service"));
	PreviousManifest.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	PreviousManifest.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	PreviousManifest.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:removed"), FloatFingerprint, 20),
		MakeSessionStateSlot(TEXT("global:score"), FloatFingerprint, 16),
	};
	FAvidScriptWasmReloadManifest CandidateManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_service"));
	CandidateManifest.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	CandidateManifest.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	CandidateManifest.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:added"), FloatFingerprint, 36),
		MakeSessionStateSlot(TEXT("global:score"), FloatFingerprint, 32),
	};

	FAvidScriptRuntimeStateMigrationResult MigrationResult;
	TestTrue(TEXT("Compatible state migrates"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		PreviousManifest,
		CandidateRuntime,
		CandidateManifest,
		MigrationResult));
	TestTrue(TEXT("Migration is attempted"), MigrationResult.bAttempted);
	TestEqual(TEXT("One compatible slot migrates"), MigrationResult.MigratedSlotCount, 1);
	TestEqual(TEXT("Four bytes migrate"), MigrationResult.MigratedByteCount, 4);
	TestEqual(TEXT("Added and removed slots are skipped"), MigrationResult.SkippedSlotCount, 2);
	TArray<uint8> MigratedScore;
	MigratedScore.SetNumZeroed(4);
	TestTrue(TEXT("Candidate migrated score reads"), CandidateRuntime.ReadStateBytes(32, MigratedScore, MemoryError));
	TestEqual(TEXT("Candidate receives exact score bytes"), MigratedScore, ScoreBytes);

	FAvidScriptWasmReloadManifest IncompatibleManifest = CandidateManifest;
	IncompatibleManifest.StateMigration.Slots[1].TypeFingerprint =
		TEXT("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	TestFalse(TEXT("Incompatible stable type is rejected"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		PreviousManifest,
		CandidateRuntime,
		IncompatibleManifest,
		MigrationResult));
	TestEqual(TEXT("Incompatible state category"), MigrationResult.ErrorCategory, FString(TEXT("state_migration_incompatible")));
	TestEqual(TEXT("Incompatible state identifies stable id"), MigrationResult.StableId, FString(TEXT("global:score")));

	FAvidScriptWasmReloadManifest ExactPreferredCandidate = CandidateManifest;
	ExactPreferredCandidate.StateMigration.ContractVersion = 2;
	ExactPreferredCandidate.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:score"), FloatFingerprint, 40, { TEXT("global:removed") }),
		MakeSessionStateSlot(TEXT("global:renamed"), FloatFingerprint, 44, { TEXT("global:score") }),
	};
	FAvidScriptWasmReloadManifest ExactPreferredPrevious = PreviousManifest;
	ExactPreferredPrevious.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:score"), FloatFingerprint, 16),
	};
	TestTrue(TEXT("Exact primary match takes priority over alias"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		ExactPreferredPrevious,
		CandidateRuntime,
		ExactPreferredCandidate,
		MigrationResult));
	TestEqual(TEXT("Exact primary does not report alias match"), MigrationResult.AliasedSlotCount, 0);
	TArray<uint8> ExactBytes;
	ExactBytes.SetNumZeroed(4);
	TestTrue(TEXT("Exact primary destination reads"), CandidateRuntime.ReadStateBytes(40, ExactBytes, MemoryError));
	TestEqual(TEXT("Exact primary destination receives score"), ExactBytes, ScoreBytes);

	FAvidScriptWasmReloadManifest RenameAliasCandidate = CandidateManifest;
	RenameAliasCandidate.StateMigration.ContractVersion = 2;
	RenameAliasCandidate.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:renamed_score"), FloatFingerprint, 48, { TEXT("global:score") }),
	};
	TestTrue(TEXT("Rename alias migrates state"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		PreviousManifest,
		CandidateRuntime,
		RenameAliasCandidate,
		MigrationResult));
	TestEqual(TEXT("Rename alias count"), MigrationResult.AliasedSlotCount, 1);
	TArray<uint8> RenamedBytes;
	RenamedBytes.SetNumZeroed(4);
	TestTrue(TEXT("Rename alias destination reads"), CandidateRuntime.ReadStateBytes(48, RenamedBytes, MemoryError));
	TestEqual(TEXT("Rename alias destination receives score"), RenamedBytes, ScoreBytes);

	const TArray<uint8> AliasClaimOriginal = { 0x41, 0x42, 0x43, 0x44 };
	TestTrue(TEXT("Alias claim destination initializes"), CandidateRuntime.WriteStateBytes(60, AliasClaimOriginal, MemoryError));
	FAvidScriptWasmReloadManifest AliasClaimCandidate = CandidateManifest;
	AliasClaimCandidate.StateMigration.ContractVersion = 2;
	AliasClaimCandidate.StateMigration.Slots = {
		MakeSessionStateSlot(
			TEXT("global:renamed_combined"),
			FloatFingerprint,
			60,
			{ TEXT("global:removed"), TEXT("global:score") }),
	};
	TestFalse(TEXT("Two aliases cannot claim one candidate slot"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		PreviousManifest,
		CandidateRuntime,
		AliasClaimCandidate,
		MigrationResult));
	TestEqual(TEXT("Alias claim conflict category"), MigrationResult.ErrorCategory, FString(TEXT("state_migration_incompatible")));
	TestEqual(TEXT("Alias claim conflict identifies second previous id"), MigrationResult.StableId, FString(TEXT("global:score")));
	TArray<uint8> AliasClaimAfterFailure;
	AliasClaimAfterFailure.SetNumZeroed(4);
	TestTrue(TEXT("Alias claim destination reads after rejection"), CandidateRuntime.ReadStateBytes(60, AliasClaimAfterFailure, MemoryError));
	TestEqual(TEXT("Alias claim rejection leaves candidate bytes unchanged"), AliasClaimAfterFailure, AliasClaimOriginal);

	const TArray<uint8> ExactAliasClaimOriginal = { 0x51, 0x52, 0x53, 0x54 };
	TestTrue(TEXT("Exact alias claim destination initializes"), CandidateRuntime.WriteStateBytes(64, ExactAliasClaimOriginal, MemoryError));
	FAvidScriptWasmReloadManifest ExactAliasClaimPrevious = PreviousManifest;
	ExactAliasClaimPrevious.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:score"), FloatFingerprint, 16),
		MakeSessionStateSlot(TEXT("global:removed"), FloatFingerprint, 20),
	};
	FAvidScriptWasmReloadManifest ExactAliasClaimCandidate = CandidateManifest;
	ExactAliasClaimCandidate.StateMigration.ContractVersion = 2;
	ExactAliasClaimCandidate.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:score"), FloatFingerprint, 64, { TEXT("global:removed") }),
	};
	TestFalse(TEXT("Exact and alias cannot claim one candidate slot"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		ExactAliasClaimPrevious,
		CandidateRuntime,
		ExactAliasClaimCandidate,
		MigrationResult));
	TestEqual(TEXT("Exact alias claim conflict category"), MigrationResult.ErrorCategory, FString(TEXT("state_migration_incompatible")));
	TestEqual(TEXT("Exact alias claim conflict identifies second previous id"), MigrationResult.StableId, FString(TEXT("global:removed")));
	TArray<uint8> ExactAliasClaimAfterFailure;
	ExactAliasClaimAfterFailure.SetNumZeroed(4);
	TestTrue(TEXT("Exact alias claim destination reads after rejection"), CandidateRuntime.ReadStateBytes(64, ExactAliasClaimAfterFailure, MemoryError));
	TestEqual(TEXT("Exact alias claim rejection leaves candidate bytes unchanged"), ExactAliasClaimAfterFailure, ExactAliasClaimOriginal);

	FAvidScriptWasmReloadManifest VersionRegressionCandidate = CandidateManifest;
	PreviousManifest.StateMigration.ContractVersion = 2;
	VersionRegressionCandidate.StateMigration.ContractVersion = 1;
	TestFalse(TEXT("Candidate contract version regression is rejected"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		PreviousManifest,
		CandidateRuntime,
		VersionRegressionCandidate,
		MigrationResult));
	TestEqual(TEXT("Version regression category"), MigrationResult.ErrorCategory, FString(TEXT("state_migration_version_regression")));
	PreviousManifest.StateMigration.ContractVersion = 1;

	const TArray<uint8> PreviousFirstBytes = { 0x01, 0x02, 0x03, 0x04 };
	const TArray<uint8> PreviousSecondBytes = { 0x11, 0x12, 0x13, 0x14 };
	const TArray<uint8> PreviousThirdBytes = { 0x21, 0x22, 0x23, 0x24 };
	const TArray<uint8> PreviousFourthBytes = { 0x31, 0x32, 0x33, 0x34 };
	const TArray<uint8> CandidateFirstOriginal = { 0x41, 0x42, 0x43, 0x44 };
	const TArray<uint8> CandidateSecondOriginal = { 0x51, 0x52, 0x53, 0x54 };
	const TArray<uint8> CandidateThirdOriginal = { 0x61, 0x62, 0x63, 0x64 };
	const TArray<uint8> CandidateFourthOriginal = { 0x71, 0x72, 0x73, 0x74 };
	TestTrue(TEXT("Previous first transaction state writes"), PreviousRuntime.WriteStateBytes(24, PreviousFirstBytes, MemoryError));
	TestTrue(TEXT("Previous second transaction state writes"), PreviousRuntime.WriteStateBytes(28, PreviousSecondBytes, MemoryError));
	TestTrue(TEXT("Previous third transaction state writes"), PreviousRuntime.WriteStateBytes(32, PreviousThirdBytes, MemoryError));
	TestTrue(TEXT("Previous fourth transaction state writes"), PreviousRuntime.WriteStateBytes(36, PreviousFourthBytes, MemoryError));
	TestTrue(TEXT("Candidate first original writes"), CandidateRuntime.WriteStateBytes(68, CandidateFirstOriginal, MemoryError));
	TestTrue(TEXT("Candidate second original writes"), CandidateRuntime.WriteStateBytes(72, CandidateSecondOriginal, MemoryError));
	TestTrue(TEXT("Candidate third original writes"), CandidateRuntime.WriteStateBytes(76, CandidateThirdOriginal, MemoryError));
	TestTrue(TEXT("Candidate fourth original writes"), CandidateRuntime.WriteStateBytes(80, CandidateFourthOriginal, MemoryError));
	FAvidScriptWasmReloadManifest TransactionPrevious = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_service"));
	TransactionPrevious.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	TransactionPrevious.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	TransactionPrevious.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:tx_first"), FloatFingerprint, 24),
		MakeSessionStateSlot(TEXT("global:tx_second"), FloatFingerprint, 28),
		MakeSessionStateSlot(TEXT("global:tx_third"), FloatFingerprint, 32),
		MakeSessionStateSlot(TEXT("global:tx_fourth"), FloatFingerprint, 36),
	};
	FAvidScriptWasmReloadManifest TransactionCandidate = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_service"));
	TransactionCandidate.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	TransactionCandidate.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	TransactionCandidate.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:tx_first"), FloatFingerprint, 68),
		MakeSessionStateSlot(TEXT("global:tx_second"), FloatFingerprint, 72),
		MakeSessionStateSlot(TEXT("global:tx_third"), FloatFingerprint, 76),
		MakeSessionStateSlot(TEXT("global:tx_fourth"), FloatFingerprint, 80),
	};
	const TArray<int32> FailedWriteAttempts = { 4, 5 };
	CandidateRuntime.SetStateWriteFailuresForTesting(FailedWriteAttempts);
	TestFalse(TEXT("Forward and restore write failures fail transaction"), FAvidScriptRuntimeStateMigration::Migrate(
		PreviousRuntime,
		TransactionPrevious,
		CandidateRuntime,
		TransactionCandidate,
		MigrationResult));
	CandidateRuntime.ClearStateWriteFailureForTesting();
	TestEqual(TEXT("Write failure category"), MigrationResult.ErrorCategory, FString(TEXT("state_migration_write_failed")));
	TestEqual(TEXT("Write failure identifies failed restore slot"), MigrationResult.StableId, FString(TEXT("global:tx_third")));
	TestTrue(TEXT("Write failure reports restore failure"), MigrationResult.ErrorDetails.Contains(TEXT("restore failed")));
	TArray<uint8> CandidateFirstAfterFailure;
	CandidateFirstAfterFailure.SetNumZeroed(4);
	TestTrue(TEXT("Candidate first state reads after rollback"), CandidateRuntime.ReadStateBytes(68, CandidateFirstAfterFailure, MemoryError));
	TestEqual(TEXT("Candidate first state is restored after later restore failure"), CandidateFirstAfterFailure, CandidateFirstOriginal);
	TArray<uint8> CandidateSecondAfterFailure;
	CandidateSecondAfterFailure.SetNumZeroed(4);
	TestTrue(TEXT("Candidate second state reads after rollback"), CandidateRuntime.ReadStateBytes(72, CandidateSecondAfterFailure, MemoryError));
	TestEqual(TEXT("Candidate second state is restored after later restore failure"), CandidateSecondAfterFailure, CandidateSecondOriginal);
	TArray<uint8> CandidateThirdAfterFailure;
	CandidateThirdAfterFailure.SetNumZeroed(4);
	TestTrue(TEXT("Candidate third state reads after failed restore"), CandidateRuntime.ReadStateBytes(76, CandidateThirdAfterFailure, MemoryError));
	TestEqual(TEXT("Candidate third state retains migrated bytes after failed restore"), CandidateThirdAfterFailure, PreviousThirdBytes);
	TArray<uint8> CandidateFourthAfterFailure;
	CandidateFourthAfterFailure.SetNumZeroed(4);
	TestTrue(TEXT("Candidate fourth state reads after failed forward write"), CandidateRuntime.ReadStateBytes(80, CandidateFourthAfterFailure, MemoryError));
	TestEqual(TEXT("Candidate fourth state remains original after failed forward write"), CandidateFourthAfterFailure, CandidateFourthOriginal);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeSessionStateMigrationRollbackTest,
	"AvidScript.Architecture.Session.StateMigrationRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeSessionStateMigrationRollbackTest::RunTest(const FString& Parameters)
{
	const TCHAR* PreviousFingerprint = TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	const TCHAR* CandidateFingerprint = TEXT("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	FAvidScriptWasmReloadManifest PreviousManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_session"));
	PreviousManifest.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	PreviousManifest.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	PreviousManifest.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:score"), PreviousFingerprint, 16),
		MakeSessionStateSlot(TEXT("global:legacy_score"), PreviousFingerprint, 20),
	};
	FAvidScriptWasmReloadManifest CandidateManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_session"));
	CandidateManifest.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	CandidateManifest.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	CandidateManifest.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:score"), CandidateFingerprint, 32),
	};

	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmReloadResult ReloadResult;
	TestTrue(TEXT("State session starts"), Session.LoadInitialModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		PreviousManifest,
		ReloadResult));

	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("State session ticks before rejected reload"), Session.Tick(1.0f / 60.0f, TickResult));
	TestFalse(TEXT("Incompatible state candidate is rejected"), Session.ReloadModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		CandidateManifest,
		ReloadResult));
	TestTrue(TEXT("State migration was attempted"), ReloadResult.bStateMigrationAttempted);
	TestTrue(TEXT("Rejected state candidate preserves old runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("State rejection category"), ReloadResult.ErrorCategory, FString(TEXT("state_migration_incompatible")));
	TestEqual(TEXT("State rejection identifies stable id"), ReloadResult.StateMigrationStableId, FString(TEXT("global:score")));
	TestEqual(TEXT("Old state module remains active"), Session.GetSnapshot().ModuleId, FString(TEXT("state_session")));
	TestTrue(TEXT("Old state runtime continues ticking"), Session.Tick(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Old state runtime tick count continues"), Session.GetSnapshot().TickCallCount, 2);

	FAvidScriptWasmReloadManifest ClaimedCandidateManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("state_session"));
	ClaimedCandidateManifest.StateMigration.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	ClaimedCandidateManifest.StateMigration.OwnerTypeId = TEXT("type:Game.Script");
	ClaimedCandidateManifest.StateMigration.ContractVersion = 2;
	ClaimedCandidateManifest.StateMigration.Slots = {
		MakeSessionStateSlot(TEXT("global:score"), PreviousFingerprint, 36, { TEXT("global:legacy_score") }),
	};
	TestFalse(TEXT("Duplicate candidate claim rejects session reload"), Session.ReloadModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		ClaimedCandidateManifest,
		ReloadResult));
	TestTrue(TEXT("Claim rejection preserves old runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("Claim rejection category"), ReloadResult.ErrorCategory, FString(TEXT("state_migration_incompatible")));
	TestEqual(TEXT("Claim rejection identifies alias previous id"), ReloadResult.StateMigrationStableId, FString(TEXT("global:legacy_score")));
	TestEqual(TEXT("Old module remains active after claim rejection"), Session.GetSnapshot().ModuleId, FString(TEXT("state_session")));
	TestTrue(TEXT("Old runtime ticks after claim rejection"), Session.Tick(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Old runtime tick count continues after claim rejection"), Session.GetSnapshot().TickCallCount, 3);

	FAvidScriptWasmSmokeResult StopResult;
	Session.StopAndUnload(StopResult);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeSessionHotResultTest,
	"AvidScript.Architecture.Session.HotResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeSessionHotResultTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmReloadResult ReloadResult;
	TestTrue(
		TEXT("hot session starts"),
		Session.LoadInitialModule(
			GSessionCompatibleModule,
			UE_ARRAY_COUNT(GSessionCompatibleModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(
				TEXT("hot_session_v1")),
			ReloadResult));

	FAvidScriptWasmSmokeResult Sentinel;
	Sentinel.ModuleId = TEXT("untouched");
	Sentinel.ErrorMessage = TEXT("session_sentinel");
	Sentinel.TickCallCount = 404;
	TestTrue(
		TEXT("session hot Tick succeeds"),
		Session.TickHot(1.0f / 60.0f, Sentinel));
	TestEqual(
		TEXT("session hot Tick leaves result string untouched"),
		Sentinel.ErrorMessage,
		FString(TEXT("session_sentinel")));
	TestEqual(
		TEXT("session hot Tick leaves result scalar untouched"),
		Sentinel.TickCallCount,
		404);

	const FAvidScriptWasmHotSnapshot HotSnapshot =
		Session.GetLiveHotSnapshot();
	TestTrue(
		TEXT("session hot snapshot reports live Runtime"),
		HotSnapshot.bRuntimeLoaded);
	TestEqual(
		TEXT("session hot snapshot records Tick"),
		HotSnapshot.TickCallCount,
		1);

	FAvidScriptWasmSmokeResult CapturedResult;
	TestTrue(
		TEXT("session captures full observation outside hot call"),
		Session.CaptureLiveSnapshot(CapturedResult));
	TestEqual(
		TEXT("captured observation identifies module"),
		CapturedResult.ModuleId,
		FString(TEXT("hot_session_v1")));
	TestEqual(
		TEXT("captured observation records Tick"),
		CapturedResult.TickCallCount,
		1);

	TestTrue(
		TEXT("hot session reload succeeds"),
		Session.ReloadModule(
			GSessionCompatibleModule,
			UE_ARRAY_COUNT(GSessionCompatibleModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(
				TEXT("hot_session_v2")),
			ReloadResult));
	TestTrue(
		TEXT("reloaded session hot Tick succeeds"),
		Session.TickHot(1.0f / 60.0f, Sentinel));
	TestEqual(
		TEXT("reloaded Runtime starts a fresh hot Tick count"),
		Session.GetLiveHotSnapshot().TickCallCount,
		1);

	FAvidScriptWasmSmokeResult StopResult;
	TestTrue(
		TEXT("hot session stops"),
		Session.StopAndUnload(StopResult));
	TestFalse(
		TEXT("hot Tick without Runtime fails closed"),
		Session.TickHot(1.0f / 60.0f, Sentinel));
	TestEqual(
		TEXT("failed hot Tick materializes diagnostics"),
		Sentinel.ErrorCategory,
		FString(TEXT("invalid_state")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeSessionCandidateBeginRollbackTest,
	"AvidScript.Architecture.Session.CandidateBeginRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeSessionCandidateBeginRollbackTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	Session.SetHostContext(HostContext);
	FAvidScriptWasmReloadResult ReloadResult;
	TestTrue(
		TEXT("initial session starts"),
		Session.LoadInitialModule(
			GSessionCompatibleModule,
			UE_ARRAY_COUNT(GSessionCompatibleModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("session_live")),
			ReloadResult));

	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("initial live runtime ticks"), Session.Tick(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("initial tick count"), Session.GetSnapshot().TickCallCount, 1);

	USceneComponent* BorrowedCandidateComponent =
		NewObject<USceneComponent>(GetTransientPackage());
	const FTransform OriginalCandidateTransform(
		FRotator::ZeroRotator,
		FVector(10.0, 20.0, 30.0));
	BorrowedCandidateComponent->SetWorldTransform(OriginalCandidateTransform);
	bool bCandidateBorrowSucceeded = false;
	bool bCandidateEffectPrepared = false;
	Session.SetCandidateBeginPlayObserverForTesting(
		[&Session,
		 &Registry,
		 BorrowedCandidateComponent,
		 &bCandidateBorrowSucceeded,
		 &bCandidateEffectPrepared](IAvidScriptBindingHostEffectJournal* Journal)
		{
			IAvidScriptObjectOwnershipDomain* Ownership =
				Session.GetTestSnapshot().HostContext.ObjectOwnership;
			FAvidScriptObjectHandleResult BorrowResult;
			bCandidateBorrowSucceeded = Ownership != nullptr
				&& Ownership->Borrow(
					Registry,
					*BorrowedCandidateComponent,
					BorrowResult);
			if (!bCandidateBorrowSucceeded || Journal == nullptr)
			{
				return;
			}
			FAvidScriptBindingHostEffectPrepareResult PrepareResult;
			bCandidateEffectPrepared = Journal->PrepareEffect(
				Registry,
				BorrowResult.Handle,
				*BorrowedCandidateComponent,
				EAvidScriptBindingReloadEffect::SceneComponentTransform,
				PrepareResult);
			if (bCandidateEffectPrepared)
			{
				BorrowedCandidateComponent->SetWorldLocation(
					FVector(900.0, 800.0, 700.0));
			}
		});

	TestFalse(
		TEXT("candidate BeginPlay trap rejects reload"),
		Session.ReloadModule(
			GSessionBeginTrapModule,
			UE_ARRAY_COUNT(GSessionBeginTrapModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("session_trap")),
			ReloadResult));
	TestTrue(TEXT("rollback preserves old runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestTrue(TEXT("candidate reload opens host effect transaction"), ReloadResult.bHostEffectTransactionAttempted);
	TestTrue(TEXT("candidate acquires a borrowed handle before trapping"), bCandidateBorrowSucceeded);
	TestTrue(TEXT("candidate captures its borrowed component transform"), bCandidateEffectPrepared);
	TestEqual(TEXT("candidate rollback releases its borrowed registry slot"), Registry.GetLiveHandleCount(), 0);
	TestFalse(TEXT("trapping candidate does not commit host effects"), ReloadResult.bHostEffectTransactionCommitted);
	TestTrue(TEXT("candidate trap attempts host effect rollback"), ReloadResult.bHostEffectRollbackAttempted);
	TestTrue(TEXT("borrowed component host effect rollback succeeds"), ReloadResult.bHostEffectRollbackSucceeded);
	TestEqual(TEXT("host effect transaction captures one component"), ReloadResult.HostEffectCapturedObjectCount, 1);
	TestEqual(TEXT("host effect transaction restores one component"), ReloadResult.HostEffectRestoredObjectCount, 1);
	TestTrue(
		TEXT("component transform is restored before its borrowed lease is released"),
		BorrowedCandidateComponent->GetComponentTransform().Equals(
			OriginalCandidateTransform,
			0.01));
	TestEqual(TEXT("old module remains active"), Session.GetSnapshot().ModuleId, FString(TEXT("session_live")));
	TestEqual(TEXT("one reload is rejected"), Session.GetSnapshot().RejectedReloadCount, 1);
	TestEqual(TEXT("session remains running"), Session.GetSnapshot().LifecycleState, EAvidScriptLifecycleState::Running);

	TestTrue(TEXT("old runtime continues ticking"), Session.Tick(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("old tick count continues"), Session.GetSnapshot().TickCallCount, 2);

	FAvidScriptWasmSmokeResult StopResult;
	Session.StopAndUnload(StopResult);
	TestEqual(TEXT("session returns to empty"), Session.GetSnapshot().LifecycleState, EAvidScriptLifecycleState::Empty);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeSessionTypedOwnerValidationTest,
	"AvidScript.Architecture.Session.TypedOwnerValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeSessionTypedOwnerValidationTest::RunTest(const FString& Parameters)
{
	TSharedPtr<const FAvidScriptBindingPackage> ActorOwnerPackage;
	TSharedPtr<const FAvidScriptBindingPackage> PawnOwnerPackage;
	TSharedPtr<const FAvidScriptBindingPackage> LegacyPackage;
	FString PackageError;
	if (!TestTrue(TEXT("Actor owner package loads"), MakeTypedOwnerSessionPackage(false, ActorOwnerPackage, PackageError))
		|| !TestTrue(TEXT("Pawn owner package loads"), MakeTypedOwnerSessionPackage(true, PawnOwnerPackage, PackageError))
		|| !TestTrue(TEXT("Legacy package loads"), MakeLegacySessionPackage(LegacyPackage, PackageError)))
	{
		AddError(PackageError);
		return false;
	}
	TestEqual(TEXT("Actor package expects Actor Self"), ActorOwnerPackage->GetExpectedSelfClass(), AActor::StaticClass());
	TestEqual(TEXT("Pawn package expects Pawn Self"), PawnOwnerPackage->GetExpectedSelfClass(), APawn::StaticClass());
	TestEqual(TEXT("Valid legacy package has no Expected Self"), LegacyPackage->GetExpectedSelfClass(), static_cast<UClass*>(nullptr));

	FAvidScriptWasmReloadManifest LegacyPackedOwnerManifest = MakeSessionManifest(
		TEXT("typed_owner_legacy_packed"),
		LegacyPackage);
	LegacyPackedOwnerManifest.RequiredImports = {
		FAvidScriptWasmRequiredImport{ TEXT("avidscript"), TEXT("avid_owner_get_handle") }
	};
	FAvidScriptRuntimeSession LegacyPackedOwnerSession;
	int32 LegacyPackedOwnerBeginPlayCount = 0;
	LegacyPackedOwnerSession.SetCandidateBeginPlayObserverForTesting(
		[&LegacyPackedOwnerBeginPlayCount]()
		{
			++LegacyPackedOwnerBeginPlayCount;
		});
	FAvidScriptWasmReloadResult LegacyPackedOwnerResult;
	TestFalse(TEXT("Packed owner cannot borrow a legacy package"), LegacyPackedOwnerSession.LoadInitialModule(
		GSessionPackedOwnerImportModule,
		UE_ARRAY_COUNT(GSessionPackedOwnerImportModule),
		LegacyPackedOwnerManifest,
		LegacyPackedOwnerResult));
	TestEqual(
		TEXT("Legacy packed owner rejection has a stable category"),
		LegacyPackedOwnerResult.ErrorCategory,
		FString(TEXT("binding_package_import_mismatch")));
	TestEqual(
		TEXT("Legacy packed owner rejection occurs before BeginPlay"),
		LegacyPackedOwnerBeginPlayCount,
		0);
	TestFalse(TEXT("Legacy packed owner rejection leaves no live runtime"), LegacyPackedOwnerSession.IsLiveLoaded());

	UWorld* World = nullptr;
	AActor* Owner = nullptr;
	if (!TestTrue(TEXT("Typed owner test world and owner are created"), CreateSessionOwnerWorld(World, Owner)))
	{
		DestroySessionOwnerWorld(World);
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroySessionOwnerWorld(World);
	};

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle OwnerHandle = Registry.RegisterObject(Owner, RegisterResult, false);
	if (!TestTrue(TEXT("Valid Actor owner registers"), RegisterResult.bSucceeded))
	{
		AddError(RegisterResult.ErrorMessage);
		return false;
	}

	FAvidScriptWasmHostContext ValidOwnerContext;
	ValidOwnerContext.ObjectRegistry = &Registry;
	ValidOwnerContext.OwnerHandle = OwnerHandle;
	ValidOwnerContext.World = World;
	const FAvidScriptWasmReloadManifest PawnCandidateManifest = MakeSessionManifest(
		TEXT("typed_owner_candidate"),
		PawnOwnerPackage);

	FAvidScriptRuntimeSession InitialSession;
	InitialSession.SetHostContext(ValidOwnerContext);
	int32 InitialBeginPlayCount = 0;
	InitialSession.SetCandidateBeginPlayObserverForTesting(
		[&InitialBeginPlayCount]()
		{
			++InitialBeginPlayCount;
		});
	FAvidScriptWasmReloadResult InitialResult;
	TestFalse(TEXT("Pawn candidate rejects Actor owner before BeginPlay"), InitialSession.LoadInitialModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		PawnCandidateManifest,
		InitialResult));
	const FAvidScriptRuntimeSessionTestSnapshot InitialSnapshot = InitialSession.GetTestSnapshot();
	TestEqual(TEXT("Initial mismatch executes zero observed BeginPlay calls"), InitialBeginPlayCount, 0);
	TestEqual(TEXT("Initial mismatch category is stable"), InitialResult.ErrorCategory, FString(TEXT("runtime_owner_type_mismatch")));
	TestTrue(TEXT("Initial mismatch reports expected Pawn"), InitialResult.ErrorMessage.Contains(TEXT("expected=Pawn")));
	TestTrue(TEXT("Initial mismatch reports actual Actor"), InitialResult.ErrorMessage.Contains(TEXT("actual=Actor")));
	TestTrue(TEXT("Initial mismatch previous module is empty"), InitialResult.PreviousModuleId.IsEmpty());
	TestEqual(TEXT("Initial mismatch identifies candidate"), InitialResult.CandidateModuleId, PawnCandidateManifest.ModuleId);
	TestTrue(TEXT("Initial mismatch active module is empty"), InitialResult.ActiveModuleId.IsEmpty());
	TestFalse(TEXT("Initial mismatch does not claim rollback preservation"), InitialResult.bRollbackPreservedLiveRuntime);
	TestNoOwnerValidationTransaction(*this, TEXT("Initial mismatch"), InitialResult);
	TestFalse(TEXT("Initial mismatch leaves no live runtime"), InitialSnapshot.Runtime.bHasActiveRuntime);
	TestFalse(TEXT("Initial mismatch leaves scheduler detached"), InitialSnapshot.bSchedulerAttached);
	TestEqual(TEXT("Initial mismatch leaves lifecycle empty"), InitialSnapshot.Runtime.LifecycleState, EAvidScriptLifecycleState::Empty);
	TestEqual(TEXT("Initial mismatch does not increment reload rejection count"), InitialSnapshot.Runtime.RejectedReloadCount, 0);
	TestEqual(TEXT("Initial mismatch does not increment reload success count"), InitialSnapshot.Runtime.SuccessfulReloadCount, 0);
	TestTrue(TEXT("Initial mismatch leaves live manifest empty"), InitialSnapshot.LiveManifest.ModuleId.IsEmpty());
	TestTrue(TEXT("Initial mismatch leaves live runtime identity empty"), InitialSnapshot.LiveRuntimeIdentity == nullptr);

	FAvidScriptRuntimeSession MissingRegistrySession;
	FAvidScriptWasmHostContext MissingRegistryContext;
	MissingRegistryContext.OwnerHandle = OwnerHandle;
	MissingRegistryContext.World = World;
	MissingRegistrySession.SetHostContext(MissingRegistryContext);
	FAvidScriptWasmReloadResult MissingRegistryResult;
	TestFalse(TEXT("Missing registry fails closed"), MissingRegistrySession.LoadInitialModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		MakeSessionManifest(TEXT("typed_owner_missing_registry"), PawnOwnerPackage),
		MissingRegistryResult));
	TestEqual(TEXT("Missing registry uses stable category"), MissingRegistryResult.ErrorCategory, FString(TEXT("runtime_owner_type_mismatch")));
	TestTrue(TEXT("Missing registry result includes unresolved actual"), MissingRegistryResult.ErrorMessage.Contains(TEXT("actual=<unresolved:missing_registry>")));

	const FAvidScriptObjectHandle InvalidHandles[] = {
		FAvidScriptObjectHandle(),
		FAvidScriptObjectHandle{ OwnerHandle.Slot, 0 }
	};
	for (int32 InvalidIndex = 0; InvalidIndex < UE_ARRAY_COUNT(InvalidHandles); ++InvalidIndex)
	{
		FAvidScriptRuntimeSession InvalidHandleSession;
		FAvidScriptWasmHostContext InvalidHandleContext = ValidOwnerContext;
		InvalidHandleContext.OwnerHandle = InvalidHandles[InvalidIndex];
		InvalidHandleSession.SetHostContext(InvalidHandleContext);
		FAvidScriptWasmReloadResult InvalidHandleResult;
		const FString Label = FString::Printf(TEXT("Invalid owner handle %d"), InvalidIndex);
		TestFalse(*Label, InvalidHandleSession.LoadInitialModule(
			GSessionCompatibleModule,
			UE_ARRAY_COUNT(GSessionCompatibleModule),
			MakeSessionManifest(FString::Printf(TEXT("typed_owner_invalid_%d"), InvalidIndex), PawnOwnerPackage),
			InvalidHandleResult));
		TestEqual(*FString::Printf(TEXT("%s uses stable category"), *Label), InvalidHandleResult.ErrorCategory, FString(TEXT("runtime_owner_type_mismatch")));
		TestTrue(*FString::Printf(TEXT("%s retains invalid-handle detail"), *Label), InvalidHandleResult.ErrorMessage.Contains(TEXT("actual=<unresolved:invalid_handle>")));
		TestEqual(*FString::Printf(TEXT("%s does not increment reload rejection count"), *Label), InvalidHandleSession.GetSnapshot().RejectedReloadCount, 0);
	}

	AActor* StaleOwner = World->SpawnActor<AActor>();
	FAvidScriptObjectHandleResult StaleRegisterResult;
	const FAvidScriptObjectHandle StaleHandle = Registry.RegisterObject(StaleOwner, StaleRegisterResult, false);
	if (!TestTrue(TEXT("Stale-case owner registers"), StaleRegisterResult.bSucceeded))
	{
		AddError(StaleRegisterResult.ErrorMessage);
		return false;
	}
	FAvidScriptObjectHandleResult ReleaseResult;
	TestTrue(TEXT("Stale-case owner handle releases"), Registry.ReleaseHandle(StaleHandle, ReleaseResult, false));
	FAvidScriptRuntimeSession StaleOwnerSession;
	FAvidScriptWasmHostContext StaleOwnerContext = ValidOwnerContext;
	StaleOwnerContext.OwnerHandle = StaleHandle;
	StaleOwnerSession.SetHostContext(StaleOwnerContext);
	FAvidScriptWasmReloadResult StaleOwnerResult;
	TestFalse(TEXT("Stale owner fails closed"), StaleOwnerSession.LoadInitialModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		MakeSessionManifest(TEXT("typed_owner_stale"), PawnOwnerPackage),
		StaleOwnerResult));
	TestEqual(TEXT("Stale owner uses stable category"), StaleOwnerResult.ErrorCategory, FString(TEXT("runtime_owner_type_mismatch")));
	TestTrue(TEXT("Stale owner retains registry failure detail"), StaleOwnerResult.ErrorMessage.Contains(TEXT("actual=<unresolved:generation_mismatch>")));

	FAvidScriptRuntimeSession LegacySession;
	LegacySession.SetHostContext(ValidOwnerContext);
	int32 LegacyBeginPlayCount = 0;
	LegacySession.SetCandidateBeginPlayObserverForTesting(
		[&LegacyBeginPlayCount]()
		{
			++LegacyBeginPlayCount;
		});
	FAvidScriptWasmReloadResult LegacyResult;
	TestTrue(TEXT("Valid package without Expected Self preserves legacy activation"), LegacySession.LoadInitialModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		MakeSessionManifest(TEXT("typed_owner_legacy"), LegacyPackage),
		LegacyResult));
	TestEqual(TEXT("Legacy activation proves observer reaches real BeginPlay boundary"), LegacyBeginPlayCount, 1);
	TestTrue(TEXT("Legacy package becomes active"), LegacySession.GetTestSnapshot().LiveManifest.BindingPackage.Get() == LegacyPackage.Get());
	FAvidScriptWasmSmokeResult StopResult;
	LegacySession.StopAndUnload(StopResult);

	FAvidScriptRuntimeSession ReloadSession;
	ReloadSession.SetHostContext(ValidOwnerContext);
	int32 ReloadBeginPlayCount = 0;
	ReloadSession.SetCandidateBeginPlayObserverForTesting(
		[&ReloadBeginPlayCount]()
		{
			++ReloadBeginPlayCount;
		});
	FAvidScriptWasmReloadResult ReloadResult;
	const FAvidScriptWasmReloadManifest LiveManifest = MakeSessionManifest(
		TEXT("typed_owner_live"),
		ActorOwnerPackage);
	TestTrue(TEXT("Reload session starts with matching Actor package"), ReloadSession.LoadInitialModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		LiveManifest,
		ReloadResult));
	TestEqual(TEXT("Initial live activation executes one observed BeginPlay"), ReloadBeginPlayCount, 1);
	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("Live runtime ticks before typed owner rejection"), ReloadSession.Tick(1.0f / 60.0f, TickResult));
	const FAvidScriptRuntimeSessionTestSnapshot BeforeReload = ReloadSession.GetTestSnapshot();
	TestTrue(TEXT("Scheduler is attached before rejected candidate"), BeforeReload.bSchedulerAttached);
	TestEqual(TEXT("Live lifecycle is running before rejected candidate"), BeforeReload.Runtime.LifecycleState, EAvidScriptLifecycleState::Running);
	TestTrue(TEXT("Live runtime identity exists before rejected candidate"), BeforeReload.LiveRuntimeIdentity != nullptr);
	TestTrue(TEXT("Live host context keeps the valid registry"), BeforeReload.HostContext.ObjectRegistry == &Registry);
	TestTrue(TEXT("Live host context keeps the valid owner handle"), BeforeReload.HostContext.OwnerHandle == OwnerHandle);
	TestTrue(TEXT("Live package identity is Actor package"), BeforeReload.LiveManifest.BindingPackage.Get() == ActorOwnerPackage.Get());
	TestTrue(TEXT("Candidate package identity is Pawn package"), PawnCandidateManifest.BindingPackage.Get() == PawnOwnerPackage.Get());
	TestTrue(TEXT("Candidate package differs from live package"), PawnCandidateManifest.BindingPackage.Get() != BeforeReload.LiveManifest.BindingPackage.Get());
	const int32 BeginPlayCountBeforeRejectedCandidate = ReloadBeginPlayCount;

	TestFalse(TEXT("Pawn candidate rejects unchanged Actor owner"), ReloadSession.ReloadModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		PawnCandidateManifest,
		ReloadResult));
	const FAvidScriptRuntimeSessionTestSnapshot AfterReload = ReloadSession.GetTestSnapshot();
	TestEqual(
		TEXT("Rejected candidate executes zero observed BeginPlay calls"),
		ReloadBeginPlayCount - BeginPlayCountBeforeRejectedCandidate,
		0);
	TestEqual(TEXT("Reload owner category is stable"), ReloadResult.ErrorCategory, FString(TEXT("runtime_owner_type_mismatch")));
	TestTrue(TEXT("Reload preserves live runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("Reload previous identity is live module"), ReloadResult.PreviousModuleId, LiveManifest.ModuleId);
	TestEqual(TEXT("Reload candidate identity is rejected module"), ReloadResult.CandidateModuleId, PawnCandidateManifest.ModuleId);
	TestEqual(TEXT("Reload active identity remains live module"), ReloadResult.ActiveModuleId, LiveManifest.ModuleId);
	TestNoOwnerValidationTransaction(*this, TEXT("Reload mismatch"), ReloadResult);
	TestEqual(TEXT("Rejected reload count increments exactly once"), AfterReload.Runtime.RejectedReloadCount, BeforeReload.Runtime.RejectedReloadCount + 1);
	TestEqual(TEXT("Successful reload count is unchanged"), AfterReload.Runtime.SuccessfulReloadCount, BeforeReload.Runtime.SuccessfulReloadCount);
	TestTrue(TEXT("Live runtime identity is preserved"), AfterReload.LiveRuntimeIdentity == BeforeReload.LiveRuntimeIdentity);
	TestTrue(TEXT("Live runtime remains active"), AfterReload.Runtime.bHasActiveRuntime);
	TestTrue(TEXT("Scheduler remains attached to preserved runtime"), AfterReload.bSchedulerAttached);
	TestEqual(TEXT("Lifecycle remains running"), AfterReload.Runtime.LifecycleState, BeforeReload.Runtime.LifecycleState);
	TestEqual(TEXT("Scheduler module identity is preserved"), AfterReload.Runtime.ModuleId, BeforeReload.Runtime.ModuleId);
	TestEqual(TEXT("Tick state is unchanged by rejection"), AfterReload.Runtime.TickCallCount, BeforeReload.Runtime.TickCallCount);
	TestEqual(TEXT("Timer state is unchanged by rejection"), AfterReload.Runtime.PendingTimerCount, BeforeReload.Runtime.PendingTimerCount);
	TestEqual(TEXT("Timer callback state is unchanged by rejection"), AfterReload.Runtime.TimerCallbackCount, BeforeReload.Runtime.TimerCallbackCount);
	TestEqual(TEXT("Event callback state is unchanged by rejection"), AfterReload.Runtime.EventCallbackCount, BeforeReload.Runtime.EventCallbackCount);
	TestTrue(TEXT("Session registry identity is unchanged"), AfterReload.HostContext.ObjectRegistry == BeforeReload.HostContext.ObjectRegistry);
	TestTrue(TEXT("Session owner handle is unchanged"), AfterReload.HostContext.OwnerHandle == BeforeReload.HostContext.OwnerHandle);
	TestTrue(TEXT("Session world is unchanged"), AfterReload.HostContext.World.Get() == BeforeReload.HostContext.World.Get());
	TestEqual(TEXT("Session actor write policy is unchanged"), AfterReload.HostContext.ActorWritePolicy, BeforeReload.HostContext.ActorWritePolicy);
	TestTrue(TEXT("Session host-effect journal remains null"), AfterReload.HostContext.HostEffectJournal == nullptr);
	TestLiveManifestPreserved(*this, BeforeReload, AfterReload);
	TestTrue(TEXT("Live runtime continues ticking after owner rejection"), ReloadSession.Tick(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Live runtime tick count advances after owner rejection"), ReloadSession.GetSnapshot().TickCallCount, BeforeReload.Runtime.TickCallCount + 1);

	ReloadSession.StopAndUnload(StopResult);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeSessionObjectOwnershipTest,
	"AvidScript.Architecture.Session.ObjectOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeSessionObjectOwnershipTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	FAvidScriptRuntimeSession Session;
	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	Session.SetHostContext(HostContext);

	IAvidScriptObjectOwnershipDomain* const Ownership =
		Session.GetTestSnapshot().HostContext.ObjectOwnership;
	if (!TestNotNull(TEXT("Session injects its ownership domain"), Ownership))
	{
		return false;
	}

	UAvidScriptObjectRegistryTestObject* Object =
		NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
	UAvidScriptObjectRegistryTestObject* WrongObject =
		NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
	TWeakObjectPtr<UAvidScriptObjectRegistryTestObject> WeakObject(Object);
	FAvidScriptObjectHandleResult HandleResult;
	const FAvidScriptObjectHandle ObjectHandle = Registry.RegisterObject(Object, HandleResult, false);
	TestTrue(TEXT("Object handle registers"), HandleResult.bSucceeded);
	TestFalse(TEXT("Ownership rejects a handle for a different object"), Ownership->Adopt(
		Registry,
		*WrongObject,
		ObjectHandle,
		EAvidScriptObjectFactoryKind::NewObject,
		HandleResult));
	TestEqual(TEXT("Handle mismatch category is stable"), HandleResult.ErrorCategory, FString(TEXT("ownership_handle_mismatch")));
	TestTrue(TEXT("Session adopts ordinary object"), Ownership->Adopt(
		Registry,
		*Object,
		ObjectHandle,
		EAvidScriptObjectFactoryKind::NewObject,
		HandleResult));
	TestTrue(TEXT("Ownership authority is observable"), Ownership->Owns(ObjectHandle, Object));
	FAvidScriptObjectRegistry OtherRegistry;
	TestFalse(TEXT("Ownership rejects release through another registry"), Ownership->Release(
		ObjectHandle,
		OtherRegistry,
		HandleResult));
	TestEqual(TEXT("Registry mismatch category is stable"), HandleResult.ErrorCategory, FString(TEXT("ownership_registry_mismatch")));
	TestTrue(TEXT("Registry mismatch preserves ownership"), Ownership->Owns(ObjectHandle, Object));

	Object = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("Ownership domain keeps ordinary object alive"), WeakObject.IsValid());
	TestTrue(TEXT("Explicit release accepts owned object"), Ownership->Release(
		ObjectHandle,
		Registry,
		HandleResult));
	TestFalse(TEXT("Released object leaves ownership domain"), Ownership->Owns(ObjectHandle, WeakObject.Get()));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("Released ordinary object is collectable"), WeakObject.IsValid());

	UWorld* World = nullptr;
	AActor* Owner = nullptr;
	if (!TestTrue(TEXT("Component ownership fixture world is created"), CreateSessionOwnerWorld(World, Owner)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		DestroySessionOwnerWorld(World);
	};

	UAvidScriptSessionOwnershipTestComponent* ExternalComponent =
		NewObject<UAvidScriptSessionOwnershipTestComponent>(Owner);
	const FAvidScriptObjectHandle ExternalComponentHandle =
		Registry.RegisterObject(ExternalComponent, HandleResult, false);
	TestTrue(TEXT("External-destroy component handle registers"), HandleResult.bSucceeded);
	TestTrue(TEXT("Session adopts external-destroy component"), Ownership->Adopt(
		Registry,
		*ExternalComponent,
		ExternalComponentHandle,
		EAvidScriptObjectFactoryKind::ActorComponent,
		HandleResult));
	Owner->AddInstanceComponent(ExternalComponent);
	ExternalComponent->RegisterComponent();
	ExternalComponent->DestroyComponent();
	TestTrue(TEXT("Externally destroyed component enters destruction"), ExternalComponent->IsBeingDestroyed());
	TestTrue(TEXT("Externally destroyed component keeps handle authority until cleanup"), Ownership->Owns(
		ExternalComponentHandle,
		ExternalComponent));

	UAvidScriptSessionOwnershipTestComponent::GetDestructionOrder().Reset();
	UAvidScriptSessionOwnershipTestComponent* FirstComponent =
		NewObject<UAvidScriptSessionOwnershipTestComponent>(Owner);
	FirstComponent->DestructionOrderId = 1;
	const FAvidScriptObjectHandle FirstComponentHandle =
		Registry.RegisterObject(FirstComponent, HandleResult, false);
	TestTrue(TEXT("First component handle registers"), HandleResult.bSucceeded);
	TestTrue(TEXT("Session adopts first dynamic component"), Ownership->Adopt(
		Registry,
		*FirstComponent,
		FirstComponentHandle,
		EAvidScriptObjectFactoryKind::ActorComponent,
		HandleResult));
	Owner->AddInstanceComponent(FirstComponent);
	FirstComponent->RegisterComponent();

	UAvidScriptSessionOwnershipTestComponent* SecondComponent =
		NewObject<UAvidScriptSessionOwnershipTestComponent>(Owner);
	SecondComponent->DestructionOrderId = 2;
	const FAvidScriptObjectHandle SecondComponentHandle =
		Registry.RegisterObject(SecondComponent, HandleResult, false);
	TestTrue(TEXT("Second component handle registers"), HandleResult.bSucceeded);
	TestTrue(TEXT("Session adopts second dynamic component"), Ownership->Adopt(
		Registry,
		*SecondComponent,
		SecondComponentHandle,
		EAvidScriptObjectFactoryKind::ActorComponent,
		HandleResult));
	Owner->AddInstanceComponent(SecondComponent);
	SecondComponent->RegisterComponent();

	UAvidScriptObjectRegistryTestObject* ForeignObject =
		NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
	const FAvidScriptObjectHandle ForeignHandle = Registry.RegisterObject(ForeignObject, HandleResult, false);
	TestTrue(TEXT("Foreign handle registers"), HandleResult.bSucceeded);
	TestFalse(TEXT("Session rejects release without authority"), Ownership->Release(
		ForeignHandle,
		Registry,
		HandleResult));
	TestEqual(TEXT("Authority failure category is stable"), HandleResult.ErrorCategory, FString(TEXT("ownership_violation")));

	FAvidScriptWasmReloadResult LoadResult;
	TestTrue(TEXT("Reentrant fixture runtime loads"), Session.LoadEmbeddedSmoke(LoadResult));
	bool bReentrantStopSucceeded = true;
	FAvidScriptWasmSmokeResult ReentrantStopResult;
	bool bContextMutationRejected = false;
	Session.SetLiveExecutionObserverForTesting(
		[&Session, &Registry, &bReentrantStopSucceeded, &ReentrantStopResult, &bContextMutationRejected]()
		{
			Session.ClearHostContext();
			bContextMutationRejected = Session.GetTestSnapshot().HostContext.ObjectRegistry == &Registry;
			bReentrantStopSucceeded = Session.StopAndUnload(ReentrantStopResult);
		});
	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("Live runtime ticks around reentrant stop attempt"), Session.Tick(1.0f / 60.0f, TickResult));
	TestFalse(TEXT("Reentrant stop is rejected"), bReentrantStopSucceeded);
	TestTrue(TEXT("Reentrant context mutation is rejected"), bContextMutationRejected);
	TestEqual(TEXT("Reentrant stop category is stable"), ReentrantStopResult.ErrorCategory, FString(TEXT("reentrant_operation")));
	TestFalse(TEXT("Rejected reentrant stop preserves first component"), FirstComponent->IsBeingDestroyed());
	TestFalse(TEXT("Rejected reentrant stop preserves second component"), SecondComponent->IsBeingDestroyed());

	FAvidScriptWasmSmokeResult StopResult;
	TestTrue(TEXT("Session stop cleans owned objects"), Session.StopAndUnload(StopResult));
	TestTrue(TEXT("First owned dynamic component is destroyed"), FirstComponent->IsBeingDestroyed());
	TestTrue(TEXT("Second owned dynamic component is destroyed"), SecondComponent->IsBeingDestroyed());
	const TArray<int32>& DestructionOrder = UAvidScriptSessionOwnershipTestComponent::GetDestructionOrder();
	TestEqual(TEXT("Session cleanup destroys two live components"), DestructionOrder.Num(), 2);
	if (DestructionOrder.Num() == 2)
	{
		TestEqual(TEXT("Session cleanup starts with newest component"), DestructionOrder[0], 2);
		TestEqual(TEXT("Session cleanup ends with oldest component"), DestructionOrder[1], 1);
	}
	TestEqual(TEXT("Only foreign handle remains after ownership cleanup"), Registry.GetLiveHandleCount(), 1);
	TestTrue(TEXT("Foreign handle remains releasable"), Registry.ReleaseHandle(ForeignHandle, HandleResult, false));

	FAvidScriptObjectRegistry ReentrantCleanupRegistry;
	FAvidScriptRuntimeSession ReentrantCleanupSession;
	FAvidScriptWasmHostContext ReentrantCleanupContext;
	ReentrantCleanupContext.ObjectRegistry = &ReentrantCleanupRegistry;
	ReentrantCleanupSession.SetHostContext(ReentrantCleanupContext);
	FAvidScriptWasmReloadResult ReentrantCleanupLoadResult;
	TestTrue(TEXT("Cleanup reentrancy fixture runtime loads"),
		ReentrantCleanupSession.LoadEmbeddedSmoke(ReentrantCleanupLoadResult));
	IAvidScriptObjectOwnershipDomain* ReentrantCleanupOwnership =
		ReentrantCleanupSession.GetTestSnapshot().HostContext.ObjectOwnership;
	UAvidScriptSessionOwnershipTestComponent* ReentrantCleanupComponent =
		NewObject<UAvidScriptSessionOwnershipTestComponent>(Owner);
	Owner->AddInstanceComponent(ReentrantCleanupComponent);
	ReentrantCleanupComponent->RegisterComponent();
	const FAvidScriptObjectHandle ReentrantCleanupHandle =
		ReentrantCleanupRegistry.RegisterObject(
			ReentrantCleanupComponent,
			HandleResult,
			false);
	TestTrue(TEXT("Cleanup reentrancy fixture component is adopted"),
		ReentrantCleanupOwnership != nullptr
		&& ReentrantCleanupOwnership->Adopt(
			ReentrantCleanupRegistry,
			*ReentrantCleanupComponent,
			ReentrantCleanupHandle,
			EAvidScriptObjectFactoryKind::ActorComponent,
			HandleResult));
	bool bCleanupReentrantTickSucceeded = true;
	FAvidScriptWasmSmokeResult CleanupReentrantTickResult;
	UAvidScriptSessionOwnershipTestComponent::GetDestructionObserver() =
		[&ReentrantCleanupSession,
			&bCleanupReentrantTickSucceeded,
			&CleanupReentrantTickResult]()
		{
			bCleanupReentrantTickSucceeded = ReentrantCleanupSession.Tick(
				1.0f / 60.0f,
				CleanupReentrantTickResult);
		};
	ReentrantCleanupSession.ClearHostContext();
	TestFalse(TEXT("Host-context cleanup rejects component-destruction reentry"),
		bCleanupReentrantTickSucceeded);
	TestEqual(TEXT("Cleanup reentry has the stable category"),
		CleanupReentrantTickResult.ErrorCategory,
		FString(TEXT("reentrant_operation")));
	TestTrue(TEXT("Host-context cleanup still destroys the owned component"),
		ReentrantCleanupComponent->IsBeingDestroyed());
	TestEqual(TEXT("Host-context cleanup releases every owned handle"),
		ReentrantCleanupRegistry.GetLiveHandleCount(),
		0);
	FAvidScriptWasmSmokeResult ReentrantCleanupStopResult;
	TestTrue(TEXT("Cleanup reentrancy fixture stops after context clear"),
		ReentrantCleanupSession.StopAndUnload(ReentrantCleanupStopResult));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeServicesAttachDetachTest,
	"AvidScript.Architecture.Session.RuntimeServicesAttachDetach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeServicesAttachDetachTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;
	TestTrue(TEXT("service fixture loads"), Runtime.LoadEmbeddedSmokeModule(Result));
	TestTrue(TEXT("service fixture begins"), Runtime.BeginPlay(Result));

	FAvidScriptRuntimeScheduler Scheduler;
	FAvidScriptRuntimeEventRouter EventRouter(Scheduler);
	Scheduler.Attach(Runtime);
	TestTrue(TEXT("attached scheduler ticks"), Scheduler.Tick(1.0f / 60.0f, Result));
	TestEqual(TEXT("scheduler exposes tick count"), Scheduler.GetTickCallCount(), 1);

	TestFalse(TEXT("invalid event is rejected"), EventRouter.Dispatch(-1, 1.0f, Result));
	TestEqual(TEXT("invalid event category"), Result.ErrorCategory, FString(TEXT("invalid_argument")));
	TestEqual(TEXT("invalid event leaves runtime running"), Scheduler.GetLifecycleState(), EAvidScriptLifecycleState::Running);
	FAvidScriptGameplayEvent TypedInputEvent;
	TypedInputEvent.Type = EAvidScriptGameplayEventType::Input;
	TypedInputEvent.PrimaryId = 1;
	TestTrue(TEXT("attached router accepts optional typed event"), EventRouter.Dispatch(TypedInputEvent, Result));

	TestTrue(TEXT("attached runtime stops"), Runtime.EndPlay(Result));
	TestFalse(TEXT("stopped runtime rejects scheduler tick"), Scheduler.Tick(1.0f / 60.0f, Result));
	TestEqual(TEXT("stopped tick category"), Result.ErrorCategory, FString(TEXT("invalid_state")));
	TestFalse(TEXT("stopped runtime rejects routed event"), EventRouter.Dispatch(1, 1.0f, Result));
	TestEqual(TEXT("stopped event category"), Result.ErrorCategory, FString(TEXT("invalid_state")));
	TestFalse(TEXT("stopped runtime rejects typed event"), EventRouter.Dispatch(TypedInputEvent, Result));
	TestEqual(TEXT("stopped typed event category"), Result.ErrorCategory, FString(TEXT("invalid_state")));

	Scheduler.Detach();
	TestFalse(TEXT("detached scheduler rejects tick"), Scheduler.Tick(1.0f / 60.0f, Result));
	TestEqual(TEXT("detached tick category"), Result.ErrorCategory, FString(TEXT("invalid_state")));
	TestFalse(TEXT("detached event router rejects dispatch"), EventRouter.Dispatch(1, 1.0f, Result));
	TestEqual(TEXT("detached event category"), Result.ErrorCategory, FString(TEXT("invalid_state")));
	TestFalse(TEXT("detached event router rejects typed dispatch"), EventRouter.Dispatch(TypedInputEvent, Result));
	TestEqual(TEXT("detached typed event category"), Result.ErrorCategory, FString(TEXT("invalid_state")));

	Runtime.Unload(Result);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeGeneratedSelfCapabilityBoundaryTest,
	"AvidScript.Runtime.Session.GeneratedSelfCapabilityBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeGeneratedSelfCapabilityBoundaryTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	TStrongObjectPtr<AActor> FirstOwner(NewObject<AActor>());
	TStrongObjectPtr<AActor> SecondOwner(NewObject<AActor>());
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle FirstHandle = Registry.RegisterObject(
		FirstOwner.Get(),
		RegisterResult,
		false);
	const FAvidScriptObjectHandle SecondHandle = Registry.RegisterObject(
		SecondOwner.Get(),
		RegisterResult,
		false);
	if (!TestTrue(
			TEXT("Capability fixture handles are valid"),
			FirstHandle.IsValid() && SecondHandle.IsValid()))
	{
		return false;
	}

	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmHostContext Context;
	FAvidScriptBindingInvocationInstrumentation Instrumentation;
	Context.ObjectRegistry = &Registry;
	Context.OwnerHandle = FirstHandle;
	Context.BindingInvocationInstrumentation = &Instrumentation;
	Runtime.SetHostContext(Context);
	TestEqual(
		TEXT("Generated success status is preserved"),
		Runtime.RecordGeneratedStatusForTesting(
			EAvidScriptVmTypedHostStatus::Succeeded),
		EAvidScriptVmTypedHostStatus::Succeeded);
	TestEqual(
		TEXT("Dedicated S1 fallback is converted to fail-closed rejection"),
		Runtime.RecordGeneratedStatusForTesting(
			EAvidScriptVmTypedHostStatus::FallbackRequired),
		EAvidScriptVmTypedHostStatus::Rejected);
	TestEqual(
		TEXT("Generated rejection status is preserved"),
		Runtime.RecordGeneratedStatusForTesting(
			EAvidScriptVmTypedHostStatus::Rejected),
		EAvidScriptVmTypedHostStatus::Rejected);
	TestEqual(
		TEXT("Generated hit counter is exact"),
		Instrumentation.GeneratedNativeS1HitCount,
		uint64(1));
	TestEqual(
		TEXT("Generated fallback counter is exact"),
		Instrumentation.GeneratedNativeS1FallbackCount,
		uint64(1));
	TestEqual(
		TEXT("Generated reject counter is exact"),
		Instrumentation.GeneratedNativeS1RejectCount,
		uint64(1));
	TestEqual(
		TEXT("Every generated import contributes to the common host call count"),
		Runtime.GetHostImportCallCountForTesting(),
		3);
	Runtime.BeginTypedCallbackEpochForTesting();

	UObject* ResolvedObject = nullptr;
	TestTrue(
		TEXT("Explicit null world permits an otherwise valid Self capability"),
		Runtime.ResolveSelfCapabilityForTesting(
			static_cast<int32>(FirstHandle.Slot),
			static_cast<int32>(FirstHandle.Generation),
			UObject::StaticClass(),
			ResolvedObject));
	TestEqual(
		TEXT("First Self resolves to the first owner"),
		ResolvedObject,
		static_cast<UObject*>(FirstOwner.Get()));

	const uint64 FirstContextEpoch = Runtime.GetReloadEpochForTesting();
	Context.OwnerHandle = SecondHandle;
	Runtime.SetHostContext(Context);
	TestNotEqual(
		TEXT("Changing HostContext advances the capability epoch"),
		Runtime.GetReloadEpochForTesting(),
		FirstContextEpoch);
	TestFalse(
		TEXT("Old Self arguments fail closed after HostContext replacement"),
		Runtime.ResolveSelfCapabilityForTesting(
			static_cast<int32>(FirstHandle.Slot),
			static_cast<int32>(FirstHandle.Generation),
			UObject::StaticClass(),
			ResolvedObject));
	TestTrue(
		TEXT("Replacement Self is resolved instead of reusing the old weak object"),
		Runtime.ResolveSelfCapabilityForTesting(
			static_cast<int32>(SecondHandle.Slot),
			static_cast<int32>(SecondHandle.Generation),
			UObject::StaticClass(),
			ResolvedObject));
	TestEqual(
		TEXT("Replacement Self resolves to the second owner"),
		ResolvedObject,
		static_cast<UObject*>(SecondOwner.Get()));
	Runtime.EndTypedCallbackEpochForTesting();
	Runtime.BeginTypedCallbackEpochForTesting();
	TestTrue(
		TEXT("Stable registry revision permits Self reuse across callback epochs"),
		Runtime.ResolveSelfCapabilityForTesting(
			static_cast<int32>(SecondHandle.Slot),
			static_cast<int32>(SecondHandle.Generation),
			UObject::StaticClass(),
			ResolvedObject));
	FAvidScriptObjectHandleResult ReleaseResult;
	TestTrue(
		TEXT("Second owner handle is released for revision invalidation"),
		Registry.ReleaseHandle(
			SecondHandle,
			ReleaseResult,
			false));
	TestFalse(
		TEXT("Registry revision invalidates a cached Self capability immediately"),
		Runtime.ResolveSelfCapabilityForTesting(
			static_cast<int32>(SecondHandle.Slot),
			static_cast<int32>(SecondHandle.Generation),
			UObject::StaticClass(),
			ResolvedObject));
	Runtime.EndTypedCallbackEpochForTesting();

	TWeakObjectPtr<UWorld> StaleWorld;
	{
		UWorld* UnreferencedWorld = NewObject<UWorld>();
		StaleWorld = UnreferencedWorld;
		UnreferencedWorld->MarkAsGarbage();
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	if (!TestTrue(
			TEXT("World fixture weak pointer is stale"),
			StaleWorld.IsStale()))
	{
		return false;
	}

	Context.World = StaleWorld;
	Runtime.SetHostContext(Context);
	Runtime.BeginTypedCallbackEpochForTesting();
	TestFalse(
		TEXT("A stale weak world fails closed instead of becoming unconstrained"),
		Runtime.ResolveSelfCapabilityForTesting(
			static_cast<int32>(SecondHandle.Slot),
			static_cast<int32>(SecondHandle.Generation),
			UObject::StaticClass(),
			ResolvedObject));
	Runtime.EndTypedCallbackEpochForTesting();
	Runtime.ClearHostContext();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeGeneratedHostEffectBoundaryTest,
	"AvidScript.Runtime.Session.GeneratedHostEffectBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeGeneratedHostEffectBoundaryTest::RunTest(
	const FString& Parameters)
{
	const FString FunctionPackageHash = FString::ChrN(64, TEXT('8'));
	const FString PropertyPackageHash = FString::ChrN(64, TEXT('9'));
	FAvidScriptGeneratedBindingRegistry& GeneratedRegistry =
		FAvidScriptGeneratedBindingRegistry::Get();
	GeneratedRegistry.UnregisterPackage(FunctionPackageHash);
	GeneratedRegistry.UnregisterPackage(PropertyPackageHash);
	ON_SCOPE_EXIT
	{
		GeneratedRegistry.UnregisterPackage(FunctionPackageHash);
		GeneratedRegistry.UnregisterPackage(PropertyPackageHash);
	};

	FAvidScriptGeneratedBindingEntry FunctionEntry;
	FunctionEntry.PackageHash = FunctionPackageHash;
	FunctionEntry.StableId = FString::ChrN(64, TEXT('8'));
	FunctionEntry.DescriptorIdentity = TEXT("test::generated-effect");
	FunctionEntry.Shape = EAvidScriptGeneratedBindingShape::I32PairToI32;
	FunctionEntry.ReceiverMode = EAvidScriptGeneratedReceiverMode::SelfBound;
	FunctionEntry.I32PairCall = &GeneratedSessionPairCall;

	FAvidScriptGeneratedBindingEntry PropertyEntry;
	PropertyEntry.PackageHash = PropertyPackageHash;
	PropertyEntry.StableId = FString::ChrN(64, TEXT('9'));
	PropertyEntry.DescriptorIdentity = TEXT("test::generated-property");
	PropertyEntry.Shape =
		EAvidScriptGeneratedBindingShape::PropertyI32GetSet;
	PropertyEntry.ReceiverMode = EAvidScriptGeneratedReceiverMode::SelfBound;
	PropertyEntry.PropertyI32Call = &GeneratedSessionPropertyCall;

	FString Error;
	if (!TestTrue(
			TEXT("Function effect fixture registers"),
			GeneratedRegistry.RegisterPackage(
				FunctionPackageHash,
				MakeArrayView(&FunctionEntry, 1),
				Error))
		|| !TestTrue(
			TEXT("Property effect fixture registers"),
			GeneratedRegistry.RegisterPackage(
				PropertyPackageHash,
				MakeArrayView(&PropertyEntry, 1),
				Error)))
	{
		return false;
	}

	TStrongObjectPtr<AAvidScriptActorBindingTestActor> Owner(
		NewObject<AAvidScriptActorBindingTestActor>());
	TStrongObjectPtr<AAvidScriptActorBindingTestActor> StableReceiver(
		NewObject<AAvidScriptActorBindingTestActor>());
	FAvidScriptObjectRegistry ObjectRegistry;
	FAvidScriptObjectHandleResult HandleResult;
	const FAvidScriptObjectHandle OwnerHandle = ObjectRegistry.RegisterObject(
		Owner.Get(),
		HandleResult,
		false);
	const FAvidScriptObjectHandle StableReceiverHandle =
		ObjectRegistry.RegisterObject(
			StableReceiver.Get(),
			HandleResult,
			false);
	FProperty* Property = FindFProperty<FProperty>(
		AAvidScriptActorBindingTestActor::StaticClass(),
		GET_MEMBER_NAME_CHECKED(
			AAvidScriptActorBindingTestActor,
			HostEffectObjectProperty));
	if (!TestNotNull(TEXT("Reflected property fixture resolves"), Property))
	{
		return false;
	}

	const TSharedPtr<const FAvidScriptBindingPackage> PropertyPackage =
		FAvidScriptBindingPackage::MakeGeneratedPlanForTesting(
			PropertyPackageHash,
			PropertyEntry.StableId,
			PropertyEntry.DescriptorIdentity,
			PropertyEntry.Shape,
			AAvidScriptActorBindingTestActor::StaticClass(),
			Property,
			true,
			true,
			EAvidScriptBindingReloadEffect::ReflectedProperty);
	const TSharedPtr<const FAvidScriptBindingPackage> FunctionPackage =
		FAvidScriptBindingPackage::MakeGeneratedPlanForTesting(
			FunctionPackageHash,
			FunctionEntry.StableId,
			FunctionEntry.DescriptorIdentity,
			FunctionEntry.Shape,
			AAvidScriptActorBindingTestActor::StaticClass(),
			nullptr,
			false,
			true,
			EAvidScriptBindingReloadEffect::ActorTransform);
	const TSharedPtr<const FAvidScriptBindingPackage> UnsupportedPackage =
		FAvidScriptBindingPackage::MakeGeneratedPlanForTesting(
			FunctionPackageHash,
			FunctionEntry.StableId,
			FunctionEntry.DescriptorIdentity,
			FunctionEntry.Shape,
			AAvidScriptActorBindingTestActor::StaticClass(),
			nullptr,
			false,
			true,
			EAvidScriptBindingReloadEffect::Unsupported);
	if (!TestTrue(
			TEXT("Generated effect test packages are created"),
			PropertyPackage.IsValid()
				&& FunctionPackage.IsValid()
				&& UnsupportedPackage.IsValid()))
	{
		return false;
	}

	FGeneratedHostEffectJournal Journal;
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &ObjectRegistry;
	Context.OwnerHandle = OwnerHandle;
	Context.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	Context.HostEffectJournal = &Journal;
	TestTrue(
		TEXT("Generated property set prepares reflected-property rollback"),
		PropertyPackage->PrepareGeneratedHostEffect(
			0,
			OwnerHandle,
			*Owner,
			Context));
	TestEqual(
		TEXT("Reflected-property journal is called exactly once"),
		Journal.PropertyPrepareCount,
		1);
	TestEqual(
		TEXT("Reflected-property journal receives the property"),
		Journal.LastProperty,
		Property);

	TestTrue(
		TEXT("Generated function prepares its declared reload effect"),
		FunctionPackage->PrepareGeneratedHostEffect(
			0,
			StableReceiverHandle,
			*StableReceiver,
			Context));
	TestTrue(
		TEXT("Function journal receives the actual StableBorrow receiver handle"),
		Journal.LastHandle == StableReceiverHandle);
	TestEqual(
		TEXT("Function journal receives the declared effect"),
		Journal.LastEffect,
		EAvidScriptBindingReloadEffect::ActorTransform);

	TestFalse(
		TEXT("Unsupported candidate reload effect rejects before the thunk"),
		UnsupportedPackage->PrepareGeneratedHostEffect(
			0,
			StableReceiverHandle,
			*StableReceiver,
			Context));
	Context.HostEffectJournal = nullptr;
	Context.WritePolicy = EAvidScriptActorWritePolicy::ReadOnly;
	TestFalse(
		TEXT("Generated writes cannot bypass the host read-only policy"),
		FunctionPackage->PrepareGeneratedHostEffect(
			0,
			StableReceiverHandle,
			*StableReceiver,
			Context));
	Context.WritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	TestTrue(
		TEXT("Normal execution with no journal stays O(1) and accepts the plan"),
		UnsupportedPackage->PrepareGeneratedHostEffect(
			0,
			StableReceiverHandle,
			*StableReceiver,
			Context));

	TArray<FAvidScriptVmTypedHostImport> TypedImports;
	FString TypedImportError;
	TestTrue(
		TEXT("Active leases publish one complete typed import list"),
		FunctionPackage->BuildTypedHostImports(
			TypedImports,
			TypedImportError));
	TestEqual(
		TEXT("Typed import publication count is exact"),
		TypedImports.Num(),
		1);
	TArray<FAvidScriptPreparedGeneratedBinding> PreparedBindings;
	TestTrue(
		TEXT("Active leases publish one complete prepared binding list"),
		FunctionPackage->BuildPreparedGeneratedBindings(
			PreparedBindings,
			TypedImportError));
	TestEqual(
		TEXT("Prepared binding publication count is exact"),
		PreparedBindings.Num(),
		1);
	TestEqual(
		TEXT("Prepared binding freezes the ordinal"),
		PreparedBindings[0].BindingOrdinal,
		0u);
	TestEqual(
		TEXT("Prepared binding freezes the expected class"),
		PreparedBindings[0].ExpectedClass,
		AAvidScriptActorBindingTestActor::StaticClass());
	TestTrue(
		TEXT("Prepared binding retains host-effect semantics"),
		FunctionPackage->PrepareGeneratedHostEffect(
			PreparedBindings[0],
			StableReceiverHandle,
			*StableReceiver,
			Context));
	TestEqual(
		TEXT("Prepared write effect is classified for the direct lane"),
		FunctionPackage->ResolvePreparedHostEffectMode(
			PreparedBindings[0],
			Context),
		EAvidScriptPreparedHostEffectMode::DirectWrite);

	FAvidScriptWasmRuntimeInstance FusedRuntime;
	FAvidScriptBindingInvocationInstrumentation FusedInstrumentation;
	FAvidScriptWasmHostContext FusedContext;
	FusedContext.ObjectRegistry = &ObjectRegistry;
	FusedContext.OwnerHandle = StableReceiverHandle;
	FusedContext.ActorWritePolicy =
		EAvidScriptActorWritePolicy::AllowWrites;
	FusedContext.BindingInvocationInstrumentation =
		&FusedInstrumentation;
	FusedRuntime.SetHostContext(FusedContext);
	FusedRuntime.SetBindingPackageForTesting(FunctionPackage);
	FString PreparedImportError;
	if (!TestTrue(
			TEXT("Fused runtime publishes prepared typed imports"),
			FusedRuntime.BuildPreparedTypedHostImportsForTesting(
				PreparedImportError)))
	{
		AddError(PreparedImportError);
		return false;
	}
	const TArray<FAvidScriptVmTypedHostImport>& FusedImports =
		FusedRuntime.GetPreparedTypedHostImportsForTesting();
	if (!TestEqual(
			TEXT("Fused runtime publishes one prepared call site"),
			FusedImports.Num(),
			1))
	{
		return false;
	}
	const FAvidScriptVmPreparedTypedHostTarget& FusedTarget =
		FusedImports[0].PreparedTarget;
	int32 FusedValue = 0;
	FusedRuntime.BeginTypedCallbackEpochForTesting();
	TestEqual(
		TEXT("First fused call succeeds"),
		FusedTarget.SelfI32Pair(
			FusedTarget.Context,
			static_cast<int32>(StableReceiverHandle.Slot),
			static_cast<int32>(StableReceiverHandle.Generation),
			2,
			3,
			FusedValue),
		EAvidScriptVmTypedHostStatus::Succeeded);
	TestEqual(TEXT("First fused result is exact"), FusedValue, 5);
	TestEqual(
		TEXT("Second fused call reuses the callback receiver"),
		FusedTarget.SelfI32Pair(
			FusedTarget.Context,
			static_cast<int32>(StableReceiverHandle.Slot),
			static_cast<int32>(StableReceiverHandle.Generation),
			5,
			8,
			FusedValue),
		EAvidScriptVmTypedHostStatus::Succeeded);
	TestEqual(TEXT("Second fused result is exact"), FusedValue, 13);
	TestEqual(
		TEXT("One callback performs one receiver proof"),
		FusedInstrumentation.GeneratedFusedRevalidateCount,
		uint64(1));
	TestEqual(
		TEXT("The repeated call uses one receiver fast hit"),
		FusedInstrumentation.GeneratedFusedFastHitCount,
		uint64(1));
	TestEqual(
		TEXT("One call site is prepared once per callback"),
		FusedInstrumentation.GeneratedFusedCallSitePrepareCount,
		uint64(1));

	FusedRuntime.BeginTypedCallbackEpochForTesting();
	TestEqual(
		TEXT("Nested callback receives an independent fused frame"),
		FusedTarget.SelfI32Pair(
			FusedTarget.Context,
			static_cast<int32>(StableReceiverHandle.Slot),
			static_cast<int32>(StableReceiverHandle.Generation),
			1,
			1,
			FusedValue),
		EAvidScriptVmTypedHostStatus::Succeeded);
	FusedRuntime.EndTypedCallbackEpochForTesting();
	TestEqual(
		TEXT("Outer callback frame is restored after nesting"),
		FusedTarget.SelfI32Pair(
			FusedTarget.Context,
			static_cast<int32>(StableReceiverHandle.Slot),
			static_cast<int32>(StableReceiverHandle.Generation),
			3,
			4,
			FusedValue),
		EAvidScriptVmTypedHostStatus::Succeeded);
	FusedRuntime.EndTypedCallbackEpochForTesting();
	TestEqual(
		TEXT("Nested callback performs one additional receiver proof"),
		FusedInstrumentation.GeneratedFusedRevalidateCount,
		uint64(2));
	TestEqual(
		TEXT("Restored outer callback reuses its receiver proof"),
		FusedInstrumentation.GeneratedFusedFastHitCount,
		uint64(2));
	TestEqual(
		TEXT("Nested entry and outer restoration reprepare the call site"),
		FusedInstrumentation.GeneratedFusedCallSitePrepareCount,
		uint64(3));

	TStrongObjectPtr<AAvidScriptActorBindingTestActor> InvalidatedReceiver(
		NewObject<AAvidScriptActorBindingTestActor>());
	const FAvidScriptObjectHandle InvalidatedReceiverHandle =
		ObjectRegistry.RegisterObject(
			InvalidatedReceiver.Get(),
			HandleResult,
			false);
	FusedRuntime.BeginTypedCallbackEpochForTesting();
	TestEqual(
		TEXT("Receiver invalidation fixture enters the fused path"),
		FusedTarget.SelfI32Pair(
			FusedTarget.Context,
			static_cast<int32>(InvalidatedReceiverHandle.Slot),
			static_cast<int32>(InvalidatedReceiverHandle.Generation),
			4,
			5,
			FusedValue),
		EAvidScriptVmTypedHostStatus::Succeeded);
	InvalidatedReceiver->MarkAsGarbage();
	TestEqual(
		TEXT("An invalid receiver fails closed in the same callback"),
		FusedTarget.SelfI32Pair(
			FusedTarget.Context,
			static_cast<int32>(InvalidatedReceiverHandle.Slot),
			static_cast<int32>(InvalidatedReceiverHandle.Generation),
			6,
			7,
			FusedValue),
		EAvidScriptVmTypedHostStatus::Rejected);
	FusedRuntime.EndTypedCallbackEpochForTesting();

	const FAvidScriptPreparedGeneratedBinding PreparedBinding =
		PreparedBindings[0];
	FusedRuntime.BeginTypedCallbackEpochForTesting();
	TestEqual(
		TEXT("Revocation fixture enters the fused path"),
		FusedTarget.SelfI32Pair(
			FusedTarget.Context,
			static_cast<int32>(StableReceiverHandle.Slot),
			static_cast<int32>(StableReceiverHandle.Generation),
			8,
			9,
			FusedValue),
		EAvidScriptVmTypedHostStatus::Succeeded);
	GeneratedRegistry.UnregisterPackage(FunctionPackageHash);
	TestEqual(
		TEXT("A same-callback lease revoke fails closed before the thunk"),
		FusedTarget.SelfI32Pair(
			FusedTarget.Context,
			static_cast<int32>(StableReceiverHandle.Slot),
			static_cast<int32>(StableReceiverHandle.Generation),
			10,
			11,
			FusedValue),
		EAvidScriptVmTypedHostStatus::Rejected);
	FusedRuntime.EndTypedCallbackEpochForTesting();
	TestFalse(
		TEXT("Revocation between package load and backend load aborts publication"),
		FunctionPackage->BuildTypedHostImports(
			TypedImports,
			TypedImportError));
	TestTrue(
		TEXT("Revoked typed import publication leaves no partial list"),
		TypedImports.IsEmpty());
	TestEqual(
		TEXT("Revoked typed import publication uses the stable category"),
		TypedImportError,
		FString(TEXT("generated_binding_unavailable")));
	TestFalse(
		TEXT("Revocation aborts prepared binding publication"),
		FunctionPackage->BuildPreparedGeneratedBindings(
			PreparedBindings,
			TypedImportError));
	TestTrue(
		TEXT("Revoked prepared binding publication leaves no partial list"),
		PreparedBindings.IsEmpty());
	TestFalse(
		TEXT("A previously prepared host-effect frame fails closed after revoke"),
		FunctionPackage->PrepareGeneratedHostEffect(
			PreparedBinding,
			StableReceiverHandle,
			*StableReceiver,
			Context));
	FusedRuntime.BeginTypedCallbackEpochForTesting();
	TestEqual(
		TEXT("A revoked fused call site fails closed before the thunk"),
		FusedTarget.SelfI32Pair(
			FusedTarget.Context,
			static_cast<int32>(StableReceiverHandle.Slot),
			static_cast<int32>(StableReceiverHandle.Generation),
			1,
			2,
			FusedValue),
		EAvidScriptVmTypedHostStatus::Rejected);
	FusedRuntime.EndTypedCallbackEpochForTesting();
	return true;
}
#endif
