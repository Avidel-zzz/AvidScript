using System.Threading.Tasks;
using AvidScript;

public static class TaskLifetimeScript
{
    private const int Seed = 16;
    public static int ChildFinally;
    public static int InnerCatch;
    public static int OuterCatch;
    public static int OuterFinally;
    public static int Iterations;
    public static int AfterWait;
    internal static AvidCancellationSource Lifetime;

    public static async Task<int> Child(int value)
    {
        try
        {
            await AvidContinuations.NextTickAsync().WithCancellation(Lifetime.Token);
            return value;
        }
        catch (TaskCanceledException) { InnerCatch++; throw; }
        finally { ChildFinally++; }
    }

    public static async Task<int> Run()
    {
        Task<int> pending = Child(Seed);
        Task<int> saved = pending;
        pending = Child(Seed + 1);
        int result = 0;
        try
        {
            for (int i = 0; i < 2; i++)
            {
                Task<int> iteration = saved;
                if (i == 1) iteration = pending;
                int value = await iteration;
                result += value;
                Iterations++;
                if (i == 0) continue;
                break;
            }
        }
        catch (TaskCanceledException) { OuterCatch++; result = 90; }
        finally { OuterFinally++; }

        // Both source owners survive this unrelated wait. Cancelled Task error
        // objects must remain rooted until these owners leave the method.
        await AvidContinuations.NextTickAsync();
        AfterWait++;
        return result;
    }
}
