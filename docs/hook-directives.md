# Hook directives (`>`)

A patch file line that starts with `>` installs a function hook instead of
writing bytes into the game. Hook directives are part of the frame rate option
and are only valid inside an `@FPS` block: a directive under `@FB`, `@IB` or
`@MSAA` is refused (`Hook directive outside of @FPS.`) rather than silently
installing nothing. Like every other patch line, a directive inside a block
whose option is turned off is not parsed at all.

## `>sceDisplaySetFrameBuf_withWait()`

Hooks the display import and waits two vblanks after each present. Installed
only at `FPS=30`.

## `>sceCtrlReadBufferPositive_peekPatched()` / `>sceCtrlReadBufferPositive2_peekPatched()`

Redirects the blocking pad read to the non-blocking peek, so a game that reads
input with the blocking call is not held to 60 reads per second. Installed only
at `FPS=60`.

## `>rateDivide(<seg>:<offset>, <divisor>, void|ret=<value>[, thumb|arm][, call|frame][, args=<n>])`

Calls the game function at `<seg>:<offset>` **1 time in `<divisor>`** instead of
every time. It exists for logic that is written as one step per rendered frame
and that therefore runs at double speed once `@FPS` unlocks the display: some
of those counters cannot be scaled in place, because they are array indices or
pixel offsets, because they are compared against a value computed at run time,
or because the compare sits in a 2-byte instruction whose immediate cannot hold
twice the value.

```
>rateDivide(0:0xECC38, 1 + (fps_limit / 60), ret=1)   # minigame 10 update
```

### `<divisor>`

An interpreter expression, evaluated once when the patch file is read, so it
follows the configuration instead of hard-coding 2. It must evaluate to at
least 1 at **every** frame rate setting the patch file supports; `1` means the
function is called every time, i.e. no hook is installed at all, which is what
makes the directive inert at the game's own rate.

`1 + (fps_limit / 60)` is the recommended form: 1 at `FPS=20` and `FPS=30`, 2 at
`FPS=60`. `fps_limit / 30` also gives 2 at 60 and 1 at 30, but it is **0** at
`FPS=20`, which is not a rate and is refused (`Rate divisor out of range`) - the
error is reported for the whole file, so write a divisor that is defined
everywhere. The largest accepted divisor is 16.

### `void` / `ret=<value>` (required)

A skipped call never reaches the game, so whatever the game reads back is
fabricated by the plugin. There is no safe default, so the line has to say:

* `void` - the game discards the result (0 is returned);
* `ret=<value>` - the game reads the result, and `<value>` is what a skipped
  call hands back. It has to be a value the function itself could have
  returned and that means "carry on" for the caller: a status flag that is read
  as "still running" is the usual case, and returning the "finished" value
  instead would tear the game's state down on the first skipped call.

### `thumb` / `arm`

Instruction set of the target, Thumb unless the line says `arm`. Wrong here
means taiHEN writes a branch decoded the wrong way.

### `call` / `frame`

What is counted, and it is the part to think about:

* `call` (**the default**) - the hook counts its own invocations and makes the
  first call of every group of `<divisor>`. Nothing but the call being decided
  can move that counter, so it cannot drift against the function it is attached
  to, and it cannot freeze while the game is not presenting - loading screens,
  blocking waits and step-wait poll loops all keep working.
* `frame` - the decision is taken from a displayed frame counter instead, so
  that every call made in one frame is made or skipped together. Use it **only**
  for a target that is called several times in one frame on behalf of several
  objects, where those objects have to stay in step with each other. It costs an
  import hook on `sceDisplaySetFrameBuf`, and the counter is advanced on
  whichever thread presents - which for a GXM display-queue title is not the
  thread running the game logic, so a call can land either side of a frame
  boundary.

  While nothing is being presented a frame counter is not a clock, and the
  failure has two halves. Parked on a frame the divisor skips, the hook would
  skip every call for as long as the stall lasts, and a game waiting on that
  function would never make progress. Parked on a frame the divisor makes, the
  hook would make *every* call - no rate division at all, silently, which is the
  double-speed bug the directive exists to fix. Both are bounded the same way:
  after 256 consecutive calls with the frame counter unmoved, the slot falls
  back to counting its own calls until the display moves again, so the game
  keeps the rate it asked for in either phase. `call` mode has none of this to
  worry about, which is why it is the default.

### `args=<n>` - a declaration, not a check

The wrapper carries `r0`-`r3` in and `r0` out, and that is all it can carry. A
target that takes stack arguments, or that returns a float, a struct or a
64-bit value, **must not be pointed at**: a skipped call hands the game a
fabricated `r0`, leaves `r1` alone and drops whatever the caller put on the
stack.

The plugin cannot tell such a target from any other. An ARM prologue does not
say how many arguments a function takes or what it returns, so there is nothing
to derive and **nothing is derived** - a line that omits `args=` is not checked
against anything, and `args=4` is the default because it means "the author did
not say", not "checked and found to be four".

