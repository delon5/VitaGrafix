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
  import hook on `sceDisplaySetFrameBuf`, the counter is advanced on whichever
  thread presents (which for a GXM display-queue title is not the thread running
  the game logic, so a call can land either side of a frame boundary), and while
  nothing is being presented it is not a rate at all - after 256 consecutive
  skipped calls the plugin makes the next one, so a stall costs speed instead of
  hanging the game.

### `args=<n>`

Declares how many arguments the target takes. The wrapper carries `r0`-`r3` and
returns `r0`, so `args=5` or more is refused rather than called in a way that
would drop the arguments the caller put on the stack. A target that returns a
float, a struct or a 64-bit value cannot be rate divided either; write
`retfloat`, `retstruct` or `ret64=...` in place of the return clause to get an
explicit refusal for such a target instead of a corrupted call.

### What the hook must not be pointed at

* A function that draws, presents, or enters a nested frame loop: skipping the
  call would then drop a frame rather than a logic step.
* A function shared with callers that must keep running at full rate - the hook
  is on the function, not on the call site.
* An offset that is not the entry point of a function. taiHEN checks the
  segment index; the plugin additionally refuses an offset outside the module's
  segments and one that is not an instruction boundary, but a plausible wrong
  address inside the segment is hooked wherever it lands, and a hook writes a
  branch into code.

### Limits

* 24 rate divided hooks per title (`Too many hooked functions, limit: 24`).
* One hook per address: repeating an identical line is a no-op, but two lines
  about one address that disagree about the divisor, the substitute return or
  the counting mode are refused as a patch bug.
* A skipped call returns without continuing the taiHEN chain - that is the
  point of the directive, but it means another plugin's hook on the same game
  address would not run for a skipped call.
