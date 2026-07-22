using System;

internal static class Program
{
    private static int Main()
    {
        try
        {
            int count = CSharpGuestLoweringTests.Run()
                + CSharpGuestOperationTests.Run()
                + CSharpGuestAdvancedTests.Run()
                + CSharpGuestFlowTests.Run()
                + CSharpGuestDataTests.Run()
                + CSharpGuestReferenceTests.Run()
                + CSharpGuestCliTests.Run()
                + CSharpGuestStateSchemaTests.Run()
                + CSharpGuestDebugMapTests.Run()
                + CSharpGuestMalformedTests.Run()
                + CSharpGuestOperatorTests.Run()
                + CSharpGuestClassReferenceTests.Run();
            Console.WriteLine($"AvidScript.CSharpGuest.Tests: {count}/{count} passed");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }
}
