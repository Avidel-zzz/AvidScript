#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"

#include "AvidScriptHash.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptRuntimeArtifact.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"
#include "Session/AvidScriptRuntimeExecutionDomain.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "Validation/AvidScriptWasmImportPolicy.h"
#include "UObject/ObjectKey.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptGeneratedTypeRuntimeHost, Log, All);

namespace
{
struct FGeneratedTypeRuntimePackage
{
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	FAvidScriptRuntimeArtifact Artifact;
};

struct FGeneratedTypeRuntimeInstance
{
	TWeakObjectPtr<UObject> Receiver;
	FAvidScriptObjectHandle ReceiverHandle;
	uint32 TypeOrdinal = 0;
	TUniquePtr<FAvidScriptRuntimeSession> Session;
};

struct FGeneratedTypePackageFile
{
	FString RelativePath;
	FString Sha256;
};

FString NormalizePackagePath(const FString& Path)
{
	FString Normalized = FPaths::ConvertRelativePathToFull(Path);
	FPaths::CollapseRelativeDirectories(Normalized, true);
	FPaths::NormalizeFilename(Normalized);
	return Normalized;
}

bool IsLowercaseSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character)
			&& (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

bool ReadPackageFileEntry(
	const TSharedPtr<FJsonObject>& Root,
	const TCHAR* FieldName,
	FGeneratedTypePackageFile& OutEntry)
{
	const TSharedPtr<FJsonObject>* EntryObject = nullptr;
	return Root.IsValid()
		&& Root->TryGetObjectField(FieldName, EntryObject)
		&& EntryObject != nullptr
		&& EntryObject->IsValid()
		&& (*EntryObject)->TryGetStringField(TEXT("file"), OutEntry.RelativePath)
		&& (*EntryObject)->TryGetStringField(TEXT("sha256"), OutEntry.Sha256)
		&& !OutEntry.RelativePath.IsEmpty()
		&& FPaths::IsRelative(OutEntry.RelativePath)
		&& IsLowercaseSha256(OutEntry.Sha256);
}

bool ResolvePackageFile(
	const FString& DescriptorPath,
	const FGeneratedTypePackageFile& Entry,
	FString& OutPath,
	FString& OutError)
{
	const FString ProjectRoot = NormalizePackagePath(FPaths::ProjectDir());
	const FString DescriptorDirectory = NormalizePackagePath(FPaths::GetPath(DescriptorPath));
	const FString DescriptorCandidate = NormalizePackagePath(
		FPaths::Combine(DescriptorDirectory, Entry.RelativePath));
	const FString ProjectCandidate = NormalizePackagePath(
		FPaths::Combine(ProjectRoot, Entry.RelativePath));
	for (const FString& Candidate : { DescriptorCandidate, ProjectCandidate })
	{
		if ((FPaths::IsUnderDirectory(Candidate, ProjectRoot) || Candidate == ProjectRoot)
			&& FPaths::FileExists(Candidate))
		{
			OutPath = Candidate;
			return true;
		}
	}

	OutError = FString::Printf(
		TEXT("generated type package file is missing or outside the project: %s"),
		*Entry.RelativePath);
	return false;
}

bool LoadVerifiedPackageFile(
	const FString& Path,
	const FString& ExpectedSha256,
	TArray<uint8>& OutBytes,
	FString& OutError)
{
	if (!FFileHelper::LoadFileToArray(OutBytes, *Path))
	{
		OutError = FString::Printf(
			TEXT("generated type package file could not be read: %s"),
			*Path);
		return false;
	}
	const FString ActualSha256 = FAvidScriptHash::Sha256Hex(OutBytes);
	if (ActualSha256 != ExpectedSha256)
	{
		OutError = FString::Printf(
			TEXT("generated type package file hash mismatch: %s"),
			*Path);
		return false;
	}
	return true;
}

bool DeserializeJsonObject(
	const TArray<uint8>& Bytes,
	TSharedPtr<FJsonObject>& OutObject)
{
	FString Json;
	FFileHelper::BufferToString(Json, Bytes.GetData(), Bytes.Num());
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

bool TeardownInstance(
	FGeneratedTypeRuntimeInstance& Instance,
	FAvidScriptObjectRegistry& ObjectRegistry,
	FString& OutError,
	const bool bOwnerCollected = false)
{
	bool bSucceeded = true;
	if (Instance.Session)
	{
		FAvidScriptGeneratedInvocation ActiveInvocation;
		if (Instance.Session->IsOperationActive()
			|| FAvidScriptGeneratedTypeRouter::Get().GetActiveInvocation(ActiveInvocation))
		{
			OutError = TEXT("generated type teardown requires the active invocation to return");
			return false;
		}
		if (bOwnerCollected)
		{
			if (!Instance.Session->StopAndUnloadForCollectedGeneratedOwner())
			{
				OutError = TEXT("collected generated owner could not retire its idle Session");
				return false;
			}
		}
		else if (Instance.Session->IsLiveLoaded())
		{
			FAvidScriptWasmSmokeResult StopResult;
			if (!Instance.Session->StopAndUnload(StopResult))
			{
				bSucceeded = false;
				OutError = StopResult.ErrorMessage.IsEmpty()
					? TEXT("generated type Session failed to stop")
					: StopResult.ErrorMessage;
			}
		}
		FString ClearError;
		if (!Instance.Session->ClearGeneratedTypeInstance(ClearError))
		{
			if (OutError.IsEmpty())
			{
				OutError = ClearError;
			}
			// The Router still owns a pointer to this Session. Never free it after
			// a rejected unregister, even when stopping the VM already succeeded.
			return false;
		}
		Instance.Session.Reset();
	}
	if (Instance.ReceiverHandle.IsValid())
	{
		FAvidScriptObjectHandleResult ReleaseResult;
		if (!ObjectRegistry.ReleaseHandle(
			Instance.ReceiverHandle,
			ReleaseResult,
			false))
		{
			bSucceeded = false;
			if (OutError.IsEmpty())
			{
				OutError = ReleaseResult.ErrorMessage.IsEmpty()
					? TEXT("generated type ObjectHandle release failed")
					: ReleaseResult.ErrorMessage;
			}
		}
		Instance.ReceiverHandle = {};
	}
	return bSucceeded;
}

bool IsCompleteRuntimePackage(
	const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& Registry,
	const FAvidScriptRuntimeArtifact& Artifact,
	FString& OutError)
{
	if (!Registry.IsValid() || Registry->Num() == 0)
	{
		OutError = TEXT("generated type package registry is empty");
		return false;
	}
	if (Artifact.Manifest.ModuleId.IsEmpty()
		|| Artifact.Manifest.RequiredExports.IsEmpty()
		|| (Artifact.VmArtifact.ExecutionBytes.IsEmpty()
			&& Artifact.VmArtifact.CanonicalWasmBytes.IsEmpty()))
	{
		OutError = TEXT("generated type package Runtime artifact is incomplete");
		return false;
	}
	return true;
}

bool AreBodyOnlyCompatibleRegistries(
	const FAvidScriptGeneratedTypeRegistrySnapshot& Current,
	const FAvidScriptGeneratedTypeRegistrySnapshot& Candidate,
	FString& OutReason)
{
	const TConstArrayView<FAvidScriptGeneratedTypePlan> CurrentTypes = Current.GetTypes();
	const TConstArrayView<FAvidScriptGeneratedTypePlan> CandidateTypes = Candidate.GetTypes();
	if (CurrentTypes.Num() != CandidateTypes.Num())
	{
		OutReason = TEXT("generated type count changed");
		return false;
	}
	for (int32 TypeIndex = 0; TypeIndex < CurrentTypes.Num(); ++TypeIndex)
	{
		const FAvidScriptGeneratedTypePlan& CurrentType = CurrentTypes[TypeIndex];
		const FAvidScriptGeneratedTypePlan& CandidateType = CandidateTypes[TypeIndex];
		if (CurrentType.TypeOrdinal != CandidateType.TypeOrdinal
			|| CurrentType.StableTypeId != CandidateType.StableTypeId
			|| CurrentType.Class != CandidateType.Class)
		{
			OutReason = FString::Printf(
				TEXT("generated type identity changed at ordinal %d"),
				TypeIndex);
			return false;
		}
		if (CurrentType.Members.Num() != CandidateType.Members.Num())
		{
			OutReason = FString::Printf(
				TEXT("generated member count changed for type ordinal %u"),
				CurrentType.TypeOrdinal);
			return false;
		}
		for (int32 MemberIndex = 0; MemberIndex < CurrentType.Members.Num(); ++MemberIndex)
		{
			const FAvidScriptGeneratedMemberPlan& CurrentMember =
				CurrentType.Members[MemberIndex];
			const FAvidScriptGeneratedMemberPlan& CandidateMember =
				CandidateType.Members[MemberIndex];
			if (CurrentMember.MemberOrdinal != CandidateMember.MemberOrdinal
				|| CurrentMember.Kind != CandidateMember.Kind
				|| CurrentMember.StableMemberId != CandidateMember.StableMemberId
				|| CurrentMember.Property != CandidateMember.Property
				|| CurrentMember.Function != CandidateMember.Function
				|| CurrentMember.GetterImportName != CandidateMember.GetterImportName
				|| CurrentMember.SetterImportName != CandidateMember.SetterImportName
				|| CurrentMember.ExportName != CandidateMember.ExportName
				|| CurrentMember.bLifecycle != CandidateMember.bLifecycle)
			{
				OutReason = FString::Printf(
					TEXT("generated member shape changed at type %u member %d"),
					CurrentType.TypeOrdinal,
					MemberIndex);
				return false;
			}
		}
	}
	OutReason.Reset();
	return true;
}
}

struct FAvidScriptGeneratedTypeRuntimeHost::FImpl
{
	bool bStarted = false;
	bool bMutationInProgress = false;
	bool bTeardownPending = false;
	TOptional<FGeneratedTypeRuntimePackage> Package;
	FAvidScriptObjectRegistry ObjectRegistry;
	TMap<FObjectKey, TUniquePtr<FGeneratedTypeRuntimeInstance>> Instances;
	FDelegateHandle PostGarbageCollectHandle;
	FDelegateHandle WorldPostActorTickHandle;
	FTSTicker::FDelegateHandle CollectedSweepTickerHandle;
#if WITH_DEV_AUTOMATION_TESTS
	int32 ReloadFailureAfterInstanceCountForTesting = INDEX_NONE;
#endif
};

FAvidScriptGeneratedTypeRuntimeHost& FAvidScriptGeneratedTypeRuntimeHost::Get()
{
	static FAvidScriptGeneratedTypeRuntimeHost Host;
	return Host;
}

bool FAvidScriptGeneratedTypeRuntimeHost::IsCommandletExecutionSuppressed()
{
#if WITH_EDITOR
	return IsRunningCommandlet()
		&& FParse::Param(
			FCommandLine::Get(),
			TEXT("AvidScriptSuppressGeneratedTypeExecution"));
#else
	return false;
#endif
}

#if WITH_DEV_AUTOMATION_TESTS
TUniquePtr<FAvidScriptGeneratedTypeRuntimeHost>
FAvidScriptGeneratedTypeRuntimeHost::CreateIsolatedForTesting()
{
	TUniquePtr<FAvidScriptGeneratedTypeRuntimeHost> Host(
		new FAvidScriptGeneratedTypeRuntimeHost());
	Host->Startup();
	return Host;
}

void FAvidScriptGeneratedTypeRuntimeHost::SetReloadFailureAfterInstanceCountForTesting(
	const int32 InstanceCount)
{
	check(Impl && Impl->bStarted && IsInGameThread());
	Impl->ReloadFailureAfterInstanceCountForTesting = InstanceCount;
}

FAvidScriptRuntimeSession* FAvidScriptGeneratedTypeRuntimeHost::GetInstanceSessionForTesting(const UObject& Receiver) const
{
	if (!Impl || !IsInGameThread()) return nullptr;
	const auto* Instance = Impl->Instances.Find(FObjectKey(&Receiver));
	return Instance && Instance->IsValid() ? (*Instance)->Session.Get() : nullptr;
}
#endif

bool FAvidScriptGeneratedTypeRuntimeHost::CanMutateInstances(FString& OutError) const
{
	if (!Impl || !Impl->bStarted || !IsInGameThread())
	{
		OutError = TEXT("generated type mutation requires a started GameThread host");
		return false;
	}
	FAvidScriptGeneratedInvocation ActiveInvocation;
	if (Impl->bMutationInProgress || FAvidScriptGeneratedTypeRouter::Get().GetActiveInvocation(ActiveInvocation))
	{
		OutError = TEXT("generated type mutation rejected during an active invocation or host transaction");
		return false;
	}
	for (const auto& Pair : Impl->Instances)
	{
		if (Pair.Value && Pair.Value->Session && Pair.Value->Session->IsOperationActive())
		{
			OutError = TEXT("generated type mutation rejected during an active Session operation");
			return false;
		}
	}
	return true;
}

void FAvidScriptGeneratedTypeRuntimeHost::QueueCollectedInstanceSweep()
{
	if (!Impl || !Impl->bStarted || !IsInGameThread()
		|| Impl->CollectedSweepTickerHandle.IsValid()) return;
	bool bHasCollectedReceiver = false;
	for (const auto& Pair : Impl->Instances)
	{
		if (Pair.Value && !Pair.Value->Receiver.IsValid())
		{
			bHasCollectedReceiver = true;
			break;
		}
	}
	if (bHasCollectedReceiver)
	{
		// Run after GC and any active Guest call have returned. The generated
		// shell normally calls EndInstance from EndPlay; this closes missed exits.
		Impl->CollectedSweepTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FAvidScriptGeneratedTypeRuntimeHost::SweepCollectedInstances), 0.0f);
	}
}

