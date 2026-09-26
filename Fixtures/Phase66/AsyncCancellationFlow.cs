using System;
using System.Threading.Tasks;
using AvidScript;

public static class CancellationScript
{
    public static int Trace;
    public static int InnerCatch;
    public static int OuterCatch;
    public static int WrongCatch;
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
}
