"""Every shipped command has a working --help (issue #131).

The shipped set is read from packaging/build-apkg.sh, from the lines that copy
a binary into "${STAGE_BIN}/", so this test follows packaging: a command that
starts shipping is checked from then on.

A working --help (and -h) exits 0, prints a "usage: <command>" line on stdout
and nothing on stderr, and leaves --version as apkg/start.sh's self-check
expects it: exit 0, stdout starting with "<command> ".

Two ways to run it:
  python3 tests/test_cli_help.py BIN_DIR
      checks every shipped command in BIN_DIR (for example the bin/ folder of
      a built package); exit 1 if any is missing or fails.
  python -m pytest tests/test_cli_help.py
      with WDF_BIN_DIR set, the same strict check; without it, builds
      wdf-worker (g++) and wdf-service (cargo) from source when those tools
      exist and skips the commands it cannot build offline.
"""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD_APKG = ROOT / "packaging" / "build-apkg.sh"

# cp "<anything>/<name>" "${STAGE_BIN}/"
_STAGE_LINE = re.compile(r'^\s*cp\s+"[^"]*/([^"/]+)"\s+"\$\{STAGE_BIN\}/"\s*$')


def shipped_commands(script=BUILD_APKG):
    """Names of the binaries build-apkg.sh stages into the package's bin/."""
    names = []
    for line in Path(script).read_text(encoding="utf-8").splitlines():
        m = _STAGE_LINE.match(line)
        if m:
            names.append(m.group(1))
    return names


