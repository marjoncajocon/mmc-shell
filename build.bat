@echo off
rem build.bat - builds mmc with zig (https://ziglang.org), used as a C compiler
rem
rem   build                 mmc.exe, mmc-shell.exe and mmc-term.exe for this PC
rem   build cross           every platform, into dist\
rem   build test            run the tests of the terminal core
rem   build install DIR     copy the programs into DIR (add DIR to your PATH)
rem   build release         the downloads of a release, into release\
rem   build clean
rem
rem mmc.exe and mmc-shell.exe are the same program: the shell. Windows already
rem has an mmc.exe (Microsoft Management Console) in System32 that wins the
rem PATH lookup, so from outside the name that always works is mmc-shell.
rem mmc-term.exe is the terminal window; it starts the shell inside itself.

setlocal
cd /d "%~dp0"

set ZIG=zig
where zig >nul 2>nul || set ZIG=D:\env\zig\zig.exe

set CFLAGS=-std=c11 -O2 -s -Wall -Wextra -pedantic
rem the default font: JetBrains Mono with ligatures, the Powerline and Nerd Font icons
set FONTS=JetBrainsMonoNerdFontMono-Regular.ttf JetBrainsMonoNerdFontMono-Bold.ttf JetBrainsMonoNerdFontMono-Italic.ttf JetBrainsMonoNerdFontMono-BoldItalic.ttf JetBrainsMonoNerdFont-OFL.txt JetBrainsMonoNerdFont-README.md
set BASE=mutil.c mpath.c mos.c
set SRC=mmc.c mparse.c mexpand.c mpattern.c marith.c mregex.c mvar.c mexec.c mjobs.c mbuiltin.c mbvars.c mbio.c mbtest.c mline.c mcomp.c %BASE%
set CORE=tgrid.c tvt.c ttheme.c tfont.c tshape.c tdraw.c
set TSRC=mterm.c tapp.c tpty.c twin32.c tx11.c tcocoa.c %CORE% %BASE%
rem Windows only: the .rc files put the icon (mmc.ico) and version details in
set WINRES=mmc.rc -lshell32
set TWINRES=mterm.rc -Wl,--subsystem,windows -lgdi32 -luser32 -lshell32

if "%1"=="" goto native
if "%1"=="cross" goto cross
if "%1"=="test" goto test
if "%1"=="install" goto install
if "%1"=="release" goto release
if "%1"=="clean" goto clean
echo usage: build [cross ^| test ^| install DIR ^| release ^| clean]
exit /b 2

:native
%ZIG% cc %CFLAGS% -target x86_64-windows-gnu -o mmc.exe %SRC% %WINRES% || exit /b 1
copy /y mmc.exe mmc-shell.exe >nul
%ZIG% cc %CFLAGS% -target x86_64-windows-gnu -o mmc-term.exe %TSRC% %TWINRES% || exit /b 1
echo built mmc.exe, mmc-shell.exe and mmc-term.exe
exit /b 0

:test
%ZIG% cc %CFLAGS% -target x86_64-windows-gnu -o ttest.exe ttest.c %CORE% %BASE% -lgdi32 -lshell32 || exit /b 1
.\ttest.exe
exit /b %ERRORLEVEL%

:cross
if not exist dist mkdir dist
for %%T in (x86_64 aarch64) do (
  echo %%T-windows
  %ZIG% cc %CFLAGS% -target %%T-windows-gnu -o dist\mmc-shell-%%T-windows.exe %SRC% %WINRES% || exit /b 1
  %ZIG% cc %CFLAGS% -target %%T-windows-gnu -o dist\mmc-term-%%T-windows.exe %TSRC% %TWINRES% || exit /b 1
  echo %%T-linux
  rem static musl: the same program runs on any Linux, and on Android (Termux, adb shell)
  %ZIG% cc %CFLAGS% -target %%T-linux-musl -static -o dist\mmc-%%T-linux %SRC% || exit /b 1
  rem the window loads libX11 at run time: that needs the dynamic C library
  %ZIG% cc %CFLAGS% -target %%T-linux-gnu -o dist\mmc-term-%%T-linux %TSRC% -ldl -lm -lpthread || exit /b 1
  echo %%T-macos
  %ZIG% cc %CFLAGS% -target %%T-macos -o dist\mmc-%%T-macos %SRC% || exit /b 1
  %ZIG% cc %CFLAGS% -target %%T-macos -o dist\mmc-term-%%T-macos %TSRC% -lm || exit /b 1
)
echo arm-linux (older 32 bit Android phones)
%ZIG% cc %CFLAGS% -target arm-linux-musleabihf -static -o dist\mmc-arm-linux %SRC% || exit /b 1
if exist dist\*.pdb del dist\*.pdb
echo done, see dist\
exit /b 0

:install
if "%~2"=="" (
  echo usage: build install DIR
  exit /b 2
)
if not exist mmc-term.exe call "%~f0" || exit /b 1
if not exist "%~2" mkdir "%~2"
for %%F in ("%~2\*.exe.old*") do del "%%F" >nul 2>nul
call :put mmc.exe "%~2\mmc.exe" || exit /b 1
call :put mmc.exe "%~2\mmc-shell.exe" || exit /b 1
call :put mmc-term.exe "%~2\mmc-term.exe" || exit /b 1
copy /y LICENSE "%~2\LICENSE" >nul
rem the JetBrains Mono Nerd Font travels with mmc: mmc-term looks in usr\share\fonts first
if not exist "%~2\usr\share\fonts" mkdir "%~2\usr\share\fonts"
for %%F in (%FONTS%) do copy /y %%F "%~2\usr\share\fonts\%%F" >nul
echo installed mmc.exe, mmc-shell.exe and mmc-term.exe in "%~2"
echo add "%~2" to your PATH, then type: mmc-term  (the window)  or  mmc-shell
exit /b 0

rem One archive per system, named mmc-shell-VERSION-SYSTEM, each with a folder
rem of that name inside: only the programs, LICENSE and the font (no source).
rem Windows gets .zip, Linux and macOS .tar.gz (the mtree lists give the
rem programs their x bit, which a file on a Windows disk does not have).
rem SHA256SUMS.txt lets people check a download: sha256sum -c SHA256SUMS.txt
:release
set VER=
for /f "tokens=3" %%V in ('findstr /b /c:"#define MMC_VERSION" mmc.h') do set VER=%%~V
if "%VER%"=="" (
  echo cannot read MMC_VERSION from mmc.h
  exit /b 1
)
rem the tar of Windows 10 and newer (bsdtar): it also writes zip files
set TAR=%SystemRoot%\System32\tar.exe
call "%~f0" test || exit /b 1
call "%~f0" cross || exit /b 1
if exist release rmdir /s /q release
mkdir release
call :winpkg x86_64 x64 || exit /b 1
call :winpkg aarch64 arm64 || exit /b 1
call :unixpkg x86_64 linux linux-x64 || exit /b 1
call :unixpkg aarch64 linux linux-arm64 || exit /b 1
call :unixpkg x86_64 macos macos-x64 || exit /b 1
call :unixpkg aarch64 macos macos-arm64 || exit /b 1
call :unixpkg arm linux linux-arm shell || exit /b 1
rem written with \n line ends, or sha256sum -c on Linux fails
powershell -NoProfile -Command "$l = Get-ChildItem release -File | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower() + '  ' + $_.Name }; [IO.File]::WriteAllText((Join-Path (Resolve-Path release) 'SHA256SUMS.txt'), ($l -join [char]10) + [char]10)" || exit /b 1
echo mmc %VER% is ready in release\
dir /b release
exit /b 0

rem %1 = zig's name of the CPU, %2 = the name in the archive
:winpkg
set PKG=mmc-shell-%VER%-windows-%2
set D=release\%PKG%
mkdir "%D%\usr\share\fonts"
copy /y dist\mmc-shell-%1-windows.exe "%D%\mmc.exe" >nul || exit /b 1
copy /y dist\mmc-shell-%1-windows.exe "%D%\mmc-shell.exe" >nul || exit /b 1
copy /y dist\mmc-term-%1-windows.exe "%D%\mmc-term.exe" >nul || exit /b 1
copy /y LICENSE "%D%\LICENSE" >nul
for %%F in (%FONTS%) do copy /y %%F "%D%\usr\share\fonts\%%F" >nul
"%TAR%" -a -cf "release\%PKG%.zip" -C release %PKG% || exit /b 1
rmdir /s /q "%D%"
exit /b 0

rem %1 = CPU, %2 = linux or macos, %3 = the name in the archive,
rem %4 = "shell" for the shell alone (32 bit ARM has no mmc-term)
:unixpkg
set PKG=mmc-shell-%VER%-%3
set M=release\%PKG%.mtree
> "%M%" echo #mtree
>>"%M%" echo %PKG%/mmc type=file mode=0755 contents=dist/mmc-%1-%2
if not "%4"=="shell" >>"%M%" echo %PKG%/mmc-term type=file mode=0755 contents=dist/mmc-term-%1-%2
>>"%M%" echo %PKG%/LICENSE type=file mode=0644 contents=LICENSE
if not "%4"=="shell" for %%F in (%FONTS%) do >>"%M%" echo %PKG%/usr/share/fonts/%%F type=file mode=0644 contents=%%F
"%TAR%" -czf "release\%PKG%.tar.gz" "@%M%" || exit /b 1
del "%M%"
exit /b 0

:clean
for %%F in (mmc.exe mmc-shell.exe mmc-term.exe ttest.exe mmc.pdb mmc-term.pdb ttest.pdb) do if exist %%F del %%F
if exist dist rmdir /s /q dist
exit /b 0

rem copies %1 to %2. A program that is running cannot be overwritten, but
rem Windows lets us rename it: the old one moves aside, the new one fits.
:put
copy /y %1 %2 >nul 2>nul && exit /b 0
ren %2 "%~nx2.old%RANDOM%" || exit /b 1
copy /y %1 %2 >nul || exit /b 1
echo   (%~nx2 is running: it keeps the old version until you restart it)
exit /b 0
