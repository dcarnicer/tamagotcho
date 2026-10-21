# PlatformIO pre-script: the firmware version string, TGC_VERSION.
#
#   1. the TGC_VERSION environment variable, if set (the release workflow sets
#      it to the release tag, e.g. "v0.1.0");
#   2. else `git describe --tags --always --dirty --match "v[0-9]*"` (local and
#      CI builds: "v0.1.0-3-gabc1234-dirty", or a bare commit id when no
#      release tag is reachable, as in CI's shallow checkout). The --match keeps
#      the decks release tags (`decks-v2`) out of the firmware version;
#   3. else "dev" (no git, e.g. a source archive).
#
# The define goes to src/Version.cpp only (a build middleware), so a new commit
# recompiles that one file instead of the framework and every library; the rest
# of the firmware asks tgc::firmwareVersion(). Version.cpp falls back to "dev"
# when the define is missing, and the native test env never builds it.
import os
import re
import subprocess

Import("env")  # noqa: F821 -- provided by SCons

_MAX_LEN = 40  # the About screen and the portal show it as is


def _clean(raw):
    # Only characters that are safe in a C string and on every screen.
    return re.sub(r"[^0-9A-Za-z._+~-]", "_", (raw or "").strip())[:_MAX_LEN]


def _git_describe(project_dir):
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty", "--match", "v[0-9]*"],
            cwd=project_dir,
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    return out.stdout


def _version():
    from_env = _clean(os.environ.get("TGC_VERSION"))
    if from_env:
        return from_env, "TGC_VERSION"
    described = _clean(_git_describe(env.subst("$PROJECT_DIR")))
    if described:
        return described, "git describe"
    return "dev", "fallback"


VERSION, SOURCE = _version()
print("Tamagotcho version: %s (%s)" % (VERSION, SOURCE))

_VERSION_SRC = os.path.normcase(os.path.join(env.subst("$PROJECT_SRC_DIR"), "Version.cpp"))


def _with_version(env, node):
    if os.path.normcase(node.srcnode().get_abspath()) != _VERSION_SRC:
        return node
    venv = env.Clone()
    venv.Append(CPPDEFINES=[("TGC_VERSION", venv.StringifyMacro(VERSION))])
    return venv.Object(node)


env.AddBuildMiddleware(_with_version, "*Version.cpp")
