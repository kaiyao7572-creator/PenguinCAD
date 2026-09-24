#!/usr/bin/env python3
"""Fails if any key is bound to two commands.

Qt fires NEITHER action on an ambiguous shortcut, so a collision does not
pick a winner -- it silently disables both. F once meant Fillet and Fit
All and did nothing at all.

Reads the sources rather than the running registry so it needs no window.
Each command's Shortcut() is looked for only between its own Id() and the
next one, so a command without a shortcut never borrows its neighbour's.
"""
import collections
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src')
ID = re.compile(r'std::string Id\(\) const override\s*\{\s*return "([^"]+)";')
SHORTCUT = re.compile(r'std::string Shortcut\(\) const override\s*\{\s*return "([^"]*)";')

bound = collections.defaultdict(list)
commands = 0
for root, _, files in os.walk(ROOT):
    for name in files:
        if not name.endswith('.cpp'):
            continue
        text = open(os.path.join(root, name), errors='ignore').read()
        ids = list(ID.finditer(text))
        for i, m in enumerate(ids):
            end = ids[i + 1].start() if i + 1 < len(ids) else len(text)
            commands += 1
            s = SHORTCUT.search(text, m.end(), end)
            if s and s.group(1):
                bound[s.group(1).lower()].append(m.group(1))

clashes = {k: v for k, v in bound.items() if len(v) > 1}
print(f"  {commands} commands, {len(bound)} shortcuts")
if commands < 20:
    print("  FAIL  found too few commands -- the scanner no longer matches the source")
    sys.exit(1)
for key, ids in sorted(clashes.items()):
    print(f"  FAIL  {key!r} is bound to {', '.join(sorted(ids))}")
if clashes:
    sys.exit(1)
print("  PASS  no key is bound to two commands")
