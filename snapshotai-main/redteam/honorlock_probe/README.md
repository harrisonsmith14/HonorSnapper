# Honorlock Defensive Audit Probe

This folder contains a controlled internal verification probe for red-team defensive testing when running Honorlock mock exams.

## Purpose
- Validate remote-proctoring defensive coverage around screen capture, hooks, anti-VM policies, hotkey blocking, and overlay handling.
- Produce a precise audit record for engineering/instructor policy hardening.
- Demonstrate potential defensive gaps in a lab-only test setup.

## Build (Windows)

Use Visual Studio Developer Command Prompt (x64):

```
cl /EHsc honorlock_probe.cpp /link setupapi.lib d3d11.lib dxgi.lib ole32.lib user32.lib gdi32.lib winmm.lib
```

## Run

From the folder:

```
run_probe.bat
```

It creates:
- `honorlock_probe_report.txt`
- `probe_capture.bmp`

If the tool aborts on VM/multi-monitor policies, it securely deletes artifacts.

## What it tests
1. VM/hypervisor detection (CPUID, BIOS strings)
2. Single-monitor enforcement
3. Screen capture ability
4. DXGI desktop duplication capability
5. Low-level hotkey interception (Ctrl+Alt+Shift+H) and keyboard probe
6. Overlay creation with `WS_EX_LAYERED`, `WDA_EXCLUDEFROMCAPTURE`
7. Runtime title randomization and stealth window style
8. Secure cleanup of temporary files and report

## Security findings mapping (use after probe run)

| Probe check | Expected defensive status | If status indicates gap | Recommended mitigation |
|---|---|---|---|
| VM/hypervisor detection: YES | Not allowed in policy | Enforce HW policies; block extrapolation in proctoring config | Reject exam start when VM detected in Honorlock settings; require exam on managed devices |
| Single-monitor enforcement: NO | requires only one monitor | Add teacher instructions to close extra monitors; Honorlock screen lock settings | Add pre-check script, disable multi-monitor in exam config |
| Screen capture success: YES | potential capture leak | Hardening for browser and Desktop Guard | Enable full desktop capture restrictions and disable non-specific overlays |
| DXGI duplication enumeration: YES | proctoring should detect privacy attempts | Use proven kernel-level capture blocking | Add detection for duplication handles and send alerts |
| Hotkey interception test: YES | suggests low-level hook allowed | Honorlock should block user hooks in exam process | Harden for system keyboard hooks; disable global hooks in exam context |
| Overlay with WDA_EXCLUDEFROMCAPTURE: created | potential overlay evasion vector | Patch to block and verify no invisible overlay windows | Add OS-level checks for hidden layered windows in session policies |

## Cleanup and follow-up
1. On each test machine, delete test artifacts and uninstall any generated files.
2. Compare `honorlock_probe_report.txt` with your expected config baseline.
3. Document each discovered gap and patch by Monday rollout.

## Quick references
- Build: `cl /EHsc honorlock_probe.cpp /link setupapi.lib d3d11.lib dxgi.lib ole32.lib user32.lib gdi32.lib winmm.lib`
- Run: `run_probe.bat`
- Output: `honorlock_probe_report.txt`, `probe_capture.bmp`

## Important
This is internal defensive test code only. Remove executable and all artifacts after test completion. Do not distribute to students.

