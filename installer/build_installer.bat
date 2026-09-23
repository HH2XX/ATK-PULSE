@echo off
setlocal
cd /d "%~dp0"

if not exist ..\dist mkdir ..\dist

echo [Building Installer Resource...]
where g++ >nul 2>nul
if %ERRORLEVEL% equ 0 (
    windres --codepage 65001 -i installer.rc -o installer.o -O coff
    if errorlevel 1 exit /b 1
    echo [Compiling atk-tray-setup.exe with MinGW...]
    g++ -O3 -mwindows -municode -s -static installer.cpp installer.o -lole32 -lshell32 -luser32 -lgdi32 -ladvapi32 -luuid -o ..\dist\atk-tray-setup.exe
    if errorlevel 1 exit /b 1
    if exist installer.o del installer.o
    goto done
)

where cl >nul 2>nul
if %ERRORLEVEL% equ 0 (
    echo [Compiling atk-tray-setup.exe with MSVC...]
    rc /nologo /c 65001 /fo installer.res installer.rc
    if errorlevel 1 exit /b 1
    cl /nologo /O2 /utf-8 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN installer.cpp installer.res /Fe:..\dist\atk-tray-setup.exe /link /SUBSYSTEM:WINDOWS ole32.lib shell32.lib user32.lib gdi32.lib advapi32.lib uuid.lib
    if errorlevel 1 (
        echo [ERROR] Installer compilation failed.
        exit /b 1
    )
    if exist installer.res del installer.res
    if exist installer.obj del installer.obj
    goto done
)

echo [ERROR] Neither g++ nor cl.exe was found in PATH.
exit /b 1

:done
if exist ..\dist\atk-tray-setup.exe (
    echo.
    echo [SUCCESS] Installer built: dist\atk-tray-setup.exe
    dir ..\dist\atk-tray-setup.exe | findstr atk-tray-setup.exe
) else (
    echo.
    echo [ERROR] Failed to build installer!
    exit /b 1
)
