@echo off
rem Builds GT3 PC as build.cmd does, then packages the build as an engine plugin:
rem ONE folder holding what swaps the game's engine - this tree's launcher and
rem game (gt3launcher.exe, gt3pc.exe, and SDL3.dll, which both load) and
rem engine.txt, the name the launcher's engine selector lists it by.  A tree
rem with PC VR (docs\vr.md) also has openxr_loader.dll, which the game loads
rem when [VR] Enabled is on: it is packaged too, else the engine would run as
rem a flat screen game, the reason only a line in gt3pc.log.
rem
rem     "Build Mod Plugin.cmd" [name]
rem
rem name: the engine's name and its folder's, build\plugins\<name> (default
rem "GT3 PC Mod").  The folder is what a user adds in the launcher's engine
rem selector (Add engine...).  The same programs run as an engine there - the
rem selector starts the folder's launcher with --engine --shared <its own
rem folder>, so the settings (gt3pc.ini) are the user's one file - and by
rem themselves when started directly.  What the engine keeps for itself is
rem written into the folder as it is used, never by this script: its mod
rem folders and disc images (gt3pc_engine.ini), its memory cards (memcard1,
rem memcard2; since 2026-11-15 its save folders, saves\<its name>\ beside the
rem shared gt3pc.ini, the slots' in gt3pc_engine.ini [Saves]), its installed
rem versions (data_US ...) and its caches.  Packaging
rem again copies the programs over the old ones and leaves all that alone.
rem A mod's data is not part of the plugin: a mod folder of its own
rem (docs\launcher.md "Engines").
rem The build brings the interface's translations up to date and asks for the
rem ones missing at the console (build.cmd: blank skips one, skipall the rest).
setlocal
set "ROOT=%~dp0"
set "NAME=%~1"
set "NAMED=1"
if "%NAME%"=="" set "NAMED=" & set "NAME=GT3 PC Mod"
call "%ROOT%build.cmd"
if errorlevel 1 set "RESULT=1" & goto end
set "OUT=%ROOT%build\plugins\%NAME%"
if not exist "%OUT%\" mkdir "%OUT%"
if not exist "%OUT%\" echo The folder "%OUT%" could not be made: a name a folder cannot have? & set "RESULT=1" & goto end
rem (no parentheses in the texts of this block: one would end it)
for %%F in (gt3launcher.exe gt3pc.exe SDL3.dll) do (
    if not exist "%ROOT%build\bin\%%F" echo build\bin\%%F is missing: the build did not make it. & set "RESULT=1" & goto end
    copy /Y "%ROOT%build\bin\%%F" "%OUT%\" >nul
    if errorlevel 1 echo "%OUT%\%%F" could not be written: is that engine running? & set "RESULT=1" & goto end
)
rem (the licence texts of the third-party software in the programs, licences\: handed on with them)
if not exist "%ROOT%build\bin\licences\" goto nolic
xcopy /E /I /Y /Q "%ROOT%build\bin\licences" "%OUT%\licences" >nul
if errorlevel 1 echo "%OUT%\licences" could not be written: is that engine running? & set "RESULT=1" & goto end
:nolic
rem (the OpenXR loader: only a tree with PC VR builds one)
if not exist "%ROOT%build\bin\openxr_loader.dll" goto noxr
copy /Y "%ROOT%build\bin\openxr_loader.dll" "%OUT%\" >nul
if errorlevel 1 echo "%OUT%\openxr_loader.dll" could not be written: is that engine running? & set "RESULT=1" & goto end
rem (the loader is Apache-2.0 or MIT: a copy of it handed on needs the licence text beside it)
for /d %%D in ("%ROOT%third_party\OpenXR-SDK-release-*") do if exist "%%D\LICENSE" copy /Y "%%D\LICENSE" "%OUT%\openxr_loader-LICENSE.txt" >nul
:noxr
rem (a name given, or none yet: an engine.txt edited by hand is kept otherwise)
if defined NAMED goto name
if exist "%OUT%\engine.txt" goto done
:name
>"%OUT%\engine.txt" echo(%NAME%
:done
echo.
echo Engine plugin "%NAME%":
echo   %OUT%
echo Add that folder in the launcher's engine selector (Add engine...).
set "RESULT=0"
:end
rem (started from Explorer - double-clicked: keep the window open)
echo %cmdcmdline% | findstr /i /c:"%~nx0" >nul && call "%ROOT%tools\from_explorer.cmd" && pause
exit /b %RESULT%
