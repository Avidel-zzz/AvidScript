using System;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            if (args.Length == 1 && args[0] == "--async-invocation")
            {
                int focusedCount = CSharpGuestAsyncInvocationTests.Run() + CSharpGuestUeAsyncTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.AsyncInvocation: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--managed-async")
            {
                int focusedCount = CSharpGuestManagedAsyncTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.ManagedAsync: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--ue-delegates")
            {
                int focusedCount = CSharpGuestUeDispatchTests.Run(methodGroups: true);
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.UeDelegates: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--ue-dispatch")
            {
                int focusedCount = CSharpGuestUeDispatchTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.UeDispatch: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--ue-method-frames")
            {
                int focusedCount = CSharpGuestUeMethodFrameTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.UeMethodFrames: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--ue-receivers")
            {
                int focusedCount = CSharpGuestUeReceiverTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.UeReceivers: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--reference-objects")
            {
                int focusedCount = CSharpGuestReferenceObjectTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.ReferenceObjects: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--closures")
            {
                int focusedCount = CSharpGuestClosureExecutionTests.Run();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.Closures: {focusedCount}/{focusedCount} passed");
                return 0;
            }
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
            if (args.Length == 1 && args[0] == "--generated-event-state")
            {
                int focusedCount = CSharpGuestEventStateTests.RunGeneratedFacade();
                Console.WriteLine($"AvidScript.CSharpGuest.Tests.GeneratedEventState: {focusedCount}/{focusedCount} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--delegate-events")
            {
                int focusedCount = CSharpGuestDelegateEventTests.Run() + CSharpGuestEventStateTests.Run();
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
                throw new ArgumentException("Supported arguments: --delegate-events, --captured-assignments, --lexical-captures, --reference-objects; omit arguments for the full suite.");
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
                + CSharpGuestDelegateEventTests.Run() + CSharpGuestEventStateTests.Run()
                + CSharpGuestContinuationTests.Run()
                + CSharpGuestManagedAsyncTests.Run()
                + CSharpGuestAsyncInvocationTests.Run()
                + CSharpGuestUeAsyncTests.Run()
                + CSharpGuestShortCircuitTests.Run()
                + CSharpGuestUeTypeTests.Run()
                + CSharpGuestLocalFunctionTests.Run()
                + CSharpGuestLexicalCaptureTests.Run()
                + CSharpGuestDelegateSignatureTests.Run()
                + CSharpGuestManagedDelegateTests.Run()
                + CSharpGuestLambdaTests.Run()
                + CSharpGuestClosureContractTests.Run() + CSharpGuestClosureExecutionTests.Run()
                + CSharpGuestClassContractTests.Run() + CSharpGuestDispatchContractTests.Run() + CSharpGuestUeReceiverTests.Run() + CSharpGuestMethodCatalogTests.Run()
                + CSharpGuestUeMethodFrameTests.Run() + CSharpGuestUeDispatchTests.Run()
                + CSharpGuestUeDispatchTests.Run(methodGroups: true);
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
