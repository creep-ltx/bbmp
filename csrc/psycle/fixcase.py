#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Windows sources include files by names in another case than the files have.
For every #include in the given trees that names no file but would on a
case-blind file system, make a symlink with the name as it is written."""
import os, re, sys
roots = [os.path.abspath(r) for r in sys.argv[1:]]
inc = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
made = 0
def find_nocase(base, rel):
    cur = base
    parts = rel.replace('\\', '/').split('/')
    real = []
    for p in parts:
        if p in ('.', ''): continue
        if p == '..':
            cur = os.path.dirname(cur); real.append('..'); continue
        try: names = os.listdir(cur)
        except OSError: return None
        hit = p if p in names else next((n for n in names if n.lower() == p.lower()), None)
        if hit is None: return None
        real.append(hit); cur = os.path.join(cur, hit)
    return cur
for root in roots:
    for d, _, files in os.walk(root):
        for f in files:
            if not f.lower().endswith(('.cpp', '.hpp', '.h', '.c', '.cc')): continue
            path = os.path.join(d, f)
            if os.path.islink(path): continue
            try: text = open(path, 'rb').read()
            except OSError: continue
            for m in inc.finditer(text):
                name = m.group(1).decode('latin1').replace('\\', '/')
                for base in [d] + roots:
                    want = os.path.normpath(os.path.join(base, name))
                    if os.path.exists(want): break
                    real = find_nocase(base, name)
                    if real and os.path.isfile(real) and os.path.isdir(os.path.dirname(want)):
                        os.symlink(os.path.relpath(real, os.path.dirname(want)), want)
                        made += 1
                        break
print(f"{made} links for includes in another case", file=sys.stderr)