bool FAvidScriptGeneratedTypeRuntimeHost::SweepCollectedInstances(float DeltaTime)
{
	if (!Impl) return false;
	Impl->CollectedSweepTickerHandle.Reset();
	FString GuardError;
	if (!CanMutateInstances(GuardError))
	{
		QueueCollectedInstanceSweep();
		return false;
	}
	TGuardValue<bool> MutationGuard(Impl->bMutationInProgress, true);
	for (auto Iterator = Impl->Instances.CreateIterator(); Iterator; ++Iterator)
	{
		if (!Iterator.Value() || Iterator.Value()->Receiver.IsValid()) continue;
		FString Error;
		if (!TeardownInstance(*Iterator.Value(), Impl->ObjectRegistry, Error, true))
		{
			UE_LOG(LogAvidScriptGeneratedTypeRuntimeHost, Warning,
				TEXT("Generated host collected-instance teardown: %s"), *Error);
		}
		if (!Iterator.Value()->Session && !Iterator.Value()->ReceiverHandle.IsValid())
		{
			Iterator.RemoveCurrent();
		}
	}
	return false;
}

FAvidScriptGeneratedTypeRuntimeHost::FAvidScriptGeneratedTypeRuntimeHost() = default;

void FAvidScriptGeneratedTypeRuntimeHost::PumpWorldContinuations(UWorld& World)
{
	if (!Impl || !Impl->bStarted || Impl->bTeardownPending || World.bIsTearingDown
		|| !World.IsGameWorld() || !World.HasBegunPlay()) return;
	FString GuardError;
	if (!CanMutateInstances(GuardError)) return;
	// Do not retain a receiver or allocate a per-frame snapshot. Host mutation is
	// blocked while Guest callbacks run, just as during package transactions.
	TGuardValue<bool> MutationGuard(Impl->bMutationInProgress, true);
	for (const auto& Pair : Impl->Instances)
	{
		const auto& Instance = Pair.Value;
		if (!Instance || !Instance->Receiver.IsValid() || !Instance->Session
			|| Instance->Session->HostContext.World.Get() != &World) continue;
		FAvidScriptWasmSmokeResult Result;
		if (!Instance->Session->PumpGeneratedContinuations(Result))
		{
			UE_LOG(LogAvidScriptGeneratedTypeRuntimeHost, Warning,
				TEXT("Generated async dispatch failed: %s"), *Result.ErrorMessage);
		}
	}
	QueueCollectedInstanceSweep();
}

