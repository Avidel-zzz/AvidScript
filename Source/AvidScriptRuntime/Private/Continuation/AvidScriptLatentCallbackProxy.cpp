#include "Continuation/AvidScriptLatentCallbackProxy.h"

#include "Continuation/AvidScriptSessionContinuations.h"

void UAvidScriptLatentCallbackProxy::Arm(
	TWeakPtr<FAvidScriptSessionContinuations> InOwner,
	const int64 InToken)
{
	Owner = MoveTemp(InOwner);
	Token = InToken;
}

void UAvidScriptLatentCallbackProxy::Disarm()
{
	Owner.Reset();
	Token = 0;
}

void UAvidScriptLatentCallbackProxy::OnLatentCompleted(const int32 Linkage)
{
	const TSharedPtr<FAvidScriptSessionContinuations> PinnedOwner = Owner.Pin();
	if (PinnedOwner && Token != 0)
	{
		PinnedOwner->HandleLatentCompletion(Token, Linkage);
	}
}
