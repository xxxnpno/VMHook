#requires -Version 5.1
<#
.SYNOPSIS
  Local mirror of the GitHub Actions Windows JVM matrix
  (.github/workflows/ci.yml -> build-and-unit-test + jvm-windows), run in a
  SELF-CONTAINED CLOSED ENVIRONMENT under .localci\ (git-ignored).

  Temurin JDKs are downloaded + cached PER VERSION into .localci\jdks\ (never
  installed system-wide, never re-downloaded if already present). The GitHub CI
  is unchanged and still authoritative; this is a fast local pre-flight so the
  Windows MSVC-ABI cells (msvc / clang-cl) -- where the #38 stalls and the java8
  on_exception issues live, and which MinGW cannot reproduce -- can be validated
  before the slow GitHub round-trip.

.DESCRIPTION
  Per requested compiler (auto-detected: mingw always; msvc/clang if installed)
  it builds the example DLL + injector ONCE (CMake Release == the build job's
  uploaded artifact) and runs the no-JVM ctest lane. Then per requested Java
  version it compiles the fixtures with THAT JDK's javac and runs the injection
  integration test exactly as CI does:
      java -Xmx4g -Xmn3g -cp out vmhook.Main   (the #38 heap lever)
      injector.exe <pid>                       (CreateRemoteThread + LoadLibraryW)
      check test_results.txt for [FAIL] / the TOTAL line / a 120 s stall.

.EXAMPLE
  .\.localci\run-local-ci.ps1                      # all detected compilers x all 7 JDKs
  .\.localci\run-local-ci.ps1 -Compilers clang -Java 25   # reproduce clang.java25 fast
  .\.localci\run-local-ci.ps1 -Compilers msvc  -Java 8    # reproduce msvc.java8 fast
  .\.localci\run-local-ci.ps1 -NoBuild -Compilers mingw -Java 17,21  # reuse build, 2 cells
  .\.localci\run-local-ci.ps1 -UnitOnly            # just build + no-JVM ctest, no JVM cells
#>
[CmdletBinding()]
param(
    [string[]] $Java      = @('8', '11', '17', '21', '24', '25', '26'),
    [string[]] $Compilers = @(),     # empty => auto-detect (mingw + any of msvc/clang present)
    [switch]   $NoBuild,             # reuse existing per-compiler builds
    [switch]   $UnitOnly,            # build + ctest only; skip the JVM cells
    [int]      $TimeoutSec = 120,    # matches CI's 120 s stall timeout
    [int]      $Parallel  = 0        # concurrent cells: 0 = auto (RAM/core based), 1 = sequential, N = N at once
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ── Closed-environment layout (all git-ignored) ──────────────────────────────
$RepoRoot  = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Env       = $PSScriptRoot                      # .localci\
$JdkDir    = Join-Path $Env 'jdks'              # extracted JDKs, one dir per major
$CacheDir  = Join-Path $Env 'cache'             # downloaded archives (kept, never re-fetched)
$BuildRoot = Join-Path $Env 'builds'            # per-compiler CMake build dirs
$WorkRoot  = Join-Path $Env 'work'              # per-cell scratch (out\, test_results.txt)
$LogDir    = Join-Path $Env 'logs'
foreach ($d in @($JdkDir, $CacheDir, $BuildRoot, $WorkRoot, $LogDir)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
}

function Write-Head([string] $msg) { Write-Host ""; Write-Host "==== $msg ====" -ForegroundColor Cyan }
function Write-Ok  ([string] $msg) { Write-Host "  [OK]   $msg" -ForegroundColor Green }
function Write-Bad ([string] $msg) { Write-Host "  [FAIL] $msg" -ForegroundColor Red }
function Write-Note([string] $msg) { Write-Host "  [..]   $msg" -ForegroundColor DarkGray }

# Run a native tool capturing stdout+stderr to $Log, returning its exit code.
# Uses Start-Process (OS-level redirection) so a tool's stderr/warnings are NOT
# turned into terminating NativeCommandError records under $ErrorActionPreference=Stop
# (the Windows PowerShell 5.1 native-stderr trap).
function Invoke-Native {
    param(
        [Parameter(Mandatory)] [string]   $File,
        [string[]]                         $Arguments = @(),
        [Parameter(Mandatory)] [string]   $Log,
        [string]                           $WorkDir = $RepoRoot
    )
    $outF = "$Log.out"; $errF = "$Log.err"
    $p = Start-Process -FilePath $File -ArgumentList $Arguments -WorkingDirectory $WorkDir `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outF -RedirectStandardError $errF
    "" | Out-File -FilePath $Log -Encoding utf8
    foreach ($f in @($outF, $errF)) { if (Test-Path $f) { Get-Content $f -ErrorAction SilentlyContinue | Out-File -FilePath $Log -Append -Encoding utf8 } }
    return $p.ExitCode
}

# ── JDK acquisition (idempotent: cache the zip, extract once) ─────────────────
function Get-JavaHome([string] $major) {
    # Already extracted?  (Temurin unzips to a jdk-<full>+<b> subdir.)
    $existing = Get-ChildItem -Path (Join-Path $JdkDir $major) -Recurse -Filter 'javac.exe' -ErrorAction SilentlyContinue |
                Select-Object -First 1
    if ($existing) { return (Split-Path -Parent (Split-Path -Parent $existing.FullName)) }

    $zip = Join-Path $CacheDir "temurin-$major-windows-x64.zip"
    if (-not (Test-Path $zip) -or (Get-Item $zip).Length -lt 1MB) {
        $url = "https://api.adoptium.net/v3/binary/latest/$major/ga/windows/x64/jdk/hotspot/normal/eclipse"
        Write-Note "downloading Temurin JDK $major (one-time) -> $([IO.Path]::GetFileName($zip))"
        try {
            Import-Module BitsTransfer -ErrorAction Stop
            Start-BitsTransfer -Source $url -Destination $zip -ErrorAction Stop
        } catch {
            Write-Note "BITS unavailable/failed, falling back to Invoke-WebRequest"
            Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
        }
    } else {
        Write-Note "JDK $major archive already cached -> reusing (no download)"
    }

    $dest = Join-Path $JdkDir $major
    if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest -Force | Out-Null }
    Write-Note "extracting JDK $major"
    Expand-Archive -Path $zip -DestinationPath $dest -Force

    $javac = Get-ChildItem -Path $dest -Recurse -Filter 'javac.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $javac) { throw "JDK ${major}: javac.exe not found after extracting $zip" }
    return (Split-Path -Parent (Split-Path -Parent $javac.FullName))
}

# ── Visual Studio discovery (Build Tools or full IDE) ────────────────────────
$script:VsPath = $null
$script:VsPathResolved = $false
function Get-VsInstallPath {
    if ($script:VsPathResolved) { return $script:VsPath }
    $script:VsPathResolved = $true
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        # -products * is REQUIRED to see Build Tools (the default filter only
        # matches Community/Professional/Enterprise IDE installs).
        $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($p) { $script:VsPath = "$p".Trim() }
    }
    return $script:VsPath
}
# The clang++ that ships with VS (Component.VC.Llvm.Clang) — MSVC-ABI, not on PATH.
function Get-VsClangBin {
    $p = Get-VsInstallPath
    if (-not $p) { return $null }
    $bin = Join-Path $p 'VC\Tools\Llvm\x64\bin'
    if (Test-Path (Join-Path $bin 'clang++.exe')) { return $bin }
    return $null
}

# ── MSVC environment import (cl / clang need vcvars64 in the session) ─────────
$script:VcVarsImported = $false
function Import-VcVars {
    if ($script:VcVarsImported) { return $true }
    $vsPath = Get-VsInstallPath
    if (-not $vsPath) { return $false }
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { return $false }
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue }
    }
    $script:VcVarsImported = $true
    return $true
}

# ── Compiler detection ───────────────────────────────────────────────────────
function Detect-Compilers {
    $found = @()
    if (Get-Command g++ -ErrorAction SilentlyContinue) { $found += 'mingw' }
    $vs = Get-VsInstallPath
    if ($vs) { $found += 'msvc' }
    # clang needs the MSVC STL/linker (vcvars); accept clang++ on PATH OR the
    # clang++ bundled inside the VS install (VC.Llvm.Clang component).
    if ($vs -and ((Get-Command clang++ -ErrorAction SilentlyContinue) -or (Get-VsClangBin))) { $found += 'clang' }
    return $found
}

function Get-CompilerCxx([string] $c) {
    switch ($c) {
        'mingw' { return @{ cc = 'gcc';   cxx = 'g++';     needVcVars = $false } }
        'msvc'  { return @{ cc = 'cl';    cxx = 'cl';      needVcVars = $true  } }
        'clang' { return @{ cc = 'clang'; cxx = 'clang++'; needVcVars = $true  } }
    }
    throw "unknown compiler '$c'"
}

# ── Build the DLL + injector once per compiler (== the CI artifact) ──────────
function Build-Compiler([string] $c) {
    $info = Get-CompilerCxx $c
    $buildDir = Join-Path $BuildRoot $c
    $dll = Join-Path $buildDir 'vmhook.dll'
    $inj = Join-Path $buildDir 'injector.exe'

    if ($NoBuild -and (Test-Path $dll) -and (Test-Path $inj)) {
        Write-Ok "$c : reusing existing build (-NoBuild)"
        return @{ ok = $true; dll = $dll; inj = $inj; buildDir = $buildDir }
    }
    if ($info.needVcVars) {
        if (-not (Import-VcVars)) { Write-Bad "$c : MSVC vcvars64 not found (install VS Build Tools w/ 'Desktop C++')"; return @{ ok = $false } }
    }
    if ($c -eq 'clang' -and -not (Get-Command clang++ -ErrorAction SilentlyContinue)) {
        $cb = Get-VsClangBin
        if ($cb) { $env:PATH = "$cb;$env:PATH"; Write-Note "clang : using VS-bundled clang++ ($cb)" }
        else { Write-Bad "$c : clang++ not found (on PATH or in VS); install LLVM or the VS 'C++ Clang' component"; return @{ ok = $false } }
    }
    Write-Head "BUILD $c (CMake Release -> vmhook.dll + injector.exe)"
    $log = Join-Path $LogDir "build-$c.log"
    # Pin the binaries into THIS compiler's tree; vmhook defaults them to
    # <repo>/build, which every compiler would otherwise share.
    $cfgArgs = @('-S', $RepoRoot, '-B', $buildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$buildDir",
                "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=$buildDir",
                 "-DCMAKE_C_COMPILER=$($info.cc)", "-DCMAKE_CXX_COMPILER=$($info.cxx)")
    $rc = Invoke-Native -File 'cmake' -Arguments $cfgArgs -Log "$log.configure"
    if ($rc -ne 0) { Write-Bad "$c : cmake configure failed (see $log.configure)"; return @{ ok = $false } }
    $rc = Invoke-Native -File 'cmake' -Arguments @('--build', $buildDir, '--config', 'Release', '--parallel') -Log "$log.build"
    if ($rc -ne 0) { Write-Bad "$c : build failed (see $log.build)"; return @{ ok = $false } }

    Write-Note "$c : running no-JVM ctest lane"
    $rc = Invoke-Native -File 'ctest' -Arguments @('--test-dir', $buildDir, '--output-on-failure', '-C', 'Release') -Log "$log.ctest"
    $ctestOk = ($rc -eq 0)
    if (-not (Test-Path $dll) -or -not (Test-Path $inj)) { Write-Bad "$c : vmhook.dll / injector.exe missing after build"; return @{ ok = $false } }
    if ($ctestOk) { Write-Ok "$c : build + ctest green" } else { Write-Bad "$c : ctest FAILED (see $log)" }
    return @{ ok = $true; dll = $dll; inj = $inj; buildDir = $buildDir; ctestOk = $ctestOk }
}

# ── One JVM cell: javac fixtures + java + inject + check (mirrors ci.yml) ─────
function Run-Cell([string] $c, [string] $major, [hashtable] $build) {
    $javaHome = Get-JavaHome $major
    $javac = Join-Path $javaHome 'bin\javac.exe'
    $javaExe = Join-Path $javaHome 'bin\java.exe'

    $work = Join-Path $WorkRoot "$c-java$major"
    if (Test-Path $work) { Remove-Item $work -Recurse -Force }
    New-Item -ItemType Directory -Path (Join-Path $work 'out') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $work 'bin') -Force | Out-Null
    # injector loads vmhook.dll from its OWN directory -> keep them together
    Copy-Item $build.dll (Join-Path $work 'bin\vmhook.dll') -Force
    Copy-Item $build.inj (Join-Path $work 'bin\injector.exe') -Force

    # 1) compile fixtures with THIS JDK (catches per-version javac issues)
    $srcs = @()
    $srcs += (Get-ChildItem (Join-Path $RepoRoot 'example\vmhook\*.java')).FullName
    $srcs += (Get-ChildItem (Join-Path $RepoRoot 'example\vmhook\fixtures\*.java')).FullName
    $javacLog = Join-Path $work 'javac'
    $rc = Invoke-Native -File $javac -Arguments (@('-encoding', 'UTF-8', '-d', (Join-Path $work 'out')) + $srcs) -Log $javacLog
    if ($rc -ne 0) {
        $jerr = @(Get-Content "$javacLog.err" -ErrorAction SilentlyContinue | Where-Object { $_ -match 'error' } | Select-Object -First 4)
        return @{ cell = "$c.java$major"; status = 'JAVAC-FAIL'; detail = ($jerr -join ' / ') }
    }

    # 2) launch the target JVM with the #38 heap lever
    Push-Location $work
    try {
        $jproc = Start-Process -FilePath $javaExe `
            -ArgumentList '-Xmx4g', '-Xmn3g', '-cp', 'out', 'vmhook.Main' `
            -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput 'java_stdout.txt' -RedirectStandardError 'java_stderr.txt'
        Start-Sleep -Seconds 5

        # 3) inject
        $iproc = Start-Process -FilePath (Join-Path $work 'bin\injector.exe') `
            -ArgumentList "$($jproc.Id)" -PassThru -Wait -WindowStyle Hidden `
            -RedirectStandardOutput 'injector_stdout.txt' -RedirectStandardError 'injector_stderr.txt'
        if ($iproc.ExitCode -ne 0) {
            if (-not $jproc.HasExited) { Stop-Process -Id $jproc.Id -Force -ErrorAction SilentlyContinue }
            return @{ cell = "$c.java$major"; status = 'INJECT-FAIL'; detail = (Get-Content 'injector_stdout.txt' -ErrorAction SilentlyContinue | Select-Object -Last 3) -join ' / ' }
        }

        # 4) wait for the suite (a stall here == the #38 GC-safepoint hang)
        if (-not $jproc.WaitForExit($TimeoutSec * 1000)) {
            Stop-Process -Id $jproc.Id -Force -ErrorAction SilentlyContinue
            $lastMod = (Get-Content 'test_results.txt' -ErrorAction SilentlyContinue | Where-Object { $_ -match '=== module:' } | Select-Object -Last 1)
            return @{ cell = "$c.java$major"; status = 'STALL'; detail = "no exit in ${TimeoutSec}s; last module: $lastMod" }
        }
    } finally { Pop-Location }

    # 5) verdict from test_results.txt (authoritative, like CI)
    $resFile = Join-Path $work 'test_results.txt'
    if (-not (Test-Path $resFile)) { return @{ cell = "$c.java$major"; status = 'NO-RESULTS'; detail = "test_results.txt not created (java exit=$($jproc.ExitCode))" } }
    $res  = Get-Content $resFile
    $pass = @($res | Where-Object { $_ -match '^\[PASS\]' })
    $fail = @($res | Where-Object { $_ -match '^\[FAIL\]' })
    $info = @($res | Where-Object { $_ -match '^\[INFO\]' })
    $total = @($res | Where-Object { $_ -match '^TOTAL:' })
    if ($fail.Count -gt 0) {
        return @{ cell = "$c.java$major"; status = 'FAIL'; detail = "$($fail.Count) FAIL: " + (($fail | Select-Object -First 5) -join '; '); pass = $pass.Count }
    }
    if ($total.Count -eq 0) {
        $lastMod = ($res | Where-Object { $_ -match '=== module:' } | Select-Object -Last 1)
        return @{ cell = "$c.java$major"; status = 'INCOMPLETE'; detail = "no TOTAL line (crash); last module: $lastMod"; pass = $pass.Count }
    }
    return @{ cell = "$c.java$major"; status = 'PASS'; detail = "$($total[0])"; pass = $pass.Count; info = $info.Count }
}

# ── Main ─────────────────────────────────────────────────────────────────────
$detected = Detect-Compilers
if ($Compilers.Count -eq 0) { $Compilers = $detected }

# Auto-parallelism: bound by cores AND RAM — each cell is a live `java -Xmx4g`,
# so budget ~5 GB/cell and leave ~4 GB for the OS. (GitHub parallelises freely
# across separate runners; locally we are capped by one machine.)
if ($Parallel -le 0) {
    $cores = [Environment]::ProcessorCount
    try { $ramGB = [int][math]::Floor((Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).TotalPhysicalMemory / 1GB) } catch { $ramGB = 16 }
    $byRam = [math]::Floor(($ramGB - 4) / 5)
    $Parallel = [math]::Min($cores - 1, $byRam)
    if ($Parallel -lt 1) { $Parallel = 1 }
    if ($Parallel -gt 6) { $Parallel = 6 }
}

Write-Head "LOCAL CI  (closed env: $Env)"
Write-Host "  repo        : $RepoRoot"
Write-Host "  compilers   : requested [$($Compilers -join ', ')]  detected [$($detected -join ', ')]"
Write-Host "  java        : [$($Java -join ', ')]"
Write-Host "  parallelism : $Parallel concurrent cell(s)"
$missing = $Compilers | Where-Object { $detected -notcontains $_ }
if ($missing) { Write-Host "  unavailable : [$($missing -join ', ')] -> SKIPPED (install to extend coverage to those cells)" -ForegroundColor Yellow }
$Compilers = @($Compilers | Where-Object { $detected -contains $_ })
if (-not $Compilers) { Write-Bad "no usable compilers detected (need at least MinGW g++ on PATH)"; exit 2 }

$results = @()

# 1) Build every compiler up front (serial — builds are CPU-heavy) + ctest lane.
$builds = @{}
foreach ($c in $Compilers) {
    $build = Build-Compiler $c
    if (-not $build.ok) { $results += @{ cell = "$c (build)"; status = 'BUILD-FAIL'; detail = "see logs\build-$c.log*" }; continue }
    if ($build.ContainsKey('ctestOk') -and -not $build.ctestOk) { $results += @{ cell = "$c ctest"; status = 'CTEST-FAIL'; detail = "no-JVM lane" } }
    $builds[$c] = $build
}

if (-not $UnitOnly) {
    # 2) Pre-resolve (download+extract) every JDK ONCE, serially, so concurrent
    #    cells never race on the same download.
    foreach ($j in $Java) { Write-Note "ensuring JDK $j present"; [void](Get-JavaHome "$j") }

    $cells = @()
    foreach ($c in @($builds.Keys)) { foreach ($j in $Java) { $cells += @{ c = $c; j = "$j" } } }

    if ($Parallel -le 1 -or $cells.Count -le 1) {
        # ── sequential ──
        foreach ($cell in $cells) {
            Write-Note "running cell $($cell.c) . java $($cell.j) ..."
            $r = Run-Cell $cell.c $cell.j $builds[$cell.c]
            if ($r.status -eq 'PASS') { Write-Ok "$($r.cell) : $($r.detail)$(if ($r.info) { "  ($($r.info) INFO)" })" } else { Write-Bad "$($r.cell) : $($r.status) -- $($r.detail)" }
            $results += $r
        }
    } else {
        # ── parallel ── each cell is an isolated single-cell subprocess that
        #    reuses the pre-built compiler (-NoBuild) and runs in its own work
        #    dir; throttled to $Parallel at once.
        Write-Note "running $($cells.Count) cells, $Parallel at a time (each = javac + java -Xmx4g + inject)"
        $pending = [System.Collections.ArrayList]@($cells)
        $running = [System.Collections.ArrayList]@()
        while ($pending.Count -gt 0 -or $running.Count -gt 0) {
            while ($running.Count -lt $Parallel -and $pending.Count -gt 0) {
                $cell = $pending[0]; $pending.RemoveAt(0)
                $clog = Join-Path $LogDir "cell-$($cell.c)-java$($cell.j).log"
                $proc = Start-Process -FilePath 'powershell' -PassThru -WindowStyle Hidden `
                            -RedirectStandardOutput $clog -RedirectStandardError "$clog.err" `
                            -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
                                            '-Compilers', $cell.c, '-Java', $cell.j, '-NoBuild', '-Parallel', '1', '-TimeoutSec', "$TimeoutSec")
                [void]$running.Add(@{ proc = $proc; cell = $cell; log = $clog })
                Write-Note "  -> launched $($cell.c).java$($cell.j) (pid $($proc.Id))"
            }
            Start-Sleep -Milliseconds 700
            foreach ($d in @($running | Where-Object { $_.proc.HasExited })) {
                # Derive the verdict from the cell's OWN [OK]/[FAIL] line — a
                # Start-Process -PassThru object's .ExitCode is unreliable here
                # (often $null, and $null -eq 0 is false -> spurious FAIL).
                $line = (Get-Content $d.log -ErrorAction SilentlyContinue | Where-Object { $_ -match '\[(OK|FAIL)\][^:]*:' } | Select-Object -Last 1)
                $cellName = "$($d.cell.c).java$($d.cell.j)"
                if ($line) {
                    $status = if ($line -match '^\s*\[OK\]') { 'PASS' } else { 'FAIL' }
                    $detail = ($line -replace '^\s*\[(OK|FAIL)\]\s+[^:]*:\s*', '').Trim()
                } else {
                    $status = 'FAIL'; $detail = "no verdict line - subprocess died (see $($d.log))"
                }
                if ($status -eq 'PASS') { Write-Ok "$cellName : $detail" } else { Write-Bad "$cellName : $detail" }
                $results += @{ cell = $cellName; status = $status; detail = $detail }
                [void]$running.Remove($d)
            }
        }
    }
}

Write-Head "SUMMARY"
$results | ForEach-Object {
    $tag = if ($_.status -eq 'PASS') { '[OK]  ' } else { '[FAIL]' }
    "{0} {1,-22} {2,-12} {3}" -f $tag, $_.cell, $_.status, $_.detail
} | Write-Host
$bad = @($results | Where-Object { $_.status -ne 'PASS' })
Write-Host ""
if ($bad.Count -eq 0) { Write-Host "ALL $($results.Count) CELLS GREEN" -ForegroundColor Green; exit 0 }
else { Write-Host "$($bad.Count)/$($results.Count) cells NOT green" -ForegroundColor Red; exit 1 }
