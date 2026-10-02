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

SolarOS carries a small setup and recovery guide in firmware, together with a
searchable directory of the topics available in the current build. Full guides
and scripting API references come from the downloaded manual. Before that
manual is installed, other topics explain how to download it.

The built-in guide covers documentation setup, Wi-Fi connection, storage mounts,
firmware updates, basic diagnostics, and session controls. Run `commands` to
list the commands in this firmware, `man --list` to list manual topics, or
`man -k QUERY` to search the topic directory.

Run `help` to open the foreground documentation browser. Topics are grouped in
a tree with all groups initially folded. Use Left and Right or Enter on a group
to fold and unfold it, then select a topic and press Enter to read it. The tree
retains its selection, scroll position, and folded groups when the topic closes.
On a graphic display the topic opens in `reader`; text shells use `less`. Both
consume the same `man:TOPIC` source, so TOML frontmatter is never shown as
document content.

## Refresh from solar-os.eu

On devices with Wi-Fi, PSRAM, and an SD card, install or refresh the full manual
without installing new firmware. The downloaded manual remains available
without a network connection while the SD card is mounted. Devices without
download support retain the setup and recovery guide; the full manual is also
available at [solar-os.eu](https://solar-os.eu/).

First connect Wi-Fi, inspect the available persistent disks, and mount the
default removable volume if necessary:

```text
wifi on
disk lsblk
disk mount
```

Use the display Wi-Fi menu with `wifi`, or connect from a text shell with
`wifi connect SSID PASSWORD`. Run `man command.wifi` for saved-network and
connection commands. `disk lsblk` lists the detected volumes; `disk mount`
mounts the default removable volume. Run `man command.disk` if you need to
select a different volume.

Then run:

```text
help update
```

SolarOS requests the documentation published for its exact running firmware
version. It verifies the catalog with the OTA public key, downloads the single
`manual.zip` archive authenticated by that catalog, and extracts it into a
temporary revision. Every extracted Markdown page is then checked against its
signed size and SHA-256 before activation. The command shows width-aware
download and extraction progress, including on the narrow display shell at text
size 16. An interrupted or invalid download leaves the previous manual active.

`help status` shows whether the external revision or embedded fallback is in
use. After a firmware upgrade, SolarOS keeps using a valid signed manual that
was downloaded for the previous version until `help update` installs the exact
current-version manual. `help`, `man`, and `help status` warn that this retained
manual may be outdated. `help reset` stops using the downloaded revision; it
does not remove the immutable cached files from the SD card.

The maintenance word `status` intentionally takes precedence over the bare
manual alias. Use the exact topic ID `help command.status` to open the shell
`status` command page.

## Why versions must match

Documentation can affect scripts produced by the agent. A newly downloaded
manual is therefore activated only when its catalog names the exact running
firmware version. A previously verified manual remains available after an OS
upgrade instead of silently falling back to embedded content, but SolarOS marks
it as potentially outdated until it is refreshed or reset.

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
