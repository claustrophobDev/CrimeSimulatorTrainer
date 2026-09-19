@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

echo ===============================================
echo CrimeTrainer v0.5 - Windows x64 build
echo Visual Studio 2026 / 2022 auto-detect
echo ===============================================
echo.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_PATH="
set "VS_VERSION="
set "VS_MAJOR="
set "CMAKE_EXE="
set "GENERATOR="
set "GENERATOR_KIND="
set "NINJA_EXE="

rem ------------------------------------------------
rem 1. Find Visual Studio with the C++ x64 toolset.
rem ------------------------------------------------
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if not defined VS_PATH set "VS_PATH=%%I"
    )
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do (
        if not defined VS_VERSION set "VS_VERSION=%%I"
    )
)

rem Fallbacks for default Visual Studio 2026 locations.
if not defined VS_PATH (
    for %%E in (Community Professional Enterprise BuildTools Insiders) do (
        if not defined VS_PATH if exist "%ProgramFiles%\Microsoft Visual Studio\18\%%E\VC\Auxiliary\Build\vcvars64.bat" (
            set "VS_PATH=%ProgramFiles%\Microsoft Visual Studio\18\%%E"
            set "VS_VERSION=18.0.0"
        )
    )
)

rem Fallbacks for default Visual Studio 2022 locations.
if not defined VS_PATH (
    for %%E in (Community Professional Enterprise BuildTools) do (
        if not defined VS_PATH if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
            set "VS_PATH=%ProgramFiles%\Microsoft Visual Studio\2022\%%E"
            set "VS_VERSION=17.0.0"
        )
    )
)

if not defined VS_PATH goto :vs_missing

for /f "tokens=1 delims=." %%M in ("%VS_VERSION%") do set "VS_MAJOR=%%M"

echo Visual Studio: "%VS_PATH%"
echo VS version:    %VS_VERSION%

rem ------------------------------------------------
rem 2. Prefer the CMake bundled with this VS instance.
rem    Fall back to a standalone CMake from PATH.
rem ------------------------------------------------
if exist "%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not defined CMAKE_EXE (
    for /f "delims=" %%I in ('where cmake.exe 2^>nul') do (
        if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
    )
)

if not defined CMAKE_EXE if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
if not defined CMAKE_EXE goto :cmake_missing

echo CMake:         "%CMAKE_EXE%"
"%CMAKE_EXE%" --version | findstr /B /C:"cmake version"
echo.

rem ------------------------------------------------
rem 3. Select the newest native VS generator that
rem    this exact CMake executable actually supports.
rem ------------------------------------------------
if "%VS_MAJOR%"=="18" (
    "%CMAKE_EXE%" --help | findstr /C:"Visual Studio 18 2026" >nul
    if not errorlevel 1 (
        set "GENERATOR=Visual Studio 18 2026"
        set "GENERATOR_KIND=VS"
    )
)

if not defined GENERATOR if "%VS_MAJOR%"=="17" (
    "%CMAKE_EXE%" --help | findstr /C:"Visual Studio 17 2022" >nul
    if not errorlevel 1 (
        set "GENERATOR=Visual Studio 17 2022"
        set "GENERATOR_KIND=VS"
    )
)

