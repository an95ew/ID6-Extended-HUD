#!/usr/bin/env python3
"""Checks that ExtendedHUD's halves agree: plugin/ExtendedHUD.xml (RHMI description), plugin/ExtendedHUD.manifest, src/extended_hud.cpp
(the shell: action/model/component constants) and src/hud/hud_app.[ch] (folder, port, worker command line). Nothing checks this at
build time; a mismatch shows in the car as a dead checkbox, a tick that never shows, or an app that never appears.

    python3 .claude/skills/extendedhud/check_app.py

Checks: ids unique across actions, models, states and components; every component's action/model exists with the right kind;
the C++ constants name ids that exist (kSportBarAction is an raAction, kSportBarModel an raBoolModel, kStatusModel the Richtext
model, kAlwaysComponent the "Always" checkbox); the manifest's name/file/DescFile say ExtendedHUD and its connector id is used by no
other manifest in native/plugin/ or the known ID6 Apps connector table; HUD_APP_DIR ends in /ExtendedHUD; the worker's UDP port is not
ShiftDeck's DRIVE_PORT. Exit status 1 on any problem."""
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / 'native'

# Connector IDs used by coexisting ID6 Apps plugins so ExtendedHUD never collides with them.
RESERVED_CONNECTORS = {
    '100': 'BoostDeck',
    '102': 'EngineCodes',
    '103': 'MhdMaps',
    '104': 'ShiftDeck',
    '107': 'HudTest',
    '111': 'Bootmod3Suite',
    '112': 'XDriveControl',
    '120': 'ExhaustFlap',
}


def main():
    problems = []
    app = ET.parse(NATIVE / 'plugin' / 'ExtendedHUD.xml').getroot().find('pluginApp')
    ids, actions, models, comps = {}, {}, {}, {}

    def note(kind, el):
        i = int(el.get('id'))
        if i in ids:
            problems.append('id %d used twice (%s and %s)' % (i, ids[i], kind))
        ids[i] = kind
        return i

    for el in app.find('actions'):
        actions[note(el.tag, el)] = el
    for el in app.find('models'):
        models[note(el.tag, el)] = el
    for st in app.find('hmiStates'):
        note('hmiState', st)
        for c in st.find('components'):
            comps[note(c.tag, c)] = c
            a, m = int(c.get('action', -1)), int(c.get('model', -1))
            if c.tag == 'checkbox':
                if a not in actions or actions[a].tag != 'raAction':
                    problems.append('checkbox %s: action %d is not an raAction' % (c.get('id'), a))
                if m not in models or models[m].tag != 'raBoolModel':
                    problems.append('checkbox %s: model %d is not an raBoolModel' % (c.get('id'), m))
            elif c.tag == 'label' and (m not in models or models[m].tag != 'raDataModel'):
                problems.append('label %s: model %d is not an raDataModel' % (c.get('id'), m))
    note('entryButton', app.find('entryButton'))

    cpp = (NATIVE / 'src' / 'extended_hud.cpp').read_text()
    const = {k: int(v) for k, v in re.findall(r'\b(k\w+) = (\d+)u', cpp)}
    want = {'kSportBarAction': ('a', 'raAction'), 'kAlwaysAction': ('a', 'raAction'), 'kNavAction': ('a', 'raAction'),
            'kSportBarModel': ('m', 'raBoolModel'), 'kAlwaysModel': ('m', 'raBoolModel'), 'kNavModel': ('m', 'raBoolModel'),
            'kStatusModel': ('m', 'raDataModel'), 'kAlwaysComponent': ('c', 'checkbox')}
    for name, (kind, tag) in want.items():
        if name not in const:
            problems.append('extended_hud.cpp: %s missing' % name)
            continue
        table = {'a': actions, 'm': models, 'c': comps}[kind]
        el = table.get(const[name])
        if el is None or el.tag != tag:
            problems.append('extended_hud.cpp: %s = %d is not a %s in the XML' % (name, const[name], tag))
    always = comps.get(const.get('kAlwaysComponent', -1))
    if always is not None and int(always.get('action')) != const.get('kAlwaysAction'):
        problems.append('kAlwaysComponent is not the checkbox of kAlwaysAction')
    if 'pluginAppInfo' not in cpp or '"ExtendedHUD"' not in cpp:
        problems.append('extended_hud.cpp: pluginAppInfo must name "ExtendedHUD"')

    man = (NATIVE / 'plugin' / 'ExtendedHUD.manifest').read_text()
    for frag in ('<name>ExtendedHUD</name>', '{pluginDir}/ExtendedHUD</file>', '{pluginDir}/ExtendedHUD.xml'):
        if frag not in man:
            problems.append('manifest: %s missing' % frag)
    ours = re.search(r'<id>(\d+)</id>', man).group(1)
    others = dict(RESERVED_CONNECTORS)
    for p in (NATIVE / 'plugin').glob('*.manifest'):
        if p.name != 'ExtendedHUD.manifest':
            for i in re.findall(r'<id>(\d+)</id>', p.read_text()):
                others[i] = p.name
    if ours in others:
        problems.append('connector %s also used by %s' % (ours, others[ours]))

    h = (NATIVE / 'src' / 'hud' / 'hud_app.h').read_text()
    c = (NATIVE / 'src' / 'hud' / 'hud_app.c').read_text()
    if not re.search(r'#define HUD_APP_DIR "[^"]*/Plugins/ExtendedHUD"', h):
        problems.append('hud_app.h: HUD_APP_DIR must end in /Plugins/ExtendedHUD')
    port = re.search(r'HUD_APP_UDP_PORT = (\d+)', h).group(1)
    drive = re.search(r'DRIVE_PORT = (\d+)', (NATIVE / 'src' / 'drive_frame.h').read_text()).group(1)
    if port == drive:
        problems.append('UDP port %s is ShiftDeck\'s DRIVE_PORT (the installed ShiftDeck plugin binds it)' % port)
    if '"--udp-port", "%s"' % port not in c:
        problems.append('hud_app.c passes a --udp-port other than HUD_APP_UDP_PORT %s' % port)

    print('ExtendedHUD: %d actions, %d models, %d components, connector %s, UDP %s' % (
        len(actions), len(models), len(comps), ours, port))
    print('ids in use: %s; free from %d up' % (', '.join(map(str, sorted(ids))), max(ids) + 1))
    if problems:
        print('\nPROBLEMS:\n  ' + '\n  '.join(problems))
        sys.exit(1)
    print('OK: XML, manifest, extended_hud.cpp and hud_app agree')


if __name__ == '__main__':
    main()
