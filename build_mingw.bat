@echo off
setlocal

gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -static-libgcc unimg.c -lz -o unimg.exe
if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

echo.
echo Built unimg.exe successfully.
endlocal