FAvidScriptGeneratedTypeRuntimeHost::~FAvidScriptGeneratedTypeRuntimeHost()
{
	ensureMsgf(!Impl, TEXT("Generated type Runtime host must shut down before static destruction."));
}

bool FAvidScriptGeneratedTypeRuntimeHost::Startup()
{
	if (!IsInGameThread())
	{
		return false;
	}
	if (Impl)
	{
		return Impl->bStarted;
	}
	Impl = MakeUnique<FImpl>();
	Impl->bStarted = true;
	Impl->PostGarbageCollectHandle = FCoreUObjectDelegates::GetPostGarbageCollect().AddRaw(
		this, &FAvidScriptGeneratedTypeRuntimeHost::QueueCollectedInstanceSweep);
	Impl->WorldPostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddLambda(
		[this](UWorld* World, ELevelTick TickType, float DeltaSeconds)
		{
			if (World && TickType == LEVELTICK_All) PumpWorldContinuations(*World);
		});
	return true;
}

void FAvidScriptGeneratedTypeRuntimeHost::Shutdown()
{
	if (!Impl)
	{
		return;
	}
	FString GuardError;
	if (!CanMutateInstances(GuardError))
	{
		UE_LOG(LogAvidScriptGeneratedTypeRuntimeHost, Warning, TEXT("%s"), *GuardError);
		return;
	}
	{
		TGuardValue<bool> MutationGuard(Impl->bMutationInProgress, true);
		for (auto Iterator = Impl->Instances.CreateIterator(); Iterator; ++Iterator)
		{
			FString Error;
			if (!TeardownInstance(*Iterator.Value(), Impl->ObjectRegistry, Error,
				!Iterator.Value()->Receiver.IsValid()))
				UE_LOG(LogAvidScriptGeneratedTypeRuntimeHost, Warning, TEXT("Generated host teardown: %s"), *Error);
			if (!Iterator.Value()->Session && !Iterator.Value()->ReceiverHandle.IsValid()) Iterator.RemoveCurrent();
		}
	}
	if (!Impl->Instances.IsEmpty())
	{
		Impl->bTeardownPending = true;
		return;
	}
	FCoreUObjectDelegates::GetPostGarbageCollect().Remove(Impl->PostGarbageCollectHandle);
	FWorldDelegates::OnWorldPostActorTick.Remove(Impl->WorldPostActorTickHandle);
	if (Impl->CollectedSweepTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Impl->CollectedSweepTickerHandle);
		Impl->CollectedSweepTickerHandle.Reset();
	}
	Impl->ObjectRegistry.Reset();
	Impl->Package.Reset();
	Impl.Reset();
}

