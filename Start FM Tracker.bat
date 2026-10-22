@echo off
rem Launches FM Tracker. Needs Python 3.8+ (python.org installer includes tkinter).
cd /d "%~dp0"
where pyw >nul 2>nul
if %errorlevel%==0 (
    start "" pyw -3 FMTracker.pyw %*
    exit /b
)
where pythonw >nul 2>nul
if %errorlevel%==0 (
    start "" pythonw FMTracker.pyw %*
    exit /b
)
echo.
echo  Python 3 was not found.
echo  Install it from https://www.python.org/downloads/  (tick "Add python.exe to PATH"),
echo  then run this file again.
echo.
pause
