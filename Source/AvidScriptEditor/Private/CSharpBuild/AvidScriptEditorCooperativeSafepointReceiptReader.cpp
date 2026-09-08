#include "CSharpBuild/AvidScriptEditorCooperativeSafepointReceiptReader.h"

#include "AvidScriptHash.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
bool IsEditorSafepointLowercaseSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}

bool TryGetEditorSafepointUInt(
	const FJsonObject& Object,
	const TCHAR* FieldName,
	uint32& OutValue)
{
	double Number = 0.0;
	if (!Object.TryGetNumberField(FieldName, Number)
		|| !FMath::IsFinite(Number)
		|| Number < 0.0
		|| Number > static_cast<double>(MAX_uint32))
	{
		return false;
	}
	const uint32 Value = static_cast<uint32>(Number);
	if (static_cast<double>(Value) != Number)
	{
		return false;
	}
	OutValue = Value;
	return true;
}

bool SetEditorSafepointReceiptError(
	FString& OutErrorCategory,
	FString& OutError,
	const TCHAR* Category,
	FString Error)
{
	OutErrorCategory = Category;
	OutError = MoveTemp(Error);
	return false;
}

bool ResolveEditorSafepointReceiptPath(
	const FString& OutputRoot,
	const FString& ReceiptFile,
	FString& OutPath)
{
	FString NormalizedRoot = FPaths::ConvertRelativePathToFull(OutputRoot);
	FPaths::NormalizeFilename(NormalizedRoot);
	FPaths::CollapseRelativeDirectories(NormalizedRoot);
	if (FPaths::IsRelative(ReceiptFile))
	{
		FString ProjectRelativeCandidate = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), ReceiptFile));
		FPaths::NormalizeFilename(ProjectRelativeCandidate);
		FPaths::CollapseRelativeDirectories(ProjectRelativeCandidate);
		OutPath = FPaths::IsUnderDirectory(
			ProjectRelativeCandidate,
			NormalizedRoot)
			? MoveTemp(ProjectRelativeCandidate)
			: FPaths::Combine(NormalizedRoot, ReceiptFile);
	}
	else
	{
		OutPath = ReceiptFile;
	}
	OutPath = FPaths::ConvertRelativePathToFull(OutPath);
	FPaths::NormalizeFilename(OutPath);
	FPaths::CollapseRelativeDirectories(OutPath);
	return FPaths::IsUnderDirectory(OutPath, NormalizedRoot)
		&& OutPath.EndsWith(
			TEXT(".safepoints.json"),
			ESearchCase::IgnoreCase);
}
} // namespace