bool FAvidScriptGeneratedTypeRuntimeHost::InstallPackage(
	const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& Registry,
	const FAvidScriptRuntimeArtifact& Artifact,
	FString& OutError)
{
	OutError.Reset();
	if (!CanMutateInstances(OutError)) return false;
	TGuardValue<bool> MutationGuard(Impl->bMutationInProgress, true);
	if (!Impl->Instances.IsEmpty())
	{
		OutError = TEXT("generated type package replacement requires zero active instances");
		return false;
	}
	if (!IsCompleteRuntimePackage(Registry, Artifact, OutError))
	{
		return false;
	}

	Impl->Package.Emplace(FGeneratedTypeRuntimePackage{ Registry, Artifact });
	Impl->bTeardownPending = false;
	return true;
}

bool FAvidScriptGeneratedTypeRuntimeHost::ReloadPackage(
	const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& Registry,
	const FAvidScriptRuntimeArtifact& Artifact,
	FAvidScriptGeneratedTypePackageReloadResult& OutResult,
	FString& OutError)
{
	OutResult = FAvidScriptGeneratedTypePackageReloadResult();
	OutError.Reset();
	if (!CanMutateInstances(OutError)) return false;
	if (Impl->bTeardownPending)
	{
		OutError = TEXT("generated type reload requires pending instance teardown to complete");
		return false;
	}
	TGuardValue<bool> MutationGuard(Impl->bMutationInProgress, true);
	if (!Impl->Package.IsSet())
	{
		OutError = TEXT("generated type package reload requires an installed GameThread package");
		return false;
	}
	if (!IsCompleteRuntimePackage(Registry, Artifact, OutError))
	{
		return false;
	}

	const FGeneratedTypeRuntimePackage PreviousPackage = Impl->Package.GetValue();
	if (!AreBodyOnlyCompatibleRegistries(
		*PreviousPackage.Registry,
		*Registry,
		OutResult.StructuralChangeReason))
	{
		OutResult.Disposition =
			EAvidScriptGeneratedTypePackageReloadDisposition::NativeRebuildRequired;
		OutResult.CandidateInstanceCount = Impl->Instances.Num();
		OutResult.bRollbackPreservedLivePackage = true;
		OutError = FString::Printf(
			TEXT("generated type package reload requires a native rebuild: %s"),
			*OutResult.StructuralChangeReason);
		return false;
	}

	OutResult.CandidateInstanceCount = Impl->Instances.Num();
	// Fence every old Session before the first candidate can produce native callbacks.
	for (auto& Pair : Impl->Instances)
		if (Pair.Value && Pair.Value->Session) Pair.Value->Session->bPackageReloadBarrier = true;
	const auto ReleaseBarriers = [this]()
	{
		for (auto& Pair : Impl->Instances)
			if (Pair.Value && Pair.Value->Session) Pair.Value->Session->bPackageReloadBarrier = false;
	};
	const auto PoisonLiveDomains = [this, &OutError]()
	{
		FAvidScriptWasmSmokeResult Failure;
		Failure.ModuleId = Impl->Package->Artifact.Manifest.ModuleId;
		Failure.ErrorCategory = TEXT("execution_domain_package_failed");
		Failure.ErrorMessage = OutError;
		for (auto& Pair : Impl->Instances)
			if (Pair.Value && Pair.Value->Session && Pair.Value->Session->LiveDomain)
				Pair.Value->Session->LiveDomain->Poison(Failure);
	};
	ON_SCOPE_EXIT { ReleaseBarriers(); };
	TArray<FGeneratedTypeRuntimeInstance*> PreparedInstances;
	TMap<FAvidScriptRuntimeExecutionDomain*, TSharedPtr<FAvidScriptRuntimeExecutionDomain>> CandidateDomains;
	PreparedInstances.Reserve(Impl->Instances.Num());
	bool bFailedCandidateRestored = true;
	for (TPair<FObjectKey, TUniquePtr<FGeneratedTypeRuntimeInstance>>& Pair : Impl->Instances)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (Impl->ReloadFailureAfterInstanceCountForTesting == PreparedInstances.Num())
		{
			Impl->ReloadFailureAfterInstanceCountForTesting = INDEX_NONE;
			OutError = TEXT("generated type package reload injected a deterministic test failure");
			break;
		}
#endif
		FGeneratedTypeRuntimeInstance* const Instance = Pair.Value.Get();
		if (Instance == nullptr || !Instance->Session || !Instance->Session->IsLiveLoaded())
		{
			OutError = TEXT("generated type package reload found an inactive Runtime Session");
			break;
		}
		FAvidScriptWasmReloadResult SessionResult;
		auto& CandidateDomain = CandidateDomains.FindOrAdd(Instance->Session->LiveDomain.Get());
		if (!Instance->Session->ReloadArtifactInternal(Artifact, SessionResult, true, &CandidateDomain))
		{
			bFailedCandidateRestored = !SessionResult.bHostEffectRollbackAttempted || SessionResult.bHostEffectRollbackSucceeded;
			OutError = SessionResult.ErrorMessage.IsEmpty()
				? TEXT("generated type Runtime Session rejected the candidate package")
				: SessionResult.ErrorMessage;
			break;
		}
		PreparedInstances.Add(Instance);
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (Impl->ReloadFailureAfterInstanceCountForTesting == PreparedInstances.Num())
		OutError = TEXT("generated type package reload injected a deterministic test failure");
	Impl->ReloadFailureAfterInstanceCountForTesting = INDEX_NONE;
#endif

	OutResult.PreparedInstanceCount = PreparedInstances.Num();
	if (PreparedInstances.Num() == Impl->Instances.Num() && OutError.IsEmpty())
	{
		for (auto* Instance : PreparedInstances)
			if (!Instance->Session->ValidatePreparedActivation(OutError)) break;
	}
	if (PreparedInstances.Num() != Impl->Instances.Num() || !OutError.IsEmpty())
	{
		bool bRollbackSucceeded = bFailedCandidateRestored;
		for (int32 Index = PreparedInstances.Num() - 1; Index >= 0; --Index)
		{
			FAvidScriptWasmReloadResult RollbackResult;
			++PreparedInstances[Index]->Session->RejectedReloadCount;
			if (!PreparedInstances[Index]->Session->DiscardPreparedActivation(RollbackResult))
			{
				bRollbackSucceeded = false;
				if (!RollbackResult.ErrorMessage.IsEmpty())
				{
					OutError += FString::Printf(
						TEXT("; rollback failed: %s"),
						*RollbackResult.ErrorMessage);
				}
				continue;
			}
			++OutResult.RolledBackInstanceCount;
		}
		OutResult.bRollbackPreservedLivePackage = bRollbackSucceeded;
		if (!bRollbackSucceeded)
		{
			PoisonLiveDomains(); // Failed restoration must not execute Guest EndPlay.
			ReleaseBarriers();
			Impl->bTeardownPending = true;
			for (auto Iterator = Impl->Instances.CreateIterator(); Iterator; ++Iterator)
			{
				FString TeardownError;
				TeardownInstance(*Iterator.Value(), Impl->ObjectRegistry, TeardownError);
				if (!Iterator.Value()->Session && !Iterator.Value()->ReceiverHandle.IsValid()) Iterator.RemoveCurrent();
			}
			OutError += Impl->Instances.IsEmpty()
				? TEXT("; all generated type Sessions were torn down fail-closed")
				: TEXT("; generated type teardown remains pending without releasing registered Sessions");
		}
		return false;
	}
	for (auto* Instance : PreparedInstances)
	{
		FAvidScriptWasmReloadResult CommitResult;
		if (!Instance->Session->CommitPreparedActivation(CommitResult))
		{
			// Publication performs no Guest calls. If native lifetime changes still
			// invalidate a prepared participant, never expose a mixed package.
			OutError = TEXT("generated package publication invalidated; stopping all instances");
			for (int32 Index = PreparedInstances.Num() - 1; Index >= 0; --Index)
			{
				FAvidScriptWasmReloadResult DiscardResult;
				PreparedInstances[Index]->Session->DiscardPreparedActivation(DiscardResult);
			}
			PoisonLiveDomains(); // Do not run either generation after partial publication.
			ReleaseBarriers();
			Impl->bTeardownPending = true;
			for (auto Iterator = Impl->Instances.CreateIterator(); Iterator; ++Iterator)
			{
				FString TeardownError;
				TeardownInstance(*Iterator.Value(), Impl->ObjectRegistry, TeardownError);
				if (!Iterator.Value()->Session && !Iterator.Value()->ReceiverHandle.IsValid()) Iterator.RemoveCurrent();
			}
			return false;
		}
		++Instance->Session->SuccessfulReloadCount;
		++OutResult.ReloadedInstanceCount;
	}

	// Body-only compatibility preserves the canonical immutable type identities
	// used by the shared Runtime imports and by owners joining after this reload.
	Impl->Package.Emplace(FGeneratedTypeRuntimePackage{ PreviousPackage.Registry, Artifact });
	OutResult.Disposition =
		EAvidScriptGeneratedTypePackageReloadDisposition::BodyOnlyApplied;
	return true;
}