What `args=<n>`, `ret64`, `retfloat` and `retstruct` buy is a way for an author
who *has* worked the shape out to write it down and get the line refused at
parse time (`Hook target ABI is not supported.`) instead of shipping a hook
that corrupts the call. Treat them as an assertion you make, in the same
category as `thumb`/`arm`: the plugin holds you to it and cannot check it for
you. Reading the target's disassembly first is the real guard.

### A target that calls itself

Only the **outermost** call of a nest takes part in the count. A call the hooked
function makes into itself is always made, because it only exists at all when
the outermost call was made; counting it would consume a tick that belongs to no
game step, and skipping it would truncate the recursion and hand every level
below the top the fabricated return value. The plugin tracks this per slot and
per thread, so a call arriving on another thread while one is inside is counted
as the ordinary call it is rather than mistaken for recursion.

The tracking has room for four threads inside one target at once. A fifth
concurrent call - which a self-calling game function does not produce on its own
- is treated as an outermost call, which is what it is; the cost is only that
its own recursion, if any, would be counted.

### What the hook must not be pointed at

* A function that draws, presents, or enters a nested frame loop: skipping the
  call would then drop a frame rather than a logic step.

  There is one shape that is not this, and it has to be argued rather than
  assumed: a **modal sub-loop** that the target enters, runs to completion and
  returns from within the one call, e.g. a menu that blocks until it is
  dismissed. It presents, but it does not put the target inside the game's own
  frame loop, so skipping the *outer* call skips a whole modal episode rather
  than a frame of one. Point the hook at a target that does this only if you
  have read the sub-loop and can say that is what it does.
* A function shared with callers that must keep running at full rate - the hook
  is on the function, not on the call site.
* **A function whose behaviour depends on an input edge that is sampled
  somewhere else.** This is the trap that killed the directive's first intended
  use. If the game's input sampler rebuilds a press/repeat mask once per
  *rendered* frame and the mask's bits are one poll wide, then a target that
  reads that mask and is called 1 time in 2 simply never sees half the presses -
  the unread mask is overwritten by the next poll. That is lost input, not late
  input, and no divisor fixes it. Check what the target reads, and where it is
  produced, before pointing the hook at it.
* An offset that is not the entry point of a function. taiHEN checks the
  segment index; the plugin additionally refuses an offset that is not an
  instruction boundary, one outside the module's segments, one in a segment
  without the execute bit (that is data, not code), and every target at all when
  the module's segment table is unavailable - a hook writes a branch into code,
  so an unchecked target is refused rather than armed. But a plausible wrong
  address *inside* the text segment is hooked wherever it lands.

### Limits

* 24 rate divided hooks per title (`Too many hooked functions, limit: 24`).
* One hook per address: repeating an identical line is a no-op, but two lines
  about one address that disagree about **any** argument - the divisor, the
  substitute return, the counting mode, the instruction set or the declared
  argument count - are refused as a patch bug. Nothing is resolved in favour of
  whichever line came first. (Two lines that both resolve to divisor 1 install
  nothing and are not compared.)
* A skipped call returns without continuing the taiHEN chain - that is the
  point of the directive, but it means another plugin's hook on the same game
  address would not run for a skipped call.

## Checking a directive before it ships

`tests/test_patchlist` parses and installs every `>` line of a patch file with
the plugin's own parser (`src/patch_hook.c`, built against the stub
`vitasdk`/`taihen` headers in `tests/stubs`), so a directive is checked by the
code that will run it rather than by a second parser that can disagree with it.
Each one is reported with the arguments it resolved to at the configuration
being checked:

```
$ tests/test_patchlist patch/PCSG/PCSG00490.txt --fps 60 --seg 0:0x21B3F0
00121 HOOK OK rateDivide 0:000ECC38 divisor=2 ret=0x1 thumb call args=4 install=yes
```

`install=no` means the configuration resolves the directive to something the
plugin does not install - a divisor of 1, or an import hook that belongs to
another frame rate setting. A directive that would fail on hardware is reported
as an error with the plugin's own status, and the tool exits non-zero:

```
00121 HOOK ERR 12 23 Rate divisor out of range, allowed: 1..16 | >rateDivide(0:0xECC38, fps_limit / 30, ret=1)
```

`--seg <index>:<size>[:<perms>]` passes the module's segments as VGDump's
`info.txt` prints them; `<perms>` carries the ELF program header flags and
defaults to `5` (read+execute), so a data segment is written `--seg 1:0x5CBC0:6`.
Without any `--seg` the checker has no segment table, and then - exactly as the
plugin does on a console where `sceKernelGetModuleInfo()` failed - every
`>rateDivide()` target is refused with `No module segment info, hook target
cannot be checked.` rather than reported as fine. So always pass the segments of
the build you are checking. `VG_LOG=1` in the environment prints the plugin's
own log alongside the report.

Run the directives against every frame rate setting, not just the one the patch
was written for: a divisor that collapses to 0, a conflicting pair of lines and
a full slot pool are all decided by the configuration.
`tests/test_patchlist_directives.sh` freezes the expected report for
`tests/patchlist_directives.txt`, which holds one example of every form and
every refusal.
