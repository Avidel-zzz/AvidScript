$ErrorActionPreference = "Stop"
$BuildDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $BuildDir "AvidScriptCSharpBindingPackage.ps1")

function Fail-AvidScriptPreparedSemantic {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $Exception = [System.InvalidOperationException]::new("$Code $Message")
    $Exception.Data["AvidScriptCode"] = $Code
    throw $Exception
}

function Assert-AvidScriptPreparedSemantic {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        Fail-AvidScriptPreparedSemantic -Code $Code -Message $Message
    }
}

function Resolve-AvidScriptPreparedSemanticProjectPath {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$FieldName
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        Fail-AvidScriptPreparedSemantic -Code $Code -Message "$FieldName must be a non-empty project-relative or absolute path."
    }

    return Resolve-AvidScriptBindingPath -RootPath $ProjectRoot -Path $Path
}

function Import-AvidScriptPreparedSemanticJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Label
    )

    try {
        return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
    }
    catch {
        Fail-AvidScriptPreparedSemantic -Code $Code -Message "$Label JSON is invalid: $($_.Exception.Message)"
    }
}

function Assert-AvidScriptPreparedSemanticUsedImports {
    param(
        [Parameter(Mandatory = $true)]$AuthorizationModel,
        [Parameter(Mandatory = $true)]$ExpectedAuthorizationPackage
    )

    $ExpectedImportsByKey = [System.Collections.Generic.Dictionary[string, object]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($Import in @($ExpectedAuthorizationPackage.RequiredImports)) {
        $ExpectedImportsByKey["$($Import.Module)`n$($Import.Name)"] = $Import
    }

    $SeenKeys = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    $UsedImports = @($AuthorizationModel.used_imports)
    Assert-AvidScriptPreparedSemantic `
        -Condition ([int]$AuthorizationModel.used_import_count -eq $UsedImports.Count) `
        -Code "ASBI4402" `
        -Message "Prepared binding_authorization.used_import_count does not match used_imports."

    foreach ($Import in $UsedImports) {
        $StableId = [string]$Import.stable_id
        $Ordinal = [int]$Import.ordinal
        $Module = [string]$Import.module
        $Name = [string]$Import.name
        $Signature = [string]$Import.signature
        Assert-AvidScriptPreparedSemantic `
            -Condition ((Test-AvidScriptBindingSha256 $StableId) -and
                $Ordinal -ge 0 -and
                -not [string]::IsNullOrWhiteSpace($Module) -and
                -not [string]::IsNullOrWhiteSpace($Name) -and
                -not [string]::IsNullOrWhiteSpace($Signature)) `
            -Code "ASBI4402" `
            -Message "Prepared binding_authorization.used_imports contains invalid identity fields."
        $Key = "$Module`n$Name"
        Assert-AvidScriptPreparedSemantic `
            -Condition ($SeenKeys.Add($Key)) `
            -Code "ASBI4402" `
            -Message "Prepared binding_authorization.used_imports contains duplicate import $Module.$Name."
        Assert-AvidScriptPreparedSemantic `
            -Condition ($ExpectedImportsByKey.ContainsKey($Key)) `
            -Code "ASBI4402" `
            -Message "Prepared binding_authorization.used_imports contains unauthorized import $Module.$Name."
        $ExpectedImport = $ExpectedImportsByKey[$Key]
        Assert-AvidScriptPreparedSemantic `
            -Condition ([string]$ExpectedImport.StableId -ceq $StableId -and
                [int]$ExpectedImport.Ordinal -eq $Ordinal -and
                [string]$ExpectedImport.Signature -ceq $Signature) `
            -Code "ASBI4402" `
            -Message "Prepared binding_authorization.used_imports identity differs from the selected authorization package for $Module.$Name."
    }
}

function Import-AvidScriptCSharpPreparedSemantic {
    param(
        [Parameter(Mandatory = $true)][string]$PreparedReportPath,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$ExpectedSourcePath,
        [Parameter(Mandatory = $true)]$ExpectedAuthorizationPackage,
        [Parameter(Mandatory = $true)][string]$FrontendDestinationPath,
        [Parameter(Mandatory = $true)][string]$SemanticDestinationPath
    )

    $ProjectRootFullPath = Get-AvidScriptBindingFullPath $ProjectRoot
    $PreparedReportFullPath = Get-AvidScriptBindingFullPath $PreparedReportPath
    $ExpectedSourceFullPath = Get-AvidScriptBindingFullPath $ExpectedSourcePath
    $FrontendDestinationFullPath = Get-AvidScriptBindingFullPath $FrontendDestinationPath
    $SemanticDestinationFullPath = Get-AvidScriptBindingFullPath $SemanticDestinationPath

    Assert-AvidScriptPreparedSemantic `
        -Condition (Test-Path -LiteralPath $PreparedReportFullPath -PathType Leaf) `
        -Code "ASBI4403" `
        -Message "Prepared semantic report is missing: $PreparedReportFullPath"
    Assert-AvidScriptPreparedSemantic `
        -Condition (Test-Path -LiteralPath $ExpectedSourceFullPath -PathType Leaf) `
        -Code "ASBI4401" `
        -Message "Expected source file is missing: $ExpectedSourceFullPath"

    $PreparedReport = Import-AvidScriptPreparedSemanticJson `
        -Path $PreparedReportFullPath `
        -Code "ASBI4403" `
        -Label "Prepared semantic report"
    Assert-AvidScriptPreparedSemantic `
        -Condition ([int]$PreparedReport.schema_version -eq 1 -and
            [string]$PreparedReport.result -ceq "direct_abi_built" -and
            [bool]$PreparedReport.succeeded) `
        -Code "ASBI4403" `
        -Message "Prepared semantic report must be schema_version=1, result=direct_abi_built, succeeded=true."

    $ReportSourcePath = Resolve-AvidScriptPreparedSemanticProjectPath `
        -ProjectRoot $ProjectRootFullPath `
        -Path ([string]$PreparedReport.source.file) `
        -Code "ASBI4401" `
        -FieldName "Prepared semantic report source.file"
    Assert-AvidScriptPreparedSemantic `
        -Condition ($ReportSourcePath.Equals($ExpectedSourceFullPath, [System.StringComparison]::OrdinalIgnoreCase)) `
        -Code "ASBI4401" `
        -Message "Prepared semantic report source.file does not match the current source path."
    $ExpectedSourceSha256 = Get-AvidScriptBindingSha256Hex $ExpectedSourceFullPath
    Assert-AvidScriptPreparedSemantic `
        -Condition ((Test-AvidScriptBindingSha256 ([string]$PreparedReport.source.sha256)) -and
            [string]$PreparedReport.source.sha256 -ceq $ExpectedSourceSha256) `
        -Code "ASBI4401" `
        -Message "Prepared semantic report source SHA-256 does not match the current source file."

    $AuthorizationModel = $PreparedReport.binding_authorization
    Assert-AvidScriptPreparedSemantic `
        -Condition ($null -ne $AuthorizationModel) `
        -Code "ASBI4402" `
        -Message "Prepared semantic report is missing binding_authorization."
    Assert-AvidScriptPreparedSemantic `
        -Condition ([bool]$AuthorizationModel.required) `
        -Code "ASBI4402" `
        -Message "Prepared binding_authorization must be required for custom C# builds."

    $AuthorizationManifestPath = Resolve-AvidScriptPreparedSemanticProjectPath `
        -ProjectRoot $ProjectRootFullPath `
        -Path ([string]$AuthorizationModel.manifest_file) `
        -Code "ASBI4402" `
        -FieldName "Prepared binding_authorization.manifest_file"
    $AuthorizationDescriptorPath = Resolve-AvidScriptPreparedSemanticProjectPath `
        -ProjectRoot $ProjectRootFullPath `
        -Path ([string]$AuthorizationModel.descriptor_file) `
        -Code "ASBI4402" `
        -FieldName "Prepared binding_authorization.descriptor_file"
    $AuthorizationReferenceSourcePath = Resolve-AvidScriptPreparedSemanticProjectPath `
        -ProjectRoot $ProjectRootFullPath `
        -Path ([string]$AuthorizationModel.reference_source_file) `
        -Code "ASBI4402" `
        -FieldName "Prepared binding_authorization.reference_source_file"
    Assert-AvidScriptPreparedSemantic `
        -Condition ([string]$AuthorizationModel.package_name -ceq [string]$ExpectedAuthorizationPackage.PackageName -and
            [string]$AuthorizationModel.package_hash -ceq [string]$ExpectedAuthorizationPackage.PackageHash -and
            $AuthorizationManifestPath.Equals([string]$ExpectedAuthorizationPackage.ManifestPath, [System.StringComparison]::OrdinalIgnoreCase) -and
            [string]$AuthorizationModel.manifest_sha256 -ceq [string]$ExpectedAuthorizationPackage.ManifestSha256 -and
            $AuthorizationDescriptorPath.Equals([string]$ExpectedAuthorizationPackage.DescriptorPath, [System.StringComparison]::OrdinalIgnoreCase) -and
            [string]$AuthorizationModel.descriptor_sha256 -ceq [string]$ExpectedAuthorizationPackage.DescriptorSha256 -and
            $AuthorizationReferenceSourcePath.Equals([string]$ExpectedAuthorizationPackage.ReferenceSourcePath, [System.StringComparison]::OrdinalIgnoreCase) -and
            [string]$AuthorizationModel.reference_source_sha256 -ceq [string]$ExpectedAuthorizationPackage.ReferenceSourceSha256 -and
            [int]$AuthorizationModel.profile_import_count -eq @($ExpectedAuthorizationPackage.RequiredImports).Count) `
        -Code "ASBI4402" `
        -Message "Prepared binding_authorization identity does not match the current authorization package."
    Assert-AvidScriptPreparedSemanticUsedImports `
        -AuthorizationModel $AuthorizationModel `
        -ExpectedAuthorizationPackage $ExpectedAuthorizationPackage

    $PreparedOutputRoot = Resolve-AvidScriptPreparedSemanticProjectPath `
        -ProjectRoot $ProjectRootFullPath `
        -Path ([string]$PreparedReport.output_root) `
        -Code "ASBI4404" `
        -FieldName "Prepared semantic report output_root"
    $FrontendSourcePath = Resolve-AvidScriptPreparedSemanticProjectPath `
        -ProjectRoot $ProjectRootFullPath `
        -Path ([string]$PreparedReport.artifacts.frontend_file) `
        -Code "ASBI4404" `
        -FieldName "Prepared semantic report artifacts.frontend_file"
    $SemanticSourcePath = Resolve-AvidScriptPreparedSemanticProjectPath `
        -ProjectRoot $ProjectRootFullPath `
        -Path ([string]$PreparedReport.artifacts.semantic_file) `
        -Code "ASBI4404" `
        -FieldName "Prepared semantic report artifacts.semantic_file"
    Assert-AvidScriptPreparedSemantic `
        -Condition (Test-AvidScriptBindingPathContained -RootPath $PreparedOutputRoot -CandidatePath $FrontendSourcePath) `
        -Code "ASBI4404" `
        -Message "Prepared frontend artifact escapes the prepared output_root."
    Assert-AvidScriptPreparedSemantic `
        -Condition (Test-AvidScriptBindingPathContained -RootPath $PreparedOutputRoot -CandidatePath $SemanticSourcePath) `
        -Code "ASBI4404" `
        -Message "Prepared semantic artifact escapes the prepared output_root."

    Assert-AvidScriptPreparedSemantic `
        -Condition (Test-Path -LiteralPath $FrontendSourcePath -PathType Leaf) `
        -Code "ASBI4403" `
        -Message "Prepared frontend artifact is missing: $FrontendSourcePath"
    Assert-AvidScriptPreparedSemantic `
        -Condition (Test-Path -LiteralPath $SemanticSourcePath -PathType Leaf) `
        -Code "ASBI4403" `
        -Message "Prepared semantic artifact is missing: $SemanticSourcePath"
    $ActualFrontendSha256 = Get-AvidScriptBindingSha256Hex $FrontendSourcePath
    $ActualSemanticSha256 = Get-AvidScriptBindingSha256Hex $SemanticSourcePath
    Assert-AvidScriptPreparedSemantic `
        -Condition ((Test-AvidScriptBindingSha256 ([string]$PreparedReport.frontend.artifact_sha256)) -and
            [string]$PreparedReport.frontend.artifact_sha256 -ceq $ActualFrontendSha256) `
        -Code "ASBI4403" `
        -Message "Prepared frontend artifact SHA-256 does not match the report."
    Assert-AvidScriptPreparedSemantic `
        -Condition ((Test-AvidScriptBindingSha256 ([string]$PreparedReport.semantic.artifact_sha256)) -and
            [string]$PreparedReport.semantic.artifact_sha256 -ceq $ActualSemanticSha256) `
        -Code "ASBI4403" `
        -Message "Prepared semantic artifact SHA-256 does not match the report."

    $FrontendModel = Import-AvidScriptPreparedSemanticJson `
        -Path $FrontendSourcePath `
        -Code "ASBI4403" `
        -Label "Prepared frontend artifact"
    $SemanticModel = Import-AvidScriptPreparedSemanticJson `
        -Path $SemanticSourcePath `
        -Code "ASBI4403" `
        -Label "Prepared semantic artifact"

    Assert-AvidScriptPreparedSemantic `
        -Condition ([int]$PreparedReport.frontend.schema_version -eq 1 -and
            [string]$PreparedReport.frontend.version -ceq "1.0" -and
            [int]$FrontendModel.schema_version -eq 1 -and
            [string]$FrontendModel.frontend_version -ceq "1.0" -and
            [bool]$FrontendModel.succeeded -and
            [string]$FrontendModel.source.sha256 -ceq $ExpectedSourceSha256) `
        -Code "ASBI4403" `
        -Message "Prepared frontend artifact contract is invalid."
    # The Semantic schema names the Frontend source hash frontend_sha256; it is not the artifact file hash.
    Assert-AvidScriptPreparedSemantic `
        -Condition ([int]$PreparedReport.semantic.schema_version -eq 5 -and
            [string]$PreparedReport.semantic.version -ceq "1.5" -and
            [bool]$PreparedReport.semantic.succeeded -and
            [string]$PreparedReport.semantic.source_sha256 -ceq $ExpectedSourceSha256 -and
            [string]$PreparedReport.semantic.frontend_sha256 -ceq $ExpectedSourceSha256 -and
            [int]$SemanticModel.schema_version -eq 5 -and
            [string]$SemanticModel.semantic_version -ceq "1.5" -and
            [bool]$SemanticModel.succeeded -and
            [string]$SemanticModel.source.sha256 -ceq $ExpectedSourceSha256 -and
            [string]$SemanticModel.source.frontend_sha256 -ceq $ExpectedSourceSha256) `
        -Code "ASBI4403" `
        -Message "Prepared semantic artifact contract is invalid."

    Publish-AvidScriptBindingFilePairAtomic `
        -FirstSourcePath $FrontendSourcePath `
        -FirstDestinationPath $FrontendDestinationFullPath `
        -SecondSourcePath $SemanticSourcePath `
        -SecondDestinationPath $SemanticDestinationFullPath

    return [pscustomobject]@{
        FrontendModel = $FrontendModel
        SemanticModel = $SemanticModel
        PreparedReportPath = $PreparedReportFullPath
        PreparedReportSha256 = Get-AvidScriptBindingSha256Hex $PreparedReportFullPath
    }
}
