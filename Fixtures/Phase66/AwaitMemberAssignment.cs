using System;
using System.Threading.Tasks;
using AvidScript;

// Shared by the .NET oracle and the future Guest execution suite.
public sealed class AwaitAssignmentTarget
{
    public int Field = 11;
    private int stored = 11;
    public int SetterCalls;
    public bool ThrowOnSet;
    public int Id;

    public int Value
    {
        get { return stored; }
        set
        {
            SetterCalls++;
            AwaitMemberAssignment.TotalSetterCalls++;
            AwaitMemberAssignment.Record(4);
            if (ThrowOnSet) throw new InvalidOperationException();
            stored = value;
            AwaitMemberAssignment.LastAssignedId = Id;
            AwaitMemberAssignment.LastAssignedValue = value;
        }
    }

    public async Task<int> AssignImplicit(bool property)
    {
        try
        {
            if (property) Value = await AwaitMemberAssignment.Produce(0);
            else Field = await AwaitMemberAssignment.Produce(0);
            return 7;
        }
        finally
        {
            AwaitMemberAssignment.CleanupCount++;
            AwaitMemberAssignment.Record(8);
        }
    }
}

public static class AwaitMemberAssignment
{
    public static AwaitAssignmentTarget Original = new AwaitAssignmentTarget();
    public static AwaitAssignmentTarget Replacement = new AwaitAssignmentTarget();
    public static AwaitAssignmentTarget Current = Original;
    public static AvidCancellationSource Lifetime;
    public static int ReceiverCalls;
    public static int ProducerCalls;
    public static int CleanupCount;
    public static int Trace;
    public static int TotalSetterCalls;
    public static int LastAssignedId;
    public static int LastAssignedValue;

    public static void Record(int digit) { Trace = Trace * 10 + digit; }

    private static AwaitAssignmentTarget SelectTarget(int mode)
    {
        ReceiverCalls++;
        Record(1);
        if (mode == 7) throw new ArgumentException();
        if (mode == 6) return null!;
        return Current;
    }

    public static async Task<int> Produce(int mode)
    {
        ProducerCalls++;
        Record(2);
        // The assignment must still address the object selected before this call.
        Current = Replacement;
        if (mode == 3) throw new ArgumentException();
        if (mode == 1) return 7;
        await AvidContinuations.NextTickAsync().WithCancellation(Lifetime.Token);
        Record(3);
        if (mode == 2) throw new ArgumentException();
        return 7;
    }

    public static async Task<int> Assign(bool property, int mode)
    {
        try
        {
            if (property) SelectTarget(mode).Value = await Produce(mode);
            else SelectTarget(mode).Field = await Produce(mode);
            return 7;
        }
        finally
        {
            CleanupCount++;
            Record(8);
        }
    }

    public static async Task<int> AssignTwice(bool property)
    {
        try
        {
            for (int index = 0; index < 2; index++)
            {
                if (property) SelectTarget(0).Value = await Produce(0);
                else SelectTarget(0).Field = await Produce(0);
            }
            return 14;
        }
        finally
        {
            CleanupCount++;
            Record(8);
        }
    }

    private static AwaitAssignmentTarget CreateTemporary()
    {
        ReceiverCalls++;
        Record(1);
        AwaitAssignmentTarget target = new AwaitAssignmentTarget();
        target.Id = 42;
        return target;
    }

    public static async Task<int> AssignTemporary()
    {
        try
        {
            // No source local, static field or caller retains this receiver.
            CreateTemporary().Value = await Produce(0);
            return LastAssignedValue;
        }
        finally
        {
            CleanupCount++;
            Record(8);
        }
    }

    public static async Task<int> AssignTaskLocal(bool property, bool ready)
    {
        Task<int> pending = Produce(ready ? 1 : 0);
        Task<int> alias = pending;
        try
        {
            // This Task has already changed Current before the LHS is evaluated.
            if (property) SelectTarget(0).Value = await alias;
            else SelectTarget(0).Field = await alias;
            return 7;
        }
        finally
        {
            CleanupCount++;
            Record(8);
        }
    }
}
