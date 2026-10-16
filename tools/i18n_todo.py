#!/usr/bin/env python3
"""Print the next untranslated catalog entries: python tools/i18n_todo.py [count]."""
import os, sys
ROOT = os.path.join(os.path.dirname(__file__), "..")
done = set()
for f in os.listdir(os.path.join(ROOT, "i18n")):
    if f.endswith(".tsv"):
        for line in open(os.path.join(ROOT, "i18n", f), encoding="utf-8"):
            if line.strip() and not line.startswith("#"):
                done.add(line.split("\t", 1)[0])
todo = [l.rstrip("\n") for l in open(os.path.join(ROOT, "i18n", "strings.txt"), encoding="utf-8") if l.strip() and l.rstrip("\n") not in done]
n = int(sys.argv[1]) if len(sys.argv) > 1 else 120
print(f"{len(todo)} left")
for s in todo[:n]:
    print(s)
