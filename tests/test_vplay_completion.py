from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SHELL = (ROOT / "src/apps/solar_os_shell.c").read_text(encoding="utf-8")
VPLAY = (ROOT / "src/apps/solar_os_vplay.c").read_text(encoding="utf-8")


class VplayCompletionPolicyTest(unittest.TestCase):
    def test_options_match_the_player_and_are_package_gated(self):
        self.assertIn(
            '#if SOLAR_OS_PACKAGE_APP_VPLAY\n'
            'static const char * const vplay_options[] = {"-fit", "-actual"};',
            SHELL,
        )
        for option in ("-fit", "-actual"):
            self.assertIn(f'strcmp(arg, "{option}")', VPLAY)
        self.assertIn(
            '#if SOLAR_OS_PACKAGE_APP_VPLAY\n'
            '    SHELL_COMPLETION_OPTIONS(path_vplay, vplay_options),',
            SHELL,
        )

    def test_paths_complete_after_a_size_option(self):
        self.assertIn(
            'static const char * const path_vplay_after_option[] = '
            '{"vplay", SHELL_COMPLETION_ANY};',
            SHELL,
        )
        self.assertIn(
            'SHELL_COMPLETION_PATH(path_vplay_after_option, false)', SHELL
        )

    def test_bare_command_uses_shared_file_and_directory_completion(self):
        commands = SHELL.split('static bool shell_is_path_command(', 1)[1].split(
            'static bool shell_path_completion_dirs_only(', 1
        )[0]
        self.assertIn(
            '#if SOLAR_OS_PACKAGE_APP_VPLAY\n'
            '           strcmp(command, "vplay") == 0 ||',
            commands,
        )


if __name__ == "__main__":
    unittest.main()
