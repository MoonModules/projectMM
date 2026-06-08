"""Windows-only shim: set the C locale to UTF-8 before running idf.py.

ESP-IDF's idf.py refuses to start when `locale.getlocale()` reports a non-
UTF-8 encoding ("Support for Unicode is required, locale … with 1252 …").
On Dutch / German / French Windows installs the default C locale is cp1252.
PYTHONUTF8=1 fixes Python's I/O encoding but does NOT change getlocale() —
that comes from the C library and only `locale.setlocale()` updates it within
the running process.

Windows offers no env-var equivalent that Python honours at interpreter
startup, so the workaround is: set the locale FIRST inside the same Python
process, THEN execute idf.py via runpy. The locale change persists for the
remainder of the process, so IDF's check passes.

invoked as `python _idf_win_shim.py <args-for-idf.py>` from build_esp32.py.
"""

import locale
import os
import runpy
import sys

# Try UTF-8 locale names in order of preference. The first one the C library
# accepts wins. en_US.UTF-8 has been supported on Windows 10 1803+ via the
# CRT's UTF-8 codepage; older systems get C.UTF-8 (also Windows-supported in
# recent UCRT) or fall through.
for loc in ("en_US.UTF-8", "en_US.utf8", "C.UTF-8", ".UTF-8"):
    try:
        locale.setlocale(locale.LC_ALL, loc)
        break
    except locale.Error:
        continue
else:
    sys.stderr.write(
        "_idf_win_shim: no UTF-8 locale could be set. Run idf.py from a "
        "Windows install with UTF-8 region support enabled, or set "
        "Control Panel → Region → Administrative → \"Use "
        "Unicode UTF-8 for worldwide language support\".\n"
    )
    sys.exit(2)

idf_tools_dir = os.path.join(os.environ["IDF_PATH"], "tools")
idf_py = os.path.join(idf_tools_dir, "idf.py")

# runpy.run_path does NOT add the script's directory to sys.path the way
# `python idf.py` does, but idf.py imports `python_version_checker` as a
# sibling module. Prepend the tools dir so those sibling imports resolve.
if idf_tools_dir not in sys.path:
    sys.path.insert(0, idf_tools_dir)

sys.argv = [idf_py] + sys.argv[1:]
runpy.run_path(idf_py, run_name="__main__")
