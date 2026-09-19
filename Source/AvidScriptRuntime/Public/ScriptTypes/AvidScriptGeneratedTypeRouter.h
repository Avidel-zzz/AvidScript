#pragma once

#include "AvidScriptObjectRegistry.h"
#include "ScriptTypes/AvidScriptGeneratedTypeDispatcher.h"
#include "UObject/ObjectKey.h"

class FAvidScriptGeneratedTypeRouter;

enum class EAvidScriptGeneratedInvocationFailure : uint8
{
	None, InvalidReceiver, UnregisteredReceiver, InvalidGeneration,
	DepthLimit, EntryLimit, IdentityExhausted, TargetFailure, GenerationChanged
};

// Native call-chain identity, not a Guest capability or a cross-domain ABI.
struct FAvidScriptGeneratedInvocation
{
	uint64 InvocationId = 0;
	uint64 RootInvocationId = 0;
	uint64 ParentInvocationId = 0;
	uint64 SourceRegistrationId = 0;
	uint64 TargetRegistrationId = 0;
	uint64 ExecutionGeneration = 0;
	FAvidScriptObjectHandle ReceiverHandle;
	uint32 TypeOrdinal = 0;
	uint32 MemberOrdinal = 0;
	uint32 Depth = 0;
};

class AVIDSCRIPTRUNTIME_API IAvidScriptGeneratedTypeInstance
{
public:
	virtual ~IAvidScriptGeneratedTypeInstance() = default;

	// Zero means no code generation has been published. Retired targets keep the
	// last generation for rejection diagnostics; this is not entry authorization.
	// Successful code replacement advances it even if a VM address is reused.
	virtual uint64 GetGeneratedExecutionGeneration() const = 0;

	virtual bool InvokeGeneratedTypeMember(
		UObject& Receiver,
		const FAvidScriptObjectHandle& ReceiverHandle,
		uint32 TypeOrdinal,
		uint32 MemberOrdinal,
		TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments,
		void* Result) = 0;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptGeneratedTypeInstanceRegistration
{
public:
	FAvidScriptGeneratedTypeInstanceRegistration() = default;
	~FAvidScriptGeneratedTypeInstanceRegistration();
	FAvidScriptGeneratedTypeInstanceRegistration(
		FAvidScriptGeneratedTypeInstanceRegistration&& Other);
	FAvidScriptGeneratedTypeInstanceRegistration& operator=(
		FAvidScriptGeneratedTypeInstanceRegistration&& Other);

	FAvidScriptGeneratedTypeInstanceRegistration(
		const FAvidScriptGeneratedTypeInstanceRegistration&) = delete;
	FAvidScriptGeneratedTypeInstanceRegistration& operator=(
		const FAvidScriptGeneratedTypeInstanceRegistration&) = delete;

	bool IsValid() const;
	bool Reset();

private:
	friend class FAvidScriptGeneratedTypeRouter;

	void Invalidate();
	void MoveFrom(FAvidScriptGeneratedTypeInstanceRegistration& Other);

	FObjectKey ReceiverKey;
	IAvidScriptGeneratedTypeInstance* Instance = nullptr;
	uint64 Serial = 0;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptGeneratedTypeRouter final
	: public IAvidScriptGeneratedTypeDispatchTarget
{
public:
	static FAvidScriptGeneratedTypeRouter& Get();
	static constexpr uint32 MaxInvocationDepth = 64;
	static constexpr uint32 MaxInvocationsPerChain = 4096;
	bool GetActiveInvocation(FAvidScriptGeneratedInvocation& OutInvocation) const;
	EAvidScriptGeneratedInvocationFailure GetInvocationFailure() const;

	bool Startup();
	void Shutdown();
	bool RegisterInstance(
		UObject& Receiver,
		const FAvidScriptObjectHandle& ReceiverHandle,
		IAvidScriptGeneratedTypeInstance& Instance,
		FAvidScriptGeneratedTypeInstanceRegistration& OutRegistration);
	bool UnregisterInstance(FAvidScriptGeneratedTypeInstanceRegistration& Registration);

	bool InvokeGeneratedTypeMember(
		UObject& Receiver,
		uint32 TypeOrdinal,
		uint32 MemberOrdinal,
		TConstArrayView<FAvidScriptGeneratedCallArgument> Arguments,
		void* Result) override;

#if WITH_DEV_AUTOMATION_TESTS
	int32 GetRegisteredInstanceCountForTesting() const;
#endif

private:
	struct FImpl;

	FAvidScriptGeneratedTypeRouter();
	~FAvidScriptGeneratedTypeRouter();
	FAvidScriptGeneratedTypeRouter(const FAvidScriptGeneratedTypeRouter&) = delete;
	FAvidScriptGeneratedTypeRouter& operator=(const FAvidScriptGeneratedTypeRouter&) = delete;

	TUniquePtr<FImpl> Impl;
};
