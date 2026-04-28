# Secondary Display Configuration in VPinballX Standalone

## Overview

VPinballX Standalone (Linux/macOS/ALP4K) uses **SDL3** for window management. This differs significantly from the Windows version of VPX.
In Standalone, windows (Playfield, Backglass, DMD, Topper) are assigned to physical displays using **SDL Display Names**.

## Configuration File

All display settings are managed in `VPinballX.ini` (located in your configuration folder or alongside the binary).

There are four main sections corresponding to the VPinball windows:

1.  **Playfield**: `[Player]`
2.  **Backglass**: `[Backglass]`
3.  **Score/DMD**: `[ScoreView]`
4.  **Topper**: `[Topper]`

## Assigning Displays

To assign a specific window to a specific monitor, use the `Display` key in the relevant section. The value must be the **exact string** of the Display Name reported by SDL.

### Example Configuration

```ini
[Player]
Display = HDMI-0
FullScreen = 1

[Backglass]
Display = HDMI-1
FullScreen = 1

[ScoreView]
Display = DP-1
FullScreen = 0
```

### Finding Your Display Names

The easiest way to find your correct Display Names is to use the built-in command line tool:

```bash
./VPinballX_GL -displayid
```

This will open a window on **every connected monitor** showing its Name (e.g., "HDMI-0"), resolution, and refresh rate. Use exactly the name shown in large text for your `VPinballX.ini`.

Alternatively, check the logs as described below:

1.  Run VPinballX Standalone from the terminal.
2.  Check the output (or `vpindebug.log` if enabled) for lines starting with `Window #... was created on display ...`.
3.  If a configured display is not found, VPinballX will warn you:
    > `"The selected display "MyWrongName" is not available. Using display "HDMI-0" instead."`

Use the name shown in the "Using display" or "created on display" log message.

### Fallback & Manual Positioning

If the `Display` key is empty or the text does not match any connected display, VPinballX defaults to the **Primary Display**.

The `X` and `Y` coordinates in the INI file are **relative to the selected display**.

*   **Scenario A (Preferred)**: You set `Display=HDMI-1`. `X=0` places the window at the top-left of HDMI-1.
*   **Scenario B (Manual)**: You leave `Display=` empty (uses Primary). If Primary is 1920px wide, setting `X=1920` will effectively place the window on the second monitor (assuming standard side-by-side desktop layout).

## Common Properties

| Property | Description |
| :--- | :--- |
| `Display` | Name of the target display (e.g. `HDMI-1`). |
| `FullScreen` | `1` for Exclusive Fullscreen, `0` for Windowed. |
| `Width` / `Height` | Window size (for Windowed mode). |
| `X` / `Y` | Position relative to the top-left of the target `Display`. |

## Troubleshooting

*   **Window Defaults to Primary**: This usually means the `Display` string didn't match exactly. Check for spaces or case sensitivity.
*   **Black Screen on ALP4K**: Ensure your Playfield is on the correct output. The ALP4K playfield is often recognized as the primary display, but verified the name via logs.
