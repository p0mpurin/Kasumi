#!/usr/bin/env python3
"""Wrap the text pieces of snprintf calls in i18n_text().

Fixed text is translated where it is drawn (ui.c), but a message put
together with snprintf ("Streaming from %s") no longer matches its English
entry once the values are in. This wraps every literal inside an snprintf
call that is in i18n/strings.txt -- the format and literal arguments -- so
they are translated before they are combined. Safe to run again: wrapped
literals are left alone.

    python tools/i18n_wrap.py [--dry-run]
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..")
# Shared code, then each console's own (platform.h's backends).
SOURCE_DIRS = ["source", os.path.join("source", "3ds"), os.path.join("source", "vita")]


def source_path(name):
    for folder in SOURCE_DIRS:
        path = os.path.join(ROOT, folder, name)
        if os.path.exists(path):
            return path
    return None
# Per file, the snprintf destinations that end up on screen (a regex on the
# first argument). Everything else (headers, URLs, SDP, JSON) stays as is.
DESTINATIONS = {
    "screens.c": r".", "main.c": r".", "shortcut.c": r"g_result",
    "steam_link.c": r"status|^text$|^title$|^why$", "xcloud.c": r"status|^text$|^why$",
    "gfn_client.c": r"->status$", "webrtc_transport.c": r"t->status|^failure$", "nvst_signal.c": r"s->status",
    "net_worker.c": r"status", "updater.c": r"g_info\.error|^why$", "report.c": r"^message$",
    "audio_output.c": r"g_status",
}
FILES = list(DESTINATIONS)
SKIP_CONTEXT = re.compile(r"diagnostic_(log|flag)|fail_code|end_error_code|\bpath\b|_PATH|url|json|header")


def find_calls(src, name):
    """(start, end) of each `name(...)` call, end just past the ')'."""
    i = 0
    while True:
        m = re.compile(r"\b" + name + r"\s*\(").search(src, i)
        if not m:
            return
        depth = 0
        j = m.end() - 1
        while j < len(src):
            c = src[j]
            if c == '"':
                j += 1
                while src[j] != '"':
                    j += 2 if src[j] == "\\" else 1
            elif c == "'":
                j += 1
                while src[j] != "'":
                    j += 2 if src[j] == "\\" else 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    yield m.start(), j + 1
                    break
            j += 1
        i = j + 1


LIT_GROUP = re.compile(r'"(?:[^"\\\n]|\\.)*"(?:\s*"(?:[^"\\\n]|\\.)*")*')


def content(group):
    return "".join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', group))


def main():
    dry = "--dry-run" in sys.argv
    catalog = set(l.rstrip("\n") for l in open(os.path.join(ROOT, "i18n", "strings.txt"), encoding="utf-8"))
    total = 0
    for name in FILES:
        path = source_path(name)
        src = open(path, encoding="utf-8", newline="").read()
        edits = []
        for start, end in find_calls(src, "snprintf"):
            call = src[start:end]
            # The destination (first argument) says where the text goes.
            dest = call[call.index("(") + 1:call.index(",")] if "," in call else ""
            if not re.search(DESTINATIONS[name], dest.strip()) or SKIP_CONTEXT.search(dest) or                     "diagnostic_" in src[max(0, start - 40):start]:
                continue
            for m in LIT_GROUP.finditer(call):
                text = content(m.group(0))
                if text not in catalog:
                    continue
                before = call[:m.start()].rstrip()
                if before.endswith("i18n_text("):
                    continue
                edits.append((start + m.start(), start + m.end()))
        if not edits:
            continue
        total += len(edits)
        print(f"{name}: {len(edits)} literal(s)")
        for a, b in sorted(edits, reverse=True):
            src = src[:a] + "i18n_text(" + src[a:b] + ")" + src[b:]
        if '#include "i18n.h"' not in src:
            # After the file's own header include (the first #include line).
            first = re.search(r'#include "[^"]+"\r?\n', src)
            nl = "\r\n" if "\r\n" in src else "\n"
            src = src[:first.end()] + '#include "i18n.h"' + nl + src[first.end():]
        if not dry:
            open(path, "w", encoding="utf-8", newline="").write(src)
    print("total", total)


if __name__ == "__main__":
    main()
