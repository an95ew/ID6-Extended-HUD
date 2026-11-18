---
name: hmi-menu
description: RHMI rules for an iDrive plugin menu (one id space, raAction/raBoolModel/textIdModel, hmiState pages, handler registration). XML, text ids and handler code must agree. For ExtendedHUD use extendedhud (native/plugin/ExtendedHUD.xml, native/src/extended_hud.cpp).
---

# iDrive plugin menu

The XML, the text ids and the handler code are three pieces that must agree. Nothing checks that at build time. A mismatch shows in the car as a dead
row, a checkbox that never ticks, or an empty label. A checker that parses all three can print the id tree, the ids in use and the first free id.

## XML rules
- Entry: `pluginApp applicationType="Custom" insertStateID="20" insertAfterFocusID="8195"` puts the row in the Media/Radio list
  (`ST_Ent_Selection`, state 20) after "Now playing in Apple CarPlay". `entryButton` carries a `combinedAction` = `raAction` + `hmiAction`
  that opens the plugin's first hmiState.
- **One id space** for actions, models, hmiStates, components and the entry button. Never reuse or renumber: ids also sit in code.
- Actions: `raAction` (callback into the plugin), `hmiAction target="<state>"` (opens a page, no code), `combinedAction`.
- Models: `textIdModel textId="n"` (label), `raBoolModel` (tick set by the plugin).
- Components (in display order, `text=""`, label from the model):
  - `checkbox`: raAction + raBoolModel + textIdModel;
  - `radiobutton`: same; the plugin ticks one and clears the rest itself (the HMI doesn't do it for a plugin);
  - `button`: hmiAction (submenu) or raAction (command) + textIdModel.
- hmiState has `text` + `textModel` for its title. BACK returns to the parent with no wiring.

## Code rules
- A table of options `{switch file, action, model, label, on}` with an enum index. Switch file in
  `/var/opt/hmi/data/rw/carplaymenu` (present = on; survives updates and restarts).
- The `onInit` slot is replaced: it registers `addHandler(action, stub)` per option, radio value and command, calls the base `onInit`,
  then `updateModel` for every model. **The stub array must follow the enum order.**
- A stub flips the file and the model; in safe mode it only saves; otherwise it applies at once or logs "applies after restart".
- Things needing a start (patches, hooks, wrappers) run in an `applyAtStart()` behind a build check; unknown build = menu only.
- Anything needing a boot-time link also needs the boot script to read its switch file.
- Handlers run on the HMI thread: no blocking work (network, MOST, long I/O go to threads).

## Recipes
- **Checkbox:** raAction + raBoolModel + textIdModel + `checkbox` in the page; text key; enum entry, table row, stub at the same index.
- **Submenu:** new hmiState + `button` in the parent with an hmiAction; title text; no code.
- **Radio group:** one radiobutton per value; table of values; a setter that writes the value file and `updateModel`s the whole group.
- **Command button:** `button` + raAction + handler constant.
- **Rename:** text only. **Move:** move the element, ids stay. **Remove:** drop element, action, models, text, enum, row, stub; don't reuse the switch file name.

## Pitfalls
- The HMI reads XML and texts at start: menu changes need an HMI restart.
- On iDrive 6 the HMI loads such a plugin only with A4A / A4A_REMOTE_APPS / REMOTE_HMI / TOUCH_COMMAND coded.
- ASCII text is safest; every language gets the English text.
- A row that logs nothing when ticked has an action without a handler.

## In this repo
`native/plugin/ExtendedHUD.xml` (+ `.manifest`), `native/src/extended_hud.cpp` (-> `libExtendedHUD.so`), settings and
status in `native/src/hud/hud_app.[ch]`. Same RHMI rules apply; see skill `extendedhud` for how it is built and shipped.
