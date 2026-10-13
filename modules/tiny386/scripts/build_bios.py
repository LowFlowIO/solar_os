"""Build the bundled ROMs from their corresponding, pinned SeaBIOS source."""
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ASSETS = Path(__file__).resolve().parents[1] / "bios"
REVISION = "81ec9ec0bcf45df11fb7f98339ec9036b546fca0"

with tempfile.TemporaryDirectory(prefix="tiny386-seabios-") as directory:
    source = Path(directory)
    with tarfile.open(ASSETS / "seabios-source.tar.xz") as archive:
        archive.extractall(source, filter="data")
    with (ASSETS / "tiny386.patch").open("rb") as patch:
        subprocess.run(["patch", "-p1", "--batch"], cwd=source, stdin=patch, check=True)
    shutil.copyfile(ASSETS / "config", source / ".config")
    (source / ".version").write_text(REVISION + "\n")
    args = ["make", "PYTHON=python3", "EXTRAVERSION=-SolarOS-tiny386-0.1.0"]
    subprocess.run([*args, "olddefconfig"], cwd=source, check=True)
    subprocess.run([*args, "-j4"], cwd=source, check=True)
    for name in ("bios.bin", "vgabios.bin"):
        shutil.copyfile(source / "out" / name, ASSETS / name)
