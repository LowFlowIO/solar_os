+++
id = "navigation"
title = "Navigation and keys"
section = "concept"
summary = "Learn common shortcuts and built-in controls on compact targets"
aliases = ["shortcuts"]
keywords = "navigation keys keyboard shortcuts controls combinations escape enter alt ctrl shift pager t-lora-pager t-deck cl-32 odroid wheel trackball"
packages_any = []
+++
# Navigation and keys

SolarOS accepts input from keyboards, board buttons, wheels, and trackballs.
Use the common keys below, or find your device's built-in controls in the
target tables. These tables describe the default mappings; custom keyboard
profiles can change them.

`Alt+Enter` means hold Alt, press Enter, then release both keys. A **tap**
means press and release a key. A **hold** keeps a modifier or layer active
while you press another key.

## Basic navigation

| Key | Action |
| --- | --- |
| Up / Down | Move between menu items or scroll, depending on the app. |
| Left / Right | Move across the launcher grid; move the cursor or change a value where supported. |
| Enter | Select an item, open it, or confirm a prompt. |
| Escape | Go back or cancel; exits apps that support Escape. |
| Page Up / Page Down | Move a page at a time where supported. |
| Home / End | Move to the start or end where supported. |
| Alt+Enter | Toggle full screen in shared text menus. Small menus start in full screen automatically. |

Run `launcher` for the graphical app grid, or type an app's command name at
the shell prompt. App-specific controls appear in the app's help and the
[Application reference](apps.md). Escape is handled by each app; use the
app-close shortcut below when you need to close the foreground app.

## Sessions and app shortcuts

| Action | Local display keyboard | Port terminal (USB CDC, UART, Telnet) |
| --- | --- | --- |
| Close the foreground app | Ctrl+Alt+Del | Ctrl+] |
| Next retained session | Alt+Tab or Alt+Right | Ctrl+Z suspends a resumable app and returns to the prompt; use `fg` to resume it. |
| Previous retained session | Alt+Left | Use `fg ID` to resume a specific retained app. |
| Move local keyboard focus to another display | Ctrl+Alt+Arrow | Run `session focus TARGET`. |

Either Alt key works for session switching and display focus, including
AltGr on compact keyboards. Switching sessions retains resumable apps;
closing an app returns to its display's shell or resumes its parent app.
Use `session list` to see session IDs and [Sessions](sessions.apps.md) for
more controls.

## At the shell prompt

| Key | Action |
| --- | --- |
| Enter | Run the typed command. |
| Up / Down | Browse command history. |
| Left / Right | Move the insertion cursor. |
| Backspace / Delete | Delete before / at the cursor. |
| Escape | Clear the typed command without running it. |
| Tab | Complete a command, subcommand, or path. |
| Tab twice | List matching completions; at an empty prompt, list commands. |

## LilyGO T-LoRa-Pager (T-Pager)

| Control | Action |
| --- | --- |
| Wheel clockwise | Down — next menu item or scroll down. |
| Wheel counterclockwise | Up — previous item or scroll up. |
| Press wheel | Enter — select/open; emitted when the wheel is released. |
| Keyboard Enter | Enter. |
| Hold Space + Enter | Escape — back/cancel; exits apps that support Escape. |
| Tap Space | Type a space. |
| Hold Space + another key | Type its symbol; Q–P give digits 1–0. |
| Hold the Caps-labelled key | Shift — use with another key for its shifted character. |
| Hold Alt | Alt modifier. |
| Backspace | Delete the character before the cursor. |

The default keyboard profile has no Left, Right, Tab, or Ctrl mappings.
The wheel supplies Up/Down, so it cannot move across launcher columns.
Use a custom keyboard profile or a compatible external keyboard for those
keys, horizontal launcher navigation, and the usual session-switch and
app-close combinations. You can also launch apps by typing their command
names. Space+Enter remains Escape rather than the universal app-close key.

## LilyGO T-Deck Plus