bool LoadAvidScriptEditorCooperativeSafepointMetadata(
	const TSharedRef<FJsonObject>& ManifestObject,
	const FString& OutputRoot,
	const FString& CanonicalWasmSha256,
	FAvidScriptEditorCooperativeSafepointMetadata& OutMetadata,
	FString& OutErrorCategory,
	FString& OutError)
{
	OutMetadata = FAvidScriptEditorCooperativeSafepointMetadata();
	OutErrorCategory.Reset();
	OutError.Reset();
	const TSharedPtr<FJsonObject>* CompilationObject = nullptr;
	if (!ManifestObject->TryGetObjectField(
			TEXT("compilation"),
			CompilationObject))
	{
		return true;
	}
	if (CompilationObject == nullptr || !CompilationObject->IsValid())
	{
		return SetEditorSafepointReceiptError(
			OutErrorCategory,
			OutError,
			TEXT("vm_artifact_safepoint_manifest_invalid"),
			TEXT("The manifest compilation field must be an object."));
	}
	const TSharedPtr<FJsonObject>* SafepointObject = nullptr;
	if (!(*CompilationObject)->TryGetObjectField(
			TEXT("cooperative_safepoints"),
			SafepointObject))
	{
		return true;
	}
	if (SafepointObject == nullptr || !SafepointObject->IsValid())
	{
		return SetEditorSafepointReceiptError(
			OutErrorCategory,
			OutError,
			TEXT("vm_artifact_safepoint_manifest_invalid"),
			TEXT("The cooperative_safepoints field must be an object."));
	}

	bool bEnabled = false;
	if (!(*SafepointObject)->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		return SetEditorSafepointReceiptError(
			OutErrorCategory,
			OutError,
			TEXT("vm_artifact_safepoint_manifest_invalid"),
			TEXT("The cooperative_safepoints object requires an enabled flag."));
	}
	if (!bEnabled)
	{
		return true;
	}

	FString ReceiptFile;
	FString ReceiptSha256;
	FString ManifestSiteSha256;
	uint32 ManifestProofSchemaVersion = 0;
	uint32 ManifestPollInterval = 0;
	uint32 ManifestSiteCount = 0;
	bool bManifestVerified = false;
	if (!(*SafepointObject)->TryGetStringField(
			TEXT("receipt_file"),
			ReceiptFile)
		|| !(*SafepointObject)->TryGetStringField(
			TEXT("receipt_sha256"),
			ReceiptSha256)
		|| !TryGetEditorSafepointUInt(
			**SafepointObject,
			TEXT("schema_version"),
			ManifestProofSchemaVersion)
		|| !TryGetEditorSafepointUInt(
			**SafepointObject,
			TEXT("poll_interval"),
			ManifestPollInterval)
		|| !TryGetEditorSafepointUInt(
			**SafepointObject,
			TEXT("site_count"),
			ManifestSiteCount)
		|| !(*SafepointObject)->TryGetStringField(
			TEXT("site_sha256"),
			ManifestSiteSha256)
		|| !(*SafepointObject)->TryGetBoolField(
			TEXT("verified"),
			bManifestVerified)
		|| !bManifestVerified
		|| ManifestProofSchemaVersion != 2
		|| ManifestPollInterval == 0
		|| ManifestPollInterval > 65536
		|| !IsEditorSafepointLowercaseSha256(ReceiptSha256)
		|| !IsEditorSafepointLowercaseSha256(ManifestSiteSha256))
	{
		return SetEditorSafepointReceiptError(
			OutErrorCategory,
			OutError,
			TEXT("vm_artifact_safepoint_manifest_invalid"),
			TEXT("The enabled cooperative_safepoints summary is incomplete or invalid."));
	}

	FString ReceiptPath;
	if (!ResolveEditorSafepointReceiptPath(
			OutputRoot,
			ReceiptFile,
			ReceiptPath))
	{
		return SetEditorSafepointReceiptError(
			OutErrorCategory,
			OutError,
			TEXT("vm_artifact_safepoint_receipt_path_invalid"),
			TEXT("The cooperative safepoint receipt must remain inside the C# output root."));
	}
	TArray<uint8> ReceiptBytes;
	if (!FFileHelper::LoadFileToArray(ReceiptBytes, *ReceiptPath)
		|| ReceiptBytes.IsEmpty())
	{
		return SetEditorSafepointReceiptError(
			OutErrorCategory,
			OutError,
			TEXT("vm_artifact_safepoint_receipt_read_failed"),
			FString::Printf(
				TEXT("The cooperative safepoint receipt could not be read: %s"),
				*ReceiptPath));
	}
	const FString ObservedReceiptSha256 =
		FAvidScriptHash::Sha256Hex(ReceiptBytes);
	if (ObservedReceiptSha256 != ReceiptSha256)
	{
		return SetEditorSafepointReceiptError(
			OutErrorCategory,
			OutError,
			TEXT("vm_artifact_safepoint_receipt_hash_mismatch"),
			TEXT("The cooperative safepoint receipt SHA-256 differs from the manifest."));
	}

	const FUTF8ToTCHAR ConvertedReceipt(
		reinterpret_cast<const ANSICHAR*>(ReceiptBytes.GetData()),
		ReceiptBytes.Num());
	const FString ReceiptJson(
		ConvertedReceipt.Length(),
		ConvertedReceipt.Get());
	TSharedPtr<FJsonObject> ReceiptObject;
	if (!FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(ReceiptJson),
			ReceiptObject)
		|| !ReceiptObject.IsValid())
	{
		return SetEditorSafepointReceiptError(
			OutErrorCategory,
			OutError,
			TEXT("vm_artifact_safepoint_receipt_invalid"),
			TEXT("The cooperative safepoint receipt is not valid JSON."));
	}

	const TSharedPtr<FJsonObject>* GuestIrObject = nullptr;
	FString ManifestGuestIrSha256;
	FString ReceiptGuestIrSha256;
	FString ReceiptWasmSha256;
	uint32 ReceiptSchemaVersion = 0;
	const TSharedPtr<FJsonObject>* ProofObject = nullptr;
	if (!ManifestObject->TryGetObjectField(TEXT("guest_ir"), GuestIrObject)
		|| GuestIrObject == nullptr
		|| !GuestIrObject->IsValid()
		|| !(*GuestIrObject)->TryGetStringField(
			TEXT("sha256"),
			ManifestGuestIrSha256)
		|| !TryGetEditorSafepointUInt(
			*ReceiptObject,
			TEXT("schema_version"),
			ReceiptSchemaVersion)
		|| !ReceiptObject->TryGetStringField(
			TEXT("guest_ir_sha256"),
			ReceiptGuestIrSha256)
		|| !ReceiptObject->TryGetStringField(
			TEXT("wasm_sha256"),
			ReceiptWasmSha256)
		|| !ReceiptObject->TryGetObjectField(TEXT("proof"), ProofObject)
		|| ProofObject == nullptr
		|| !ProofObject->IsValid())
	{
		return SetEditorSafepointReceiptError(
			OutErrorCategory,
			OutError,
			TEXT("vm_artifact_safepoint_receipt_invalid"),
			TEXT("The cooperative safepoint receipt identity envelope is incomplete."));
	}

	uint32 ProofSchemaVersion = 0;
	uint32 PollInterval = 0;
	uint32 LoopPollBlockCount = 0;
	uint32 RecursiveFunctionCount = 0;
	uint32 SiteCount = 0;
	FString SiteSha256;
	bool bVerified = false;
	if (!TryGetEditorSafepointUInt(
			**ProofObject,
			TEXT("schema_version"),
			ProofSchemaVersion)
		|| !TryGetEditorSafepointUInt(
			**ProofObject,
			TEXT("poll_interval"),
			PollInterval)
		|| !TryGetEditorSafepointUInt(
			**ProofObject,
			TEXT("loop_poll_blocks"),
			LoopPollBlockCount)
		|| !TryGetEditorSafepointUInt(
			**ProofObject,
			TEXT("recursive_functions"),
			RecursiveFunctionCount)
		|| !TryGetEditorSafepointUInt(
			**ProofObject,
			TEXT("site_count"),
			SiteCount)
		|| !(*ProofObject)->TryGetStringField(
			TEXT("site_sha256"),
			SiteSha256)
		|| !(*ProofObject)->TryGetBoolField(TEXT("verified"), bVerified)
		|| ReceiptSchemaVersion != 1
		|| ProofSchemaVersion != 2
		|| PollInterval == 0
		|| PollInterval > 65536
		|| !bVerified
		|| !IsEditorSafepointLowercaseSha256(ManifestGuestIrSha256)
		|| !IsEditorSafepointLowercaseSha256(ReceiptGuestIrSha256)
		|| !IsEditorSafepointLowercaseSha256(ReceiptWasmSha256)
		|| !IsEditorSafepointLowercaseSha256(SiteSha256)
		|| ManifestGuestIrSha256 != ReceiptGuestIrSha256
		|| CanonicalWasmSha256 != ReceiptWasmSha256
		|| ManifestProofSchemaVersion != ProofSchemaVersion
		|| ManifestPollInterval != PollInterval
		|| ManifestSiteCount != SiteCount
		|| ManifestSiteSha256 != SiteSha256
		|| static_cast<uint64>(LoopPollBlockCount)
			+ static_cast<uint64>(RecursiveFunctionCount)
			!= SiteCount)
	{
		return SetEditorSafepointReceiptError(
			OutErrorCategory,
			OutError,
			TEXT("vm_artifact_safepoint_receipt_invalid"),
			TEXT("The cooperative safepoint receipt differs from the manifest or canonical build identities."));
	}

	OutMetadata.bEnabled = true;
	OutMetadata.ReceiptPath = MoveTemp(ReceiptPath);
	OutMetadata.ReceiptSha256 = ObservedReceiptSha256;
	OutMetadata.VmReceipt.ReceiptSchemaVersion =
		static_cast<int32>(ReceiptSchemaVersion);
	OutMetadata.VmReceipt.GuestIrIdentity = MoveTemp(ReceiptGuestIrSha256);
	OutMetadata.VmReceipt.CanonicalWasmIdentity = ReceiptWasmSha256;
	OutMetadata.VmReceipt.ProofSchemaVersion =
		static_cast<int32>(ProofSchemaVersion);
	OutMetadata.VmReceipt.PollInterval = PollInterval;
	OutMetadata.VmReceipt.LoopPollBlockCount = LoopPollBlockCount;
	OutMetadata.VmReceipt.RecursiveFunctionCount = RecursiveFunctionCount;
	OutMetadata.VmReceipt.SiteCount = SiteCount;
	OutMetadata.VmReceipt.SiteSha256 = MoveTemp(SiteSha256);
	OutMetadata.VmReceipt.bVerified = true;
	return true;
}
