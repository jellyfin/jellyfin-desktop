@echo off
REM Jellyfin Desktop - Run built executable
REM Run build.bat first

setlocal
call "%~dp0common.bat" || exit /b 1
call "%~dp0common.bat" :setup_runtime || exit /b 1

REM === Run ===
"%BUILD_DIR%\src\%EXE_NAME%" %*
set EXIT_CODE=%ERRORLEVEL%

endlocal & exit /b %EXIT_CODE%
