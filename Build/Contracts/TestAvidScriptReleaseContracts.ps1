$ErrorActionPreference = 'Stop'
$BuildRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$PluginRoot = Split-Path -Parent $BuildRoot
$RunnerPath = Join-Path $BuildRoot 'InvokeAvidScriptRelease.ps1'
$HeaderPath = Join-Path `
    $PluginRoot `
    'Source/AvidScriptEditor/Public/Commandlets/AvidScriptReleaseCommandlet.h'
$CommandletPath = Join-Path `
    $PluginRoot `
    'Source/AvidScriptEditor/Private/Commandlets/AvidScriptReleaseCommandlet.cpp'
$BuildConfigPath = Join-Path `
    $PluginRoot `
    'Source/AvidScriptEditor/Public/AvidScriptEditorCSharpBuildService.h'
$BuildInvokerPath = Join-Path `
    $PluginRoot `
    'Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBuildInvoker.cpp'
$BuildPipelinePath = Join-Path `
    $PluginRoot `
    'Source/AvidScriptEditor/Private/CSharpBuild/AvidScriptEditorCSharpBuildPipeline.cpp'
$GeneratedRuntimeHostHeaderPath = Join-Path `
    $PluginRoot `
    'Source/AvidScriptRuntime/Public/ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.h'
$GeneratedRuntimeHostSourcePath = Join-Path `
    $PluginRoot `
    'Source/AvidScriptRuntime/Private/ScriptTypes/AvidScriptGeneratedTypeRuntimeHost.cpp'
$GeneratedModulePath = Join-Path `
    $PluginRoot `
    'Source/AvidScriptGenerated/Private/AvidScriptGeneratedModule.cpp'
$Root = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ("AvidScriptReleaseContract_$PID`_$([Guid]::NewGuid().ToString('N'))")
$Passed = 0
$Total = 5
$Failure = $null

function Invoke-ReleaseContractTest {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    try {
        & $Body
        $script:Passed++
    }
    catch {
        throw "$Name failed: $($_.Exception.Message)"
    }
}

function Get-ReleaseScriptAst {
    param([Parameter(Mandatory = $true)][string]$Path)

    $Tokens = $null
    $Errors = $null
    $Ast = [System.Management.Automation.Language.Parser]::ParseFile(
        $Path,
        [ref]$Tokens,
        [ref]$Errors)
    if ($Errors.Count -ne 0) {
        throw "$Path contains PowerShell parse errors: $($Errors.Message -join '; ')"
    }
    return $Ast
}

