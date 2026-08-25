[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Test,
    [string]$QtDir = "",
    [string]$Config = "Release",
    [string]$PlatformName = "win_64"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildPath = Join-Path $root "build\$PlatformName"
$distPath = Join-Path $root "dist\$PlatformName"
$binPath = Join-Path $distPath "qt-magnet.exe"

if (-not $QtDir) {
    if ($env:QT_ROOT_DIR -and (Test-Path $env:QT_ROOT_DIR)) {
        $QtDir = $env:QT_ROOT_DIR
    } elseif ($env:QTDIR -and (Test-Path $env:QTDIR)) {
        $QtDir = $env:QTDIR
    } elseif (Test-Path "C:\Qt") {
        $qtVersions = Get-ChildItem "C:\Qt" -Directory | Where-Object { $_.Name -match '^\d+\.\d+' } | Sort-Object { [version]($_.Name -replace '^(\d+(\.\d+)*).*', '$1') } -Descending
        foreach ($v in $qtVersions) {
            $candidate = Join-Path $v.FullName "mingw_64"
            if (Test-Path $candidate) {
                $QtDir = $candidate
                break
            }
        }
    }
}

$toolsDirs = @()
if (Test-Path "C:\Qt\Tools") {
    $cmakeTool = Get-ChildItem "C:\Qt\Tools" -Directory -Filter "*CMake*" | Select-Object -First 1
    if ($cmakeTool -and (Test-Path (Join-Path $cmakeTool.FullName "bin"))) {
        $toolsDirs += (Join-Path $cmakeTool.FullName "bin")
    }
    $ninjaTool = Join-Path "C:\Qt\Tools" "Ninja"
    if (Test-Path $ninjaTool) {
        $toolsDirs += $ninjaTool
    }
    $mingwTool = Get-ChildItem "C:\Qt\Tools" -Directory -Filter "*mingw*" | Select-Object -First 1
    if ($mingwTool -and (Test-Path (Join-Path $mingwTool.FullName "bin"))) {
        $toolsDirs += (Join-Path $mingwTool.FullName "bin")
    }
}
if ($QtDir -and (Test-Path (Join-Path $QtDir "bin"))) {
    $toolsDirs += (Join-Path $QtDir "bin")
}
if ($toolsDirs.Count -gt 0) {
    $env:PATH = ($toolsDirs -join ";") + ";$env:PATH"
}

if ($Clean) {
    if (Test-Path $buildPath) { Remove-Item -Recurse -Force $buildPath }
    if (Test-Path $distPath) { Remove-Item -Recurse -Force $distPath }
}

New-Item -ItemType Directory -Force -Path $buildPath | Out-Null
New-Item -ItemType Directory -Force -Path $distPath | Out-Null

$generator = "MinGW Makefiles"
if (Get-Command ninja -ErrorAction SilentlyContinue) {
    $generator = "Ninja"
}

$cmakeArgs = @(
    "-B", $buildPath,
    "-G", $generator,
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_INSTALL_PREFIX=$distPath"
)
if ($QtDir) {
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$QtDir"
}

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }

& cmake --build $buildPath --config $Config
if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE" }

& cmake --install $buildPath --config $Config

$deployCmd = $null
if ($QtDir -and (Test-Path (Join-Path $QtDir "bin\windeployqt.exe"))) {
    $deployCmd = Join-Path $QtDir "bin\windeployqt.exe"
} elseif (Get-Command "windeployqt" -ErrorAction SilentlyContinue) {
    $deployCmd = "windeployqt"
}

if ((Test-Path $binPath) -and $deployCmd) {
    $deployArgs = @(
        "--no-opengl-sw",
        "--no-system-d3d-compiler",
        "--no-system-dxc-compiler",
        "--no-translations",
        "--compiler-runtime",
        $binPath
    )
    & $deployCmd @deployArgs | Out-Null

    $unwantedFiles = @("opengl32sw.dll", "D3Dcompiler_47.dll", "dxcompiler.dll", "dxil.dll")
    foreach ($f in $unwantedFiles) {
        $target = Join-Path $distPath $f
        if (Test-Path $target) { Remove-Item -Force $target }
    }
    $unwantedDirs = @("generic", "iconengines", "networkinformation", "imageformats")
    foreach ($d in $unwantedDirs) {
        $targetDir = Join-Path $distPath $d
        if (Test-Path $targetDir) { Remove-Item -Recurse -Force $targetDir }
    }
}

if ($Test) {
    $testBin = Join-Path $buildPath "smoketests.exe"
    if (Test-Path $testBin) {
        & $testBin
        if ($LASTEXITCODE -ne 0) { throw "Tests failed with exit code $LASTEXITCODE" }
    }
}
