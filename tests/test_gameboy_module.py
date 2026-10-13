from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class GameboyModuleTest(unittest.TestCase):
    def test_real_core_lifecycle_audio_save_and_failures(self):
        with tempfile.TemporaryDirectory() as tmp:
            source_root=Path(tmp,"source")
            shutil.copytree(ROOT/"include",source_root/"include")
            shutil.copytree(ROOT/"modules/gameboy/main",source_root/"modules/gameboy/main")
            shutil.copyfile(ROOT/"tests/host/gameboy_module_test.c",source_root/"gameboy_module_test.c")
            objects=[]
            sources=[("modules/gameboy/main/gameboy.c", ["-Dmain=gameboy_module_main"]),
                     ("modules/gameboy/main/apu.c", []),
                     ("modules/gameboy/main/solar_os_gameboy_rom.c", []),
                     ("modules/gameboy/main/solar_os_gameboy_video.c", []),
                     ("gameboy_module_test.c", [])]
            for index,(source,extra) in enumerate(sources):
                obj=Path(tmp,str(index)+".o");objects.append(str(obj))
                subprocess.run(["cc","-std=c11","-O2","-Wall","-Wextra","-Werror",
                                "-I",str(source_root/"include"),"-I",str(source_root/"modules/gameboy/main"),
                                *extra,"-c",str(source_root/source),"-o",str(obj)],check=True)
            binary=Path(tmp,"gameboy")
            subprocess.run(["cc",*objects,"-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
