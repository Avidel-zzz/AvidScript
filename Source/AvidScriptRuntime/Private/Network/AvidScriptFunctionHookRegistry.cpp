#include "Network/AvidScriptFunctionHookRegistry.h"

#include "UObject/Class.h"
#include "UObject/Stack.h"
#include "UObject/WeakObjectPtr.h"

namespace
{
struct FAvidScriptInstalledFunctionRoute
{
	TWeakObjectPtr<UObject> Source;
	IAvidScriptFunctionHookSink* Sink = nullptr;
	uint32 HandlerOrdinal = MAX_uint32;
};

struct FAvidScriptInstalledFunctionHook
{
	FNativeFuncPtr Original = nullptr;
	TArray<FAvidScriptInstalledFunctionRoute> Routes;
};

TMap<UFunction*, FAvidScriptInstalledFunctionHook>& GetHooks()
{
	static TMap<UFunction*, FAvidScriptInstalledFunctionHook> Hooks;
	return Hooks;
}

void InvokeAvidScriptFunctionHook(
	UObject* Context,
	FFrame& Stack,
	RESULT_DECL)
{
	check(IsInGameThread());
	UFunction* const Function = Stack.CurrentNativeFunction;
	FAvidScriptInstalledFunctionHook* const Hook =
		Function == nullptr ? nullptr : GetHooks().Find(Function);
	if (Hook == nullptr)
	{
		return;
	}

	for (const FAvidScriptInstalledFunctionRoute& Route : Hook->Routes)
	{
		if (Route.Source.Get() == Context && Route.Sink != nullptr)
		{
			Route.Sink->HandleAvidScriptInboundFunction(
				Route.HandlerOrdinal,
				*Function,
				Stack.Locals);
			return;
		}
	}

	if (Hook->Original != nullptr)
	{
		Hook->Original(Context, Stack, RESULT_PARAM);
	}
}

bool ValidateRoutes(
	IAvidScriptFunctionHookSink& Sink,
	const TConstArrayView<FAvidScriptFunctionHookRoute> Routes,
	FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	TSet<FString> CandidateKeys;
	for (const FAvidScriptFunctionHookRoute& Route : Routes)
	{
		if (!IsValid(Route.Source)
			|| !IsValid(Route.Function)
			|| Route.HandlerOrdinal == MAX_uint32
			|| Route.Function->GetNativeFunc() == nullptr)
		{
			OutError = TEXT("function_hook_route_invalid");
			return false;
		}
		const FString Key = FString::Printf(
			TEXT("%p:%p"),
			Route.Source,
			Route.Function);
		if (CandidateKeys.Contains(Key))
		{
			OutError = TEXT("function_hook_route_duplicate");
			return false;
		}
		CandidateKeys.Add(Key);

		if (const FAvidScriptInstalledFunctionHook* Hook =
			GetHooks().Find(Route.Function))
		{
			if (Route.Function->GetNativeFunc()
				!= &InvokeAvidScriptFunctionHook)
			{
				OutError = TEXT("function_hook_thunk_replaced");
				return false;
			}
			for (const FAvidScriptInstalledFunctionRoute& Existing :
				Hook->Routes)
			{
				if (Existing.Source.Get() == Route.Source
					&& Existing.Sink != &Sink)
				{
					OutError = TEXT("function_hook_route_owned");
					return false;
				}
			}
		}
		else if (Route.Function->GetNativeFunc()
			== &InvokeAvidScriptFunctionHook)
		{
			OutError = TEXT("function_hook_registry_desynchronized");
			return false;
		}
	}
	return true;
}

void RemoveRoutesInternal(IAvidScriptFunctionHookSink& Sink)
{
	TArray<UFunction*> EmptyHooks;
	for (TPair<UFunction*, FAvidScriptInstalledFunctionHook>& Pair : GetHooks())
	{
		Pair.Value.Routes.RemoveAll(
			[&Sink](const FAvidScriptInstalledFunctionRoute& Route)
			{
				return Route.Sink == &Sink;
			});
		if (Pair.Value.Routes.IsEmpty())
		{
			if (!IsValid(Pair.Key))
			{
				EmptyHooks.Add(Pair.Key);
			}
			else if (Pair.Key->GetNativeFunc()
				== &InvokeAvidScriptFunctionHook)
			{
				Pair.Key->SetNativeFunc(Pair.Value.Original);
				EmptyHooks.Add(Pair.Key);
			}
		}
	}
	for (UFunction* Function : EmptyHooks)
	{
		GetHooks().Remove(Function);
	}
}
} // namespace

bool FAvidScriptFunctionHookRegistry::ValidateReplacement(
	IAvidScriptFunctionHookSink& Sink,
	const TConstArrayView<FAvidScriptFunctionHookRoute> Routes,
	FString& OutError)
{
	return ValidateRoutes(Sink, Routes, OutError);
}

bool FAvidScriptFunctionHookRegistry::ReplaceRoutes(
	IAvidScriptFunctionHookSink& Sink,
	const TConstArrayView<FAvidScriptFunctionHookRoute> Routes,
	FString& OutError)
{
	if (!ValidateRoutes(Sink, Routes, OutError))
	{
		return false;
	}
	RemoveRoutesInternal(Sink);
	for (const FAvidScriptFunctionHookRoute& Route : Routes)
	{
		FAvidScriptInstalledFunctionHook* Hook = GetHooks().Find(Route.Function);
		if (Hook == nullptr)
		{
			FAvidScriptInstalledFunctionHook& NewHook =
				GetHooks().Add(Route.Function);
			NewHook.Original = Route.Function->GetNativeFunc();
			Route.Function->SetNativeFunc(&InvokeAvidScriptFunctionHook);
			Hook = &NewHook;
		}
		FAvidScriptInstalledFunctionRoute& Installed =
			Hook->Routes.AddDefaulted_GetRef();
		Installed.Source = Route.Source;
		Installed.Sink = &Sink;
		Installed.HandlerOrdinal = Route.HandlerOrdinal;
	}
	return true;
}

void FAvidScriptFunctionHookRegistry::RemoveRoutes(
	IAvidScriptFunctionHookSink& Sink)
{
	check(IsInGameThread());
	RemoveRoutesInternal(Sink);
}

int32 FAvidScriptFunctionHookRegistry::NumRoutes(
	const IAvidScriptFunctionHookSink& Sink)
{
	check(IsInGameThread());
	int32 Count = 0;
	for (const TPair<UFunction*, FAvidScriptInstalledFunctionHook>& Pair :
		GetHooks())
	{
		for (const FAvidScriptInstalledFunctionRoute& Route : Pair.Value.Routes)
		{
			if (Route.Sink == &Sink)
			{
				++Count;
			}
		}
	}
	return Count;
}