bool FAvidScriptGeneratedTypeRuntimeHost::InstallPackageFromDescriptorFile(
	const FString& DescriptorPath,
	FString& OutError)
{
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	FAvidScriptRuntimeArtifact Artifact;
	if (!LoadPackageFromDescriptorFile(
		DescriptorPath,
		Registry,
		Artifact,
		OutError))
	{
		return false;
	}
	return InstallPackage(Registry, Artifact, OutError);
}

bool FAvidScriptGeneratedTypeRuntimeHost::LoadPackageFromDescriptorFile(
	const FString& DescriptorPath,
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& OutRegistry,
	FAvidScriptRuntimeArtifact& OutArtifact,
	FString& OutError)
{
	OutRegistry.Reset();
	OutArtifact = FAvidScriptRuntimeArtifact();
	OutError.Reset();
	if (!Impl || !Impl->bStarted || !IsInGameThread())
	{
		OutError = TEXT("generated type package load requires a started GameThread host");
		return false;
	}

	const FString NormalizedDescriptorPath = NormalizePackagePath(DescriptorPath);
	TArray<uint8> DescriptorBytes;
	if (!FFileHelper::LoadFileToArray(DescriptorBytes, *NormalizedDescriptorPath))
	{
		OutError = FString::Printf(
			TEXT("generated type package descriptor could not be read: %s"),
			*NormalizedDescriptorPath);
		return false;
	}
	TSharedPtr<FJsonObject> Descriptor;
	double SchemaVersion = 0.0;
	FString PackageId;
	FString ModuleName;
	FString RuntimeModuleId;
	FString ExecutionBackend;
	FString GenerationKey;
	FGeneratedTypePackageFile TypeManifestEntry;
	FGeneratedTypePackageFile RuntimeManifestEntry;
	if (!DeserializeJsonObject(DescriptorBytes, Descriptor)
		|| !Descriptor->TryGetNumberField(TEXT("schema_version"), SchemaVersion)
		|| (SchemaVersion != 1.0 && SchemaVersion != 2.0)
		|| !Descriptor->TryGetStringField(TEXT("module_name"), ModuleName)
		|| ModuleName.IsEmpty()
		|| !Descriptor->TryGetStringField(TEXT("generation_key_sha256"), GenerationKey)
		|| !IsLowercaseSha256(GenerationKey)
		|| !ReadPackageFileEntry(Descriptor, TEXT("type_manifest"), TypeManifestEntry))
	{
		OutError = TEXT("generated type package descriptor schema is invalid");
		return false;
	}

	FString TypeManifestPath;
	FString RuntimeManifestPath;
	FString CanonicalWasmPath;
	FString ExpectedCanonicalWasmSha256;
	if (!ResolvePackageFile(
			NormalizedDescriptorPath,
			TypeManifestEntry,
			TypeManifestPath,
			OutError))
	{
		return false;
	}

	TArray<uint8> TypeManifestBytes;
	if (!LoadVerifiedPackageFile(
			TypeManifestPath,
			TypeManifestEntry.Sha256,
			TypeManifestBytes,
			OutError))
	{
		return false;
	}

	const bool bUsesResolvedRuntimePackage = SchemaVersion == 2.0;
	if (!bUsesResolvedRuntimePackage)
	{
		if (!Descriptor->TryGetStringField(TEXT("package_id"), PackageId)
			|| !IsLowercaseSha256(PackageId)
			|| !Descriptor->TryGetStringField(TEXT("runtime_module_id"), RuntimeModuleId)
			|| RuntimeModuleId.IsEmpty()
			|| !Descriptor->TryGetStringField(TEXT("execution_backend"), ExecutionBackend)
			|| (ExecutionBackend != TEXT("wasmtime_jit")
				&& ExecutionBackend != TEXT("wasmtime_precompiled"))
			|| !ReadPackageFileEntry(
				Descriptor,
				TEXT("runtime_manifest"),
				RuntimeManifestEntry))
		{
			OutError = TEXT("generated type package descriptor schema is invalid");
			return false;
		}
		const FString ExpectedPackageId = FAvidScriptHash::Sha256HexUtf8(FString::Printf(
			TEXT("%s\n%s\n%s"),
			*GenerationKey,
			*TypeManifestEntry.Sha256,
			*RuntimeManifestEntry.Sha256));
		if (PackageId != ExpectedPackageId)
		{
			OutError = TEXT("generated type package identity does not match its manifest hashes");
			return false;
		}
		if (!ResolvePackageFile(
				NormalizedDescriptorPath,
				RuntimeManifestEntry,
				RuntimeManifestPath,
				OutError))
		{
			return false;
		}
		TArray<uint8> RuntimeManifestBytes;
		if (!LoadVerifiedPackageFile(
				RuntimeManifestPath,
				RuntimeManifestEntry.Sha256,
				RuntimeManifestBytes,
				OutError))
		{
			return false;
		}
		TSharedPtr<FJsonObject> RuntimeManifestObject;
		FGeneratedTypePackageFile WasmEntry;
		if (!DeserializeJsonObject(RuntimeManifestBytes, RuntimeManifestObject)
			|| !ReadPackageFileEntry(RuntimeManifestObject, TEXT("wasm"), WasmEntry))
		{
			OutError = TEXT("generated type Runtime manifest has no canonical WASM identity");
			return false;
		}
		if (!ResolvePackageFile(RuntimeManifestPath, WasmEntry, CanonicalWasmPath, OutError))
		{
			return false;
		}
		ExpectedCanonicalWasmSha256 = WasmEntry.Sha256;
	}
	else
	{
		if (!Descriptor->TryGetStringField(TEXT("module_id"), RuntimeModuleId)
			|| RuntimeModuleId.IsEmpty()
			|| !Descriptor->TryGetStringField(TEXT("package_id"), PackageId)
			|| !IsLowercaseSha256(PackageId))
		{
			OutError = TEXT("generated type package descriptor schema is invalid");
			return false;
		}
		const FString GeneratedTypePackageId = FAvidScriptHash::Sha256HexUtf8(
			FString::Printf(
				TEXT("%s\n%s\n%s\n%s"),
				*GenerationKey,
				*TypeManifestEntry.Sha256,
				*RuntimeModuleId,
				*PackageId));
		if (TypeManifestEntry.RelativePath != FString::Printf(
			TEXT("%s/type-manifest.json"),
			*GeneratedTypePackageId))
		{
			OutError = TEXT("generated type package identity does not match its manifest hashes");
			return false;
		}
		FAvidScriptResolvedModulePackage ResolvedPackage;
		FAvidScriptModuleResolveResult ResolveResult;
		if (!FAvidScriptModulePackageResolver::ResolveModule(
				FName(*RuntimeModuleId), ResolvedPackage, ResolveResult)
			|| ResolvedPackage.PackageId != PackageId)
		{
			OutError = ResolveResult.ErrorMessage.IsEmpty()
				? TEXT("generated type Cook pointer does not select the current module package")
				: ResolveResult.ErrorMessage;
			return false;
		}
		CanonicalWasmPath = ResolvedPackage.CanonicalWasmPath;

	}

	TSharedPtr<FJsonObject> TypeManifestObject;
	double TypeSchemaVersion = 0.0;
	FString TypeModuleName;
	FString TypeGenerationKey;
	if (!DeserializeJsonObject(TypeManifestBytes, TypeManifestObject)
		|| !TypeManifestObject->TryGetNumberField(TEXT("schema_version"), TypeSchemaVersion)
		|| TypeSchemaVersion != FAvidScriptGeneratedTypeRegistry::ManifestSchemaVersion
		|| !TypeManifestObject->TryGetStringField(TEXT("module_name"), TypeModuleName)
		|| TypeModuleName != ModuleName
		|| !TypeManifestObject->TryGetStringField(
			TEXT("generation_key_sha256"),
			TypeGenerationKey)
		|| TypeGenerationKey != GenerationKey)
	{
		OutError = TEXT("generated type manifest identity does not match its package descriptor");
		return false;
	}

	FString TypeManifestJson;
	FFileHelper::BufferToString(
		TypeManifestJson,
		TypeManifestBytes.GetData(),
		TypeManifestBytes.Num());
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	if (!FAvidScriptGeneratedTypeRegistry::BuildFromJson(
		TypeManifestJson,
		Registry,
		OutError))
	{
		return false;
	}
	const int64 CanonicalWasmSize = IFileManager::Get().FileSize(*CanonicalWasmPath);
	if (CanonicalWasmSize <= 0 || CanonicalWasmSize > 128LL * 1024 * 1024)
	{
		OutError = TEXT("generated type canonical WASM is missing or exceeds the preflight size limit");
		return false;
	}
	TArray<uint8> CanonicalWasmBytes;
	if (!FFileHelper::LoadFileToArray(CanonicalWasmBytes, *CanonicalWasmPath)
		|| CanonicalWasmBytes.Num() != CanonicalWasmSize)
	{
		OutError = TEXT("generated type canonical WASM could not be read for import preflight");
		return false;
	}
	const FString CanonicalWasmSha256 = FAvidScriptHash::Sha256Hex(CanonicalWasmBytes);
	if (!ExpectedCanonicalWasmSha256.IsEmpty()
		&& CanonicalWasmSha256 != ExpectedCanonicalWasmSha256)
	{
		OutError = TEXT("generated type canonical WASM hash changed before import preflight");
		return false;
	}
	FAvidScriptWasmRuntimeInstance PreflightRuntime;
	TArray<FAvidScriptVmTypedHostImport> PreflightHostImports;
	if (!PreflightRuntime.ConfigureGeneratedTypeHostBindings(
			Registry, PreflightHostImports, OutError, CanonicalWasmBytes))
	{
		return false;
	}
	TArray<FAvidScriptVmExpectedImport> RuntimeAuthorizedImports;
	RuntimeAuthorizedImports.Reserve(PreflightHostImports.Num());
	for (const FAvidScriptVmTypedHostImport& Import : PreflightHostImports)
	{
		if (!Import.bSupplementalRuntimeAuthority
			|| Import.BindingOrdinal != MAX_uint32
			|| Import.ModuleName != TEXT("avidscript"))
		{
			OutError = TEXT("generated type import preflight produced an invalid capability");
			return false;
		}
		RuntimeAuthorizedImports.Add({ Import.ModuleName, Import.ImportName });
	}

	FAvidScriptRuntimeArtifact Artifact;
	FAvidScriptRuntimeArtifactLoadResult LoadResult;
	const FScopedAvidScriptRuntimeImportAuthority RuntimeImportAuthority(
		RuntimeAuthorizedImports);
	const bool bArtifactLoaded = bUsesResolvedRuntimePackage
		? FAvidScriptRuntimeArtifactLoader::LoadPublishedModule(
			FName(*RuntimeModuleId),
			PackageId,
			Artifact,
			LoadResult)
		: FAvidScriptRuntimeArtifactLoader::LoadFromFile(
			RuntimeManifestPath,
			Artifact,
			LoadResult);
	if (!bArtifactLoaded)
	{
		OutError = LoadResult.CanonicalResult.ErrorMessage.IsEmpty()
			? TEXT("generated type Runtime artifact failed to load")
			: LoadResult.CanonicalResult.ErrorMessage;
		return false;
	}
	if (Artifact.Manifest.WasmSha256 != CanonicalWasmSha256)
	{
		OutError = TEXT("generated type import preflight and loaded canonical WASM differ");
		return false;
	}
	if (Artifact.Manifest.ModuleId != RuntimeModuleId)
	{
		OutError = TEXT("generated type Runtime module identity does not match its package descriptor");
		return false;
	}
	const bool bHasNonGeneratedImport =
		Artifact.Manifest.RequiredImports.ContainsByPredicate(
			[&RuntimeAuthorizedImports](
				const FAvidScriptWasmRequiredImport& RequiredImport)
			{
				return !RuntimeAuthorizedImports.ContainsByPredicate(
					[&RequiredImport](
						const FAvidScriptVmExpectedImport& AuthorizedImport)
					{
						return AuthorizedImport.ModuleName == RequiredImport.ModuleName
							&& AuthorizedImport.ImportName == RequiredImport.ImportName;
					});
			});
	if (!bHasNonGeneratedImport)
	{
		Artifact.Manifest.BindingPackage.Reset();
	}
	if (!bUsesResolvedRuntimePackage)
	{
		if (ExecutionBackend == TEXT("wasmtime_precompiled"))
		{
			// Preserve the loader's verified AOT selection, including target and attestation.
			if (!Artifact.bUsesPrecompiledArtifact || !LoadResult.bUsesPrecompiledArtifact
				|| LoadResult.bFellBackToJit || !Artifact.FallbackCategory.IsEmpty()
				|| Artifact.ExecutionPolicy != TEXT("require_precompiled")
				|| Artifact.BackendSelection.BackendKind != EAvidScriptVmBackendKind::Wasmtime
				|| Artifact.BackendSelection.ExecutionMode != EAvidScriptVmExecutionMode::Aot
				|| Artifact.BackendSelection.ArtifactFormat != EAvidScriptVmArtifactFormat::WasmtimeSerialized
				|| Artifact.BackendSelection.bAllowFallback
				|| Artifact.VmArtifact.ArtifactFormat != EAvidScriptVmArtifactFormat::WasmtimeSerialized
				|| Artifact.VmArtifact.ExecutionBytes.IsEmpty())
			{
				OutError = TEXT("generated type precompiled package requires a verified AOT artifact with require_precompiled policy and no JIT fallback");
				return false;
			}
		}
		else
		{
			if (Artifact.bUsesPrecompiledArtifact || LoadResult.bFellBackToJit
				|| !Artifact.ExecutionPolicy.IsEmpty()
				|| Artifact.VmArtifact.ArtifactFormat != EAvidScriptVmArtifactFormat::WasmBytecode)
			{
				OutError = TEXT("generated type JIT package cannot override an execution artifact policy");
				return false;
			}
			Artifact.BackendSelection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
			Artifact.BackendSelection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
			Artifact.BackendSelection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
			Artifact.BackendSelection.bAllowFallback = false;
			Artifact.RequestedBackend = ExecutionBackend;
			Artifact.SelectedBackend = ExecutionBackend;
			Artifact.ExecutionPolicy = TEXT("generated_type_package");
		}
	}
	OutRegistry = MoveTemp(Registry);
	OutArtifact = MoveTemp(Artifact);
	return true;
}

