#include "AvidScriptWasmRuntime.h"
#include "AvidScriptLanguageErrorCatalog.h"
#include "AvidScriptExceptionCancellationIdentity.h"
#include "Engine/World.h"

bool FAvidScriptWasmRuntimeInstance::DispatchExceptionCancellationTokenCall(
	const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult)
{
	OutResult = {};
	auto Fail = [&OutResult](const TCHAR* Category, const TCHAR* Details)
	{
		OutResult.ErrorCategory = Category;
		OutResult.Details = Details;
		return false;
	};
	if (!LanguageErrorCatalog || !LanguageErrorCatalog->SupportsExceptionCancellationToken())
		return Fail(TEXT("exception_cancellation_version"),
			TEXT("Exception token access requires catalog-bearing Guest IR 34/1.33."));
	if (!IsInGameThread() || !IsLoaded() || !ManagedHeap || ManagedHeapInvocationDepth == 0
		|| !HostContext.Continuations || !HostContext.Continuations->IsInvocationContextLive(HostContext.World.Get())
		|| ((HostContext.OwnerHandle.IsValid() || ContextInvocationDepth != 0) && !ValidateInvocationContext(HostContext)))
		return Fail(TEXT("exception_cancellation_context"),
			TEXT("Exception token access requires a live Session activation and managed VM invocation."));
	const auto Object = static_cast<AvidScript::Managed::FToken>(Call.Int64Args[0]);
	if (!ManagedHeap->IsObjectRootedInCurrentFrame(Object, ManagedHeapFrameFloor))
		return Fail(TEXT("exception_cancellation_root"),
			TEXT("Exception token access requires a live object rooted in this invocation."));
	// The native snapshot is immutable and owns no Task, source or extra root.
	// Missing data is an error; only a recorded zero denotes source-less cancellation.
	const auto* Identity = AvidScript::Continuation::FExceptionCancellationIdentity::Find(*ManagedHeap, Object);
	if (!Identity || Identity->Source > 0 || !LanguageErrorCatalog->IsCancellationType(Identity->ExceptionType))
		return Fail(TEXT("exception_cancellation_identity"),
			TEXT("The exception has no valid cancellation identity in the loaded catalog."));
	OutResult.ReturnValueI64 = Identity->Source;
	OutResult.ReturnValue = static_cast<int32>(Identity->Source);
	OutResult.bSucceeded = true;
	return true;
}
