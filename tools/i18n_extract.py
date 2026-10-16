#!/usr/bin/env python3
"""List the English text Kasumi can show, for translation.

Scans the sources for C string literals (adjacent literals joined, as the
compiler does) and keeps the ones that read like words for people, skipping
log lines, JSON keys, URLs and paths. Writes i18n/strings.txt: one entry per
line, C escapes kept (\\n, \\", \\\\). The translation files i18n/<code>.tsv
pair each entry with its translation; tools/i18n_gen.py builds the table.

    python tools/i18n_extract.py
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..")
# Files whose strings reach the screen (menus, notices, status lines).
FILES = [
    "screens.c", "main.c", "ui.c", "gfn_client.c", "xcloud.c", "steam_link.c", "steam_remote.c",
    "steam_session.c", "updater.c", "webrtc_transport.c", "nvst_signal.c", "remote_keyboard.c",
    "shortcut.c", "shortcut_cia.c", "gfn_input.c", "gallery.c", "net_worker.c", "report.c",
    "audio_output.c", "mic_capture.c", "game_prefs.c", "screenshot.c", "providers.c", "provider.c",
]
# Lines that never show text to the player.
SKIP_LINE = re.compile(
    r"diagnostic_(log|flag|checkpoint)|#include|#define\s+\w+_PATH|fopen|json_(object|pack|string|integer|"
    r"is_|array|load|dump|true|false)|strcmp|strncmp|strstr|strcasecmp|http_request|\"Content-|curl_|"
    r"printf\(\s*\"\[|svcOutputDebugString|APP_DATA_DIR|sdmc:|https?://|\.json|\.txt|\.png|\.bin")
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def literals(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    lines = text.split("\n")
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if SKIP_LINE.search(line):
            i += 1
            continue
        # Join "a" "b" across lines: a literal followed only by another literal.
        pos = 0
        while True:
            m = LITERAL.search(line, pos)
            if not m:
                break
            parts = [m.group(1)]
            end = m.end()
            j = i
            cur = line
            while True:
                rest = cur[end:]
                n = re.match(r'\s*"((?:[^"\\\n]|\\.)*)"', rest)
                if n:
                    parts.append(n.group(1))
                    end += n.end()
                    continue
                if rest.strip() == "" and j + 1 < len(lines) and re.match(r'\s*"', lines[j + 1]) \
                        and not SKIP_LINE.search(lines[j + 1]):
                    j += 1
                    cur = lines[j]
                    end = 0
                    continue
                break
            out.append(("".join(parts), i + 1))
            if j != i:
                i = j
                line = cur
            pos = end
        i += 1
    return out


# Technical text: protocol names, error codes for logs, button and country
# codes, proper names. Shown rarely or never, and the same in every language.
TECHNICAL = re.compile(
    r"NVST|SDP|ICE|curl|HTTP/1|WebSocket|JSON|allocation|alloc|MVD|DSP|Opus|Mozilla|x-nv|peer-|"
    r"48k|^PK$|\.part$|^-W|game_count|SystemInfo|KASUMI3DS|Kasumi 3DS|Kasumi New 3DS|^Nintendo$|"
    r"^Chrome$|^Connect$|Steam In-Home|258EAFA5|sign_in\?|^not started$|malformed|^ABXY$|"
    r"\{\\\"|^%s |TLS connect|Frame recv|Upgrade recv|EOF|counters|challenge|random|seed|"
    r"limit/allocation|URL length|Invalid signaling|Invalid secure|Invalid session ID")


def wanted(s):
    if not re.search(r"[A-Za-z]{2,}", s):
        return False
    if TECHNICAL.search(s):
        return False
    if re.fullmatch(r"[A-Z]{2,3}", s):                # country and button codes
        return False
    if s in ("Seiji", "Sakura", "Kin", "Ai", "Fuji", "Beni", "Matcha", "Kaki", "Sumi", "Shiro"):
        return False
    if re.fullmatch(r"[a-z0-9_./:-]+", s):          # keys, ids, paths
        return False
    if re.fullmatch(r"%[-0-9.]*[a-z]+", s):
        return False
    if re.search(r"\\x[0-9a-fA-F]{2}", s):
        return False
    words = re.findall(r"[A-Za-z]{2,}", s)
    # One lower-case word with no space is usually an identifier.
    if len(words) == 1 and " " not in s and not s[:1].isupper() and not s.isupper():
        return False
    return True


def main():
    seen = {}
    for name in FILES:
        path = os.path.join(ROOT, "source", name)
        if not os.path.exists(path):
            continue
        for s, line in literals(path):
            if wanted(s) and s not in seen:
                seen[s] = f"{name}:{line}"
    os.makedirs(os.path.join(ROOT, "i18n"), exist_ok=True)
    with open(os.path.join(ROOT, "i18n", "strings.txt"), "w", encoding="utf-8", newline="\n") as f:
        for s in seen:
            f.write(s + "\n")
    with open(os.path.join(ROOT, "i18n", "strings_where.txt"), "w", encoding="utf-8", newline="\n") as f:
        for s, where in seen.items():
            f.write(f"{where}\t{s}\n")
    print(len(seen), "strings")


if __name__ == "__main__":
    sys.exit(main())
