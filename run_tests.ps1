[CmdletBinding()]
param(
    [string]$ToolchainBin = '', [string]$Compiler = '', [string]$Windres = '', [string]$Objdump = '',
    [string]$BuildDir = '',
    [ValidateSet('x86','x64')][string]$Architecture = 'x86',
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [ValidateRange(1,3600)][int]$TimeoutSeconds = 120,
    [switch]$NoBuild
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $BuildDir) { $BuildDir = Join-Path $PSScriptRoot ('build/' + $Architecture) }
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) { $PSNativeCommandUseErrorActionPreference = $false }
try {
    if (-not $NoBuild) {
        # A child shell contains build.ps1's exit statement; propagate its exact failure.
        $shell = (Get-Process -Id $PID).Path
        $buildArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'build.ps1'),'-Mode','Tests','-BuildDir',$BuildDir,'-Configuration',$Configuration,'-Architecture',$Architecture)
        foreach ($entry in @(@('ToolchainBin',$ToolchainBin),@('Compiler',$Compiler),@('Windres',$Windres),@('Objdump',$Objdump))) {
            if ($entry[1]) { $buildArgs += @('-' + $entry[0],$entry[1]) }
        }
        & $shell @buildArgs
        if ($LASTEXITCODE -ne 0) { throw "Test build failed: exit $LASTEXITCODE" }
    }
    $BuildDir = [IO.Path]::GetFullPath($BuildDir)
    $tests = @('test_sync','test_sync_semantics','test_core_bugs','test_config_concurrency',
        'test_bridge_contract','test_experience','test_stress','test_winver','test_service_safety',
        'test_file_safety','test_aux_safety','test_tool_manager','test_ui_geometry')
    $failed = New-Object System.Collections.Generic.List[string]
    $results = @()
    foreach ($test in $tests) {
        $exe = Join-Path $BuildDir ('tests/' + $test + '.exe')
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Test executable missing: $exe" }
        $bytes = [IO.File]::ReadAllBytes($exe)
        $pe = [BitConverter]::ToInt32($bytes, 0x3C)
        $machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
        $expected = if ($Architecture -eq 'x86') { 0x14C } else { 0x8664 }
        if ($machine -ne $expected) { throw "Test architecture mismatch: expected $Architecture : $exe" }
        if ([BitConverter]::ToUInt16($bytes, $pe + 24 + 68) -ne 3) { throw "Test must retain console main(), not GUI/Unicode entry: $exe" }
        $out = Join-Path $BuildDir ('tests/' + $test + '.stdout.log')
        $err = Join-Path $BuildDir ('tests/' + $test + '.stderr.log')
        Write-Host "===== $test ====="
        $start = Get-Date
        $info = New-Object Diagnostics.ProcessStartInfo
        $info.FileName = $exe
        $info.WorkingDirectory = Split-Path -Parent $exe
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.StandardOutputEncoding = [Text.Encoding]::UTF8
        $info.StandardErrorEncoding = [Text.Encoding]::UTF8
        $proc = New-Object Diagnostics.Process
        $proc.StartInfo = $info
        if (-not $proc.Start()) { throw "Cannot start test: $test" }
        $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
        $stderrTask = $proc.StandardError.ReadToEndAsync()
        $timedOut = -not $proc.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut -and -not $proc.HasExited) {
            $taskkill = Join-Path ([Environment]::GetFolderPath('System')) 'taskkill.exe'
            & $taskkill /PID $proc.Id /T /F | Out-Host
            if ($LASTEXITCODE -ne 0 -and -not $proc.HasExited) {
                $proc.Kill()
                throw "Failed to terminate the controlled test process tree: $test"
            }
        }
        $proc.WaitForExit()
        $proc.Refresh()
        $code = $proc.ExitCode
        $stdoutTask.Result | Set-Content -LiteralPath $out -Encoding UTF8
        $stderrTask.Result | Set-Content -LiteralPath $err -Encoding UTF8
        Write-Host $stdoutTask.Result
        Write-Host $stderrTask.Result
        if ($timedOut -or $null -eq $code -or $code -ne 0) { $failed.Add($test); Write-Host "[FAILED] exit=$code timeout=$timedOut" }
        else { Write-Host '[OK] exit=0' }
        $results += [ordered]@{test=$test;exitCode=$code;timedOut=$timedOut;seconds=((Get-Date)-$start).TotalSeconds}
        $proc.Dispose()
    }
    $results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $BuildDir 'test-results.json') -Encoding UTF8
    Write-Host ("Test executables: {0} passed, {1} failed" -f ($tests.Count-$failed.Count),$failed.Count)
    if ($failed.Count) { throw ('Failed tests: ' + ($failed -join ', ')) }
    exit 0
} catch { Write-Error $_ -ErrorAction Continue; exit 1 }
