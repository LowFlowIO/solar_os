from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def production_functions(source, names):
    functions = []
    for name in names:
        match = re.search(r"^[A-Za-z_][^;{}\n]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
        if not match:
            raise AssertionError(name)
        start = match.end() - 1
        depth = 1
        end = start + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        functions.append(source[match.start():end])
    return "\n".join(functions)


class NativeVmLifecycleTest(unittest.TestCase):
    def test_vm_capture_closes_focus_overlay(self):
        source = (ROOT / "src/main.c").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "vm_focus_dispatch.inc").write_text(production_functions(
                source, ("dispatch_input_key",)))
            binary = Path(tmp, "focus")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src"), "-I", str(ROOT / "src/services"),
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/vm_focus_overlay_test.c"),
                str(ROOT / "src/services/solar_os_input.c"),
                str(ROOT / "src/services/solar_os_keys.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_presenter_suspension_reports_join_and_policy_failures(self):
        source = (ROOT / "src/services/solar_os_frame_presenter.c").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "frame_lifecycle.inc").write_text(production_functions(
                source, ("solar_os_frame_presenter_suspend_checked", "solar_os_frame_presenter_redraw")))
            binary = Path(tmp, "presenter")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                str(ROOT / "tests/host/frame_presenter_lifecycle_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_host_shutdown_and_timeout(self):
        # Compile the production lifecycle functions against deterministic task,
        # clock and presenter mocks; hardware wrappers are tested by board builds.
        source = (ROOT / "src/services/solar_os_native_vm.c").read_text()
        names = ("vm_caller", "vm_result", "vm_rgb565", "vm_text_colors",
                 "vm_stop_requested", "vm_enable_suspend", "vm_suspend_requested",
                 "vm_guest_suspend", "vm_worker", "vm_request_stop",
                 "vm_stop", "vm_suspend", "vm_resume", "vm_ready", "vm_queue_key", "vm_event",
                 "vm_resolve_path", "vm_start", "solar_os_native_vm_launch")
        functions = production_functions(source, names)
        storage = (ROOT / "src/services/solar_os_storage.c").read_text()
        paths = production_functions(storage, (
            "storage_append_path_segment", "storage_pop_path_segment",
            "solar_os_storage_normalize_path", "solar_os_storage_resolve_path_at"))
        timeout = re.search(r"^#define VM_STOP_WAIT_US .+$", source, re.M).group()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "vm_lifecycle.inc").write_text(timeout + "\n" + paths + "\n" + functions)
            binary = Path(tmp, "lifecycle")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", tmp, "-I", str(ROOT / "include"), "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/native_vm_lifecycle_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_literal_graphics_palette_and_monochrome_polarity(self):
        source = (ROOT / "src/services/solar_os_frame_presenter.c").read_text()
        functions = production_functions(source, (
            "frame_palette_index", "rgb565_luma", "frame_convert_to_mono",
            "frame_prepare_mono_palette", "frame_present"))
        bayer = re.search(r"static const uint8_t bayer4\[4\]\[4\] = \{.*?\};",
                          source, re.S).group()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "frame_palette.inc").write_text(bayer + "\n" + functions)
            binary = Path(tmp, "palette")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/frame_presenter_palette_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_vm_abi_headers_compile_in_c_and_cpp(self):
        code = ('#include <stddef.h>\n#include "solar_os_native_vm_abi.h"\n'
                '#define PREFIX_END (offsetof(solar_os_native_vm_api_v1_t, set_exit_code) '
                '+ sizeof(((solar_os_native_vm_api_v1_t *)0)->set_exit_code))\n'
                'typedef char append_only[offsetof(solar_os_native_vm_api_v1_t, '
                'stop_requested) == PREFIX_END ? 1 : -1];\n'
                'typedef char suspend_append_only[offsetof(solar_os_native_vm_api_v1_t, '
                'enable_suspend) == offsetof(solar_os_native_vm_api_v1_t, resolve_path) + '
                'sizeof(((solar_os_native_vm_api_v1_t *)0)->resolve_path) ? 1 : -1];\n')
        with tempfile.TemporaryDirectory() as tmp:
            for compiler, suffix, standard in (("cc", "c", "c11"), ("c++", "cpp", "c++17")):
                path = Path(tmp, "client." + suffix)
                path.write_text(code)
                subprocess.run([compiler, "-std=" + standard, "-Wall", "-Wextra", "-Werror",
                                "-pedantic", "-I", str(ROOT / "include"),
                                "-fsyntax-only", str(path)], check=True)


if __name__ == "__main__":
    unittest.main()
