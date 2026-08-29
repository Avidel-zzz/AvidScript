#include "AvidScriptBindingLatent.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace
{
struct FAvidScriptLatentProviderRegistryState
{
	FCriticalSection Mutex;
	TMap<FString, TSharedPtr<IAvidScriptLatentCompletionProvider>> ByProviderId;
	TMap<FString, TSharedPtr<IAvidScriptLatentCompletionProvider>> ByFunctionPath;
};

FAvidScriptLatentProviderRegistryState& GetAvidScriptLatentProviderRegistry()
{
	static FAvidScriptLatentProviderRegistryState State;
	return State;
}
}

bool FAvidScriptLatentCompletionProviderRegistry::Register(
	const TSharedRef<IAvidScriptLatentCompletionProvider>& Provider,
	FString& OutError)
{
	OutError.Reset();
	const FString ProviderId = Provider->GetProviderId();
	const FString FunctionPath = Provider->GetFunctionPath();
	const FString PayloadTypeId = Provider->GetPayloadTypeId();
	if (ProviderId.IsEmpty() || FunctionPath.IsEmpty() || PayloadTypeId.IsEmpty())
	{
		OutError = TEXT("provider_id, function_path, and payload_type_id must be nonempty");
		return false;
	}

	FAvidScriptLatentProviderRegistryState& State =
		GetAvidScriptLatentProviderRegistry();
	FScopeLock Lock(&State.Mutex);
	if (State.ByProviderId.Contains(ProviderId)
		|| State.ByFunctionPath.Contains(FunctionPath))
	{
		OutError = TEXT("provider_id and function_path must both be unique");
		return false;
	}
	State.ByProviderId.Add(ProviderId, Provider);
	State.ByFunctionPath.Add(FunctionPath, Provider);
	return true;
}

bool FAvidScriptLatentCompletionProviderRegistry::Unregister(
	const TSharedRef<IAvidScriptLatentCompletionProvider>& Provider)
{
	FAvidScriptLatentProviderRegistryState& State =
		GetAvidScriptLatentProviderRegistry();
	FScopeLock Lock(&State.Mutex);
	const FString ProviderId = Provider->GetProviderId();
	const FString FunctionPath = Provider->GetFunctionPath();
	const TSharedPtr<IAvidScriptLatentCompletionProvider>* ById =
		State.ByProviderId.Find(ProviderId);
	const TSharedPtr<IAvidScriptLatentCompletionProvider>* ByFunction =
		State.ByFunctionPath.Find(FunctionPath);
	if (ById == nullptr || ByFunction == nullptr
		|| ById->Get() != &Provider.Get()
		|| ByFunction->Get() != &Provider.Get())
	{
		return false;
	}
	State.ByProviderId.Remove(ProviderId);
	State.ByFunctionPath.Remove(FunctionPath);
	return true;
}

TSharedPtr<IAvidScriptLatentCompletionProvider>
FAvidScriptLatentCompletionProviderRegistry::FindByProviderId(
	const FString& ProviderId)
{
	FAvidScriptLatentProviderRegistryState& State =
		GetAvidScriptLatentProviderRegistry();
	FScopeLock Lock(&State.Mutex);
	const TSharedPtr<IAvidScriptLatentCompletionProvider>* Provider =
		State.ByProviderId.Find(ProviderId);
	return Provider == nullptr ? nullptr : *Provider;
}

TSharedPtr<IAvidScriptLatentCompletionProvider>
FAvidScriptLatentCompletionProviderRegistry::FindByFunctionPath(
	const FString& FunctionPath)
{
	FAvidScriptLatentProviderRegistryState& State =
		GetAvidScriptLatentProviderRegistry();
	FScopeLock Lock(&State.Mutex);
	const TSharedPtr<IAvidScriptLatentCompletionProvider>* Provider =
		State.ByFunctionPath.Find(FunctionPath);
	return Provider == nullptr ? nullptr : *Provider;
}
