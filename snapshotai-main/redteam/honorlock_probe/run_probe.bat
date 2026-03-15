@echo off
REM Run the Honorlock defensive audit probe and display report location
honorlock_probe.exe
if exist honorlock_probe_report.txt (
    type honorlock_probe_report.txt
) else (
    echo Report file missing.
)
pause
