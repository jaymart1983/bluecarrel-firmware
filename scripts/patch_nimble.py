"""
PlatformIO pre-build script: apply CrossPoint's NimBLE-Arduino patches via `git apply`.

The pinned NimBLE-Arduino initialises the ESP BLE controller with
BT_CONTROLLER_INIT_CONFIG_DEFAULT() and never touches its low-power fields, so
the controller inherits the prebuilt framework's "modem sleep off" and keeps the
radio biased for as long as the link is up. Those fields are runtime arguments to
esp_bt_controller_init(), so the fix is a patch at the call site rather than a
framework rebuild -- see scripts/nimble_patches/ for the per-fix rationale.

Each patch's idempotency is decided by git itself:
  * `git apply --check --reverse` succeeds  -> already applied, skip
  * `git apply --check`            succeeds  -> apply
  * neither succeeds                          -> abort the build

Patches live in `scripts/nimble_patches/` as one-commit-per-fix files, applied in
lexical order. Mirrors scripts/patch_jpegdec.py.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import subprocess
import sys


PATCH_DIR = os.path.join(env["PROJECT_DIR"], "scripts", "nimble_patches")  # noqa: F821


def patch_nimble(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    patches = _patch_files()
    for env_dir in os.listdir(libdeps_dir):
        nimble_dir = os.path.join(libdeps_dir, env_dir, "NimBLE-Arduino")
        if not os.path.isdir(os.path.join(nimble_dir, ".git")):
            continue
        for patch in patches:
            _apply_one(nimble_dir, patch)


def _patch_files():
    if not os.path.isdir(PATCH_DIR):
        raise RuntimeError(
            "NimBLE patches missing -- aborting build (expected directory %s)"
            % PATCH_DIR
        )
    patches = sorted(
        os.path.join(PATCH_DIR, name)
        for name in os.listdir(PATCH_DIR)
        if name.endswith(".patch")
    )
    if not patches:
        raise RuntimeError(
            "NimBLE patches missing -- aborting build (no .patch files in %s)"
            % PATCH_DIR
        )
    return patches


def _apply_one(nimble_dir, patch_path):
    name = os.path.basename(patch_path)
    if _git_apply_succeeds(nimble_dir, patch_path, reverse=True):
        return
    if not _git_apply_succeeds(nimble_dir, patch_path, reverse=False):
        # Not applied, not appliable -- the libdep source has diverged from
        # what the patch expects. Don't write a half-patched file.
        result = subprocess.run(
            ["git", "apply", "--check", patch_path],
            cwd=nimble_dir,
            capture_output=True,
            text=True,
        )
        sys.stderr.write(
            "ERROR: NimBLE patch %s does not apply cleanly:\n%s%s\n"
            % (name, result.stdout, result.stderr)
        )
        raise SystemExit(1)
    subprocess.run(["git", "apply", patch_path], cwd=nimble_dir, check=True)
    print("Applied NimBLE patch: %s" % name)


def _git_apply_succeeds(nimble_dir, patch_path, *, reverse):
    cmd = ["git", "apply", "--check"]
    if reverse:
        cmd.append("--reverse")
    cmd.append(patch_path)
    return subprocess.run(
        cmd, cwd=nimble_dir, capture_output=True, text=True
    ).returncode == 0


patch_nimble(env)  # noqa: F821
