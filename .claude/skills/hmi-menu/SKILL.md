---
name: hmi-menu
description: RHMI architecture rules for BMW iDrive 6 (NBT Evo) plugin menus (unified ID space, raAction/raBoolModel/textIdModel/raDataModel, hmiState pages, handler registration). XML, text/data models, and C++ handlers must stay synchronized. For ExtendedHUD-specific layout and validation, also see skill extendedhud.
---

# BMW iDrive 6 (RHMI) Plugin Menu Rules

In an NBT Evo RHMI plugin, the XML layout, model/text identifiers, and C++ handler registrations must remain strictly synchronized. Because mismatches are not caught at compile time, they surface at runtime on the head unit as unresponsive menu rows, non-toggling checkboxes, or blank labels. Always run a consistency checker (such as `.claude/skills/extendedhud/check_app.py`) after editing any menu definition.

## XML Layout Rules

- **Entry point:** A `pluginApp` registers an `entryButton` carrying a `combinedAction` (`raAction` + `hmiAction`) to open the plugin's root `hmiState`. (For example, `applicationType="Custom" insertStateID="20" insertAfterFocusID="8195"` places an entry inside Media/Radio `ST_Ent_Selection`; ExtendedHUD inserts under **My Vehicle**.)
- **Single unified ID space:** Actions, models, `hmiState` pages, components, and the entry button all share one numeric ID space. Never reuse or renumber IDs — `HmiMain3D` caches RHMI metadata (`RHMI-Cache`) and handler code binds directly to numeric IDs.
- **Actions:**
  - `raAction`: triggers a C++ callback in the plugin.
  - `hmiAction target="<state>"`: navigates to a submenu page without plugin code.
  - `combinedAction`: combines both.
- **Models:** `textIdModel` / `raDataModel` (text labels and dynamic status strings), `raBoolModel` (boolean checkbox/radio state updated by the plugin).
- **Components** (rendered in declaration order; set `text=""` when driven by a model):
  - `checkbox`: `raAction` + `raBoolModel` + label model.
  - `radiobutton`: same; the plugin must manage mutual exclusion explicitly (the HMI does not uncheck sibling radio buttons automatically).
  - `button`: `hmiAction` (submenu) or `raAction` (action/selection) + label model.
- **Pages:** Each `hmiState` defines its title (`text` / `textModel`). Pressing BACK automatically returns to the parent state with no custom wiring.

## Plugin Code Rules

- Keep menu options in a structured table mapping persistent settings, action IDs, model IDs, and labels.
- During `onInit`, register `addHandler(action, handler)` for every `raAction`, invoke base initialization, and call `updateModel` for every dynamic model. Keep handler tables synchronized with enum/index ordering.
- Handlers execute directly on the HMI UI thread: **never perform blocking work** (network I/O, MOST transactions, long filesystem scans) inside a handler. Persist small settings atomically and offload active work to child processes or worker threads.

## Common Menu Recipes

- **Add a checkbox:** Define `raAction`, `raBoolModel`, label model, and `checkbox` in XML; add setting persistence, handler registration, and `updateModel` call in C++.
- **Add a submenu:** Define a new `hmiState` and link to it via a `button` with an `hmiAction` in the parent state.
- **Add a radio/choice group:** Define one `raAction` and control per choice; update the persisted setting and refresh all sibling models on selection.
- **Rename / move / remove:** Renaming modifies text only; moving reorders XML elements while keeping IDs unchanged; removing deletes the element/handler while retiring its numeric ID permanently.

## Pitfalls

- The HMI loads XML and resource definitions at startup: menu changes require an iDrive restart.
- Some NBT Evo configurations require Apps/Remote HMI coding (`A4A` / `A4A_REMOTE_APPS` / `REMOTE_HMI` / `TOUCH_COMMAND`) for third-party plugins to appear.
- Plain ASCII labels are the safest cross-locale default across iDrive language settings.
- A row that triggers no log output or state change when selected usually has an `raAction` ID without a matching `addHandler` registration.

## In This Repository

ExtendedHUD's menu lives in `native/plugin/ExtendedHUD.xml` (+ `ExtendedHUD.manifest`), `native/src/extended_hud.cpp` (`libExtendedHUD.so`), and `native/src/hud/hud_app.[ch]`. Validate with `python3 .claude/skills/extendedhud/check_app.py` and see skill `extendedhud` for build and deployment steps.
