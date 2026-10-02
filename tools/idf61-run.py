"""idf61-run.py -- run ESP-IDF v6.1 commands with a fully controlled environment.

Companion to idf553-run.py (which serves the MASTER firmware on IDF v5.5.3).
This one serves the SLAVE firmware (xiaozhi-esp32) on IDF v6.1.

Why a Python launcher instead of tools/idf-run.ps1:
  Two independent environment problems make the .ps1 wrapper unusable here.

  1. Execution policy. This machine reports `Restricted`, so `idf-run.ps1`'s internal
     `. 'tools/activate-idf.ps1'` is refused ("running scripts is disabled on this
     system"). idf-run.ps1 does not stop on that error (`$ErrorActionPreference =
     'Continue'`), so it prints its banner and then runs the command with a completely
     UNACTIVATED environment -- cmake then dies with
         "unable to find a build program corresponding to Ninja" / "Could NOT find Git".
     That is exactly the confusing failure this launcher avoids.

  2. Environment re-injection. Every child process in this session is re-injected with
     `MSYSTEM=MINGW64` and a fixed PATH. `tools/idf.py` refuses to run when MSYSTEM is
     set (its __main__ only prints a warning and never calls main()), and cmake cannot
     see ninja/git. Only Python's subprocess with an explicit `env` dict bypasses it.

Usage:
    # Recommended: go through the project's own build script (it sets -DBOARD_NAME
    # and -DSDKCONFIG_DEFAULTS). The board arg MUST carry the vendor prefix.
    python tools/idf61-run.py --cmd "python scripts/build.py waveshare/esp32-s3-touch-lcd-1.85-atzgauge"

    # Plain idf.py passthrough (fine once the board is already configured in sdkconfig)
    python tools/idf61-run.py build
    python tools/idf61-run.py -p COM3 flash
    python tools/idf61-run.py fullclean
"""

import glob
import os
import shlex
import subprocess
import sys

IDF_ROOT = r'D:\esp'
IDF_DIR = os.path.join(IDF_ROOT, 'esp-idf')
TOOLS_PATH = r'D:\esp\tools'
GIT_DIR = r'D:\esp\mingit'
PROJECT_DIR = r'D:\AtzGauge\xiaozhi-esp32\src'
PY_ENV = os.path.join(TOOLS_PATH, 'python_env', 'idf6.1_py3.13_env')
PY = os.path.join(PY_ENV, 'Scripts', 'python.exe')


def last(pattern):
    """Pick the NEWEST matching directory.

    IMPORTANT -- this is the opposite of idf553-run.py's `first()`.
    The shared IDF_TOOLS_PATH holds tools for BOTH v5.5.3 and v6.1:
        cmake           3.30.2 (v5.5.3)   /  4.0.3 (v6.1)
        xtensa-esp-elf  esp-14.2.0        /  esp-15.2.0
    ESP-IDF v6.1 was installed second, so it resolves to the NEWER set. Picking the
    older one here reproduces the master's configuration and the build fails.
    """
    hits = sorted(glob.glob(pattern))
    return hits[-1] if hits else None


def build_path():
    t = os.path.join(TOOLS_PATH, 'tools')
    entries = [
        os.path.join(IDF_DIR, 'components', 'espcoredump'),
        os.path.join(IDF_DIR, 'components', 'partition_table'),
        os.path.join(IDF_DIR, 'components', 'app_update'),
        last(os.path.join(t, 'xtensa-esp-elf-gdb', '*', 'xtensa-esp-elf-gdb', 'bin')),
        last(os.path.join(t, 'xtensa-esp-elf', '*', 'xtensa-esp-elf', 'bin')),
        last(os.path.join(t, 'riscv32-esp-elf', '*', 'riscv32-esp-elf', 'bin')),
        last(os.path.join(t, 'esp32ulp-elf', '*', 'esp32ulp-elf', 'bin')),
        last(os.path.join(t, 'cmake', '*', 'bin')),
        last(os.path.join(t, 'ninja', '*')),
        last(os.path.join(t, 'idf-exe', '*')),
        last(os.path.join(t, 'ccache', '*', 'ccache-*-windows-x86_64')),
        last(os.path.join(t, 'dfu-util', '*', 'dfu-util-*-win64')),
        os.path.join(PY_ENV, 'Scripts'),
        os.path.join(IDF_DIR, 'tools'),
        os.path.join(GIT_DIR, 'cmd'),
        os.path.join(GIT_DIR, 'mingw64', 'bin'),
    ]
    keep = [e for e in entries if e and os.path.isdir(e)]
    return ';'.join(keep) + ';' + os.environ.get('PATH', '')


