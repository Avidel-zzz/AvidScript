#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptGameplayEvent.h"
#include "AvidScriptRuntimeSession.h"
#include "Session/AvidScriptRuntimeEventRouter.h"
#include "Session/AvidScriptRuntimeScheduler.h"
#include "StateMigration/AvidScriptRuntimeStateMigration.h"

#include "GameFramework/Actor.h"
#include "Misc/EngineVersion.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
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
	TSharedPtr<const FAvidScriptBindingPackage>& OutPackage,
	FString& OutError)
{
	FAvidScriptBindingPackageModel Model;
	Model.SchemaVersion = 6;
	Model.GeneratorVersion = TEXT("50.0.session_test");
	Model.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	Model.Source = TEXT("ue_reflection");
	Model.PackageName = TEXT("avidscript.test.session_typed_owner");

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
	Model.SelfTypeId = ActorType.StableId;

	FAvidScriptBindingClassReferenceModel ActorReference;
	ActorReference.Ordinal = 0;
	ActorReference.ScriptName = TEXT("ActorClass");
	ActorReference.ClassPath = TEXT("/Script/Engine.Actor");
	ActorReference.BaseClassPath = TEXT("/Script/Engine.Actor");
	ActorReference.LoadPolicy = TEXT("EditorLoad");
	ActorReference.ResultTypeId = ActorType.StableId;
	ActorReference.StableId = FAvidScriptBindingDescriptorIdentity::MakeClassReferenceStableId(
		ActorReference.ClassPath,
		ActorReference.BaseClassPath,
		ActorReference.LoadPolicy);
	Model.ClassReferences.Add(ActorReference);
	Model.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Model);
	Model.PackageHash = FAvidScriptBindingDescriptorIdentity::MakePackageHash(Model);

	const FString DescriptorJson = FString::Printf(
		TEXT("{\"schema_version\":6,\"generator_version\":\"%s\",\"engine_version\":\"%s\",\"source\":\"ue_reflection\",\"package_name\":\"%s\",\"package_hash\":\"%s\",\"selection_hash\":\"%s\",\"self_type_id\":\"%s\",\"types\":[{\"stable_id\":\"%s\",\"canonical_type\":\"%s\",\"kind\":\"object_handle\",\"cpp_type\":\"UObject\",\"size\":8,\"alignment\":4,\"abi_types\":[\"i\",\"i\"],\"object_type_ordinal\":0,\"class_path\":\"%s\",\"base_type_id\":\"\"},{\"stable_id\":\"%s\",\"canonical_type\":\"%s\",\"kind\":\"object_handle\",\"cpp_type\":\"AActor\",\"size\":8,\"alignment\":4,\"abi_types\":[\"i\",\"i\"],\"object_type_ordinal\":1,\"class_path\":\"%s\",\"base_type_id\":\"%s\"}],\"class_references\":[{\"stable_id\":\"%s\",\"ordinal\":0,\"script_name\":\"ActorClass\",\"class_path\":\"/Script/Engine.Actor\",\"base_class_path\":\"/Script/Engine.Actor\",\"load_policy\":\"EditorLoad\",\"result_type_id\":\"%s\"}],\"bindings\":[]}"),
		*Model.GeneratorVersion,
		*Model.EngineVersion,
		*Model.PackageName,
		*Model.PackageHash,
		*Model.SelectionHash,
		*Model.SelfTypeId,
		*ObjectType.StableId,
		*ObjectType.CanonicalType,
		*ObjectType.ClassPath,
		*ActorType.StableId,
		*ActorType.CanonicalType,
		*ActorType.ClassPath,
		*ActorType.BaseTypeId,
		*ActorReference.StableId,
		*ActorReference.ResultTypeId);

	FAvidScriptBindingPackageLoadResult LoadResult;
	if (FAvidScriptBindingPackage::LoadDescriptor(DescriptorJson, OutPackage, LoadResult))
	{
		return true;
	}

	OutError = LoadResult.ErrorCategory + TEXT(": ") + LoadResult.ErrorDetails;
	return false;
}

