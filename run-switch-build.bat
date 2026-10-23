@echo off
rem Builds re:Blue for Nintendo Switch with Docker Desktop and writes
rem switch-build.log next to this file. See docs\SWITCH.md.
rem
rem The game folder is the "game" folder next to this repository
rem (..\game, with default.xex, pack\, movie\ ...). Pass another one as the
rem first argument:   run-switch-build.bat D:\Games\reblue\game
rem
rem The first run builds the host tools image and takes a long while; later runs
rem only rebuild what changed. The NRO is copied to ..\SD-card\switch\reblue\.
cd /d "%~dp0"
echo re:Blue Switch build - %date% %time% > switch-build.log
where docker >> switch-build.log 2>&1
if errorlevel 1 (echo RESULT: Docker is not installed. >> switch-build.log & echo Docker is not installed. & goto end)
docker info > nul 2>&1
if errorlevel 1 (echo RESULT: Docker Desktop is not running. Start it and try again. >> switch-build.log & echo Docker Desktop is not running. & goto end)

rem Options after the game folder (or alone) go to build-switch.ps1, e.g.
rem   run-switch-build.bat -Lto        link-time optimisation (~16 GB RAM for Docker)
rem   run-switch-build.bat D:\Games\reblue\game -Lto
set GAMEARG=
if "%~1"=="" goto args_done
set "FIRST=%~1"
if "%FIRST:~0,1%"=="-" goto args_done
set GAMEARG=-Game "%~1"
shift
:args_done

echo Building re:Blue for Switch... (progress in switch-build.log)
powershell -NoProfile -ExecutionPolicy Bypass -File tools\switch\build-switch.ps1 %GAMEARG% %1 %2 %3 >> switch-build.log 2>&1
set BUILDRC=%errorlevel%
echo RESULT: build exit code %BUILDRC% >> switch-build.log
if not "%BUILDRC%"=="0" (echo Build FAILED, see switch-build.log & goto end)

if not exist ..\SD-card\switch\reblue mkdir ..\SD-card\switch\reblue
copy /y out\switch\reblue.nro ..\SD-card\switch\reblue\reblue.nro >> switch-build.log
copy /y out\switch\reblue.elf ..\SD-card\switch\reblue\reblue.elf >> switch-build.log
if exist out\switch\reblue_gputest.nro copy /y out\switch\reblue_gputest.nro ..\SD-card\switch\reblue\reblue_gputest.nro >> switch-build.log
echo Build OK: ..\SD-card\switch\reblue\reblue.nro
:end
echo Done. See switch-build.log
pause
