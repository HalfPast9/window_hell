@echo off
call "C:\Users\Shri\qnx800\qnxsdp-env.bat"
cd /d "C:\Users\Shri\Documents\clonedrepos\window_hell"
call make qnx-x86
echo MAKE_EXIT=%ERRORLEVEL%
