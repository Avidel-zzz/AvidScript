using System;
using System.Collections.Generic;

namespace AvidScript.CSharpSemantic;

[Flags]
public enum SemanticAsyncExceptionOwnerState
{
    Unreachable = 0,
    Normal = 1,
    Fault = 2,
    HandledFault = 4,
    Cancellation = 8,
    HandledCancellation = 16,
}

// The source Task remains an owner on every fault/cancel path until a terminal
// transfer. A shared finally may be entered by several states, so consumers
// must use the full state set rather than infer ownership from a segment alone.
public static class SemanticAsyncExceptionOwnerFlow
{
    public static bool TryAnalyze(SemanticAsyncMethod method,
        out IReadOnlyDictionary<int, SemanticAsyncExceptionOwnerState> incoming)
    {
        incoming = new Dictionary<int, SemanticAsyncExceptionOwnerState>();
        if (method?.ExceptionPlan is null || method.Segments is null
            || method.EntrySegmentOrdinal < 0
            || method.EntrySegmentOrdinal >= method.Segments.Count)
            return false;

        SemanticAsyncExceptionOwnerState[] states =
            new SemanticAsyncExceptionOwnerState[method.Segments.Count];
        bool languageCancellation = method.ExceptionPlan.CancellationTypeId is not null;
        Queue<(int Ordinal, SemanticAsyncExceptionOwnerState State)> pending = new();
        pending.Enqueue((method.EntrySegmentOrdinal, SemanticAsyncExceptionOwnerState.Normal));
        while (pending.Count > 0)
        {
            (int ordinal, SemanticAsyncExceptionOwnerState state) = pending.Dequeue();
            if (ordinal < 0 || ordinal >= states.Length || state == 0) return false;
            if ((states[ordinal] & state) != 0) continue;
            states[ordinal] |= state;
            SemanticAsyncSegment? segment = method.Segments[ordinal];
            SemanticAsyncControlTransfer? transfer = segment?.Transfer;
            if (transfer is null) return false;
            switch (transfer.Kind)
            {
                case SemanticAsyncMethod.GotoTransferKind:
                    pending.Enqueue((transfer.PrimaryTarget, state));
                    break;
                case SemanticAsyncMethod.BranchTransferKind:
                    pending.Enqueue((transfer.PrimaryTarget, state));
                    pending.Enqueue((transfer.SecondaryTarget, state));
                    break;
                case SemanticAsyncMethod.AwaitTransferKind:
                    if (state != SemanticAsyncExceptionOwnerState.Normal) return false;
                    pending.Enqueue((transfer.PrimaryTarget, state));
                    if (transfer.SecondaryTarget >= 0)
                        pending.Enqueue((transfer.SecondaryTarget,
                            SemanticAsyncExceptionOwnerState.Fault));
                    if (transfer.CancellationTarget is int cancellationTarget)
                        pending.Enqueue((cancellationTarget,
                            SemanticAsyncExceptionOwnerState.Cancellation));
                    break;
                case SemanticAsyncMethod.CatchMatchTransferKind:
                    if (state != SemanticAsyncExceptionOwnerState.Fault
                        && !(languageCancellation && state == SemanticAsyncExceptionOwnerState.Cancellation))
                        return false;
                    pending.Enqueue((transfer.PrimaryTarget,
                        state == SemanticAsyncExceptionOwnerState.Fault
                            ? SemanticAsyncExceptionOwnerState.HandledFault
                            : SemanticAsyncExceptionOwnerState.HandledCancellation));
                    pending.Enqueue((transfer.SecondaryTarget, state));
                    break;
                case SemanticAsyncMethod.EndCatchTransferKind:
                case SemanticAsyncMethod.RethrowTransferKind:
                    if (!languageCancellation || state is not
                        (SemanticAsyncExceptionOwnerState.HandledFault
                            or SemanticAsyncExceptionOwnerState.HandledCancellation)) return false;
                    pending.Enqueue((transfer.PrimaryTarget,
                        transfer.Kind == SemanticAsyncMethod.EndCatchTransferKind
                            ? SemanticAsyncExceptionOwnerState.Normal
                            : state == SemanticAsyncExceptionOwnerState.HandledFault
                                ? SemanticAsyncExceptionOwnerState.Fault
                                : SemanticAsyncExceptionOwnerState.Cancellation));
                    break;
                case SemanticAsyncMethod.PropagateExceptionTransferKind:
                    if (!languageCancellation || state is not
                        (SemanticAsyncExceptionOwnerState.Fault
                            or SemanticAsyncExceptionOwnerState.Cancellation)) return false;
                    break;
                case SemanticAsyncMethod.PropagateFaultTransferKind:
                    if (state is not (SemanticAsyncExceptionOwnerState.Fault
                        or SemanticAsyncExceptionOwnerState.HandledFault)) return false;
                    break;
                case SemanticAsyncMethod.PropagateCancellationTransferKind:
                    if (state != SemanticAsyncExceptionOwnerState.Cancellation) return false;
                    break;
                case SemanticAsyncMethod.ReturnTransferKind:
                    if (state is not (SemanticAsyncExceptionOwnerState.Normal
                        or SemanticAsyncExceptionOwnerState.HandledFault)
                        && !(languageCancellation
                            && state == SemanticAsyncExceptionOwnerState.HandledCancellation)) return false;
                    break;
                case SemanticAsyncMethod.ThrowTransferKind:
                    break;
                default:
                    return false;
            }
        }

        for (int ordinal = 0; ordinal < states.Length; ++ordinal)
            if (states[ordinal] == SemanticAsyncExceptionOwnerState.Unreachable)
                return false;
        Dictionary<int, SemanticAsyncExceptionOwnerState> result = new(states.Length);
        for (int ordinal = 0; ordinal < states.Length; ++ordinal)
            result.Add(ordinal, states[ordinal]);
        incoming = result;
        return true;
    }
}