FAvidScriptWasmReloadManifest MakeTypedOwnerSessionManifest(
	const FString& ModuleId,
	const TSharedPtr<const FAvidScriptBindingPackage>& Package)
{
	FAvidScriptWasmReloadManifest Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(ModuleId);
	Manifest.BindingPackage = Package;
	return Manifest;
}
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
	FAvidScriptRuntimeSessionCandidateBeginRollbackTest,
	"AvidScript.Architecture.Session.CandidateBeginRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeSessionCandidateBeginRollbackTest::RunTest(const FString& Parameters)
{
	FAvidScriptRuntimeSession Session;
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

	TestFalse(
		TEXT("candidate BeginPlay trap rejects reload"),
		Session.ReloadModule(
			GSessionBeginTrapModule,
			UE_ARRAY_COUNT(GSessionBeginTrapModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("session_trap")),
			ReloadResult));
	TestTrue(TEXT("rollback preserves old runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestTrue(TEXT("candidate reload opens host effect transaction"), ReloadResult.bHostEffectTransactionAttempted);
	TestFalse(TEXT("trapping candidate does not commit host effects"), ReloadResult.bHostEffectTransactionCommitted);
	TestTrue(TEXT("candidate trap attempts host effect rollback"), ReloadResult.bHostEffectRollbackAttempted);
	TestTrue(TEXT("empty host effect rollback succeeds"), ReloadResult.bHostEffectRollbackSucceeded);
	TestEqual(TEXT("empty host effect transaction captures no objects"), ReloadResult.HostEffectCapturedObjectCount, 0);
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
	TSharedPtr<const FAvidScriptBindingPackage> TypedOwnerPackage;
	FString PackageError;
	if (!TestTrue(TEXT("Typed owner package loads"), MakeTypedOwnerSessionPackage(TypedOwnerPackage, PackageError)))
	{
		AddError(PackageError);
		return false;
	}
	TestEqual(TEXT("Typed owner package expects Actor Self"), TypedOwnerPackage->GetExpectedSelfClass(), AActor::StaticClass());

	FAvidScriptObjectRegistry Registry;
	UObject* WrongOwner = NewObject<UObject>(GetTransientPackage());
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle WrongOwnerHandle = Registry.RegisterObject(WrongOwner, RegisterResult, false);
	if (!TestTrue(TEXT("Wrong owner registers"), RegisterResult.bSucceeded))
	{
		AddError(RegisterResult.ErrorMessage);
		return false;
	}

	FAvidScriptWasmHostContext WrongOwnerContext;
	WrongOwnerContext.ObjectRegistry = &Registry;
	WrongOwnerContext.OwnerHandle = WrongOwnerHandle;
	const FAvidScriptWasmReloadManifest TypedOwnerManifest = MakeTypedOwnerSessionManifest(
		TEXT("typed_owner_candidate"),
		TypedOwnerPackage);

	FAvidScriptRuntimeSession InitialSession;
	InitialSession.SetHostContext(WrongOwnerContext);
	FAvidScriptWasmReloadResult InitialResult;
	TestFalse(TEXT("Wrong owner rejects initial load before BeginPlay"), InitialSession.LoadInitialModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		TypedOwnerManifest,
		InitialResult));
	TestEqual(TEXT("Wrong owner initial category is stable"), InitialResult.ErrorCategory, FString(TEXT("runtime_owner_type_mismatch")));
	TestTrue(TEXT("Wrong owner initial result includes expected class"), InitialResult.ErrorMessage.Contains(TEXT("expected=Actor")));
	TestTrue(TEXT("Wrong owner initial result includes actual class"), InitialResult.ErrorMessage.Contains(TEXT("actual=Object")));
	TestFalse(TEXT("Wrong owner leaves BeginPlay callback count at zero"), InitialResult.RuntimeResult.bBeginPlayCalled);
	TestFalse(TEXT("Wrong owner initial load leaves no live runtime"), InitialSession.IsLiveLoaded());

	FAvidScriptRuntimeSession MissingRegistrySession;
	FAvidScriptWasmHostContext MissingRegistryContext;
	MissingRegistryContext.OwnerHandle = WrongOwnerHandle;
	MissingRegistrySession.SetHostContext(MissingRegistryContext);
	FAvidScriptWasmReloadResult MissingRegistryResult;
	TestFalse(TEXT("Missing registry fails closed"), MissingRegistrySession.LoadInitialModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		MakeTypedOwnerSessionManifest(TEXT("typed_owner_missing_registry"), TypedOwnerPackage),
		MissingRegistryResult));
	TestEqual(TEXT("Missing registry uses stable category"), MissingRegistryResult.ErrorCategory, FString(TEXT("runtime_owner_type_mismatch")));
	TestTrue(TEXT("Missing registry result includes unresolved actual"), MissingRegistryResult.ErrorMessage.Contains(TEXT("actual=<unresolved:missing_registry>")));

	FAvidScriptObjectHandleResult ReleaseResult;
	TestTrue(TEXT("Wrong owner handle releases"), Registry.ReleaseHandle(WrongOwnerHandle, ReleaseResult, false));
	FAvidScriptRuntimeSession StaleOwnerSession;
	StaleOwnerSession.SetHostContext(WrongOwnerContext);
	FAvidScriptWasmReloadResult StaleOwnerResult;
	TestFalse(TEXT("Stale owner fails closed"), StaleOwnerSession.LoadInitialModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		MakeTypedOwnerSessionManifest(TEXT("typed_owner_stale"), TypedOwnerPackage),
		StaleOwnerResult));
	TestEqual(TEXT("Stale owner uses stable category"), StaleOwnerResult.ErrorCategory, FString(TEXT("runtime_owner_type_mismatch")));
	TestTrue(TEXT("Stale owner retains registry failure detail"), StaleOwnerResult.ErrorMessage.Contains(TEXT("actual=<unresolved:generation_mismatch>")));

	UObject* ReloadWrongOwner = NewObject<UObject>(GetTransientPackage());
	FAvidScriptObjectHandleResult ReloadRegisterResult;
	const FAvidScriptObjectHandle ReloadWrongOwnerHandle = Registry.RegisterObject(ReloadWrongOwner, ReloadRegisterResult, false);
	if (!TestTrue(TEXT("Reload wrong owner registers"), ReloadRegisterResult.bSucceeded))
	{
		AddError(ReloadRegisterResult.ErrorMessage);
		return false;
	}

	FAvidScriptRuntimeSession ReloadSession;
	FAvidScriptWasmReloadResult ReloadResult;
	TestTrue(TEXT("Reload session starts with untyped runtime"), ReloadSession.LoadInitialModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("typed_owner_live")),
		ReloadResult));
	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("Live runtime ticks before typed owner rejection"), ReloadSession.Tick(1.0f / 60.0f, TickResult));
	const FAvidScriptRuntimeSessionSnapshot BeforeReload = ReloadSession.GetSnapshot();

	FAvidScriptWasmHostContext ReloadWrongOwnerContext;
	ReloadWrongOwnerContext.ObjectRegistry = &Registry;
	ReloadWrongOwnerContext.OwnerHandle = ReloadWrongOwnerHandle;
	ReloadSession.SetHostContext(ReloadWrongOwnerContext);
	TestFalse(TEXT("Wrong owner rejects reload candidate"), ReloadSession.ReloadModule(
		GSessionCompatibleModule,
		UE_ARRAY_COUNT(GSessionCompatibleModule),
		TypedOwnerManifest,
		ReloadResult));
	TestEqual(TEXT("Reload owner category is stable"), ReloadResult.ErrorCategory, FString(TEXT("runtime_owner_type_mismatch")));
	TestTrue(TEXT("Reload preserves live runtime"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestFalse(TEXT("Owner rejection never begins candidate"), ReloadResult.RuntimeResult.bBeginPlayCalled);
	TestFalse(TEXT("Owner rejection does not open transaction"), ReloadResult.bHostEffectTransactionAttempted);
	TestEqual(TEXT("Rejected candidate keeps live module"), ReloadSession.GetSnapshot().ModuleId, BeforeReload.ModuleId);
	TestEqual(TEXT("Rejected candidate keeps scheduler tick state"), ReloadSession.GetSnapshot().TickCallCount, BeforeReload.TickCallCount);
	TestTrue(TEXT("Live runtime continues ticking after owner rejection"), ReloadSession.Tick(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Live runtime tick count advances after owner rejection"), ReloadSession.GetSnapshot().TickCallCount, BeforeReload.TickCallCount + 1);

	FAvidScriptWasmSmokeResult StopResult;
	ReloadSession.StopAndUnload(StopResult);
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
#endif
