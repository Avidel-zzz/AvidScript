param(
    [Parameter(Mandatory = $true)][string]$PipeName,
    [Parameter(Mandatory = $true)][string]$SemanticRequestPath,
    [Parameter(Mandatory = $true)][string]$GuestRequestPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'AvidScriptCSharpCompilerWorker.ps1')
$checks = 0
function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
$response = $null
$request = $null
$context = $null
foreach ($path in @($SemanticRequestPath, $GuestRequestPath)) {
    $inputRequest = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $context = [pscustomobject]@{ ProtocolVersion=1; PipeName=$PipeName; ToolchainFingerprint=$inputRequest.toolchain_fingerprint; RequestTimeoutMilliseconds=10000 }
    $fields = @{}
    foreach ($property in $inputRequest.PSObject.Properties) {
        if ($property.Name -notin @('protocol_version', 'request_id', 'toolchain_fingerprint', 'stage')) { $fields[$property.Name] = $property.Value }
    }
    $request = New-AvidScriptCompilerWorkerRequest -Context $context -Stage $inputRequest.stage -Fields $fields
    Check ([int]$request.protocol_version -eq 2) 'Profile requests must use protocol 2 independently of the legacy connection context.'
    $response = Invoke-AvidScriptCompilerWorkerRaw -Context $context -Request $request
    Check ([int]$response.exit_code -in @(0,1) -and
        [string]$response.language_profile.contract_sha256 -ceq [string]$inputRequest.language_profile.contract_sha256) 'Actual pipe response must carry the profile identity.'
}
Check ([bool]$response.succeeded) 'Guest profile execution must succeed.'
foreach ($mutation in @('hash', 'name', 'missing', 'protocol')) {
    $changed = $response | ConvertTo-Json -Depth 12 | ConvertFrom-Json
    switch ($mutation) {
        'hash' { $changed.language_profile.contract_sha256 = 'b' * 64 }
        'name' { $changed.language_profile.name = 'gameplay-v2' }
        'missing' { $changed.PSObject.Properties.Remove('language_profile') }
        'protocol' { $changed.protocol_version = 1 }
    }
    $rejected = $false
    try { Assert-AvidScriptCompilerWorkerResponseIdentity -Context $context -Request $request -Response $changed }
    catch { $rejected = $true }
    Check $rejected "Response identity mutation must be rejected: $mutation"
}
Write-Output "AvidScript.LanguageProfileWorkerClient: $checks/$checks passed"
