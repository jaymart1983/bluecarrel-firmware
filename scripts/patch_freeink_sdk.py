"""
PlatformIO pre-build script: apply this fork's patches to the freeink-sdk submodule.

The submodule stays pinned to an upstream Free-Ink commit; fixes this fork needs
before they land upstream live in `scripts/freeink_patches/` as one patch per fix
(see each file's header), applied in lexical order.

Each patch's idempotency is decided by git itself:
  * `git apply --check --reverse` succeeds  -> already applied, skip
  * `git apply --check`            succeeds  -> apply
  * neither succeeds                          -> abort the build

`git apply` also works on a copy of the tree without a .git directory.
"""

import os
import subprocess
import sys


def patch_freeink_sdk(project_dir):
    sdk_dir = os.path.join(project_dir, 'freeink-sdk')
    patch_dir = os.path.join(project_dir, 'scripts', 'freeink_patches')
    if not os.path.isdir(os.path.join(sdk_dir, 'libs')):
        sys.exit('patch_freeink_sdk.py: freeink-sdk is missing; run `git submodule update --init --recursive`')
    patches = sorted(
        os.path.join(patch_dir, name)
        for name in (os.listdir(patch_dir) if os.path.isdir(patch_dir) else [])
        if name.endswith('.patch')
    )
    for patch in patches:
        _apply_one(sdk_dir, patch)


def _git_apply(sdk_dir, patch, *extra):
    return subprocess.run(['git', 'apply', *extra, patch], cwd=sdk_dir, capture_output=True, text=True)


def _apply_one(sdk_dir, patch):
    name = os.path.basename(patch)
    if _git_apply(sdk_dir, patch, '--check', '--reverse').returncode == 0:
        return
    check = _git_apply(sdk_dir, patch, '--check')
    if check.returncode != 0:
        # Not applied and not appliable: the submodule has diverged from what the
        # patch expects. Refuse rather than build a half-patched SDK.
        sys.stderr.write(f'ERROR: freeink-sdk patch {name} does not apply cleanly:\n{check.stdout}{check.stderr}\n')
        sys.exit(1)
    subprocess.run(['git', 'apply', patch], cwd=sdk_dir, check=True)
    print(f'Applied freeink-sdk patch: {name}')


# PlatformIO/SCons entry point -- Import and env are SCons builtins injected at
# runtime. Run directly with Python, it patches this checkout.
try:
    Import('env')                             # noqa: F821  # type: ignore[name-defined]
    patch_freeink_sdk(env['PROJECT_DIR'])     # noqa: F821  # type: ignore[name-defined]
except NameError:
    patch_freeink_sdk(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
