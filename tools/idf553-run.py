"""idf553-run.py -- run ESP-IDF v5.5.3 commands with a fully controlled environment.

Why this exists (and why a plain .ps1/.bat wrapper cannot work here):
  In this session every process spawned by the shell is re-injected with `MSYSTEM=MINGW64`
  and a fixed PATH, regardless of what the parent shell does (`unset MSYSTEM` does not stick,
  and PATH edits never reach children). That breaks ESP-IDF twice:

    1. `tools/idf.py` refuses to do anything when MSYSTEM is set -- its __main__ is
           if 'MSYSTEM' in os.environ: print_warning(...)   # main() is NOT called
       so the build silently exits 0 with just a warning.
    2. `cmake` (spawned by idf.py) cannot see ninja/git/xtensa-esp-elf, so configure dies with
       "unable to find a build program corresponding to Ninja" / "Could NOT find Git".

  A child process created through Python's subprocess with an explicit `env` dict does get
  exactly that environment (verified: MSYSTEM gone, custom PATH visible), so this launcher
  builds the environment by hand and spawns idf.py with it.

Usage:
    python tools/idf553-run.py build
    python tools/idf553-run.py -p COM3 app-flash
    python tools/idf553-run.py fullclean
"""

import glob
import os
import subprocess
import sys

IDF_ROOT = r'D:\esp553'
IDF_DIR = os.path.join(IDF_ROOT, 'esp-idf')
TOOLS_PATH = r'D:\esp\tools'
GIT_DIR = r'D:\esp\mingit'
PROJECT_DIR = r'D:\AtzGauge\obd_brz_gauge\repo'
PY_ENV = os.path.join(TOOLS_PATH, 'python_env', 'idf5.5_py3.13_env')
PY = os.path.join(PY_ENV, 'Scripts', 'python.exe')


def first(pattern):
    """Pick the OLDEST matching directory.

    The shared IDF_TOOLS_PATH holds tools for both v5.5.3 and v6.1 (e.g. cmake 3.30.2 and
    4.0.3, xtensa-esp-elf esp-14.2.0 and esp-15.2.0). ESP-IDF v5.5.3 was installed first and
    activate.py resolves to the older set, so the lowest version is the correct one here.
    """
    hits = sorted(glob.glob(pattern))
    return hits[0] if hits else None


def build_path():
    t = os.path.join(TOOLS_PATH, 'tools')
    entries = [
        os.path.join(IDF_DIR, 'components', 'espcoredump'),
        os.path.join(IDF_DIR, 'components', 'partition_table'),
        os.path.join(IDF_DIR, 'components', 'app_update'),
        first(os.path.join(t, 'xtensa-esp-elf-gdb', '*', 'xtensa-esp-elf-gdb', 'bin')),
        first(os.path.join(t, 'xtensa-esp-elf', '*', 'xtensa-esp-elf', 'bin')),
        first(os.path.join(t, 'riscv32-esp-elf', '*', 'riscv32-esp-elf', 'bin')),
        first(os.path.join(t, 'esp32ulp-elf', '*', 'esp32ulp-elf', 'bin')),
        first(os.path.join(t, 'cmake', '*', 'bin')),
        first(os.path.join(t, 'ninja', '*')),
        first(os.path.join(t, 'idf-exe', '*')),
        first(os.path.join(t, 'ccache', '*', 'ccache-*-windows-x86_64')),
        first(os.path.join(t, 'dfu-util', '*', 'dfu-util-*-win64')),
        os.path.join(PY_ENV, 'Scripts'),
        os.path.join(IDF_DIR, 'tools'),
        os.path.join(GIT_DIR, 'cmd'),
        os.path.join(GIT_DIR, 'mingw64', 'bin'),
    ]
    keep = [e for e in entries if e and os.path.isdir(e)]
    # Keep the original PATH tail (system dirs) so python/other tools still resolve.
    return ';'.join(keep) + ';' + os.environ.get('PATH', '')


def build_env():
    env = dict(os.environ)
    # These two are what idf.py and cmake trip over; drop them outright.
    env.pop('MSYSTEM', None)
    env.pop('MSYS', None)
    env.pop('PYTHONPATH', None)

    env['PATH'] = build_path()
    env['IDF_PATH'] = IDF_DIR
    env['IDF_TOOLS_PATH'] = TOOLS_PATH
    env['IDF_PYTHON_ENV_PATH'] = PY_ENV
    env['PYTHONUTF8'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    env['IDF_GITHUB_ASSETS'] = 'dl.espressif.cn/github_assets'
    env['GIT_EXEC_PATH'] = os.path.join(GIT_DIR, 'mingw64', 'libexec', 'git-core')

    rom_elfs = first(os.path.join(TOOLS_PATH, 'tools', 'esp-rom-elfs', '*'))
    if rom_elfs:
        env['ESP_ROM_ELF_DIR'] = rom_elfs + os.sep
    openocd = first(os.path.join(TOOLS_PATH, 'tools', 'openocd-esp32', '*', 'openocd-esp32', 'share', 'openocd', 'scripts'))
    if openocd:
        env['OPENOCD_SCRIPTS'] = openocd
    return env


def main():
    args = sys.argv[1:] or ['build']

    if not os.path.isfile(PY):
        print('ERROR: python env not found at %s' % PY, file=sys.stderr)
        return 1
    idf_py = os.path.join(IDF_DIR, 'tools', 'idf.py')
    if not os.path.isfile(idf_py):
        print('ERROR: idf.py not found at %s' % idf_py, file=sys.stderr)
        return 1

    env = build_env()
    path_entries = env['PATH'].split(';')
    print('== idf553-run ==')
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
    print('  cmd     : idf.py %s' % ' '.join(args))
    print('-' * 70)
    sys.stdout.flush()

    proc = subprocess.run([PY, idf_py] + args, env=env, cwd=PROJECT_DIR)
    print('-' * 70)
    print('exit code: %d' % proc.returncode)
    return proc.returncode


if __name__ == '__main__':
    sys.exit(main())
