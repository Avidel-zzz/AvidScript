#pragma once

#include "AvidScriptWasmRuntime.h"

class FAvidScriptRuntimeSession;
class FAvidScriptGeneratedTypeRegistrySnapshot;

// Game-thread owner of one package generation in one World/registry.
// Sessions remain non-owning members so a domain cannot keep a dead owner alive.
class FAvidScriptRuntimeExecutionDomain
{
public:
	FAvidScriptRuntimeExecutionDomain(
		TSharedPtr<FAvidScriptWasmRuntimeInstance> InRuntime,
		const FAvidScriptWasmHostContext& Context,
		TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> InTypes);
	~FAvidScriptRuntimeExecutionDomain();
	bool Matches(const FAvidScriptWasmHostContext& Context,
		const TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot>& InTypes) const;
	bool HasActiveCalls() const;
	void Attach(FAvidScriptRuntimeSession& Session);
	void Detach(FAvidScriptRuntimeSession& Session);
	void Poison(const FAvidScriptWasmSmokeResult& Failure);
	void DrainFault();
	bool IsFaulted() const { return bFaulted; }
	const TSharedPtr<FAvidScriptWasmRuntimeInstance>& GetRuntime() const { return Runtime; }

private:
	TSharedPtr<FAvidScriptWasmRuntimeInstance> Runtime;
	TWeakObjectPtr<UWorld> World;
	FAvidScriptObjectRegistry* Registry = nullptr;
	TSharedPtr<const FAvidScriptGeneratedTypeRegistrySnapshot> Types;
	TArray<FAvidScriptRuntimeSession*> Members;
	FAvidScriptWasmSmokeResult RootFailure;
	bool bFaulted = false;
	bool bDraining = false;
};
