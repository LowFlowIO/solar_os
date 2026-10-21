"""Cycle the emotion poses on the current display or an optional target.

Usage: python /examples/emotion.py [display] [mono|cozmo|auto]
"""
import sys
import solaros

poses = (
    "neutral", "blink_high", "happy", "glee", "blink_low", "sad_down",
    "sad_up", "worried", "focused", "annoyed", "surprised", "skeptic",
    "bored", "unimpressed", "sleepy", "suspicious", "squint", "angry",
    "furious", "scared", "awe",
)
target = sys.argv[1] if len(sys.argv) > 1 else None
theme = sys.argv[2] if len(sys.argv) > 2 else "mono"

with solaros.emotion.open(target, sound=True, theme=theme) as face:
    for pose in poses:
        if solaros.should_exit():
            break
        face.set(pose)
        # The face keeps animating while the script sleeps or does robot work.
        solaros.time.sleep_ms(1400)
    if not solaros.should_exit():
        face.set("happy")
        face.look(0.5, -0.2)
        face.react("surprised", hold_ms=900, sound="chirp")
        solaros.time.sleep_ms(1400)
