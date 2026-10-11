#!/usr/bin/env python3
"""Exercise already-installed Apple II and Game Boy foreground modules.

Uses the device's configured telnet console. Supply owned guest assets; the
script closes its sessions, and the Game Boy app saves modified cartridge RAM.
"""
import argparse
import re
import shlex
import time

from model_bundle_stream import Console


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--gameboy-rom', required=True)
    parser.add_argument('--apple-assets', required=True)
    parser.add_argument('--apple-disk', required=True)
    parser.add_argument('--display', default='display0')
    args = parser.parse_args()
    # Telnet may still be disposing the preceding port-shell connection.
    for attempt in range(5):
        try:
            console = Console(args.host)
            break
        except (OSError, RuntimeError):
            if attempt == 4:
                raise
            time.sleep(1)
    opened = []

    def command(*words):
        text = shlex.join(words)
        output = console.command(text, 10)
        print(text + '\n' + output, flush=True)
        return output

    def create(app, *arguments):
        output = command('session', 'create', app, args.display, *arguments)
        match = re.search(r'session (\d+) created', output)
        if not match:
            raise AssertionError(output)
        opened.append(match[1])
        return match[1]

    def close(session):
        output = command('session', 'close', session)
        if 'closed session' not in output:
            raise AssertionError(output)
        opened.remove(session)

    def state(app, expected):
        output = command('session', 'list')
        if not re.search(r'\b' + app + r'\s+' + app + r'\s+' + expected, output):
            raise AssertionError(output)

    try:
        installed = command('pkg', 'installed')
        assert 'app apple2' in installed and 'app gameboy' in installed, installed
        assert not re.search(r'^vm ', installed, re.M), installed
        apps = command('apps')
        assert re.search(r'^apple2 -', apps, re.M), apps
        assert len(re.findall(r'^gameboy -', apps, re.M)) == 1, apps
        gameboy = create('gameboy', args.gameboy_rom)
        time.sleep(3)
        state('gameboy', 'active')
        workers = command('top')
        assert re.search(r'^gameboy\s', workers, re.M), workers
        assert 'gb_emulator' not in workers, workers
        apple = create('apple2', args.apple_assets, args.apple_disk)
        time.sleep(1)
        state('apple2', 'active')
        state('gameboy', 'suspended')
        workers = command('top')
        assert re.search(r'^apple2\s', workers, re.M), workers
        assert re.search(r'^gameboy\s', workers, re.M), workers
        duplicate = command('session', 'create', 'apple2', args.display,
                            args.apple_assets, args.apple_disk)
        assert 'failed' in duplicate, duplicate
        clock = create('clock')
        state('apple2', 'suspended')
        state('gameboy', 'suspended')
        close(clock)
        state('apple2', 'active')
        close(apple)
        state('gameboy', 'active')
        for _ in range(3):
            clock = create('clock')
            state('gameboy', 'suspended')
            close(clock)
            state('gameboy', 'active')
        close(gameboy)
        workers = command('top')
        assert not re.search(r'^(gameboy|apple2|synth)\s', workers, re.M), workers
        command('log', 'show')
        print('External foreground lifecycle: PASS')
    finally:
        for session in reversed(opened):
            command('session', 'close', session)
        console.socket.close()


if __name__ == '__main__':
    main()
