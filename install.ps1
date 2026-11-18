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
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Get-ParsleyHash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-ParsleyChild([string]$Path, [string]$Root) {
    $full = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetFullPath($Root).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes installation root: $full" }
    return $full
}
function Assert-ParsleyDirectory([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path -Force
        if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Expected a regular directory: $Path" }
    }
}
function Get-ParsleyPath([string]$Value, [string]$Root) {
    $expanded = [Environment]::ExpandEnvironmentVariables($Value).Trim().Trim('"')
    try { return [IO.Path]::GetFullPath($expanded).TrimEnd('\','/') } catch { return $expanded }
}
function Get-ParsleyUpdatedPath([string]$Value, [string]$Root) {
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\','/')
    $stable = Join-Path $rootPath 'current\bin'
    $items = New-Object 'System.Collections.Generic.List[string]'
    $items.Add($stable)
    foreach ($entry in ($Value -split ';')) {
        if (-not $entry.Trim()) { continue }
        $full = Get-ParsleyPath $entry $rootPath
        $owned = $full.Equals($stable, [StringComparison]::OrdinalIgnoreCase)
        if ($full.StartsWith($rootPath + '\', [StringComparison]::OrdinalIgnoreCase)) {
            $tail = $full.Substring($rootPath.Length + 1)
            if ($tail -match '^(parsley-[^\\]+-win64|versions\\[^\\]+)\\bin$') { $owned = $true }
        }
        if (-not $owned) { $items.Add($entry) }
    }
    return ($items -join ';')
}
function Set-ParsleyPath([string]$Root) {
    $userPath = [Environment]::GetEnvironmentVariable('Path','User')
    $updated = Get-ParsleyUpdatedPath $userPath $Root
    if ($updated -ne $userPath) { [Environment]::SetEnvironmentVariable('Path',$updated,'User') }
    $env:Path = Get-ParsleyUpdatedPath $env:Path $Root
    try {
        if (-not ('ParsleyEnvironmentNotification' -as [type])) {
            Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class ParsleyEnvironmentNotification {
 [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
 public static extern IntPtr SendMessageTimeout(IntPtr window,uint message,IntPtr wParam,string lParam,uint flags,uint timeout,out IntPtr result);
}
"@
        }
        $answer = [IntPtr]::Zero
        [void][ParsleyEnvironmentNotification]::SendMessageTimeout([IntPtr]0xffff,0x1a,[IntPtr]::Zero,'Environment',2,3000,[ref]$answer)
    } catch { Write-Verbose ('Environment notification: ' + $_.Exception.Message) }
}
function Get-ParsleyJsonTokens([string]$Text) {
    $i=0
    while ($i -lt $Text.Length) {
        $c=$Text[$i]
        if ([char]::IsWhiteSpace($c) -or $c -eq [char]0xfeff) { $i++; continue }
        if ($c -eq '/' -and $i+1 -lt $Text.Length -and $Text[$i+1] -eq '/') {
            $i+=2; while($i -lt $Text.Length -and $Text[$i] -ne [char]10) { $i++ }; continue
        }
        if ($c -eq '/' -and $i+1 -lt $Text.Length -and $Text[$i+1] -eq '*') {
            $end=$Text.IndexOf('*/',$i+2,[StringComparison]::Ordinal)
            if($end -lt 0){throw 'Unterminated JSONC comment'};$i=$end+2;continue
        }
        $start=$i
        if ($c -eq '"') {
            $i++;$closed=$false
            while($i -lt $Text.Length){
                if($Text[$i] -eq '\'){$i+=2;continue}
                if($Text[$i] -eq '"'){$i++;$closed=$true;break};$i++
            }
            if(-not $closed){throw 'Unterminated JSONC string'}
            $raw=$Text.Substring($start,$i-$start)
            [pscustomobject]@{Kind='String';Start=$start;End=$i;Value=($raw | ConvertFrom-Json)}
        } elseif ('{}[]:,'.Contains([string]$c)) {
            $i++;[pscustomobject]@{Kind='Punctuation';Start=$start;End=$i;Value=[string]$c}
        } else {
            while($i -lt $Text.Length -and -not [char]::IsWhiteSpace($Text[$i]) -and -not '{}[]:,/'.Contains([string]$Text[$i])){$i++}
            if($i -eq $start){throw 'Invalid JSONC token'}
            [pscustomobject]@{Kind='Other';Start=$start;End=$i;Value=$Text.Substring($start,$i-$start)}
        }
    }
}
function Get-ParsleySettingsText([string]$Text,[string]$Executable,[string]$Root) {
    $tokens=@(Get-ParsleyJsonTokens $Text)
    if($tokens.Count -lt 2 -or $tokens[0].Value -ne '{' -or $tokens[-1].Value -ne '}'){throw 'Editor settings must be a JSONC object'}
    $depth=0;$valueToken=$null
    for($i=0;$i -lt $tokens.Count;$i++){
        $token=$tokens[$i]
        if($depth -eq 1 -and $token.Kind -eq 'String' -and $token.Value -eq 'parsley.executablePath' -and $i+2 -lt $tokens.Count -and $tokens[$i+1].Value -eq ':'){$valueToken=$tokens[$i+2]}
        if($token.Kind -eq 'Punctuation'){
            if($token.Value -in @('{','[')){$depth++}
            elseif($token.Value -in @('}',']')){$depth--;if($depth -lt 0){throw 'Invalid editor settings nesting'}}
        }
    }
    if($depth -ne 0){throw 'Unbalanced editor settings'}
    $encoded=ConvertTo-Json -InputObject $Executable -Compress
    if($null -ne $valueToken){
        if($valueToken.Kind -ne 'String'){return $Text}
        $existing=[string]$valueToken.Value
        $path=Get-ParsleyPath $existing $Root
        $owned=$path.StartsWith([IO.Path]::GetFullPath($Root).TrimEnd('\','/')+'\',[StringComparison]::OrdinalIgnoreCase)
        if($existing -notin @('parsley','parsley.exe') -and -not $owned){return $Text}
        return $Text.Substring(0,$valueToken.Start)+$encoded+$Text.Substring($valueToken.End)
    }
    $newline=[string][char]10;if($Text.Contains(([string][char]13)+[char]10)){$newline=([string][char]13)+[char]10}
    $comma=',';if($tokens[1].Value -eq '}'){$comma=''}
    $entry=$newline+'  "parsley.executablePath": '+$encoded+$comma+$newline
    return $Text.Insert($tokens[0].End,$entry)
}
function Set-ParsleyEditorSettings([string]$Path,[string]$Executable,[string]$Root) {
    $text='{}'
    if(Test-Path -LiteralPath $Path){$text=[IO.File]::ReadAllText($Path)}
    $new=Get-ParsleySettingsText $text $Executable $Root
    if($new -eq $text){return}
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    if((Test-Path -LiteralPath $Path) -and -not (Test-Path -LiteralPath ($Path+'.parsley-backup'))){Copy-Item -LiteralPath $Path -Destination ($Path+'.parsley-backup')}
    [IO.File]::WriteAllText($Path,$new,(New-Object Text.UTF8Encoding($false)))
}
function Get-ParsleyEditor([string]$Name) {
    $commandName='code.cmd';$settings='Code\User\settings.json'
    if($Name -eq 'Cursor'){$commandName='cursor.cmd';$settings='Cursor\User\settings.json'}
    $command=Get-Command $commandName -ErrorAction SilentlyContinue
    if(-not $command){return $null}
    $bin=Split-Path -Parent $command.Source
    if($Name -eq 'VSCode'){
        $app=[IO.Path]::GetFullPath((Join-Path $bin '..\Code.exe'))
        $cli=[IO.Path]::GetFullPath((Join-Path $bin '..\resources\app\out\cli.js'))
    }else{
        $app=[IO.Path]::GetFullPath((Join-Path $bin '..\..\..\Cursor.exe'))
        $cli=[IO.Path]::GetFullPath((Join-Path $bin '..\out\cli.js'))
    }
    if(-not (Test-Path -LiteralPath $cli)){
        # Some VS Code releases place resources under a build-specific directory.
        $launcher=[IO.File]::ReadAllText($command.Source)
        $entry=[regex]::Match($launcher,'(?im)"%~dp0([^"\r\n]+\\out\\cli\.js)"')
        if($entry.Success){
            $candidate=[IO.Path]::GetFullPath((Join-Path $bin $entry.Groups[1].Value))
            $cli=Assert-ParsleyChild $candidate (Split-Path -Parent $app)
        }
    }
    if(-not (Test-Path -LiteralPath $app) -or -not (Test-Path -LiteralPath $cli)){throw "Cannot locate the native $Name CLI from $($command.Source)"}
    return [pscustomobject]@{Name=$Name;Executable=$app;Cli=$cli;Settings=(Join-Path $env:APPDATA $settings)}
}
function Get-ParsleyVsixVersion([string]$Path) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip=[IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($Path))
    try{
        $entry=$zip.GetEntry('extension/package.json')
        if(-not $entry -or $entry.Length -gt 1MB){throw 'Invalid Parsley VSIX manifest'}
        $reader=New-Object IO.StreamReader($entry.Open())
        try{$manifest=$reader.ReadToEnd()|ConvertFrom-Json}finally{$reader.Dispose()}
        if($manifest.publisher -ne 'parsley-lang' -or $manifest.name -ne 'parsley-lang' -or $manifest.version -notmatch '^\d+\.\d+\.\d+$'){throw 'VSIX is not a versioned Parsley extension'}
        return [string]$manifest.version
    }finally{$zip.Dispose()}
}
function Invoke-ParsleyEditor($Editor,[string[]]$Arguments) {
    $previous=$env:ELECTRON_RUN_AS_NODE
    try{
        $env:ELECTRON_RUN_AS_NODE='1'
        & $Editor.Executable $Editor.Cli @Arguments | Out-Host
        if($LASTEXITCODE -ne 0){throw "$($Editor.Name) CLI failed with exit $LASTEXITCODE"}
    }finally{
        if($null -eq $previous){Remove-Item Env:ELECTRON_RUN_AS_NODE -ErrorAction SilentlyContinue}else{$env:ELECTRON_RUN_AS_NODE=$previous}
    }
}
function Test-ParsleyChecksum([string]$Path,[string]$Checksums) {
    if(-not (Test-Path -LiteralPath $Checksums)){throw "Missing SHA256SUMS.txt for $Path; use -SkipChecksum only for a locally trusted package"}
    $name=[IO.Path]::GetFileName($Path);$wanted=$null
    foreach($line in [IO.File]::ReadAllLines($Checksums)){
        if($line -match '^([a-fA-F0-9]{64})\s+\*?(.+)$' -and $Matches[2] -eq $name){$wanted=$Matches[1];break}
    }
    if(-not $wanted -or (Get-ParsleyHash $Path) -ne $wanted.ToLowerInvariant()){throw "SHA256 mismatch or missing checksum: $name"}
}
function Expand-ParsleyArchive([string]$Path,[string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [void][IO.Directory]::CreateDirectory($Destination)
    $zip=[IO.Compression.ZipFile]::OpenRead($Path)
    try{
        [long]$total=0
        if($zip.Entries.Count -gt 10000){throw 'Archive entry limit exceeded'}
        foreach($entry in $zip.Entries){
            $name=$entry.FullName.Replace('/',[IO.Path]::DirectorySeparatorChar)
            if([IO.Path]::IsPathRooted($name) -or $name.Contains(':')){throw "Unsafe archive entry: $name"}
            if((($entry.ExternalAttributes -shr 16) -band 0xf000) -eq 0xa000){throw 'Archive symbolic links are not allowed'}
            $target=Assert-ParsleyChild (Join-Path $Destination $name) $Destination
            $total+=$entry.Length;if($total -gt 512MB){throw 'Archive expansion limit exceeded'}
            if($name.EndsWith('\')){[void][IO.Directory]::CreateDirectory($target);continue}
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$target,$false)
        }
    }finally{$zip.Dispose()}
}
function Set-ParsleyCurrent([string]$Root,[string]$Target) {
    $versions=Join-Path $Root 'versions'
    [void](Assert-ParsleyChild $Target $versions)
    $current=Join-Path $Root 'current'
    $next=Assert-ParsleyChild (Join-Path $Root ('.current-'+[guid]::NewGuid().ToString('N'))) $Root
    $backup=Assert-ParsleyChild (Join-Path $Root ('.previous-'+[guid]::NewGuid().ToString('N'))) $Root
    if(Test-Path -LiteralPath $current){
        $item=Get-Item -LiteralPath $current -Force
        if(-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $item.LinkType -ne 'Junction'){throw "Refusing to replace an unmanaged current directory: $current"}
        $oldTarget=[string]@($item.Target)[0]
        [void](Assert-ParsleyChild $oldTarget $versions)
    }
    [void](New-Item -ItemType Junction -Path $next -Target $Target)
    $saved=$false
    try{
        if(Test-Path -LiteralPath $current){[IO.Directory]::Move($current,$backup);$saved=$true}
        try{[IO.Directory]::Move($next,$current)}catch{if($saved){[IO.Directory]::Move($backup,$current);$saved=$false};throw}
    }finally{
        # Delete junction objects only; never recurse through their targets.
        if(Test-Path -LiteralPath $next){[IO.Directory]::Delete($next)}
        if($saved -and (Test-Path -LiteralPath $backup)){[IO.Directory]::Delete($backup)}
    }
}
function Invoke-ParsleyInstall {
    if($env:OS -ne 'Windows_NT'){throw 'This installer targets Windows x64'}
    if([IntPtr]::Size -ne 8){throw 'Use a 64-bit PowerShell process'}
    if($Version -and $Version -notmatch '^\d+\.\d+\.\d+$'){throw 'Version must be a stable SemVer number, for example 2.4.0'}
    if($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$'){throw 'Repository must be owner/name'}
    $root=$Prefix;if(-not $root){$root=Join-Path $env:LOCALAPPDATA 'Programs\Parsley'}
    $root=[IO.Path]::GetFullPath($root)
    Assert-ParsleyDirectory $root
    [void][IO.Directory]::CreateDirectory($root)
    $versions=Join-Path $root 'versions';Assert-ParsleyDirectory $versions;[void][IO.Directory]::CreateDirectory($versions)
    $work=Assert-ParsleyChild (Join-Path $root ('.stage-'+[guid]::NewGuid().ToString('N'))) $root
    [void][IO.Directory]::CreateDirectory($work)
    try{
        $location=$Source
        if(-not $location){$location=$PSScriptRoot;if(-not $location){$location=(Get-Location).Path}}
        $location=[IO.Path]::GetFullPath($location)
        $package='';$release=$null;$archivePath=$Archive;$remote=$FromRelease.IsPresent
        if(-not $archivePath -and -not $remote){
            $candidateDirectories=@((Join-Path $location 'dist'),$location)
            foreach($directory in $candidateDirectories){
                if(-not (Test-Path -LiteralPath $directory -PathType Container)){continue}
                $candidates=@(Get-ChildItem -LiteralPath $directory -File -Filter 'parsley-*-win64.zip' | Where-Object {$_.Name -match '^parsley-(\d+\.\d+\.\d+)-win64\.zip$'} | Sort-Object @{Expression={[version]([regex]::Match($_.Name,'\d+\.\d+\.\d+').Value)}} -Descending)
                if($Version){$candidates=@($candidates|Where-Object {$_.Name -eq "parsley-$Version-win64.zip"})}
                if($candidates.Count){$archivePath=$candidates[0].FullName;break}
            }
            if(-not $archivePath -and (Test-Path -LiteralPath (Join-Path $location 'bin\parsley.exe'))){$package=$location}
            if(-not $archivePath -and -not $package){if($Source){throw "No matching Windows package in $Source"};$remote=$true}
        }
        if($remote){
            [Net.ServicePointManager]::SecurityProtocol=[Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            $endpoint="https://api.github.com/repos/$Repository/releases/latest"
            if($Version){$endpoint="https://api.github.com/repos/$Repository/releases/tags/v$Version"}
            try{$release=Invoke-RestMethod -Uri $endpoint -Headers @{'User-Agent'='Parsley-Installer'}}catch{throw "Cannot find the published Parsley release. Use -Source with local dist packages. $($_.Exception.Message)"}
            $asset=@($release.assets|Where-Object {$_.name -match '^parsley-\d+\.\d+\.\d+-win64\.zip$'}|Select-Object -First 1)
            if(-not $asset.Count){throw 'The release has no Windows x64 ZIP asset'}
            $archivePath=Join-Path $work $asset[0].name
            Invoke-WebRequest -UseBasicParsing -Uri $asset[0].browser_download_url -OutFile $archivePath
            if(-not $SkipChecksum){
                $sum=@($release.assets|Where-Object {$_.name -eq 'SHA256SUMS.txt'}|Select-Object -First 1)
                if(-not $sum.Count){throw 'The release has no SHA256SUMS.txt'}
                $sumPath=Join-Path $work 'SHA256SUMS.txt'
                Invoke-WebRequest -UseBasicParsing -Uri $sum[0].browser_download_url -OutFile $sumPath
                Test-ParsleyChecksum $archivePath $sumPath
            }
        }elseif($archivePath){
            $archivePath=[IO.Path]::GetFullPath($archivePath)
            if(-not $SkipChecksum){Test-ParsleyChecksum $archivePath (Join-Path (Split-Path -Parent $archivePath) 'SHA256SUMS.txt')}
        }
        if($archivePath){
            $unpacked=Join-Path $work 'unpacked';Expand-ParsleyArchive $archivePath $unpacked
            $executables=@(Get-ChildItem -LiteralPath $unpacked -Recurse -File -Filter parsley.exe|Where-Object {$_.Directory.Name -eq 'bin'})
            if($executables.Count -ne 1){throw 'The package must contain exactly one bin/parsley.exe'}
            $package=Split-Path -Parent $executables[0].DirectoryName
            $identity=Get-ParsleyHash $archivePath
        }else{
            $identity=Get-ParsleyHash (Join-Path $package 'bin\parsley.exe')
            Assert-ParsleyDirectory $package
            foreach($child in Get-ChildItem -LiteralPath $package -Recurse -Force){
                if($child.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Package directory contains a reparse point'}
            }
            $copy=Join-Path $work 'directory-package'
            [void][IO.Directory]::CreateDirectory($copy)
            foreach($item in Get-ChildItem -LiteralPath $package -Force){
                if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Package directory contains a reparse point'}
                Copy-Item -LiteralPath $item.FullName -Destination $copy -Recurse
            }
            $package=$copy
        }
        $exe=Join-Path $package 'bin\parsley.exe'
        $versionFile=Join-Path $package 'share\doc\parsley\VERSION'
        if(-not (Test-Path -LiteralPath $versionFile)){throw 'Package VERSION is missing'}
        $installedVersion=[IO.File]::ReadAllText($versionFile).Trim()
        if($installedVersion -notmatch '^\d+\.\d+\.\d+$' -or ($Version -and $Version -ne $installedVersion)){throw 'Package version does not match the requested version'}
        $reported=(& $exe --version | Out-String).Trim()
        if($LASTEXITCODE -ne 0 -or $reported -ne "parsley $installedVersion"){throw 'Package executable and VERSION disagree'}
        $editorList=@()
        foreach($name in @('VSCode','Cursor')){
            if($Editors -eq 'None' -or ($Editors -notin @('Auto','Both',$name))){continue}
            $editor=Get-ParsleyEditor $name
            if($editor){$editorList+=$editor}elseif($Editors -ne 'Auto'){throw "$name is not installed or its CLI is not on PATH"}
        }
        $vsixPath=$Vsix
        if(-not $vsixPath){$bundled=Join-Path $package 'share\parsley\editors\parsley-lang.vsix';if(Test-Path -LiteralPath $bundled){$vsixPath=$bundled}}
        if(-not $vsixPath -and $editorList.Count){
            $extensionManifest=Join-Path $package 'share\parsley\editors\vscode\package.json'
            if(-not (Test-Path -LiteralPath $extensionManifest)){throw 'Bundled extension manifest is missing'}
            $extensionVersion=([IO.File]::ReadAllText($extensionManifest)|ConvertFrom-Json).version
            $name="parsley-lang-$extensionVersion.vsix"
            $archiveDirectory='';if($archivePath){$archiveDirectory=Split-Path -Parent $archivePath}
            foreach($directory in @($archiveDirectory,(Join-Path $location 'dist'),$location)){
                if($directory -and (Test-Path -LiteralPath (Join-Path $directory $name))){$vsixPath=Join-Path $directory $name;break}
            }
            if(-not $vsixPath -and $release){
                $asset=@($release.assets|Where-Object {$_.name -eq $name}|Select-Object -First 1)
                if($asset.Count){$vsixPath=Join-Path $work $name;Invoke-WebRequest -UseBasicParsing -Uri $asset[0].browser_download_url -OutFile $vsixPath;if(-not $SkipChecksum){Test-ParsleyChecksum $vsixPath (Join-Path $work 'SHA256SUMS.txt')}}
            }
        }
        if($editorList.Count -and (-not $vsixPath -or -not (Test-Path -LiteralPath $vsixPath))){throw 'No VSIX found for the installed editors; supply -Vsix or choose -Editors None'}
        $vsixVersion='';$vsixRelative=''
        if($editorList.Count){
            $vsixVersion=Get-ParsleyVsixVersion $vsixPath
            $packagePrefix=[IO.Path]::GetFullPath($package).TrimEnd('\','/')+'\'
            $absoluteVsix=[IO.Path]::GetFullPath($vsixPath)
            if($absoluteVsix.StartsWith($packagePrefix,[StringComparison]::OrdinalIgnoreCase)){$vsixRelative=$absoluteVsix.Substring($packagePrefix.Length)}
        }
        $destination=Assert-ParsleyChild (Join-Path $versions ($installedVersion+'-'+$identity.Substring(0,12))) $versions
        if(Test-Path -LiteralPath $destination){
            Assert-ParsleyDirectory $destination
            if((Get-ParsleyHash (Join-Path $destination 'bin\parsley.exe')) -ne (Get-ParsleyHash $exe)){$destination+='-repair-'+[guid]::NewGuid().ToString('N')}
        }
        if(-not (Test-Path -LiteralPath $destination)){
            [void](Assert-ParsleyChild $package $work)
            [void](Assert-ParsleyChild $destination $versions)
            Move-Item -LiteralPath $package -Destination $destination
        }
        if($vsixRelative){$vsixPath=Join-Path $destination $vsixRelative}
        Set-ParsleyCurrent $root $destination
        $stableExe=Join-Path $root 'current\bin\parsley.exe'
        foreach($scriptName in @('install.ps1','update.ps1','install.cmd','update.cmd')){
            $scriptSource=Join-Path $destination $scriptName
            if(-not (Test-Path -LiteralPath $scriptSource) -and $PSScriptRoot){$scriptSource=Join-Path $PSScriptRoot $scriptName}
            if((Test-Path -LiteralPath $scriptSource) -and [IO.Path]::GetFullPath($scriptSource) -ne (Join-Path $root $scriptName)){Copy-Item -LiteralPath $scriptSource -Destination (Join-Path $root $scriptName) -Force}
        }
        foreach($editor in $editorList){
            Invoke-ParsleyEditor -Editor $editor -Arguments @('--install-extension',[IO.Path]::GetFullPath($vsixPath),'--force')
            if(-not $NoEditorSettings){Set-ParsleyEditorSettings $editor.Settings $stableExe $root}
        }
        if(-not $NoPath){Set-ParsleyPath $root}
        $record=[ordered]@{version=$installedVersion;current=$destination;executable=$stableExe;repository=$Repository;extension_version=$vsixVersion;installed_at=[DateTime]::UtcNow.ToString('o');editors=@($editorList|ForEach-Object {$_.Name})}
        [IO.File]::WriteAllText((Join-Path $root 'install.json'),($record|ConvertTo-Json -Depth 5),(New-Object Text.UTF8Encoding($false)))
        Write-Host "Installed Parsley $installedVersion -> $stableExe"
        if($editorList.Count){Write-Host 'Extensions updated. Reload already-open VS Code/Cursor windows to activate the new extension.'}
    }finally{
        if(Test-Path -LiteralPath $work){
            [void](Assert-ParsleyChild $work $root);Assert-ParsleyDirectory $work
            Remove-Item -LiteralPath $work -Recurse -Force
        }
    }
}
Invoke-ParsleyInstall