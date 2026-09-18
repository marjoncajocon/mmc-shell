@echo off
rem build.bat - builds mmc with zig (https://ziglang.org), used as a C compiler
rem
rem   build                 mmc.exe, mmc-shell.exe and mmc-term.exe for this PC
rem   build cross           every platform, into dist\
rem   build test            run the tests of the terminal core
rem   build install DIR     copy the programs into DIR (add DIR to your PATH)
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
set BASE=mutil.c mpath.c mos.c
set SRC=mmc.c mlex.c mexpand.c mexec.c mbuiltin.c mline.c %BASE%
set CORE=tgrid.c tvt.c ttheme.c tfont.c tdraw.c
set TSRC=mterm.c tapp.c tpty.c twin32.c tx11.c tcocoa.c %CORE% %BASE%
rem Windows only: the .rc files put the icon (mmc.ico) and version details in
set WINRES=mmc.rc -lshell32
set TWINRES=mterm.rc -Wl,--subsystem,windows -lgdi32 -luser32 -lshell32

if "%1"=="" goto native
if "%1"=="cross" goto cross
if "%1"=="test" goto test
if "%1"=="install" goto install
if "%1"=="clean" goto clean
echo usage: build [cross ^| test ^| install DIR ^| clean]
exit /b 2

:native
%ZIG% cc %CFLAGS% -target x86_64-windows-gnu -o mmc.exe %SRC% %WINRES% || exit /b 1
copy /y mmc.exe mmc-shell.exe >nul
%ZIG% cc %CFLAGS% -target x86_64-windows-gnu -o mmc-term.exe %TSRC% %TWINRES% || exit /b 1
echo built mmc.exe, mmc-shell.exe and mmc-term.exe
exit /b 0

:test
%ZIG% cc %CFLAGS% -target x86_64-windows-gnu -o ttest.exe ttest.c %CORE% %BASE% -lshell32 || exit /b 1
.\ttest.exe
exit /b %ERRORLEVEL%

:cross
if not exist dist mkdir dist
for %%T in (x86_64 aarch64) do (
  echo %%T-windows
  %ZIG% cc %CFLAGS% -target %%T-windows-gnu -o dist\mmc-shell-%%T-windows.exe %SRC% %WINRES% || exit /b 1
  %ZIG% cc %CFLAGS% -target %%T-windows-gnu -o dist\mmc-term-%%T-windows.exe %TSRC% %TWINRES% || exit /b 1
  echo %%T-linux
  %ZIG% cc %CFLAGS% -target %%T-linux-musl -static -o dist\mmc-%%T-linux %SRC% || exit /b 1
  rem the window loads libX11 at run time: that needs the dynamic C library
  %ZIG% cc %CFLAGS% -target %%T-linux-gnu -o dist\mmc-term-%%T-linux %TSRC% -ldl -lm || exit /b 1
  echo %%T-macos
  %ZIG% cc %CFLAGS% -target %%T-macos -o dist\mmc-%%T-macos %SRC% || exit /b 1
  %ZIG% cc %CFLAGS% -target %%T-macos -o dist\mmc-term-%%T-macos %TSRC% -lm || exit /b 1
)
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
rem the Hack font travels with mmc: mmc-term looks in usr\share\fonts first
if not exist "%~2\usr\share\fonts" mkdir "%~2\usr\share\fonts"
for %%F in (Hack-Regular.ttf Hack-Bold.ttf Hack-Italic.ttf Hack-LICENSE.md) do copy /y %%F "%~2\usr\share\fonts\%%F" >nul
echo installed mmc.exe, mmc-shell.exe and mmc-term.exe in "%~2"
echo add "%~2" to your PATH, then type: mmc-term  (the window)  or  mmc-shell
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
