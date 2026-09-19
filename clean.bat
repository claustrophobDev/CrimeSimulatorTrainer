@echo off
setlocal EnableExtensions
cd /d "%~dp0"

if exist "build" rmdir /S /Q "build"
if exist "dist\CrimeTrainer.exe" del /F /Q "dist\CrimeTrainer.exe"
if exist "dist\CrimeTrainerHook.dll" del /F /Q "dist\CrimeTrainerHook.dll"

echo Clean complete.
pause
