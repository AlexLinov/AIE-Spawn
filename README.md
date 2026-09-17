# AIE-Spawn

AIE-Spawn is an x64 Beacon Object File (BOF) that demonstrates privilege escalation through a misconfigured `AlwaysInstallElevated` policy. It embeds a purpose-built MSI package and operator-provided raw x64 shellcode, then invokes Windows Installer to execute the payload with elevated privileges when the policy is enabled in both HKLM and HKCU.

## Requirements

On Kali Linux or Debian, install the build dependencies:

```bash
sudo apt install make python3 gcc-mingw-w64-x86-64
```

## Build

Place the raw x64 shellcode in the project root as `shellcode.bin`, then run:

```bash
make
```

The resulting BOF is written to:

```text
dist/aie_spawn.x64.o
```

## Usage

The BOF takes no arguments:

```text
execute bof /path/to/dist/aie_spawn.x64.o
```
<img width="1307" height="474" alt="image" src="https://github.com/user-attachments/assets/b292855d-d274-49af-a9be-2c6307289892" />

## Target Requirements

`AlwaysInstallElevated` must be enabled in both of the following registry locations on the target:

```text
HKLM\Software\Policies\Microsoft\Windows\Installer
HKCU\Software\Policies\Microsoft\Windows\Installer
```

Each location must contain an `AlwaysInstallElevated` DWORD value set to `1`.

## Rebuilding the Embedded MSI

The MSI package is already embedded, so the additional MSI tooling is not required for a normal build.

If you modify any of the following files, rebuild the MSI package:

```text
src/bridge.c
installer/package.wxs
installer/CustomAction.idt
```

Install the required tooling and rebuild:

```bash
sudo apt install wixl msitools xxd
make package
make
```

`make package` rebuilds the MSI and replaces `resources/package.h` with the updated embedded package.

## Disclaimer

This project is intended solely for authorized security testing, research, and educational use. Use it only on systems you own or have explicit permission to assess. You are responsible for complying with all applicable laws and rules of engagement. The authors assume no liability for misuse or damage resulting from this software.