| Control | Action |
| --- | --- |
| Roll trackball up / down / left / right | One arrow key in the dominant direction after movement stops. |
| Press trackball | Enter — select/open. |
| Keyboard Enter | Enter. |
| Microphone-icon key | Tab — complete a shell command or argument; press twice to list matches. |
| Shift + microphone-icon key | Ctrl — keep Mic held while pressing the other key. |
| Sym + microphone-icon key | Type `0`. |
| Alt+C | Escape — back/cancel where supported. |
| Alt+Q | Close the foreground app. |
| Alt+B | Toggle the keyboard backlight through the keyboard firmware; the shortcut does not type a character. |
| Alt + trackball up / down | Page Up / Page Down. |
| Alt + trackball left / right | Previous / next retained session on the focused display. |
| Alt+Enter | Toggle full screen in shared text menus. |
| Hold Sym + another key | Type the symbol from the keyboard's symbol layer. |
| Hold Shift + another key | Type its shifted character. |

Each continuous roll produces one arrow key after 80 ms without movement.
The direction follows the greater net horizontal or vertical movement;
opposite movements cancel. Equal diagonals use the most recently moved axis.
Gestures retain the modifiers held at the last movement pulse, so they can be
released while the ball settles. Shift+left/right selects text in the editor;
Ctrl+left/right moves by words.
Keyboard keys report their actual presses and releases, supporting held controls
in foreground apps. `setterm keyrate` controls typing repeat; trackball rolls
remain single navigation taps.
For Ctrl, hold Shift and Mic in either order. Shift selects the chord and is
consumed; it can then be released while Mic continues acting as Ctrl. Releasing
Mic releases Ctrl. A plain Mic emits Tab on release, allowing Shift+Mic to be
recognized without triggering completion. Sym+Mic retains its digit `0` mapping.

## CL-32

| Control | Action |
| --- | --- |
| Arrow keys | Corresponding arrow key. |
| OK | Enter — select/open. |
| Cancel | Escape — back/cancel where supported. |
| Hold File | Left Alt modifier. |
| Hold Menu | Left Ctrl modifier. |
| File + Left / Right | Previous / next retained session on the focused display. |
| File+OK | Toggle full screen in shared text menus. |
| Tap Shift or Fn | Cycle that modifier through one-shot, locked, and off. |
| Fn active + keypad 1–9 / 0 | F1–F9 / F10. |
| Fn active + Up / Down | Page Up / Page Down. |
| Fn active + Left / Right | Home / End. |

One-shot Shift or Fn applies to the next key. Locked Shift or Fn remains
active until you turn it off.

## ODROID-GO

| Control | Action |
| --- | --- |
| D-pad | Corresponding arrow key, when ADC D-pad support is included in the firmware. |
| A | Enter — select/open. |
| B | Space. |
| SELECT | Escape — back/cancel where supported. |
| MENU | Close the foreground app. |
| VOLUME | Toggle audio mute. |

The built-in buttons do not provide a text keyboard or Alt/Ctrl modifiers.
Use a compatible external keyboard or a port terminal for typing commands
and combinations that need those keys.

See [Boards](boards.md) for target capabilities and
[Input](media.input.md#keyboard-input) for input sources and keyboard layout
settings. To customize a supported keyboard profile, use the website's
Tools → Keyboard configurator.

## Quick reference

Arrows navigate, Enter selects, and Escape goes back where supported.
Close a local display app with Ctrl+Alt+Del; in a port terminal use Ctrl+].
Alt+Tab or Alt+Right selects the next local session; Alt+Left selects the
previous one. Ctrl+Z suspends a resumable port app; fg resumes it.
T-Pager: wheel is Up/Down, press is Enter, Space+Enter is Escape, held Space
selects symbols, and held Caps is Shift. Its default profile lacks Left,
Right, Tab, and Ctrl. T-Deck Plus: trackball supplies arrows and Enter,
Mic is Tab, Shift+Mic is Ctrl, Alt+C is Escape, and Alt+Q closes the app.
CL-32: OK is Enter, Cancel is
Escape, File is Alt, and Menu is Ctrl. ODROID-GO: D-pad supplies arrows,
A is Enter, B is Space, SELECT is Escape, and MENU closes the app.
