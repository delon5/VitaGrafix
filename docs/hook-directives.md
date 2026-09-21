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

## `>rateDivide(<seg>:<offset>, <divisor>, void|ret=<value>[, thumb|arm][, call|frame][, args=<n>][, union])`

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

### `union` - let the target see the polls it skipped

A target that reads an input mask the game rebuilds once per **rendered** frame
cannot simply be rate divided: every bit of that mask is one poll wide, so a
press that lands on a poll the target skipped is overwritten before it is ever
read. That is the trap described under [what the hook must not be pointed
at](#what-the-hook-must-not-be-pointed-at), and `union` is the one way out of
it.

With `union`, for the duration of each call the slot **makes** - and not one
instruction longer - the mask is replaced by the OR of its own poll and the
polls the slot skipped. A press from a skipped poll then arrives one frame
late, which is what the game does natively at its own rate, instead of being
lost. Outside that interval the word holds the true value of the poll it
belongs to, so every full-rate consumer (field, menus, battles, event scripts)
reads exactly what it would read unpatched.

It needs three things, and refuses the line if it does not get them:

* **an `>inputUnion()` line before it** (`'union' needs an '>inputUnion()' line
  before it.`), to say where the sampler and the mask are;
* **`frame` counting** (`'union' needs 'frame' counting.`). Slots that share the
  accumulator have to agree on which polls are live, or a minigame's two rate
  divided halves land on opposite parities and only one of them sees the press.
  Call counting cannot guarantee that; `frame % divisor` does;
* **a divisor of 2 or 3** (`'union' divisor out of range, allowed: 2..3`). The
  window a `union` slot ORs together is `<divisor>` polls wide. The shortest gap
  between two pulses of one button is the sampler's auto-repeat step, three
  polls; at a divisor of 4 a window holds two repeat ticks of one button, the OR
  collapses them into one, and held input silently loses steps. Divisors 2 and 3
  are exactly safe, and are the only ones a 60 Hz unlock produces.

Every `union` slot that installs must also divide the same way (`'union' slots
do not all divide the same way.`). Slots without `union` are not held to it.

What `union` does **not** fix: auto-repeat itself. The repeat machine lives in
the sampler, which runs once per rendered frame, so at `FPS=60` a held direction
repeats twice as fast after half the delay - in the rate divided minigame
exactly as in every other menu of the game. Dividing the sampler is not an
option (that is double-registration for every other consumer), so the honest
claim for a patch built on this is "taps are correct, held input scrolls at
menu speed", not "restored to native".

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

  `>inputUnion()` and the `union` token are the answer to exactly this shape,
  and only to this shape: a mask rebuilt from zero every poll, with one-poll-wide
  bits, produced by a sampler the hook can reach. If the target's input comes
  from somewhere else, or the sampler runs inside the target (a modal sub-loop
  that runs whole frames within one call would advance the accumulator while a
  substitution is live), `union` does not model it - leave that target alone or
  study it separately.
* An offset that is not the entry point of a function. taiHEN checks the
  segment index; the plugin additionally refuses an offset that is not an
  instruction boundary, one outside the module's segments, one in a segment
  without the execute bit (that is data, not code), and every target at all when
  the module's segment table is unavailable - a hook writes a branch into code,
  so an unchecked target is refused rather than armed. But a plausible wrong
  address *inside* the text segment is hooked wherever it lands.

## `>inputUnion(<seg>:<sampler_offset>, <mask_offset>[, thumb|arm])`

Names the game's input sampler and says where in the pad structure the
press/repeat pulse mask sits. It is what a `>rateDivide(..., union)` slot reads
to find that word.

```
>inputUnion(0:0xC466C, 0xC0)                                        # input sampler
>rateDivide(0:0xECC38, 1 + (fps_limit / 60), ret=1, frame, union)   # minigame update
```

`<sampler_offset>` is the function that rebuilds the mask, and it is hooked **at
entry**. `<mask_offset>` is a byte offset into the structure that sampler takes
as its **first argument**, which is how the hook reaches the field without a
per-title global address - the plugin computes `r0 + <mask_offset>` from the
call it is in. It has to be word aligned and no more than `0x1000`
(`Input mask offset is not a word inside the pad`); anything else is a misread
offset rather than a field, and the hook would dereference it on every poll.

The hook does two things, and **writes nothing the game can see**: it publishes
the address of the mask, and it reads the word before the sampler rebuilds it -
the residual of the poll that has just ended.

Reading at *entry* rather than at exit is the part to understand. A full-rate
consumer that acts on a press zeroes the word so that nobody else sees it; by
the time the next poll starts, that swallow has happened, so the residual is 0
and contributes nothing to any later union - exactly as it goes natively, where
a swallow denies the word to every later consumer including the minigame.
Accumulating the mask as just *built* would instead hand the minigame a press
another consumer had already acted on, and it would be acted on twice. Reading
at entry also makes the design independent of where in the frame loop the
sampler sits, which differs between titles.

The engine's `-1` ("input disabled") is mapped to 0 on the way into the
accumulator. That is load-bearing, not defensive: the bit helpers every consumer
goes through begin `if (mask == -1) return 0`, so a `-1` OR'd into a union would
read as every bit set and suppress a whole live poll's input.

The line **hooks nothing on its own**. The sampler hook is installed by the
first `union` slot that needs it, so a patch file that declares the sampler and
then resolves every `union` slot to divisor 1 - the game running at its own rate
- hooks nothing at all. One sampler per title: a second line that declares a
different sampler, mask offset or instruction set is refused as a patch bug, and
an identical repeat is a no-op.

### The one leak, named

When a `union` slot swallows the word on account of a press it got from the
accumulator, that swallow stands - the plugin only puts the true poll value back
if the word is still exactly what it substituted. So a full-rate consumer that
runs *after* the rate divided target in the same frame (a pause or system menu
drawn over a minigame) can lose that poll's own bits. It is bounded to one frame
per press that landed on a skipped poll, and it is never a lost input for the
rate divided target itself.

## Limits

* 24 rate divided hooks per title (`Too many hooked functions, limit: 24`).
* One input sampler per title, and it is not hooked until a `union` slot needs
  it.
* One hook per address: repeating an identical line is a no-op, but two lines
  about one address that disagree about **any** argument - the divisor, the
  substitute return, the counting mode, the instruction set, the declared
  argument count or `union` - are refused as a patch bug. Nothing is resolved in favour of
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
00120 HOOK OK inputUnion 0:000C466C mask=pad+0xC0 thumb install=yes
00121 HOOK OK rateDivide 0:000ECC38 divisor=2 ret=0x1 thumb frame args=4 union=yes install=yes
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
