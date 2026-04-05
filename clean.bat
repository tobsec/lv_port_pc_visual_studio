@echo off
echo Cleaning build output...
rmdir /s /q "%~dp0Output" 2>nul
echo Done.
