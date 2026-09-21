using System;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
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
            int count = SemanticCompilationTests.Run() + SemanticOperationTests.Run() +
                SemanticControlFlowTests.Run() + SemanticInstanceStateTests.Run() +
                SemanticCallableTests.Run() + SemanticReachabilityTests.Run() +
                SemanticCliTests.Run() + SemanticStateContractTests.Run() +
                SemanticDelegateEventTests.Run() + SemanticContinuationTests.Run() +
                SemanticAsyncTests.Run() + SemanticAsyncScopeTests.Run() + SemanticAsyncInvocationTests.Run() + SemanticUeTypeDeclarationTests.Run() +
                SemanticCompilerWorkspaceTests.Run() + SemanticLocalFunctionTests.Run() + SemanticDelegateTypeTests.Run() + SemanticClosureTests.Run()
                + SemanticClosureAllocationTests.Run() + SemanticClassTypeTests.Run() + SemanticDispatchTests.Run() + SemanticUeMethodCatalogTests.Run();
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
