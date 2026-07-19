using System;

internal static class Program
{
    private static int Main()
    {
        try
        {
            int count = SemanticCompilationTests.Run() + SemanticOperationTests.Run() +
                SemanticControlFlowTests.Run() + SemanticInstanceStateTests.Run() +
                SemanticCallableTests.Run() + SemanticReachabilityTests.Run() +
                SemanticCliTests.Run() + SemanticStateContractTests.Run();
            Console.WriteLine($"AvidScript.CSharpSemantic.Tests: {count}/{count} passed");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }
}
