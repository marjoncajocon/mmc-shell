@echo off
rem build.bat - builds mmc with zig (https://ziglang.org), used as a C compiler
rem
rem   build                 mmc.exe and mmc-shell.exe for this PC
rem   build cross           every platform, into dist\
rem   build install DIR     copy both programs into DIR (add DIR to your PATH)
rem   build clean
rem
rem mmc.exe and mmc-shell.exe are the same program. Windows already has an
rem mmc.exe (Microsoft Management Console) in System32 that wins the PATH
rem lookup, so from outside the shell the name that always works is mmc-shell.

setlocal
cd /d "%~dp0"

set ZIG=zig
where zig >nul 2>nul || set ZIG=D:\env\zig\zig.exe

set SRC=mmc.c mlex.c mexpand.c mexec.c mbuiltin.c mline.c mpath.c mos.c mutil.c
set CFLAGS=-std=c11 -O2 -s -Wall -Wextra -pedantic

if "%1"=="" goto native
if "%1"=="cross" goto cross
if "%1"=="install" goto install
if "%1"=="clean" goto clean
echo usage: build [cross ^| install DIR ^| clean]
exit /b 2

:native
%ZIG% cc %CFLAGS% -target x86_64-windows-gnu -o mmc.exe %SRC% -lshell32 || exit /b 1
copy /y mmc.exe mmc-shell.exe >nul
echo built mmc.exe and mmc-shell.exe
exit /b 0

:cross
if not exist dist mkdir dist
for %%T in (x86_64 aarch64) do (
  echo %%T-windows
  %ZIG% cc %CFLAGS% -target %%T-windows-gnu -o dist\mmc-shell-%%T-windows.exe %SRC% -lshell32 || exit /b 1
  echo %%T-linux
  %ZIG% cc %CFLAGS% -target %%T-linux-musl -static -o dist\mmc-%%T-linux %SRC% || exit /b 1
  echo %%T-macos
  %ZIG% cc %CFLAGS% -target %%T-macos -o dist\mmc-%%T-macos %SRC% || exit /b 1
)
if exist dist\*.pdb del dist\*.pdb
echo done, see dist\
exit /b 0

:install
if "%~2"=="" (
  echo usage: build install DIR
  exit /b 2
)
if not exist mmc.exe call "%~f0" || exit /b 1
if not exist "%~2" mkdir "%~2"
copy /y mmc.exe "%~2\mmc.exe" >nul || exit /b 1
copy /y mmc.exe "%~2\mmc-shell.exe" >nul || exit /b 1
echo installed mmc.exe and mmc-shell.exe in "%~2"
echo add "%~2" to your PATH, then type: mmc-shell
exit /b 0

:clean
for %%F in (mmc.exe mmc-shell.exe mmc.pdb) do if exist %%F del %%F
if exist dist rmdir /s /q dist
exit /b 0
