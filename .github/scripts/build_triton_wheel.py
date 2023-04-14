#!/usr/bin/env python3
import shutil
import sys
from pathlib import Path
from subprocess import check_call
from tempfile import TemporaryDirectory
from typing import Optional

SCRIPT_DIR = Path(__file__).parent
REPO_DIR = SCRIPT_DIR.parent.parent


def build_triton() -> Path:
    print("Hello world")
    with TemporaryDirectory() as tmpdir:
        triton_basedir = Path(tmpdir) / "triton"
        triton_basedir = Path("/triton")
        triton_pythondir = triton_basedir / "python"
        check_call([sys.executable, "setup.py", "bdist_wheel"], cwd=triton_pythondir)
        whl_path = list((triton_pythondir / "dist").glob("*.whl"))[0]
        shutil.copy(whl_path, Path.cwd())
def main() -> None:
    from argparse import ArgumentParser

    parser = ArgumentParser("Build Triton binaries")
    parser.add_argument("--py-version", type=str)
    args = parser.parse_args()
    build_triton()
    


if __name__ == "__main__":
    main()