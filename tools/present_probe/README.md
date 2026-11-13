# Present probe (issue 049, P2.9 step 1)

A test window, not the game. It contains no game code and no game data.

It opens a window through the renderer library's own presenter (Granite's `Vulkan::WSI`) and shows a moving test
pattern at chosen instants. It answers two questions before the port's window is rebuilt:

1. How does the system show such a window (a plain Vulkan swap chain, or on Windows one through DXGI; vsync on or
   off; normal window or borderless), and what does the driver report about when each picture was shown?
2. How do a presenter and a renderer take turns on one graphics device? A second thread stands in for the GS
   thread: it draws the pictures, keeps a command buffer open while it works, sends extra work and moves its frame
   contexts on by the port's rule (move on only when that does not wait).

## Build

A project of its own with a build folder of its own; the game's program is not rebuilt by it.

    cmake -S tools/present_probe -B app/build-present-probe -G Ninja \
        -DDW5XL_PARALLEL_GS_DIR=<the paraLLEl-GS checkout the game is built with> \
        -DDW5XL_GLFW_DIR=<raylib's sources>/src/external/glfw
    cmake --build app/build-present-probe --target dw5xl_present_probe

On Windows the library's DXGI way is compiled in here (the game's build has it off).

## Run

    dw5xl_present_probe [--seconds N] [--hidden] [--borderless] [--size WxH]
        [--present vsync|unlocked|immediate|mailbox] [--dxgi] [--rate HZ] [--picture WxH]
        [--devices 1|2] [--open-ms X] [--load N] [--lead-ms X] [--hand-ms X] [--target]
        [--latency N] [--low-latency] [--per-picture] [--no-feedback]

- `--hidden`: the window is never shown (for runs that only read the turn-taking; a hidden window says nothing
  about the display). Use it with `--no-feedback`: a hidden window's pictures never reach a display, so the
  driver's reports never come, and with them asked for the presents fail or wait for ever (measured on Windows
  with an NVIDIA driver, 2026-11-13).
- `--no-feedback`: do not ask the driver when each picture was shown.
- `--present`: vsync on (`vsync`), or off as tearing allowed (`unlocked`), always tearing (`immediate`) or never
  tearing (`mailbox`).
- `--dxgi`: Windows only; the swap chain goes through DXGI, the way Direct3D games present.
- `--rate`: pictures a second on the time line; `--picture`: their size (default 2560x2048, the port's 4x).
- `--devices 2` (default): the presenter has a device object of its own on the same graphics device.
  `--devices 1`: one device object for both.
- `--open-ms`, `--load`: how long the producer keeps a command buffer open for each picture, and how many extra
  pieces of work it sends.
- `--lead-ms`, `--hand-ms`: how long before its instant a picture is made, and handed to the presenter.
- `--target`: ask the driver for a target time for each picture (`VK_EXT_present_timing`).
- `--latency N`, `--low-latency`: the presenter's own waiting for earlier presents.
- `--per-picture`: a line for every picture made, shown and reported by the driver.

## Output (standard output)

- `[probe:setup]`: the graphics card, the window, what kind of swap chain was made (`present=vulkan|dxgi`), the
  options.
- `[probe:window]` (Windows, before and after the run): whether the window can be seen at all (shown, not
  minimised, in the foreground, on top at its centre) and the compositor's pace.
- `[probe:display]` (Windows, visible window): the gaps between the display's own refreshes during the run, from
  the kernel's wait for each refresh. On a display that follows the pictures they are the pictures' gaps; at a
  fixed rate they are that rate's period. `problem=the refresh wait was refused` means the screen is off: then
  no picture reaches a display and the run measures nothing about one.
- `[probe:driver]`: whether the driver numbers presents and reports the display's refresh.
- `[probe:count]`: pictures made, shown, left out; how often the producer's frame context was held back.
- `[probe:sum]`: times in ms (middle value, 99 % point, longest, how many over 0.5 and 2 ms):
  the producer's work, its frame context step and its wait for the queue; the presenter's wait for open command
  buffers, `begin_frame`, its drawing, `end_frame` and its wait for the queue; when a picture was presented and
  when the driver says it left for the display, against its instant.

- `[probe:hang]`: the run did not end within five seconds after its time line; it says where each thread was,
  and the program ends with exit code 3.

The library's own messages go to standard error.

Never open its window on a PC whose user did not ask for it: use `--hidden` for unattended runs.
