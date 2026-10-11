from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class GameboyModuleTest(unittest.TestCase):
    def test_real_core_lifecycle_audio_save_and_failures(self):
        with tempfile.TemporaryDirectory() as tmp:
            objects=[]
            sources=[("modules/gameboy/main/gameboy.c", ["-Dmain=gameboy_module_main"]),
                     ("modules/gameboy/main/apu.c", []),
                     ("src/apps/solar_os_gameboy_rom.c", []),
                     ("src/apps/solar_os_gameboy_video.c", []),
                     ("tests/host/gameboy_module_test.c", [])]
            for index,(source,extra) in enumerate(sources):
                obj=Path(tmp,str(index)+".o");objects.append(str(obj))
                subprocess.run(["cc","-std=c11","-O2","-Wall","-Wextra","-Werror",
                                "-I",str(ROOT/"include"),"-I",str(ROOT/"src"),
                                "-I",str(ROOT/"src/services"),"-I",str(ROOT/"src/apps"),
                                *extra,"-c",str(ROOT/source),"-o",str(obj)],check=True)
            binary=Path(tmp,"gameboy")
            subprocess.run(["cc",*objects,"-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
