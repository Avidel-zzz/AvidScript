using System;
using System.Threading.Tasks;
using AvidScript;

public static class CancellationScript
{
    public static int Trace;
    public static int InnerCatch;
    public static int OuterCatch;
    public static int WrongCatch;
    public static int RepeatTrace;
    public static int ConditionalTrace;
    internal static AvidCancellationSource Lifetime;

    public static async Task<int> InnerAsync(int mode)
    {
        try
        {
            await AvidContinuations.NextTickAsync().WithCancellation(Lifetime.Token);
            return 16;
        }
        catch (TaskCanceledException)
        {
            InnerCatch++;
            if (mode == 1) return 17;
            if (mode == 2) throw;
        }
        catch (OperationCanceledException)
        {
            WrongCatch++;
            return 19;
        }
        finally { Trace = Trace * 10 + 1; }

        // A handled cancellation must release its owner before another await.
        await AvidContinuations.NextTickAsync();
        return 18;
    }

    public static async Task<int> OuterAsync(int mode)
    {
        try
        {
            int result = await InnerAsync(mode);
            return result;
        }
        catch (InvalidOperationException) { WrongCatch++; return 99; }
        catch (OperationCanceledException) { OuterCatch++; return 20; }
        finally { Trace = Trace * 10 + 2; }
    }

    public static async Task<int> UnhandledAsync()
    {
        try
        {
            int result = await InnerAsync(2);
            return result;
        }
        catch (InvalidOperationException) { WrongCatch++; return 99; }
        finally { Trace = Trace * 10 + 2; }
    }

    public static async Task<int> NestedAsync()
    {
        try
        {
            try
            {
                await AvidContinuations.NextTickAsync().WithCancellation(Lifetime.Token);
                return 16;
            }
            catch (TaskCanceledException) { InnerCatch++; throw; }
            finally { Trace = Trace * 10 + 1; }
        }
        catch (Exception) { OuterCatch++; return 21; }
        finally { Trace = Trace * 10 + 2; }
    }

    public static async Task<int> CatchAllAsync()
    {
        try
        {
            await AvidContinuations.NextTickAsync().WithCancellation(Lifetime.Token);
            return 16;
        }
        catch { InnerCatch++; return 22; }
        finally { Trace = Trace * 10 + 1; }
    }

    public static async Task<int> RepeatedAsync(int mode)
    {
        Task<int> pending = InnerAsync(2);
        Task<int> alias = pending;
        int result = 0;
        if (mode == 1)
        {
            // Exercise the ready path as well as the pending Task callback.
            await AvidContinuations.NextTickAsync();
            await AvidContinuations.NextTickAsync();
            await AvidContinuations.NextTickAsync();
        }
        try { result = await pending; }
        catch (TaskCanceledException)
        {
            OuterCatch++;
            if (mode == 2) return 21;
            if (mode == 3) throw;
            result = 20;
        }
        finally { RepeatTrace = RepeatTrace * 10 + 1; }

        // The catch owner has been released, but the local and its alias still
        // own the same Task while this unrelated continuation is suspended.
        await AvidContinuations.NextTickAsync();
        try
        {
            int again = await alias;
            result += again;
        }
        catch (OperationCanceledException) { OuterCatch++; result += 20; }
        finally { RepeatTrace = RepeatTrace * 10 + 2; }
        return result;
    }

    public static async Task<int> ConditionalAsync(int mode)
    {
        int result = 0;
        try
        {
            if (mode == 0)
            {
                Task<int> left = InnerAsync(2);
                Task<int> copied = left;
                result = await copied;
            }
            else if (mode == 1)
            {
                await AvidContinuations.NextTickAsync();
                Task<int> right = InnerAsync(2);
                result = await right;
            }
            else if (mode == 2)
                result = 5;
            else
            {
                Task<int> pending = InnerAsync(2);
                result = await pending;
                // Cancellation of the first await skips this declaration.
                Task<int> late = pending;
                int again = await late;
                result += again;
            }
        }
        catch (TaskCanceledException) { OuterCatch++; result = 30; }
        finally { ConditionalTrace = 1; }

        await AvidContinuations.NextTickAsync();
        ConditionalTrace = ConditionalTrace * 10 + 2;
        return result;
    }
}