rem ------------------------------------------------
rem 4. Older CMake may not know the VS 2026 generator.
rem    In that case initialize MSVC and use Ninja/NMake.
rem ------------------------------------------------
if not defined GENERATOR (
    echo Native Visual Studio generator is unavailable in this CMake.
    echo Falling back to the MSVC command-line environment...

    if exist "%VS_PATH%\Common7\Tools\VsDevCmd.bat" (
        call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
    ) else if exist "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" (
        call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
    ) else (
        goto :devcmd_missing
    )

    where cl.exe >nul 2>nul
    if errorlevel 1 goto :cpp_missing

    for /f "delims=" %%I in ('where ninja.exe 2^>nul') do (
        if not defined NINJA_EXE set "NINJA_EXE=%%I"
    )

    if not defined NINJA_EXE if exist "%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" (
        set "NINJA_EXE=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    )

    if defined NINJA_EXE (
        for %%D in ("!NINJA_EXE!") do set "PATH=%%~dpD;!PATH!"
        set "GENERATOR=Ninja"
        set "GENERATOR_KIND=SINGLE"
    ) else (
        where nmake.exe >nul 2>nul
        if errorlevel 1 goto :build_tool_missing
        set "GENERATOR=NMake Makefiles"
        set "GENERATOR_KIND=SINGLE"
    )
)

echo Generator:     %GENERATOR%
echo.

rem ------------------------------------------------
rem 5. Clean old outputs so a failed build can never
rem    look successful because of stale binaries.
rem ------------------------------------------------
echo [1/4] Removing old build and output files...
if exist "build" rmdir /S /Q "build"
if not exist "dist" mkdir "dist"
if exist "dist\CrimeTrainer.exe" del /F /Q "dist\CrimeTrainer.exe"
if exist "dist\CrimeTrainerHook.dll" del /F /Q "dist\CrimeTrainerHook.dll"

rem ------------------------------------------------
rem 6. Configure.
rem ------------------------------------------------
echo [2/4] Configuring x64 build...
if "%GENERATOR_KIND%"=="VS" (
    "%CMAKE_EXE%" -S "%CD%" -B "%CD%\build" -G "%GENERATOR%" -A x64
) else (
    "%CMAKE_EXE%" -S "%CD%" -B "%CD%\build" -G "%GENERATOR%" -DCMAKE_BUILD_TYPE=Release
)
if errorlevel 1 goto :fail

rem ------------------------------------------------
rem 7. Build.
rem ------------------------------------------------
echo [3/4] Building Release...
if "%GENERATOR_KIND%"=="VS" (
    "%CMAKE_EXE%" --build "%CD%\build" --config Release --parallel
) else (
    "%CMAKE_EXE%" --build "%CD%\build" --parallel
)
if errorlevel 1 goto :fail

rem ------------------------------------------------
rem 8. Copy outputs.
rem ------------------------------------------------
echo [4/4] Preparing dist...
if exist "build\bin\Release\CrimeTrainer.exe" copy /Y "build\bin\Release\CrimeTrainer.exe" "dist\CrimeTrainer.exe" >nul
if exist "build\bin\Release\CrimeTrainerHook.dll" copy /Y "build\bin\Release\CrimeTrainerHook.dll" "dist\CrimeTrainerHook.dll" >nul
if not exist "dist\CrimeTrainer.exe" if exist "build\bin\CrimeTrainer.exe" copy /Y "build\bin\CrimeTrainer.exe" "dist\CrimeTrainer.exe" >nul
if not exist "dist\CrimeTrainerHook.dll" if exist "build\bin\CrimeTrainerHook.dll" copy /Y "build\bin\CrimeTrainerHook.dll" "dist\CrimeTrainerHook.dll" >nul

if not exist "dist\CrimeTrainer.exe" goto :output_missing
if not exist "dist\CrimeTrainerHook.dll" goto :output_missing

echo.
echo ===============================================
echo BUILD OK
echo Output: "%CD%\dist"
echo ===============================================
echo.
pause
exit /b 0

:vs_missing
echo.
echo [ERROR] Visual Studio with the C++ x64 toolset was not found.
echo Open Visual Studio Installer and install:
echo   Desktop development with C++
echo   MSVC x64/x86 build tools
echo   Windows 10 or Windows 11 SDK
echo.
pause
exit /b 1

:cmake_missing
echo.
echo [ERROR] CMake was not found.
echo Add CMake to Visual Studio Installer or install CMake for Windows.
echo.
pause
exit /b 1

:devcmd_missing
echo.
echo [ERROR] Visual Studio developer environment scripts were not found.
echo Repair the Visual Studio C++ workload in Visual Studio Installer.
echo.
pause
exit /b 1

:cpp_missing
echo.
echo [ERROR] cl.exe was not found after initializing Visual Studio.
echo Install the Desktop development with C++ workload.
echo.
pause
exit /b 1

:build_tool_missing
echo.
echo [ERROR] Neither Ninja nor NMake was found.
echo Install C++ CMake tools for Windows in Visual Studio Installer.
echo.
pause
exit /b 1

:output_missing
echo.
echo [ERROR] Build finished, but output files were not found.
echo Check the compiler output above.
echo.
pause
exit /b 1

:fail
echo.
echo [ERROR] Build failed.
echo Read the first compiler or CMake error above this line.
echo.
pause
exit /b 1