bool FAvidScriptGeneratedTypeRuntimeHost::ReloadPackageFromDescriptorFile(
	const FString& DescriptorPath,
	FAvidScriptGeneratedTypePackageReloadResult& OutResult,
	FString& OutError)
{
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Registry;
	FAvidScriptRuntimeArtifact Artifact;
	if (!LoadPackageFromDescriptorFile(
		DescriptorPath,
		Registry,
		Artifact,
		OutError))
	{
		OutResult = FAvidScriptGeneratedTypePackageReloadResult();
		return false;
	}
	return ReloadPackage(Registry, Artifact, OutResult, OutError);
}

bool FAvidScriptGeneratedTypeRuntimeHost::ClearPackage(FString& OutError)
{
	OutError.Reset();
	if (!CanMutateInstances(OutError)) return false;
	TGuardValue<bool> MutationGuard(Impl->bMutationInProgress, true);
	if (!Impl->Instances.IsEmpty())
	{
		OutError = TEXT("generated type package clear requires zero active instances");
		return false;
	}
	Impl->Package.Reset();
	Impl->bTeardownPending = false;
	return true;
}

bool FAvidScriptGeneratedTypeRuntimeHost::HasInstalledPackage() const
{
	return Impl && Impl->bStarted && Impl->Package.IsSet();
}

