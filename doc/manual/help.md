+++
id = "help"
title = "Browsing and refreshing documentation"
section = "concept"
summary = "Browse the manual and refresh its signed Markdown pages"
aliases = ["documentation"]
keywords = "help documentation manual browser tui update refresh signed catalog sd fallback version"
packages_any = []
+++
# Browse and refresh documentation

Run `help` to browse topics, or search and open them from the shell:

```text
man -k connect wifi
man sessions.apps
man --list
```

In Help, use arrows or Enter to expand a group, then Enter to read a topic.
Graphic displays use Reader; text terminals use Less. Closing a page returns
to the same place in Help.

Firmware includes setup help and a topic directory. Install the full manual
for detailed guides and scripting APIs, or read it at
[solar-os.eu](https://solar-os.eu/).

## Install or refresh the manual

This requires Wi-Fi, PSRAM, and mounted SD storage. Connect Wi-Fi using `wifi`
on a display or `wifi connect SSID PASSWORD` from a text shell. Then:

```text
disk lsblk
disk mount
help update
help status
```

`disk mount` mounts the default removable volume; use `man command.disk` to
select another volume. `help update` downloads and verifies the manual for
your running firmware version. If the download fails or is interrupted, the
previous manual stays active. Once installed, it works offline while the
storage is mounted.

After an OS upgrade, a previously downloaded manual remains readable but is
marked outdated. Run `help update` to refresh it. `help status` shows the
firmware and manual versions, download state, and last error.

## Return to built-in help

```text
help reset
```

This stops using the downloaded manual and restores setup help. Cached files
may remain on SD. To read the shell's `status` command page, use
`help command.status`; `help status` reports the manual's own status.

## Quick reference

`help` opens the foldable topic tree; `help TOPIC` expands the corresponding
group and selects that topic initially. Graphic display shells open topics with
`reader`; CDC, UART, Telnet, SSH, and other text shells use `less`.
`help status` reports the active source, firmware and manual versions, revision,
page count, update state, and last error. `help update` downloads one
catalog-authenticated archive, verifies every extracted page, stores the
exact-version manual on SD, and activates it only after signature, size, and
SHA-256 verification. Use
`help command.status` for the command page rather than the maintenance status.
`help reset` immediately returns `man`, `help`, and the agent to the embedded
setup and recovery guide. Full topic guides and scripting API references
require the downloaded manual. Refreshing requires Wi-Fi, PSRAM, and SD; the
installed manual can be read offline while its storage is mounted.
