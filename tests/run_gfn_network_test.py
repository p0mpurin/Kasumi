"""Run the actual network parser with fixture responses, without console SDKs.

Uses cc on POSIX or an installed Visual Studio C compiler on Windows. Jansson
is built in a temporary directory; the vendored Vita configuration is untouched.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run():
    with tempfile.TemporaryDirectory(prefix="kasumi-network-") as temp:
        work = Path(temp)
        for source in (ROOT / "vendor/jansson/src").iterdir():
            if source.suffix in (".h", ".c"):
                shutil.copy2(source, work / source.name)
        (work / "jansson_config.h").write_text(
            "#define JSON_INLINE inline\n#define JSON_INTEGER_IS_LONG_LONG 1\n"
            "#define JSON_HAVE_LOCALECONV 0\n#define JSON_HAVE_ATOMIC_BUILTINS 0\n"
            "#define JSON_HAVE_SYNC_BUILTINS 0\n#define JSON_PARSER_MAX_DEPTH 2048\n")
        (work / "jansson_private_config.h").write_text(
            "#define HAVE_STDINT_H 1\n#define HAVE_INTTYPES_H 1\n"
            "#define HAVE_SYS_TYPES_H 1\n#define HAVE_STRTOLL 1\n"
            "#define INITIAL_HASHTABLE_ORDER 3\n")
        production = (ROOT / "source/gfn_client.c").read_text(encoding="utf-8")
        functions = []
        for name in ("copy_json_string_if_present", "connection_ip", "connection_port",
                     "parse_session_network"):
            match = re.search(r"^static [^\n]*\b" + name + r"\(.*?^}\n",
                              production, re.MULTILINE | re.DOTALL)
            if not match:
                raise RuntimeError(f"Cannot extract production function {name}")
            functions.append(match.group())
        (work / "gfn_network_under_test.inc").write_text("\n".join(functions))
        signaling = (ROOT / "source/nvst_signal.c").read_text(encoding="utf-8")
        incoming = re.search(r"^static bool incoming\(.*?^}\n", signaling,
                             re.MULTILINE | re.DOTALL)
        if not incoming:
            raise RuntimeError("Cannot extract production signaling handler")
        (work / "nvst_incoming_under_test.inc").write_text(incoming.group())
        shutil.copy2(ROOT / "tests/gfn_network_test.c", work / "test.c")
        sources = [str(p.name) for p in work.glob("*.c")]
        if os.name == "nt":
            vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / \
                "Microsoft Visual Studio/Installer/vswhere.exe"
            install = subprocess.check_output(
                [str(vswhere), "-latest", "-products", "*", "-property", "installationPath"],
                text=True).strip()
            vcvars = Path(install) / "VC/Auxiliary/Build/vcvars64.bat"
            # Paths are quoted; generated arguments contain no user-supplied shell text.
            script = f'@call "{vcvars}" >nul\n@cl /nologo /std:c11 /D_CRT_SECURE_NO_WARNINGS /DHAVE_CONFIG_H /I. /I"{ROOT / "include"}" ' + \
                " ".join(sources) + " /Fetest.exe\n@if errorlevel 1 exit /b 1\n@test.exe\n"
            (work / "run.cmd").write_text(script)
            subprocess.run(["cmd.exe", "/d", "/c", str(work / "run.cmd")], cwd=work, check=True)
        else:
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-DHAVE_CONFIG_H",
                            "-I.", f"-I{ROOT / 'include'}", *sources, "-lm", "-o", "test"],
                           cwd=work, check=True)
            subprocess.run([str(work / "test")], cwd=work, check=True)


if __name__ == "__main__":
    run()
