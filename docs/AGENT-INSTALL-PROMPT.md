# AI Agent Installation Prompt

Clone this repository, open a terminal in the repository root, start your AI coding agent (for example, `claude`), and paste the prompt below. The agent follows the repository skills (`install-extendedhud` and `update-speedcam-db`) and guides you through installation step by step, requesting explicit confirmation before every write to the head unit.

**Prerequisites:** Connect an ENET cable, switch the ignition on (engine off), unlock SSH using the Bavarian SSH Client, and keep the generated root password handy. Never paste the root password directly into the chat — enter it only when prompted via an environment variable or SSH askpass dialog.

---

## Install ExtendedHUD

```text
I want to install ExtendedHUD (this repository) on my BMW with iDrive 6 (NBT Evo) and a factory windscreen HUD.

Use the repository skill `install-extendedhud` and follow INSTALL.md. Rules:
- Read-only steps (connecting, directory listings, copying the two framework libraries locally, SHA-256 hash checks) may run without asking.
- Before EVERY write to the vehicle (mkdir, upload, chmod, mv, rm), explain exactly what will be written where and wait for my explicit confirmation.
- Never display, store, or commit my root password, VIN, or proprietary BMW framework libraries. Prompt for the password using SSH askpass or an environment variable, never in chat.
- Verify libExtendedHUD.so against my head unit's frameworks with abi_check.py before installing. If anything is unexpected (unknown node layout, missing symbols, hash mismatch, or an existing HUD plugin such as HudTest), stop and explain.
- I am at the vehicle: give one step at a time and describe the expected result.

Vehicle model & year: <e.g., 2018 BMW F30 330i>
Speed-camera country: <e.g., Ukraine / United Kingdom>
Package source: <path to downloaded extended-hud.zip> OR <build from source>

After installation, walk me through the iDrive menu rows and verify App information (version, camera country, and data date).
If my country's camera database is not bundled in the package, use the `update-speedcam-db` skill to build it, fetching only my selected country.
```

---

## Refresh Speed-Camera Database

```text
Update my ExtendedHUD speed-camera list using the `update-speedcam-db` skill for <country>. Fetch only that country, summarize what changed (total count, added, removed, data date), and ask for confirmation before copying cameras.txt to the vehicle.
```
