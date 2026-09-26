using System;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            if (args.Length == 1 && args[0] == "--static-initialization")
            {
                int focused = SemanticStaticInitializationTests.Run();
                Console.WriteLine($"AvidScript.CSharpSemantic.Tests.StaticInitialization: {focused}/{focused} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--async-member-assignment")
            {
                int focused = SemanticAsyncMemberAssignmentTests.Run();
                Console.WriteLine($"AvidScript.CSharpSemantic.Tests.AsyncMemberAssignment: {focused}/{focused} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--async-throw-routing")
            {
                int focused = SemanticAsyncThrowRoutingTests.Run();
                Console.WriteLine($"AvidScript.CSharpSemantic.Tests.AsyncThrowRouting: {focused}/{focused} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--task-local-lifetime")
            {
                int focused = SemanticAsyncTaskLocalLifetimeTests.Run();
                Console.WriteLine($"AvidScript.CSharpSemantic.Tests.TaskLocalLifetime: {focused}/{focused} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--task-owner-flow")
            {
                int focused = SemanticAsyncTaskOwnerFlowTests.Run();
                Console.WriteLine($"AvidScript.CSharpSemantic.Tests.TaskOwnerFlow: {focused}/{focused} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--async-cancellation")
            {
                int focused = SemanticAsyncCancellationTests.Run();
                Console.WriteLine($"AvidScript.CSharpSemantic.Tests.AsyncCancellation: {focused}/{focused} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--async-invocation")
            {
                int focused = SemanticAsyncInvocationTests.Run();
                Console.WriteLine($"AvidScript.CSharpSemantic.Tests.AsyncInvocation: {focused}/{focused} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--async-scopes")
            {
                int focused = SemanticAsyncScopeTests.Run();
                Console.WriteLine($"AvidScript.CSharpSemantic.Tests.AsyncScopes: {focused}/{focused} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--ue-method-catalog")
            {
                int focused = SemanticUeMethodCatalogTests.Run();
                Console.WriteLine($"AvidScript.CSharpSemantic.Tests.UeMethodCatalog: {focused}/{focused} passed");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--exception-flow")
            {
                int focused = SemanticExceptionFlowTests.Run();
                Console.WriteLine($"AvidScript.CSharpSemantic.Tests.ExceptionFlow: {focused}/{focused} passed");
                return 0;
            }
            int count = SemanticCompilationTests.Run() + SemanticOperationTests.Run() +
                SemanticControlFlowTests.Run() + SemanticInstanceStateTests.Run() +
                SemanticCallableTests.Run() + SemanticReachabilityTests.Run() +
                SemanticCliTests.Run() + SemanticStateContractTests.Run() +
                SemanticDelegateEventTests.Run() + SemanticContinuationTests.Run() +
                SemanticAsyncTests.Run() + SemanticAsyncScopeTests.Run() + SemanticAsyncInvocationTests.Run() + SemanticUeTypeDeclarationTests.Run() +
                SemanticCompilerWorkspaceTests.Run() + SemanticLocalFunctionTests.Run() + SemanticDelegateTypeTests.Run() + SemanticClosureTests.Run()
                + SemanticClosureAllocationTests.Run() + SemanticClassTypeTests.Run() + SemanticDispatchTests.Run() + SemanticUeMethodCatalogTests.Run()
                + SemanticExceptionFlowTests.Run() + SemanticAsyncCancellationTests.Run() + SemanticAsyncTaskOwnerFlowTests.Run()
                + SemanticAsyncTaskLocalLifetimeTests.Run() + SemanticAsyncThrowRoutingTests.Run()
                + SemanticAsyncMemberAssignmentTests.Run() + SemanticStaticInitializationTests.Run();
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
