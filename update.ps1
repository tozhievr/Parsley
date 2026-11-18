#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Source = '',
    [string]$Archive = '',
    [string]$Prefix = '',
    [string]$Version = '',
    [string]$Vsix = '',
    [ValidateSet('Auto','Both','VSCode','Cursor','None')][string]$Editors = 'Auto',
    [string]$Repository = 'tozhievr/Parsley',
    [switch]$FromRelease,
    [switch]$NoPath,
    [switch]$NoEditorSettings,
    [switch]$SkipChecksum
)
$ErrorActionPreference='Stop'
$installer=Join-Path $PSScriptRoot 'install.ps1'
if(-not (Test-Path -LiteralPath $installer)){throw 'install.ps1 must be next to update.ps1'}
& $installer @PSBoundParameters