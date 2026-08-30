using System;

internal static class Program
{
    private static int Main()
    {
        try
        {
            int count = UeTypeGeneratorTests.Run();
            Console.WriteLine($"AvidScript.UeTypeGenerator.Tests: {count}/{count} passed");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }
}
