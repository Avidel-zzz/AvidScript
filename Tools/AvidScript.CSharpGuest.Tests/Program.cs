using System;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            if (args.Length == 1 && args[0] == "--delegate-events")
            {
                int focusedCount = CSharpGuestDelegateEventTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.DelegateEvents: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length != 0)
            {
                throw new ArgumentException("Supported argument: --delegate-events; omit arguments for the full suite.");
            }
            int count = CSharpGuestLoweringTests.Run()
                + CSharpGuestOperationTests.Run()
                + CSharpGuestAdvancedTests.Run()
                + CSharpGuestFlowTests.Run()
                + CSharpGuestDataTests.Run()
                + CSharpGuestReferenceTests.Run()
                + CSharpGuestCliTests.Run()
                + CSharpGuestStateSchemaTests.Run()
                + CSharpGuestDebugMapTests.Run()
                + CSharpGuestDebugResumableTests.Run()
                + CSharpGuestMalformedTests.Run()
                + CSharpGuestOperatorTests.Run()
                + CSharpGuestClassReferenceTests.Run()
                + CSharpGuestObjectCapabilityTests.Run()
                + CSharpGuestArrayCapabilityTests.Run()
                + CSharpGuestCompositeCapabilityTests.Run()
                + CSharpDataLaneFusionTests.Run()
                + CSharpGuestDelegateEventTests.Run()
                + CSharpGuestContinuationTests.Run()
                + CSharpGuestUeTypeTests.Run();
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
