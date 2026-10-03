# C# owns profile definitions and source admission. No version table lives here.
function Invoke-AvidScriptCSharpProfileTool {
    param([Parameter(Mandatory = $true)]$Profile, [Parameter(Mandatory = $true)][string[]]$Arguments)
    $Raw = @(& $Profile.DotNetPath $Profile.GuestDllPath @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "ASBI4701: C# profile owner rejected input: $($Raw -join [Environment]::NewLine)" }
    try { return ($Raw -join [Environment]::NewLine) | ConvertFrom-Json -ErrorAction Stop }
    catch { throw "ASBI4701: Invalid C# profile owner response: $($_.Exception.Message)" }
}

function Assert-AvidScriptCSharpProfileIdentity {
    param([AllowNull()]$Expected, [AllowNull()]$Actual)
    if ($null -eq $Expected) {
        if ($null -ne $Actual) { throw 'ASBI4702: A profile artifact cannot be reused by a legacy build.' }
        return
    }
    if ($null -eq $Actual -or @($Actual.PSObject.Properties).Count -ne 2 -or
        [string]$Actual.name -cne [string]$Expected.name -or
        [string]$Actual.contract_sha256 -cne [string]$Expected.contract_sha256) {
        throw 'ASBI4702: Language profile identity is missing or differs from the C# owner.'
    }
}

function Assert-AvidScriptCSharpResolvedProfile {
    param([AllowNull()]$Profile)
    if ($null -eq $Profile) { return }
    $Descriptor = Invoke-AvidScriptCSharpProfileTool -Profile $Profile -Arguments @('--describe-language-profile', [string]$Profile.Identity.name)
    $Identity = [pscustomobject]@{ name = [string]$Descriptor.name; contract_sha256 = [string]$Descriptor.contract_sha256 }
    Assert-AvidScriptCSharpProfileIdentity -Expected $Identity -Actual $Profile.Identity
    if (($Profile.Definition | ConvertTo-Json -Depth 16 -Compress) -cne ($Descriptor.definition | ConvertTo-Json -Depth 16 -Compress)) {
        throw 'ASBI4702: Resolved language policy differs from the C# owner definition.'
    }
}

function Resolve-AvidScriptCSharpLanguageProfile {
    param([Parameter(Mandatory = $true)][string]$Name, [Parameter(Mandatory = $true)][string]$PluginRoot,
        [Parameter(Mandatory = $true)][string]$DotNetPath, [Parameter(Mandatory = $true)][string]$Configuration)
    # The caller already resolved the installed host. All settings are process scoped.
    $env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = '0'
    $env:DOTNET_SKIP_WORKLOAD_INTEGRITY_CHECK = '1'
    $env:DOTNET_GENERATE_ASPNET_CERTIFICATE = '0'
    $env:DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
    $env:DOTNET_NOLOGO = '1'
    $env:MSBUILDDISABLENODEREUSE = '1'
    $env:DOTNET_CLI_USE_MSBUILD_SERVER = '0'
    $env:UseSharedCompilation = 'false'
    $env:DOTNET_CLI_HOME = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AvidScript/Toolchain/GameplayBuild'
    $Project = Join-Path $PluginRoot 'Tools/AvidScript.CSharpGuest/AvidScript.CSharpGuest.csproj'
    $Raw = @(& $DotNetPath build $Project --configuration $Configuration --no-restore --disable-build-servers -m:1 -nodeReuse:false -p:UseSharedCompilation=false --nologo --verbosity quiet 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "ASBI4701: Cannot prepare C# profile owner: $($Raw -join [Environment]::NewLine)" }
    $Profile = [pscustomobject]@{
        DotNetPath = $DotNetPath
        GuestDllPath = Join-Path $PluginRoot "Tools/AvidScript.CSharpGuest/bin/$Configuration/net8.0/AvidScript.CSharpGuest.dll"
        Identity = $null
    }
    $Descriptor = Invoke-AvidScriptCSharpProfileTool -Profile $Profile -Arguments @('--describe-language-profile', $Name)
    $Profile.Identity = [pscustomobject][ordered]@{ name = [string]$Descriptor.name; contract_sha256 = [string]$Descriptor.contract_sha256 }
    $Profile | Add-Member -NotePropertyName Definition -NotePropertyValue $Descriptor.definition
    return $Profile
}

function Get-AvidScriptCSharpProfileToolIdentity {
    param([Parameter(Mandatory = $true)]$Profile)
    Assert-AvidScriptCSharpResolvedProfile -Profile $Profile
    $Root = Split-Path -Parent $Profile.GuestDllPath
    $Files = [ordered]@{}
    foreach ($Name in @('AvidScript.CSharpGuest', 'AvidScript.CSharpSemantic', 'AvidScript.CSharpFrontend', 'AvidScript.GuestIr')) {
        $Files[$Name] = (Get-FileHash -LiteralPath (Join-Path $Root "$Name.dll") -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    return $Files
}

function Get-AvidScriptCSharpProfileAdmission {
    param([Parameter(Mandatory = $true)]$Profile, [Parameter(Mandatory = $true)][string]$SemanticPath, [string]$ModuleId = '')
    $Arguments = @('--validate-language-profile', [string]$Profile.Identity.name, '--semantic', $SemanticPath)
    if (-not [string]::IsNullOrWhiteSpace($ModuleId)) { $Arguments += @('--module-id', $ModuleId) }
    $Admission = Invoke-AvidScriptCSharpProfileTool -Profile $Profile -Arguments $Arguments
    Assert-AvidScriptCSharpProfileIdentity -Expected $Profile.Identity -Actual $Admission.language_profile
    if ([int]$Admission.schema_version -ne 1 -or
        [string]$Admission.semantic_sha256 -cne (Get-FileHash -LiteralPath $SemanticPath -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw 'ASBI4701: Profile admission does not identify the current Semantic bytes.'
    }
    return $Admission
}