def help_problems(binary, name):
    """Problems with one command's --help, -h and --version; empty if none."""
    problems = []

    def run(*args):
        try:
            return subprocess.run(
                [str(binary), *args], capture_output=True, text=True, timeout=30
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            problems.append(f"{name} {' '.join(args)}: did not run ({exc})")
            return None

    for flag in ("--help", "-h"):
        r = run(flag)
        if r is None:
            continue
        if r.returncode != 0:
            problems.append(f"{name} {flag}: exit {r.returncode}, expected 0")
        if not re.search(rf"^usage: {re.escape(name)}\b", r.stdout, re.M):
            problems.append(f"{name} {flag}: no 'usage: {name}' line on stdout")
        if r.stderr:
            problems.append(f"{name} {flag}: wrote to stderr: {r.stderr.strip()[:200]}")
    r = run("--version")
    if r is not None and (r.returncode != 0 or not r.stdout.startswith(name + " ")):
        problems.append(
            f"{name} --version: exit {r.returncode}, stdout {r.stdout.strip()[:80]!r}"
            f" (start.sh self-check expects exit 0 and '{name} ...')"
        )
    return problems


def check_bin_dir(bin_dir, names=None):
    """Problems across every shipped command in bin_dir."""
    names = shipped_commands() if names is None else names
    if not names:
        return [f"no staged commands found in {BUILD_APKG}"]
    problems = []
    for name in names:
        binary = Path(bin_dir) / name
        if not (binary.is_file() and os.access(binary, os.X_OK)):
            problems.append(f"{name}: not an executable file in {bin_dir}")
            continue
        problems.extend(help_problems(binary, name))
    return problems


# --- pytest -----------------------------------------------------------------


def _build_worker(out_dir):
    cxx = shutil.which("g++")
    if not cxx:
        return None
    out = Path(out_dir) / "wdf-worker"
    subprocess.run(
        [
            cxx,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            str(ROOT / "worker/src/main.cpp"),
            "-o",
            str(out),
        ],
        check=True,
        timeout=300,
    )
    return out


def _build_service(out_dir):
    cargo = shutil.which("cargo")
    if not cargo:
        return None
    # Build a copy, so cargo's Cargo.lock lands in tmp, not in the checkout.
    src = Path(out_dir) / "service"
    shutil.copytree(ROOT / "service", src, ignore=shutil.ignore_patterns("target"))
    subprocess.run(
        [
            cargo,
            "build",
            "--quiet",
            "--offline",
            "--manifest-path",
            str(src / "Cargo.toml"),
            "--target-dir",
            str(src / "target"),
        ],
        check=True,
        timeout=600,
    )
    return src / "target" / "debug" / "wdf-service"


# Commands this test can build from source without network or ncnn. The
# others (wdf_demo, wdf_faces) need ncnn and are checked through WDF_BIN_DIR
# or the BIN_DIR mode; wdf_faces' real main also runs --help in
# indexer/tests/test_faces_cli.py.
SOURCE_BUILDS = {"wdf-worker": _build_worker, "wdf-service": _build_service}
NEEDS_NCNN = {"wdf_demo", "wdf_faces"}


def test_shipped_list_is_read_from_packaging():
    names = shipped_commands()
    assert names, 'no cp ... "${STAGE_BIN}/" lines found in build-apkg.sh'
    assert len(names) == len(set(names)), names
    # A newly shipped command must be classified here, so its --help is
    # checked by one of the paths above rather than silently not at all.
    unknown = set(names) - set(SOURCE_BUILDS) - NEEDS_NCNN
    assert not unknown, f"shipped but not covered by this test: {sorted(unknown)}"


def test_shipped_list_parser(tmp_path):
    script = tmp_path / "build.sh"
    script.write_text(
        'cp "${A}/build/x/alpha" "${STAGE_BIN}/"\n'
        '  cp "${B}/beta-cmd" "${STAGE_BIN}/"\n'
        'cp "${A}/notes.txt" "${DOCS}/"\n'
        '# cp "${A}/gamma" "${STAGE_BIN}/" is not staged\n',
        encoding="utf-8",
    )
    assert shipped_commands(script) == ["alpha", "beta-cmd"]


def _fake(tmp_path, name, body):
    path = tmp_path / name
    path.write_text("#!/bin/sh\n" + body, encoding="utf-8")
    path.chmod(0o755)
    return path


def test_checker_accepts_a_good_command(tmp_path):
    _fake(
        tmp_path,
        "good",
        'case "$1" in\n'
        '  --version) echo "good 1.0";;\n'
        '  --help|-h) printf "usage: good [--help]\\n\\nDoes things.\\n";;\n'
        "  *) exit 2;;\nesac\n",
    )
    assert check_bin_dir(tmp_path, ["good"]) == []


def test_checker_rejects_broken_commands(tmp_path):
    _fake(
        tmp_path,
        "nohelp",
        'case "$1" in --version) echo "nohelp 1.0";;'
        ' *) echo "nohelp 1.0 (banner)";; esac\n',
    )
    _fake(
        tmp_path,
        "exit2",
        'case "$1" in --version) echo "exit2 1.0";;'
        ' *) echo "usage: exit2 x"; exit 2;; esac\n',
    )
    _fake(
        tmp_path,
        "stderr",
        'case "$1" in --version) echo "stderr 1.0";;'
        ' *) echo "usage: stderr x" >&2;; esac\n',
    )
    _fake(
        tmp_path,
        "badver",
        'case "$1" in --version) exit 1;; *) echo "usage: badver x";; esac\n',
    )
    for name in ("nohelp", "exit2", "stderr", "badver", "missing"):
        assert check_bin_dir(tmp_path, [name]), name


def test_shipped_commands_in_wdf_bin_dir():
    bin_dir = os.environ.get("WDF_BIN_DIR")
    if not bin_dir:
        import pytest

        pytest.skip("WDF_BIN_DIR not set (point it at the built bin/ folder)")
    assert check_bin_dir(bin_dir) == []


def test_source_built_commands(tmp_path):
    import pytest

    checked = []
    for name in shipped_commands():
        build = SOURCE_BUILDS.get(name)
        if build is None:
            continue
        binary = build(tmp_path)
        if binary is None:
            continue
        assert help_problems(binary, name) == [], name
        checked.append(name)
    if not checked:
        pytest.skip("neither g++ nor cargo found")


def main(argv):
    if len(argv) != 2:
        print("usage: test_cli_help.py BIN_DIR", file=sys.stderr)
        return 2
    names = shipped_commands()
    problems = check_bin_dir(argv[1], names)
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    if problems:
        return 1
    print(f"--help ok for {len(names)} shipped command(s): {', '.join(names)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
