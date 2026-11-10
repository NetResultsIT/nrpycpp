# Build nrPyCpp and its test suite against a python runtime, then run the tests (Windows, MSVC).
#
#   $env:NRPYCPP_PYTHON_ROOT = "C:\deps\python"; $env:QTDIR = "C:\Qt\6.8.6\msvc2022_64"
#   powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 [-BuildDir C:\build\nrpycpp-tests] [-Config debug]
#
# NRPYCPP_PYTHON_ROOT: the "python" folder of a deps/python package (see docs\BUILDING.md)
# QTDIR: the Qt kit; VSDEVCMD: VsDevCmd.bat to use (default: the newest Visual Studio found by vswhere)
param([string]$BuildDir = "", [ValidateSet("release", "debug")][string]$Config = "release")
$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $here
if (-not $BuildDir) { $BuildDir = Join-Path $repo "build-tests" }
if (-not $env:NRPYCPP_PYTHON_ROOT) { throw "set NRPYCPP_PYTHON_ROOT to the python runtime folder" }
if (-not $env:QTDIR) { throw "set QTDIR to the Qt kit folder (e.g. C:\Qt\6.8.6\msvc2022_64)" }

$vsdevcmd = $env:VSDEVCMD
if (-not $vsdevcmd) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found" }
    $vsdevcmd = Join-Path $vs "Common7\Tools\VsDevCmd.bat"
}

# import the MSVC x64 environment into this session
$envLines = cmd /c "`"$vsdevcmd`" -arch=x64 -host_arch=x64 -no_logo && set"
foreach ($line in $envLines) { if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] } }

$qmake = Join-Path $env:QTDIR "bin\qmake.exe"
$jom = Get-Command jom -ErrorAction SilentlyContinue
$make = if ($jom) { $jom.Source } else { "nmake" }

New-Item -ItemType Directory -Force -Path "$BuildDir\lib", "$BuildDir\tests" | Out-Null
Push-Location "$BuildDir\lib"
& $qmake "$repo\lib\nrPyCpp.pro"; if ($LASTEXITCODE) { throw "qmake (lib) failed" }
& $make $Config; if ($LASTEXITCODE) { throw "build (lib) failed" }
Pop-Location
Push-Location "$BuildDir\tests"
& $qmake "$repo\tests\nrPyCppTests.pro" "CONFIG+=$Config"; if ($LASTEXITCODE) { throw "qmake (tests) failed" }
& $make; if ($LASTEXITCODE) { throw "build (tests) failed" }
Pop-Location

# nrPyCpp.dll and python3X.dll are copied into lib\last_build by the library build
$env:PATH = "$repo\lib\last_build;$env:QTDIR\bin;$env:PATH"
Remove-Item env:NRPYCPP_PYTHONHOME, env:NRPYCPP_VENV -ErrorAction SilentlyContinue
$exe = Get-ChildItem -Recurse -Path "$BuildDir\tests" -Filter tst_nrpycpp.exe | Select-Object -First 1
& $exe.FullName @args
exit $LASTEXITCODE
