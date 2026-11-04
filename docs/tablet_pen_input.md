# Tablet pen input (initial implementation)

SDL3 pen proximity, tip contact, motion, pressure/tilt and barrel-button events are translated into engine-owned input events. Inspect the latest stylus through `Input::pen()` (position, pressure, tilt, eraser, contact and button state), and per-frame edges via `isPenPressed()` and `isPenReleased()`.

This first pass handles one active pen and intentionally does not synthesize mouse clicks, as SDL and operating systems may already emit those. Editor brush pressure/strength integration is not included.

## Hardware validation checklist

- Hover and move the pen: position updates without contact.
- Touch, draw and lift: tip edges and pressure update.
- Tilt, flip to eraser and press a barrel button: state updates.
- Leave proximity: contact state clears.
- Confirm normal mouse, keyboard and gamepad use is unchanged.

Hardware validation and compilation remain outstanding.
