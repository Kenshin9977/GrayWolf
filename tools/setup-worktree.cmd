@echo off
rem Prepares a git worktree of this repo to build and run like the main checkout:
rem junctions to the main checkout's submodules and name indexes, the Oodle DLLs and the
rem profiles' local files (string cipher tables).
rem Usage: tools\setup-worktree.cmd <worktree path>
rem The main checkout is the one holding the worktree's common .git, whichever copy of this
rem script runs. Its exe folder is src\GrayWolf\x64\Dev, or src\WraithXCOD\x64\Dev (the name
rem before the rename to GrayWolf) while its untracked data has not moved yet.
rem Safe: rmdir without /s only removes an EMPTY folder; nothing is ever deleted recursively.
setlocal
if "%~1"=="" (echo usage: %~nx0 ^<worktree path^> & exit /b 2)
set "WT=%~f1"
if not exist "%WT%\.git" (echo not a worktree: %WT% & exit /b 2)
set "COMMON="
for /f "delims=" %%G in ('git -C "%WT%" rev-parse --path-format^=absolute --git-common-dir') do set "COMMON=%%G"
if not defined COMMON (echo cannot find the main checkout of %WT% & exit /b 2)
for %%I in ("%COMMON%\..") do set "MAIN=%%~fI"
if /i "%WT%"=="%MAIN%" (echo refusing: that is the main checkout & exit /b 2)

for %%S in (OpenAssetTools DirectXShaderCompiler) do call :link "%WT%\src\External\%%S" "%MAIN%\src\External\%%S"
set "MAINDEV=%MAIN%\src\GrayWolf\x64\Dev"
if not exist "%MAINDEV%\package_index\*" set "MAINDEV=%MAIN%\src\WraithXCOD\x64\Dev"
if not exist "%WT%\src\GrayWolf\x64\Dev" mkdir "%WT%\src\GrayWolf\x64\Dev"
call :link "%WT%\src\GrayWolf\x64\Dev\package_index" "%MAINDEV%\package_index"
for %%D in (oo2core_6_win64.dll oo2core_8_win64.dll) do if exist "%MAINDEV%\%%D" if not exist "%WT%\src\GrayWolf\x64\Dev\%%D" copy /y "%MAINDEV%\%%D" "%WT%\src\GrayWolf\x64\Dev\" >nul
rem the profiles' local, git-ignored files (string cipher tables, local settings): copied, never overwritten
for /d %%P in ("%MAIN%\zonekit\profiles\*") do for %%F in (strings.cipher.txt profile.local.json) do if exist "%%P\%%F" if not exist "%WT%\zonekit\profiles\%%~nxP\%%F" copy /y "%%P\%%F" "%WT%\zonekit\profiles\%%~nxP\" >nul
echo worktree ready: %WT% (main checkout %MAIN%, data from %MAINDEV%)
exit /b 0

rem :link <path> <target>  - leaves an existing junction or populated folder alone
:link
fsutil reparsepoint query "%~1" >nul 2>&1 && (echo ok, already a junction: %~1 & exit /b 0)
if exist "%~1\*" (
    dir /b "%~1" 2>nul | findstr "^" >nul && (echo left alone, not empty: %~1 & exit /b 0)
    rmdir "%~1"
)
mklink /J "%~1" "%~2" >nul && echo linked %~1
exit /b 0