def build_env():
    env = dict(os.environ)
    env.pop('MSYSTEM', None)
    env.pop('MSYS', None)
    env.pop('PYTHONPATH', None)

    env['PATH'] = build_path()
    env['IDF_PATH'] = IDF_DIR
    env['IDF_TOOLS_PATH'] = TOOLS_PATH
    env['IDF_PYTHON_ENV_PATH'] = PY_ENV
    # export.ps1 normally exports this. idf_component_manager reads it while building
    # the CLI and does Version.coerce(os.getenv('ESP_IDF_VERSION')) with no None guard,
    # so a missing value aborts with a bare
    #     TypeError: expected string or bytes-like object, got 'NoneType'
    # that says nothing about which variable is missing.
    env['ESP_IDF_VERSION'] = '6.1.0'
    env['PYTHONUTF8'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    env['IDF_GITHUB_ASSETS'] = 'dl.espressif.cn/github_assets'
    env['GIT_EXEC_PATH'] = os.path.join(GIT_DIR, 'mingw64', 'libexec', 'git-core')

    rom_elfs = last(os.path.join(TOOLS_PATH, 'tools', 'esp-rom-elfs', '*'))
    if rom_elfs:
        env['ESP_ROM_ELF_DIR'] = rom_elfs + os.sep
    openocd = last(os.path.join(TOOLS_PATH, 'tools', 'openocd-esp32', '*', 'openocd-esp32',
                                'share', 'openocd', 'scripts'))
    if openocd:
        env['OPENOCD_SCRIPTS'] = openocd
    return env


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2

    # Two entry modes:
    #   --cmd "<shell-ish command>"  -> run that command verbatim with the controlled env
    #   anything else                -> treat it as idf.py arguments
    # The --cmd form exists because the project's canonical build goes through
    # scripts/build.py (which injects -DBOARD_NAME / -DSDKCONFIG_DEFAULTS) rather
    # than calling idf.py directly.
    if args[0] == '--cmd':
        if len(args) < 2:
            print('ERROR: --cmd needs a command string', file=sys.stderr)
            return 2
        cmd = shlex.split(args[1], posix=False)
        cmd = [c.strip('"') for c in cmd]
        for extra in args[2:]:
            cmd.append(extra)
        # Rewrite a leading bare `python` to the IDF virtualenv interpreter.
        #
        # Why this is required, not cosmetic: scripts/build.py spawns idf.py with
        # `sys.executable`, i.e. whichever python is running build.py. If that is a
        # system python it has none of IDF's dependencies and idf.py dies with
        #     No module named 'rich_click'
        # ...which reads like a broken IDF install but is really just the wrong
        # interpreter. The IDF venv is the only python that can run idf.py.
        if cmd and os.path.basename(cmd[0]).lower() in ('python', 'python.exe', 'python3', 'python3.exe'):
            cmd[0] = PY
    else:
        idf_py = os.path.join(IDF_DIR, 'tools', 'idf.py')
        if not os.path.isfile(idf_py):
            print('ERROR: idf.py not found at %s' % idf_py, file=sys.stderr)
            return 1
        cmd = [PY, idf_py] + args

    if not os.path.isfile(PY):
        print('ERROR: python env not found at %s' % PY, file=sys.stderr)
        return 1

    env = build_env()
    path_entries = env['PATH'].split(';')
    print('== idf61-run ==')
    print('  idf     : %s' % IDF_DIR)
    print('  project : %s' % PROJECT_DIR)
    print('  MSYSTEM : %r  (must be None)' % env.get('MSYSTEM'))
    for name in ('ninja', 'git', 'cmake'):
        found = None
        for d in path_entries:
            cand = os.path.join(d, name + '.exe')
            if os.path.isfile(cand):
                found = cand
                break
        print('  %-6s  : %s' % (name, found or 'NOT FOUND'))
    print('  cmd     : %s' % ' '.join(cmd))
    print('-' * 70)
    sys.stdout.flush()

    proc = subprocess.run(cmd, env=env, cwd=PROJECT_DIR)
    print('-' * 70)
    print('exit code: %d' % proc.returncode)
    return proc.returncode


if __name__ == '__main__':
    sys.exit(main())
