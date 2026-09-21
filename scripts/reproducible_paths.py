"""
PlatformIO pre-build script: make the firmware image independent of the directory
it was built in.

Source-file paths reach the image through __FILE__ (asserts, ESP_LOG, the Arduino
core's log_e/log_w). They differ between build hosts -- /home/runner/.platformio on
GitHub Actions, /piohome in the build container -- and a path of a different length
shifts every byte after it, so two builds of the same commit never compared equal.
-ffile-prefix-map rewrites those prefixes to fixed names at compile time; only the
embedded strings change, never the code.

GCC applies the last matching map, so the broad prefixes are listed first and the
packages directory, which sits inside the core directory, last.
"""

import os


def add_prefix_maps(env):
    mappings = []
    for var, name in (
        ('PROJECT_DIR', '/src'),
        ('PROJECT_CORE_DIR', '/pio'),
        ('PROJECT_PACKAGES_DIR', '/pio/packages'),
    ):
        path = env.subst(f'${var}')
        if path and os.path.isabs(path):
            mappings.append((os.path.realpath(path), name))
            # A symlinked path (e.g. a mounted volume) can reach the compiler either way.
            if os.path.realpath(path) != os.path.normpath(path):
                mappings.append((os.path.normpath(path), name))
    flags = [f'-ffile-prefix-map={path}={name}' for path, name in mappings]
    env.Append(CCFLAGS=flags, ASFLAGS=flags)
    print('Reproducible paths: ' + ', '.join(f'{path} -> {name}' for path, name in mappings))


# PlatformIO/SCons entry point -- Import and env are SCons builtins injected at runtime.
Import('env')                # noqa: F821  # type: ignore[name-defined]
add_prefix_maps(env)         # noqa: F821  # type: ignore[name-defined]
