using System;

internal static class Program
{
    private static int Main()
    {
        try
        {
            int count = SemanticCompilationTests.Run() + SemanticCliTests.Run();
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
