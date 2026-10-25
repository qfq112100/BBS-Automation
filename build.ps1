# Portable MinGW-w64/LLVM-MinGW build. No system PATH or machine environment changes.
[CmdletBinding()]
param(
    [string]$ToolchainBin = '',
    [string]$Compiler = '',
    [string]$Windres = '',
    [string]$Objdump = '',
    [string]$BuildDir = '',
    [ValidateSet('x86','x64')][string]$Architecture = 'x86',
    [ValidateSet('LOLBackup','LOLBackupV3')][string]$ExecutableName = 'LOLBackup',
    [ValidateSet('App','Tests','All')][string]$Mode = 'All',
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [switch]$AuditOnly,
    [string]$Executable = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $BuildDir) { $BuildDir = Join-Path $PSScriptRoot ('build/' + $Architecture) }
$Triple = if ($Architecture -eq 'x86') { 'i686-w64-mingw32' } else { 'x86_64-w64-mingw32' }
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) { $PSNativeCommandUseErrorActionPreference = $false }
function Resolve-Tool([string]$Explicit, [string[]]$Names) {
    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit -PathType Leaf)) { throw "Tool does not exist: $Explicit" }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }
    if ($ToolchainBin) {
        foreach ($name in $Names) {
            $candidate = Join-Path $ToolchainBin $name
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Resolve-Path -LiteralPath $candidate).Path }
        }
    }
    foreach ($name in $Names) {
        $cmd = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cmd) { return $cmd.Source }
    }
    throw "Cannot locate $($Names -join ', '). Pass -ToolchainBin or explicit tool paths; no installation is performed."
}
function Invoke-Native([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Native command failed with exit code $LASTEXITCODE : $Program" }
}
$AllowedDlls = @('kernel32.dll','user32.dll','gdi32.dll','gdiplus.dll','comctl32.dll',
    'shell32.dll','ole32.dll','oleaut32.dll','advapi32.dll','shlwapi.dll','version.dll',
    'dbghelp.dll','dwmapi.dll','cabinet.dll','ntdll.dll','ucrtbase.dll','msvcrt.dll','bcrypt.dll','rpcrt4.dll')
function Test-SystemDll([string]$Name) {
    # Only the original Windows 10 UCRT contract, not arbitrary newer API-set versions.
    return ($AllowedDlls -contains $Name.ToLowerInvariant()) -or ($Name -match '^api-ms-win-crt-(conio|convert|environment|filesystem|heap|locale|math|multibyte|private|process|runtime|stdio|string|time|utility)-l1-1-0[.]dll$')
}
function Assert-Imports([string]$Exe, [int]$ExpectedSubsystem = 0) {
    $bytes = [IO.File]::ReadAllBytes($Exe)
    if ($bytes.Length -lt 256 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D) { throw "Not a DOS/PE executable: $Exe" }
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($pe -lt 64 -or $pe + 256 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550) { throw "Invalid PE header: $Exe" }
    $machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
    $optional = $pe + 24
    $magic = [BitConverter]::ToUInt16($bytes, $optional)
    $expectedMachine = if ($Architecture -eq 'x86') { 0x14C } else { 0x8664 }
    $expectedMagic = if ($Architecture -eq 'x86') { 0x10B } else { 0x20B }
    if ($machine -ne $expectedMachine -or $magic -ne $expectedMagic) { throw "Wrong PE architecture, expected $Architecture : $Exe" }
    $subsystem = [BitConverter]::ToUInt16($bytes, $optional + 68)
    if ($ExpectedSubsystem -and $subsystem -ne $ExpectedSubsystem) { throw "Incorrect GUI/console subsystem in $Exe" }
    $directories = $optional + $(if ($Architecture -eq 'x86') { 96 } else { 112 })
    if ([BitConverter]::ToUInt32($bytes, $directories + 14 * 8) -ne 0) { throw "Managed CLR executable rejected: $Exe" }
    if ([BitConverter]::ToUInt32($bytes, $directories + 13 * 8) -ne 0) { throw "Unreviewed delay-load import table rejected: $Exe" }
    if ($Configuration -eq 'Release' -and ([BitConverter]::ToUInt32($bytes, $directories + 6 * 8) -ne 0 -or [BitConverter]::ToUInt32($bytes, $pe + 16) -ne 0)) { throw "Release binary has debug directory / COFF symbols: $Exe" }
    $dllCharacteristics = [BitConverter]::ToUInt16($bytes, $optional + 70)
    if (($dllCharacteristics -band 0x140) -ne 0x140) { throw "ASLR/DEP missing: $Exe" }
    if ($Architecture -eq 'x86' -and ([BitConverter]::ToUInt16($bytes, $pe + 22) -band 0x20) -eq 0) { throw "x86 large-address awareness missing: $Exe" }
    $text = @(& $script:Objdump -p $Exe 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "objdump failed inspecting $Exe : $($text -join [Environment]::NewLine)" }
    $dlls = @($text | ForEach-Object { if ("$_" -match 'DLL Name:\s*(\S+)') { $Matches[1].ToLowerInvariant() } } | Sort-Object -Unique)
    if ($dlls.Count -eq 0) { throw "No PE imports found in $Exe" }
    foreach ($dll in $dlls) { if (-not (Test-SystemDll $dll)) { throw "Non-system import rejected in $Exe : $dll" } }
    # Presence of a system DLL alone does not prove all its functions existed on 1507.
    $post1507 = 'GetDpiForWindow|GetDpiForSystem|GetSystemDpiForProcess|GetSystemMetricsForDpi|AdjustWindowRectExForDpi|SetProcessDpiAwarenessContext|SetThreadDpiAwarenessContext|GetThreadDpiAwarenessContext|GetAwarenessFromDpiAwarenessContext|AreDpiAwarenessContextsEqual|IsValidDpiAwarenessContext|EnableNonClientDpiScaling|GetDpiAwarenessContextForProcess|SetThreadDescription|GetThreadDescription|IsWow64Process2|GetTempPath2W|GetTempPath2A'
    if (($text -join "`n") -match "(?m)^.*\s(?:$post1507)\s*$") { throw "Static post-1507 API import rejected in $Exe : $($Matches[0])" }
    $text | Set-Content -LiteralPath ($Exe + '.pe.txt') -Encoding UTF8
    # Approved packed third-party resources are opaque payloads, not BBS browser dependencies.
    # Keep native import/source checks above strict; exclude only the PE .rsrc section here.
    $sectionCount = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $optionalSize = [BitConverter]::ToUInt16($bytes, $pe + 20)
    $minimumOptional = if ($magic -eq 0x10B) { 224 } else { 240 }
    [long]$sectionTable = [long]$optional + $optionalSize
    [long]$sectionEnd = $sectionTable + [long]$sectionCount * 40
    if ($optionalSize -lt $minimumOptional -or $sectionCount -lt 1 -or $sectionCount -gt 96 -or $sectionEnd -gt $bytes.LongLength) { throw "Invalid PE section table: $Exe" }
    for ($sectionIndex = 0; $sectionIndex -lt $sectionCount; $sectionIndex++) {
        [int]$section = $sectionTable + $sectionIndex * 40
        $sectionName = [Text.Encoding]::ASCII.GetString($bytes, $section, 8).TrimEnd([char]0)
        [long]$rawSize = [BitConverter]::ToUInt32($bytes, $section + 16)
        [long]$rawOffset = [BitConverter]::ToUInt32($bytes, $section + 20)
        if ($rawSize -eq 0) { continue }
        if ($rawOffset -lt $sectionEnd -or $rawOffset + $rawSize -gt $bytes.LongLength) { throw "Invalid PE section bounds ($sectionName): $Exe" }
        if ($sectionName -ceq '.rsrc') { continue }
        $binaryText = [Text.Encoding]::ASCII.GetString($bytes, [int]$rawOffset, [int]$rawSize)
        if ($binaryText -match '(?i)WebView2|msedgewebview2|MicrosoftEdgeWebView2RuntimeInstaller') { throw "Browser runtime strings found in native PE section ($sectionName): $Exe" }
    }
    $dlls | Set-Content -LiteralPath ($Exe + '.imports.txt') -Encoding UTF8
    Write-Host ("[imports OK] {0}: {1}" -f (Split-Path -Leaf $Exe), ($dlls -join ', '))
}
$Core = @('src/core/service.cpp','src/core/sync_engine.cpp','src/core/file_ops.cpp','src/core/path_policy.cpp','src/core/root_store.cpp',
    'src/core/logger.cpp','src/core/config.cpp','src/core/monitor.cpp','src/core/game_detect.cpp','src/core/tool_manager.cpp')
function Assert-NativeSources {
    $audit = New-Object System.Collections.Generic.List[string]
    $inputs = @('src/app/main.cpp','resources/app.rc') + $Core
    foreach ($directory in @('src/app','src/common','src/core')) {
        foreach ($header in (Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot $directory) -File -Filter '*.h')) {
            $inputs += $header.FullName.Substring($PSScriptRoot.Length + 1)
        }
    }
    foreach ($relative in $inputs) {
        $path = Join-Path $PSScriptRoot $relative
        $text = [IO.File]::ReadAllText($path)
        $code = [regex]::Replace($text, '(?s)/\*.*?\*/|(?m)//[^\r\n]*', '')
        if ($code -match '(?i)WebView2|WebView2Loader|msedgewebview2|RuntimeInstaller[.]exe|ui_index[.]html') {
            throw "Forbidden browser/runtime reference in active source: $relative"
        }
        # Any dynamic library call must visibly constrain the loader to System32.
        # All current native code uses static imports or GetModuleHandle on already-loaded system DLLs.
        $code = [regex]::Replace($text, '(?s)/\*.*?\*/|(?m)//[^\r\n]*', '')
        foreach ($m in [regex]::Matches($code, '\b(?:LoadLibrary(?:Ex)?[AW]?|LoadPackagedLibrary|LdrLoadDll)\s*\([^;]*')) {
            $call = $m.Value
            if ($call -notmatch '^LoadLibraryExW\s*\(\s*L"([a-zA-Z0-9_.-]+)"\s*,\s*(?:nullptr|NULL|0)\s*,\s*LOAD_LIBRARY_SEARCH_SYSTEM32\s*\)' -or -not (Test-SystemDll $Matches[1])) {
                throw "Unreviewed dynamic DLL loading path in $relative : $call"
            }
            $audit.Add("$relative : $call")
        }
    }
    if ($audit.Count -eq 0) { $audit.Add('No active LoadLibrary/LoadPackagedLibrary/LdrLoadDll calls. System DLL imports are linked directly.') }
    $audit | Set-Content -LiteralPath (Join-Path $BuildDir 'dynamic-load-audit.txt') -Encoding UTF8
}
try {
    if (-not $AuditOnly) {
        $Compiler = Resolve-Tool $Compiler @("$Triple-clang++.exe","$Triple-g++.exe","$Triple-clang++","$Triple-g++",'g++.exe','clang++.exe','g++','clang++')
        if (-not $ToolchainBin) { $ToolchainBin = Split-Path -Parent $Compiler }
        $target = & $Compiler -dumpmachine
        $targetPattern = if ($Architecture -eq 'x86') { '^i[3-6]86-.*(mingw32|windows-gnu)$' } else { '^x86_64-.*(mingw32|windows-gnu)$' }
        if ($LASTEXITCODE -ne 0 -or $target -notmatch $targetPattern) { throw "Expected a $Architecture MinGW-w64 target, got: $target" }
    }
    $Objdump = Resolve-Tool $Objdump @('llvm-objdump.exe',"$Triple-objdump.exe",'objdump.exe','llvm-objdump',"$Triple-objdump",'objdump')
    if ($AuditOnly) {
        if (-not $Executable) { throw '-AuditOnly requires -Executable' }
        Assert-Imports ([IO.Path]::GetFullPath($Executable))
        exit 0
    }
    $BuildDir = [IO.Path]::GetFullPath($BuildDir)
    if ($BuildDir -eq [IO.Path]::GetFullPath($PSScriptRoot)) { throw 'BuildDir must not be the source root' }
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $BuildDir 'obj') -Force | Out-Null
    # Exclusive lock prevents independently launched builds from racing in one output directory.
    $lock = [IO.File]::Open((Join-Path $BuildDir '.build.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    try {
        $infoPath = Join-Path $BuildDir 'build-info.json'
        if (Test-Path -LiteralPath $infoPath) {
            $previous = Get-Content -LiteralPath $infoPath -Raw | ConvertFrom-Json
            if ($previous.PSObject.Properties['architecture'] -and $previous.architecture -ne $Architecture) {
                throw 'BuildDir belongs to another architecture; choose a separate directory (nothing was deleted).'
            }
        }
        Assert-NativeSources
        if (Get-ChildItem -LiteralPath $BuildDir -File -Filter '*.dll') { throw "BuildDir contains DLL files from another build; select a clean independent directory (nothing was deleted)." }
        $Flags = @('-std=c++17','-Wall','-Wextra','-finput-charset=UTF-8','-fexec-charset=UTF-8',
            '-DUNICODE','-D_UNICODE','-DNOMINMAX','-DWIN32_LEAN_AND_MEAN','-D_WIN32_WINNT=0x0A00','-DWINVER=0x0A00','-DNTDDI_VERSION=0x0A000000')
        $versionText = (& $Compiler --version) -join ' '
        if ($LASTEXITCODE -ne 0) { throw 'Cannot determine compiler version' }
        if ($Configuration -eq 'Release') {
            $Flags += $(if ($versionText -match 'clang') { '-O2' } else { '-O2' })
            $Flags += @('-DNDEBUG','-g0','-ffunction-sections','-fdata-sections','-flto')
        } else { $Flags += @('-O0','-g') }
        foreach ($inc in @('src/core','src/common','third_party/json','resources')) { $Flags += @('-I',(Join-Path $PSScriptRoot $inc)) }
        $Link = @('-static','-Wl,--dynamicbase','-Wl,--nxcompat',
            '-Wl,--major-os-version,6,--minor-os-version,1,--major-subsystem-version,6,--minor-subsystem-version,1',
            '-ladvapi32','-lshell32','-lole32','-luuid','-lshlwapi','-lversion','-luser32','-lcabinet')
        if ($Architecture -eq 'x64') { $Link += '-Wl,--high-entropy-va' } else { $Link += '-Wl,--large-address-aware' }
        if ($versionText -notmatch 'clang') { $Link += @('-static-libgcc','-static-libstdc++') }
        if ($Configuration -eq 'Release') { $Link += @('-s','-Wl,--gc-sections','-flto') }
        $objects = @()
        foreach ($source in $Core) {
            $object = Join-Path $BuildDir ('obj/' + [IO.Path]::GetFileNameWithoutExtension($source) + '.o')
            Write-Host "[C++] $source"
            Invoke-Native $Compiler ($Flags + @('-c',(Join-Path $PSScriptRoot $source),'-o',$object))
            $objects += $object
        }
        if ($Mode -in @('App','All')) {
            $Windres = Resolve-Tool $Windres @("$Triple-windres.exe","$Triple-windres",'windres.exe','windres')
            $resourceTarget = if ($Architecture -eq 'x86') { 'pe-i386' } else { 'pe-x86-64' }
            $resource = Join-Path $BuildDir 'obj/app.rc.o'
            Push-Location -LiteralPath (Join-Path $PSScriptRoot 'resources')
            try { Invoke-Native $Windres @('--target',$resourceTarget,'-I',(Join-Path $PSScriptRoot 'resources'),'-i','app.rc','-O','coff','-o',$resource) } finally { Pop-Location }
            $appObject = Join-Path $BuildDir 'obj/main.o'
            $AppFlags = $Flags
            if ($Configuration -eq 'Release') { $AppFlags += '-DNDEBUG' }
            Invoke-Native $Compiler ($AppFlags + @('-c',(Join-Path $PSScriptRoot 'src/app/main.cpp'),'-o',$appObject))
            $exe = Join-Path $BuildDir ($ExecutableName + '.exe')
            Invoke-Native $Compiler (@($appObject,$resource) + $objects + @('-o',$exe,'-municode','-mwindows') + $Link + @('-lgdi32','-lgdiplus','-lcomctl32','-ldwmapi','-ldbghelp','-lcabinet'))
            Assert-Imports $exe 2
        }
        if ($Mode -in @('Tests','All')) {
            $Tests = @('test_sync','test_sync_semantics','test_core_bugs','test_config_concurrency',
                'test_bridge_contract','test_experience','test_stress','test_winver','test_service_safety',
                'test_file_safety','test_aux_safety','test_tool_manager','test_ui_geometry')
            New-Item -ItemType Directory -Path (Join-Path $BuildDir 'tests') -Force | Out-Null
            foreach ($test in $Tests) {
                $source = Join-Path $PSScriptRoot ('tests/' + $test + '.cpp')
                if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required test missing: $source" }
                $object = Join-Path $BuildDir ('obj/' + $test + '.o')
                $exe = Join-Path $BuildDir ('tests/' + $test + '.exe')
                Write-Host "[test C++] $test"
                Invoke-Native $Compiler ($Flags + @('-UNDEBUG','-c',$source,'-o',$object))
                # Test targets are console main(), never -municode/-mwindows.
                $testObjects = @($objects)
                if ($test -eq 'test_tool_manager') {
                    $testObjects = @($objects | Where-Object { [IO.Path]::GetFileName($_) -ne 'tool_manager.o' })
                }
                Invoke-Native $Compiler (@($object) + $testObjects + @('-o',$exe) + $Link)
                Assert-Imports $exe 3
            }
        }
        [ordered]@{architecture=$Architecture;target=$target;executableName=$ExecutableName;compiler=$Compiler;windres=$Windres;objdump=$Objdump;configuration=$Configuration;mode=$Mode;time=(Get-Date -Format o)} |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $BuildDir 'build-info.json') -Encoding UTF8
        Write-Host "Build succeeded: $BuildDir"
    } finally { $lock.Dispose() }
    exit 0
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
