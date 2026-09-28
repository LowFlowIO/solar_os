from pathlib import Path
import tomllib
import unittest


ROOT = Path(__file__).resolve().parents[1]


class NativeModulePackagesTest(unittest.TestCase):
    def test_hello_is_an_official_versioned_module(self):
        module = ROOT / "modules" / "hello"
        with (module / "module.toml").open("rb") as stream:
            manifest = tomllib.load(stream)

        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["id"], "hello")
        self.assertEqual(manifest["target"], "esp32s3")
        self.assertEqual(manifest["native_abi"], 1)
        self.assertEqual(manifest["minimum_host_api_size"], 20)
        self.assertEqual(manifest["required_capabilities"], ["psram"])
        self.assertTrue((module / "main" / "hello.c").is_file())

    def test_native_package_service_has_compatibility_and_integrity_gates(self):
        service = (ROOT / "src/services/solar_os_module_packages.c").read_text(
            encoding="utf-8"
        )
        for contract in (
            "SOLAR_OS_OTA_PUBLIC_KEY_PEM",
            "SOLAR_OS_VERSION",
            "CONFIG_IDF_TARGET",
            "SOLAR_OS_NATIVE_ABI_VERSION",
            "minimum_host_api_size",
            "required_capabilities",
            "solar_os_crypto_sha256_matches_hex",
            "solar_os_native_elf_validate",
            "solar_os_storage_replace_file",
        ):
            self.assertIn(contract, service)

    def test_pkg_and_package_catalog_are_wired_together(self):
        packages = (ROOT / "packages/solar_os_packages.toml").read_text(
            encoding="utf-8"
        )
        shell = (ROOT / "src/shell/solar_os_shell_system.c").read_text(
            encoding="utf-8"
        )

        self.assertIn('"services/solar_os_module_packages.c"', packages)
        for dependency in (
            '"service_crypto"',
            '"service_http_client"',
            '"service_json"',
        ):
            self.assertIn(dependency, packages)
        for command in ("available", "install", "remove"):
            self.assertIn(f'strcmp(argv[1], "{command}")', shell)

        self.assertIn("PKG_NETWORK_TASK_STACK (16U * 1024U)", shell)
        self.assertIn("solar_os_task_create_pinned_internal(pkg_network_task", shell)
        self.assertIn("pkg_run_network_worker(term, PKG_NETWORK_AVAILABLE", shell)
        self.assertIn("pkg_run_network_worker(term, PKG_NETWORK_INSTALL", shell)

    def test_installed_application_modules_are_shell_commands(self):
        service = (ROOT / "src/services/solar_os_module_packages.c").read_text(
            encoding="utf-8"
        )
        native_shell = (ROOT / "src/shell/solar_os_shell_native.c").read_text(
            encoding="utf-8"
        )
        shell = (ROOT / "src/apps/solar_os_shell.c").read_text(encoding="utf-8")

        self.assertIn('static const char suffix[] = ".app.elf";', service)
        self.assertIn("solar_os_module_package_foreach_installed", service)
        self.assertIn("solar_os_shell_try_native_module", native_shell)
        self.assertIn("native_shell_run(solar_os_shell_command_io(ctx)", native_shell)
        self.assertIn("solar_os_module_package_foreach_installed", shell)

        dispatch = shell[shell.index("static bool shell_execute_line(") :]
        alias = dispatch.index("if (alias_matched)")
        native = dispatch.index("solar_os_shell_try_native_module")
        unknown = dispatch.index("shell_report_unknown_command(io")
        self.assertLess(alias, native)
        self.assertLess(native, unknown)


if __name__ == "__main__":
    unittest.main()
