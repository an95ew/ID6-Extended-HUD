# A prompt for your AI agent

Clone this repository, open a terminal in it, start Claude Code (`claude`), and paste the prompt below. The agent reads the
repo's skills (`install-extendedhud`, `update-speedcam-db`) and walks you through the install. It asks before every write to the car.

Have ready: the ENET cable plugged in, ignition on, SSH unlocked with the Bavarian SSH Client, and its root password at hand. The
agent will ask you to type the password into a prompt it gives you. Don't paste the password into the chat.

---

```text
I want to install ExtendedHUD (this repository) on my BMW with iDrive 6 (NBT Evo) and a factory windscreen HUD.

Use the repo's skill `install-extendedhud` and follow INSTALL.md. Rules:
- Read-only steps (connecting, listing, copying the two framework libraries to my laptop, hash checks) you may just do.
- Before EVERY write to the car (mkdir, upload, chmod, mv, rm) tell me exactly what goes where and wait for my yes.
- Never show, save or commit my root password, my VIN, or the BMW framework libraries. Ask me to enter the password through an
  askpass prompt or environment variable, not in this chat.
- Check the library against my car's frameworks (abi_check.py) before installing. If anything is unexpected (unknown layout,
  missing symbols, a hash mismatch, another HUD app like HudTest), stop and explain.
- I'm at the car: give me one action at a time and tell me what I should see.

My car: <model, e.g. F30 330i>, country for the speed cameras: <e.g. Ukraine / United Kingdom>.
The package: <the release extended-hud.zip I downloaded, at PATH> / <please build it from source>.

After the install, show me the iDrive menu rows and check App information (version, camera country and data date).
If my country's camera list isn't in the package, use the skill `update-speedcam-db` to build it, fetching only my country.
```

---

Later, to refresh the speed cameras:

```text
Update my ExtendedHUD speed-camera list with the skill `update-speedcam-db` for <country>. Fetch only that country, show me what
changed (count, added, removed, data date) and ask before copying cameras.txt to the car.
```
