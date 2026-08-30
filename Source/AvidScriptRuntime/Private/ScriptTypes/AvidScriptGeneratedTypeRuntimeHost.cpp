#include "ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h"

#include "AvidScriptHash.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptRuntimeArtifact.h"
#include "AvidScriptRuntimeSession.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ScriptTypes/AvidScriptGeneratedTypeRegistry.h"
#include "Validation/AvidScriptWasmImportPolicy.h"
#include "UObject/ObjectKey.h"

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
	FString& OutError)
{
	bool bSucceeded = true;
	if (Instance.Session)
	{
		if (Instance.Session->IsLiveLoaded())
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
			bSucceeded = false;
			if (OutError.IsEmpty())
			{
				OutError = ClearError;
			}
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
}

struct FAvidScriptGeneratedTypeRuntimeHost::FImpl
{
	bool bStarted = false;
	TOptional<FGeneratedTypeRuntimePackage> Package;
	FAvidScriptObjectRegistry ObjectRegistry;
	TMap<FObjectKey, TUniquePtr<FGeneratedTypeRuntimeInstance>> Instances;
};

FAvidScriptGeneratedTypeRuntimeHost& FAvidScriptGeneratedTypeRuntimeHost::Get()
{
	static FAvidScriptGeneratedTypeRuntimeHost Host;
	return Host;
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
#endif

FAvidScriptGeneratedTypeRuntimeHost::FAvidScriptGeneratedTypeRuntimeHost() = default;

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
	return true;
}

void FAvidScriptGeneratedTypeRuntimeHost::Shutdown()
{
	if (!Impl)
	{
		return;
	}
	check(IsInGameThread());
	for (TPair<FObjectKey, TUniquePtr<FGeneratedTypeRuntimeInstance>>& Pair : Impl->Instances)
	{
		FString Error;
		ensureMsgf(
			TeardownInstance(*Pair.Value, Impl->ObjectRegistry, Error),
			TEXT("Generated type Runtime host shutdown failed: %s"),
			Error.IsEmpty() ? TEXT("unknown") : *Error);
	}
	Impl->Instances.Reset();
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
	if (!Impl || !Impl->bStarted || !IsInGameThread())
	{
		OutError = TEXT("generated type package installation requires a started GameThread host");
		return false;
	}
	if (!Impl->Instances.IsEmpty())
	{
		OutError = TEXT("generated type package replacement requires zero active instances");
		return false;
	}
	if (!Registry.IsValid() || Registry->Num() == 0)
	{
		OutError = TEXT("generated type package registry is empty");
		return false;
	}
	if (Artifact.Manifest.ModuleId.IsEmpty()
		|| Artifact.Manifest.RequiredExports.IsEmpty()
		|| Artifact.VmArtifact.ExecutionBytes.IsEmpty()
			&& Artifact.VmArtifact.CanonicalWasmBytes.IsEmpty())
	{
		OutError = TEXT("generated type package Runtime artifact is incomplete");
		return false;
	}

	Impl->Package.Emplace(FGeneratedTypeRuntimePackage{ Registry, Artifact });
	return true;
}

bool FAvidScriptGeneratedTypeRuntimeHost::InstallPackageFromDescriptorFile(
	const FString& DescriptorPath,
	FString& OutError)
{
	OutError.Reset();
	if (!Impl || !Impl->bStarted || !IsInGameThread())
	{
		OutError = TEXT("generated type package installation requires a started GameThread host");
		return false;
	}
	if (!Impl->Instances.IsEmpty())
	{
		OutError = TEXT("generated type package replacement requires zero active instances");
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
		|| SchemaVersion != 1.0
		|| !Descriptor->TryGetStringField(TEXT("package_id"), PackageId)
		|| !IsLowercaseSha256(PackageId)
		|| !Descriptor->TryGetStringField(TEXT("module_name"), ModuleName)
		|| ModuleName.IsEmpty()
		|| !Descriptor->TryGetStringField(TEXT("runtime_module_id"), RuntimeModuleId)
		|| RuntimeModuleId.IsEmpty()
		|| !Descriptor->TryGetStringField(TEXT("execution_backend"), ExecutionBackend)
		|| ExecutionBackend != TEXT("wasmtime_jit")
		|| !Descriptor->TryGetStringField(TEXT("generation_key_sha256"), GenerationKey)
		|| !IsLowercaseSha256(GenerationKey)
		|| !ReadPackageFileEntry(Descriptor, TEXT("type_manifest"), TypeManifestEntry)
		|| !ReadPackageFileEntry(Descriptor, TEXT("runtime_manifest"), RuntimeManifestEntry))
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

	FString TypeManifestPath;
	FString RuntimeManifestPath;
	if (!ResolvePackageFile(
			NormalizedDescriptorPath,
			TypeManifestEntry,
			TypeManifestPath,
			OutError)
		|| !ResolvePackageFile(
			NormalizedDescriptorPath,
			RuntimeManifestEntry,
			RuntimeManifestPath,
			OutError))
	{
		return false;
	}

	TArray<uint8> TypeManifestBytes;
	TArray<uint8> RuntimeManifestBytes;
	if (!LoadVerifiedPackageFile(
			TypeManifestPath,
			TypeManifestEntry.Sha256,
			TypeManifestBytes,
			OutError)
		|| !LoadVerifiedPackageFile(
			RuntimeManifestPath,
			RuntimeManifestEntry.Sha256,
			RuntimeManifestBytes,
			OutError))
	{
		return false;
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
	TArray<FAvidScriptVmExpectedImport> RuntimeAuthorizedImports;
	for (const FAvidScriptGeneratedTypePlan& Type : Registry->GetTypes())
	{
		for (const FAvidScriptGeneratedMemberPlan& Member : Type.Members)
		{
			if (Member.Kind != EAvidScriptGeneratedMemberKind::Property)
			{
				continue;
			}
			if (!Member.GetterImportName.IsEmpty())
			{
				RuntimeAuthorizedImports.Add({
					TEXT("avidscript"),
					Member.GetterImportName
				});
			}
			if (!Member.SetterImportName.IsEmpty())
			{
				RuntimeAuthorizedImports.Add({
					TEXT("avidscript"),
					Member.SetterImportName
				});
			}
		}
	}

	FAvidScriptRuntimeArtifact Artifact;
	FAvidScriptRuntimeArtifactLoadResult LoadResult;
	const FScopedAvidScriptRuntimeImportAuthority RuntimeImportAuthority(
		RuntimeAuthorizedImports);
	if (!FAvidScriptRuntimeArtifactLoader::LoadFromFile(
		RuntimeManifestPath,
		Artifact,
		LoadResult))
	{
		OutError = LoadResult.CanonicalResult.ErrorMessage.IsEmpty()
			? TEXT("generated type Runtime artifact failed to load")
			: LoadResult.CanonicalResult.ErrorMessage;
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
	Artifact.BackendSelection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Artifact.BackendSelection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Artifact.BackendSelection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	Artifact.RequestedBackend = ExecutionBackend;
	Artifact.SelectedBackend = ExecutionBackend;
	Artifact.ExecutionPolicy = TEXT("generated_type_package");
	return InstallPackage(Registry, Artifact, OutError);
}

bool FAvidScriptGeneratedTypeRuntimeHost::ClearPackage(FString& OutError)
{
	OutError.Reset();
	if (!Impl || !Impl->bStarted || !IsInGameThread())
	{
		OutError = TEXT("generated type package clear requires a started GameThread host");
		return false;
	}
	if (!Impl->Instances.IsEmpty())
	{
		OutError = TEXT("generated type package clear requires zero active instances");
		return false;
	}
	Impl->Package.Reset();
	return true;
}

bool FAvidScriptGeneratedTypeRuntimeHost::BeginInstance(
	UObject& Receiver,
	const uint32 TypeOrdinal,
	FString& OutError)
{
	OutError.Reset();
	if (!Impl || !Impl->bStarted || !IsInGameThread() || !Impl->Package.IsSet())
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
	if (!Instance->Session->LoadInitialArtifact(Package.Artifact, LoadResult))
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
	if (!Impl || !Impl->bStarted || !IsInGameThread())
	{
		OutError = TEXT("generated type instance teardown requires a started GameThread host");
		return false;
	}

	const FObjectKey ReceiverKey(&Receiver);
	TUniquePtr<FGeneratedTypeRuntimeInstance>* const Found =
		Impl->Instances.Find(ReceiverKey);
	if (Found == nullptr || !Found->IsValid())
	{
		return true;
	}
	TUniquePtr<FGeneratedTypeRuntimeInstance> Instance = MoveTemp(*Found);
	Impl->Instances.Remove(ReceiverKey);
	return TeardownInstance(*Instance, Impl->ObjectRegistry, OutError);
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
