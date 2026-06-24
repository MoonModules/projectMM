# /// script
# dependencies = ["pytest"]
# ///
"""compute_version tests — pins the per-channel version string.

A stable build carries the core semver; a `latest` build carries a monotonic
`<core>-dev.<N>` so successive latest builds are orderable; a local build is
library.json verbatim. N counts since the last `v*` tag, with a tag-less fallback.

Run: `uv run pytest test/python`.
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "build"))
import compute_version as cv  # noqa: E402


def _lib(tmp_path, version):
    """Write a throwaway library.json holding `version`, return its path."""
    f = tmp_path / "library.json"
    f.write_text(json.dumps({"name": "projectMM", "version": version}))
    return f


def test_core_strips_prerelease_and_build():
    assert cv.core_version("2.1.0-dev") == "2.1.0"
    assert cv.core_version("2.1.0-dev.6") == "2.1.0"
    assert cv.core_version("2.1.0+build.7") == "2.1.0"
    assert cv.core_version("2.0.0") == "2.0.0"


def test_stable_is_core(monkeypatch, tmp_path):
    monkeypatch.setattr(cv, "LIBRARY_JSON", _lib(tmp_path, "2.1.0-dev"))
    assert cv.compute("stable") == "2.1.0"


def test_latest_is_core_dev_n(monkeypatch, tmp_path):
    monkeypatch.setattr(cv, "LIBRARY_JSON", _lib(tmp_path, "2.1.0-dev"))
    monkeypatch.setattr(cv, "commits_since_last_stable", lambda: 6)
    assert cv.compute("latest") == "2.1.0-dev.6"


def test_local_is_verbatim(monkeypatch, tmp_path):
    monkeypatch.setattr(cv, "LIBRARY_JSON", _lib(tmp_path, "2.1.0-dev"))
    assert cv.compute("local") == "2.1.0-dev"