function Get-ReleaseFunctionAst {
    param(
        [Parameter(Mandatory = $true)]$Ast,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $Functions = @($Ast.FindAll({
                param($Node)
                $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
                    $Node.Name -ceq $Name
            }, $true))
    if ($Functions.Count -ne 1) {
        throw "Expected exactly one function named $Name."
    }
    return $Functions[0]
}

try {
    foreach ($RequiredFile in @(
            $RunnerPath,
            $HeaderPath,
            $CommandletPath,
            $BuildConfigPath,
            $BuildInvokerPath,
            $BuildPipelinePath,
            $GeneratedRuntimeHostHeaderPath,
            $GeneratedRuntimeHostSourcePath,
            $GeneratedModulePath)) {
        if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
            throw "Required release contract file is missing: $RequiredFile"
        }
    }
    $RunnerAst = Get-ReleaseScriptAst $RunnerPath
    . $RunnerPath `
        -SourcePath 'contract-source' `
        -CSharpProjectPath 'contract-project' `
        -ModuleId 'contract_module' `
        -ArtifactStem 'contract_artifact' `
        -OutputRoot 'contract-output' `
        -DotNetPath 'contract-dotnet'
    $HeaderSource = Get-Content -Raw -LiteralPath $HeaderPath
    $CommandletSource = Get-Content -Raw -LiteralPath $CommandletPath
    $RunnerSource = Get-Content -Raw -LiteralPath $RunnerPath
    $BuildConfigSource = Get-Content -Raw -LiteralPath $BuildConfigPath
    $BuildInvokerSource = Get-Content -Raw -LiteralPath $BuildInvokerPath
    $BuildPipelineSource = Get-Content -Raw -LiteralPath $BuildPipelinePath
    $GeneratedRuntimeHostHeader = Get-Content -Raw -LiteralPath $GeneratedRuntimeHostHeaderPath
    $GeneratedRuntimeHostSource = Get-Content -Raw -LiteralPath $GeneratedRuntimeHostSourcePath
    $GeneratedModuleSource = Get-Content -Raw -LiteralPath $GeneratedModulePath

    Invoke-ReleaseContractTest -Name 'command schema' -Body {
        $ExpectedParameters = @(
            'ArtifactStem',
            'BindingPackagePath',
            'Configuration',
            'CSharpProjectPath',
            'DotNetPath',
            'EngineRoot',
            'GeneratedTypeManifestPath',
            'ModuleId',
            'OutputRoot',
            'RuntimeBindingPackagePath',
            'SourcePath')
        $ActualParameters = @($RunnerAst.ParamBlock.Parameters |
                ForEach-Object { $_.Name.VariablePath.UserPath } |
                Sort-Object)
        if ([string]::Join('|', $ActualParameters) -cne
            [string]::Join('|', $ExpectedParameters)) {
            throw 'InvokeAvidScriptRelease.ps1 exposes an unexpected parameter set.'
        }
        foreach ($RequiredToken in @(
                'UCLASS()',
                'virtual int32 Main(const FString& Params) override;',
                'UCommandlet::ParseCommandLine(*Params, Tokens, Switches)',
                'TEXT("SourcePath")',
                'TEXT("CSharpProjectPath")',
                'TEXT("ModuleId")',
                'TEXT("ArtifactStem")',
                'TEXT("OutputRoot")',
                'TEXT("DotNetPath")',
                'TEXT("BindingPackagePath")',
                'TEXT("RuntimeBindingPackagePath")',
                'TEXT("GeneratedTypeManifestPath")',
                'ParameterName.Equals(TEXT("run"), ESearchCase::IgnoreCase)',
                'TEXT("AvidScriptRelease")',
                'TEXT("argument_missing")',
                'TEXT("argument_unknown")',
                'TEXT("argument_duplicate")')) {
            if (-not ($HeaderSource.Contains($RequiredToken) -or
                    $CommandletSource.Contains($RequiredToken))) {
                throw "Commandlet schema token is missing: $RequiredToken"
            }
        }
        $Arguments = @(New-AvidScriptReleaseCommandletArguments `
                -SourcePath 'C:\Project\Source\Module.cs' `
                -CSharpProjectPath 'C:\Project\Source\Module.csproj' `
                -ModuleId 'contract_module' `
                -ArtifactStem 'contract_artifact' `
                -OutputRoot 'C:\Project\Saved\Release' `
                -DotNetPath 'C:\DotNet\dotnet.exe' `
                -BindingPackagePath 'C:\Project\Saved\Bindings\package.json' `
                -RuntimeBindingPackagePath 'C:\Project\Saved\Bindings\runtime.json' `
                -GeneratedTypeManifestPath 'C:\Project\Saved\Generated\types.json' `
                -AbsLog 'C:\Project\Saved\Logs\release.log')
        foreach ($ExpectedArgument in @(
                '-run=AvidScriptRelease',
                '-ModuleId=contract_module',
                '-ArtifactStem=contract_artifact',
                '-GeneratedTypeManifestPath=C:\Project\Saved\Generated\types.json',
                '-AvidScriptSuppressGeneratedTypeExecution',
                '-unattended',
                '-nullrhi')) {
            if ($Arguments -cnotcontains $ExpectedArgument) {
                throw "Commandlet argument is missing: $ExpectedArgument"
            }
        }
        foreach ($RequiredIsolationToken in @(
                'static bool IsCommandletExecutionSuppressed();',
                'IsRunningCommandlet()',
                'TEXT("AvidScriptSuppressGeneratedTypeExecution")')) {
            if (-not ($GeneratedRuntimeHostHeader.Contains($RequiredIsolationToken) -or
                    $GeneratedRuntimeHostSource.Contains($RequiredIsolationToken) -or
                    $GeneratedModuleSource.Contains($RequiredIsolationToken))) {
                throw "Generated Type commandlet isolation token is missing: $RequiredIsolationToken"
            }
        }
        foreach ($RequiredBuildToken in @(
                'FString GeneratedTypeManifestPath;',
                'bool bAllowGeneratedTypeImports = false;',
                'TEXT("-GeneratedTypeManifestPath")',
                'TEXT("-AllowGeneratedTypeImports")',
                'TEXT("generated_type_manifest_missing")',
                'TEXT("generated_type_manifest_unused")')) {
            if (-not ($BuildConfigSource.Contains($RequiredBuildToken) -or
                    $BuildInvokerSource.Contains($RequiredBuildToken) -or
                    $BuildPipelineSource.Contains($RequiredBuildToken))) {
                throw "Generated Type release plumbing is missing: $RequiredBuildToken"
            }
        }
    }

    Invoke-ReleaseContractTest -Name 'missing file rejection' -Body {
        $ProjectRoot = Join-Path $Root 'MissingProject'
        [void][System.IO.Directory]::CreateDirectory($ProjectRoot)
        $Rejected = $false
        try {
            Resolve-AvidScriptReleaseProjectPath `
                -Path (Join-Path $ProjectRoot 'missing.cs') `
                -ProjectRoot $ProjectRoot `
                -Label 'SourcePath' `
                -PathType Leaf | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq 'input_missing'
        }
        if (-not $Rejected) {
            throw 'A missing release input file was accepted.'
        }
    }

    Invoke-ReleaseContractTest -Name 'project path escape rejection' -Body {
        $ProjectRoot = Join-Path $Root 'EscapeProject'
        $OutsideRoot = Join-Path $Root 'OutsideProject'
        [void][System.IO.Directory]::CreateDirectory($ProjectRoot)
        [void][System.IO.Directory]::CreateDirectory($OutsideRoot)
        $OutsidePath = Join-Path $OutsideRoot 'module.cs'
        [System.IO.File]::WriteAllText($OutsidePath, 'public class Module {}')
        $Rejected = $false
        try {
            Resolve-AvidScriptReleaseProjectPath `
                -Path $OutsidePath `
                -ProjectRoot $ProjectRoot `
                -Label 'SourcePath' `
                -PathType Leaf | Out-Null
        }
        catch {
            $Rejected = [string]$_.Exception.Data['category'] -ceq
                'path_outside_project'
        }
        if (-not $Rejected) {
            throw 'A release input outside ProjectRoot was accepted.'
        }
    }

    Invoke-ReleaseContractTest -Name 'Shipping precompiled policy' -Body {
        $InvokeFunction = Get-ReleaseFunctionAst `
            -Ast $RunnerAst `
            -Name 'Invoke-AvidScriptRelease'
        $InvokeText = $InvokeFunction.Extent.Text
        if (-not $RunnerSource.Contains(
                "[ValidateSet('Development', 'Shipping')]") -or
            -not $InvokeText.Contains('-Configuration $Configuration') -or
            -not $CommandletSource.Contains(
                'EAvidScriptEditorVmArtifactPolicy::RequirePrecompiled') -or
            -not $CommandletSource.Contains(
                'BuildResult.VmArtifactPolicy != TEXT("require_precompiled")') -or
            -not $CommandletSource.Contains(
                'Policy == TEXT("require_precompiled")')) {
            throw 'Shipping is not bound to the required precompiled release path.'
        }
    }

    Invoke-ReleaseContractTest -Name 'publisher handoff' -Body {
        $PublishFunction = Get-ReleaseFunctionAst `
            -Ast $RunnerAst `
            -Name 'Publish-AvidScriptReleaseRuntimePackage'
        $PublishText = $PublishFunction.Extent.Text
        foreach ($RequiredToken in @(
                'AvidScriptModuleReleasePackage.ps1',
                'Publish-AvidScriptModuleReleasePackage',
                '-RuntimeManifestPath $RuntimeManifestPath',
                '-ProjectRoot $ProjectRoot',
                '-ModuleId $ModuleId',
                '-Configuration $Configuration')) {
            if (-not $PublishText.Contains($RequiredToken)) {
                throw "Publisher handoff token is missing: $RequiredToken"
            }
        }
        foreach ($RequiredRunnerToken in @(
                "'Verify'",
                "'-run=AvidScriptRelease'",
                "[System.IO.Path]::GetFullPath('C:\UnrealEngine')",
                '[int]$BuildVersion.MinorVersion -ne 8',
                "[Guid]::NewGuid().ToString('N')",
                '"-abslog=$AbsLog"',
                'ArgumentList.Add($Argument)',
                'avidscript_module_release_succeeded',
                'ConvertTo-Json -Depth 16 -Compress')) {
            if (-not $RunnerSource.Contains($RequiredRunnerToken)) {
                throw "Headless release runner token is missing: $RequiredRunnerToken"
            }
        }
    }
}
catch {
    $Failure = $_.Exception.Message
}
finally {
    if (Test-Path -LiteralPath $Root -PathType Container) {
        Remove-Item -LiteralPath $Root -Recurse -Force
    }
}

if ($null -ne $Failure) {
    [Console]::Error.WriteLine(
        "AvidScript release contracts: $Passed/$Total passed; FAIL: $Failure")
    exit 1
}

Write-Output "AvidScript release contracts: PASS ($Passed/$Total)"
exit 0