bool FAvidScriptGeneratedTypeRuntimeHost::BeginInstance(
	UObject& Receiver,
	const uint32 TypeOrdinal,
	FString& OutError)
{
	OutError.Reset();
	if (IsCommandletExecutionSuppressed())
	{
		return true;
	}
	if (!CanMutateInstances(OutError)) return false;
	if (Impl->bTeardownPending)
	{
		OutError = TEXT("generated type activation requires pending instance teardown to complete");
		return false;
	}
	TGuardValue<bool> MutationGuard(Impl->bMutationInProgress, true);
	if (!Impl->Package.IsSet())
	{
		OutError = TEXT("generated type instance activation requires an installed GameThread package");
		return false;
	}
	if (Receiver.HasAnyFlags(
		RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutError = TEXT("generated type instance receiver is not a live runtime object");
		return false;
	}

	const FObjectKey ReceiverKey(&Receiver);
	if (Impl->Instances.Contains(ReceiverKey))
	{
		return true;
	}
	const FGeneratedTypeRuntimePackage& Package = Impl->Package.GetValue();
	const FAvidScriptGeneratedTypePlan* const RequestedType =
		Package.Registry->FindTypeByOrdinal(TypeOrdinal);
	if (RequestedType == nullptr || RequestedType->Class == nullptr
		|| !Receiver.IsA(RequestedType->Class))
	{
		OutError = TEXT("generated type instance does not satisfy the installed type ordinal");
		return false;
	}
	const FAvidScriptGeneratedTypePlan* RuntimeType = nullptr;
	for (UClass* Class = Receiver.GetClass(); Class != nullptr; Class = Class->GetSuperClass())
	{
		RuntimeType = Package.Registry->FindTypeByClass(Class);
		if (RuntimeType != nullptr)
		{
			break;
		}
	}
	if (RuntimeType == nullptr)
	{
		OutError = TEXT("generated type instance has no registered runtime UClass ancestry");
		return false;
	}

	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ReceiverHandle =
		Impl->ObjectRegistry.RegisterObject(&Receiver, RegisterResult, false);
	if (!ReceiverHandle.IsValid())
	{
		OutError = RegisterResult.ErrorMessage.IsEmpty()
			? TEXT("generated type instance ObjectHandle registration failed")
			: RegisterResult.ErrorMessage;
		return false;
	}

	TUniquePtr<FGeneratedTypeRuntimeInstance> Instance =
		MakeUnique<FGeneratedTypeRuntimeInstance>();
	Instance->Receiver = &Receiver;
	Instance->ReceiverHandle = ReceiverHandle;
	Instance->TypeOrdinal = RuntimeType->TypeOrdinal;
	Instance->Session = MakeUnique<FAvidScriptRuntimeSession>();

	FAvidScriptWasmHostContext HostContext;
	HostContext.World = Receiver.GetWorld();
	HostContext.ObjectRegistry = &Impl->ObjectRegistry;
	HostContext.OwnerHandle = ReceiverHandle;
	Instance->Session->SetHostContext(HostContext);
	if (!Instance->Session->ConfigureGeneratedTypeInstance(
		Receiver,
		ReceiverHandle,
		RuntimeType->TypeOrdinal,
		Package.Registry,
		OutError))
	{
		TeardownInstance(*Instance, Impl->ObjectRegistry, OutError);
		return false;
	}

	FAvidScriptWasmReloadResult LoadResult;
	TSharedPtr<FAvidScriptRuntimeExecutionDomain> Domain;
	for (const auto& Pair : Impl->Instances)
	{
		const auto* Peer = Pair.Value->Session.Get();
		if (Peer && Peer->LiveDomain && Peer->HostContext.World == HostContext.World)
		{
			Domain = Peer->LiveDomain;
			break;
		}
	}
	// The candidate may execute native callbacks before it becomes a member.
	// Fence existing members until its activation has committed or failed.
	TArray<FAvidScriptRuntimeSession*> FencedPeers;
	if (Domain)
		for (auto& Pair : Impl->Instances)
			if (Pair.Value->Session && Pair.Value->Session->LiveDomain == Domain)
			{
				Pair.Value->Session->bPackageReloadBarrier = true;
				FencedPeers.Add(Pair.Value->Session.Get());
			}
	ON_SCOPE_EXIT
	{
		for (auto* Peer : FencedPeers) Peer->bPackageReloadBarrier = false;
		if (Domain) Domain->DrainFault();
	};
	if (!Instance->Session->LoadGeneratedDomainArtifact(Package.Artifact, Domain, LoadResult))
	{
		OutError = LoadResult.ErrorMessage.IsEmpty()
			? TEXT("generated type instance Runtime artifact load failed")
			: LoadResult.ErrorMessage;
		FString TeardownError;
		TeardownInstance(*Instance, Impl->ObjectRegistry, TeardownError);
		return false;
	}

	Impl->Instances.Add(ReceiverKey, MoveTemp(Instance));
	return true;
}

bool FAvidScriptGeneratedTypeRuntimeHost::EndInstance(UObject& Receiver, FString& OutError)
{
	OutError.Reset();
	if (IsCommandletExecutionSuppressed())
	{
		return true;
	}
	if (!CanMutateInstances(OutError)) return false;
	TGuardValue<bool> MutationGuard(Impl->bMutationInProgress, true);

	const FObjectKey ReceiverKey(&Receiver);
	TUniquePtr<FGeneratedTypeRuntimeInstance>* const Found =
		Impl->Instances.Find(ReceiverKey);
	if (Found == nullptr || !Found->IsValid())
	{
		return true;
	}
	const bool bSucceeded = TeardownInstance(**Found, Impl->ObjectRegistry, OutError);
	if (!(*Found)->Session && !(*Found)->ReceiverHandle.IsValid()) Impl->Instances.Remove(ReceiverKey);
	return bSucceeded;
}

bool FAvidScriptGeneratedTypeRuntimeHost::IsInstanceActive(const UObject& Receiver) const
{
	return Impl && Impl->Instances.Contains(FObjectKey(&Receiver));
}

int32 FAvidScriptGeneratedTypeRuntimeHost::GetActiveInstanceCount() const
{
	return Impl ? Impl->Instances.Num() : 0;
}

int32 FAvidScriptGeneratedTypeRuntimeHost::GetRegisteredHandleCount() const
{
	return Impl ? Impl->ObjectRegistry.GetLiveHandleCount() : 0;
}
