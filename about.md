# RhytmGuide

Turns Geometry Dash into a rhythm game.

Load a `.gdr` / `.gdr.json` macro (xBot / xdBot format) in the mod settings and minimalist guide lines will appear directly on the level where each input should happen. Lines smoothly scroll toward the player; when a line is reached, a white marker temporarily attaches to the player until the input ends.

Every real key press is compared against the macro: perfect hits show **In**, everything else shows the deviation in milliseconds (`+N ms` late, `-N ms` early).

All timing is driven by the in-game clock and updated inside the game's frame loop, so the guide stays perfectly in sync with physics and FPS. Compatible with Click Between Frames (the mod only observes inputs, it never alters them).

## Settings

- Master toggle, per-gamemode hiding (ship / wave)
- Macro file picker, FPS override, global input offset calibration
- Line color / opacity / thickness, approach window
- Marker size / opacity / tap duration
- Timing text: "In" window, match window, scale, fade time
