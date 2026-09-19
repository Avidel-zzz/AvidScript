using System;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            if (args.Length == 1 && args[0] == "--lambdas")
            {
                int focusedCount = CSharpGuestLambdaTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.Lambdas: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--managed-delegates")
            {
                int focusedCount = CSharpGuestManagedDelegateTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.ManagedDelegates: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--lexical-captures")
            {
                int focusedCount = CSharpGuestLocalFunctionTests.Run() + CSharpGuestLexicalCaptureTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.LexicalCaptures: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--delegate-events")
            {
                int focusedCount = CSharpGuestDelegateEventTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.DelegateEvents: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--captured-assignments")
            {
                int focusedCount = CSharpGuestCapturedAssignmentTests.Run()
                    + CSharpGuestFlowTests.Run()
                    + CSharpGuestReferenceTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.CapturedAssignments: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length != 0)
            {
                throw new ArgumentException("Supported arguments: --delegate-events, --captured-assignments, --lexical-captures; omit arguments for the full suite.");
            }
            int count = CSharpGuestLoweringTests.Run()
                + CSharpGuestOperationTests.Run()
                + CSharpGuestAdvancedTests.Run()
                + CSharpGuestFlowTests.Run()
                + CSharpGuestCapturedAssignmentTests.Run()
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
                + CSharpGuestShortCircuitTests.Run()
                + CSharpGuestUeTypeTests.Run()
                + CSharpGuestLocalFunctionTests.Run()
                + CSharpGuestLexicalCaptureTests.Run()
                + CSharpGuestDelegateSignatureTests.Run()
                + CSharpGuestManagedDelegateTests.Run()
                + CSharpGuestLambdaTests.Run();
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
