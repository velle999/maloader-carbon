# maloader-carbon

A fork of [maloader](https://github.com/shinh/maloader) that runs Carbon
applications built for Mac OS X on Intel, starting with one: the i386 build
of *Halo: Combat Evolved* (Halo Universal 2.0, Westlake Interactive and
MacSoft, 2006), running natively on 32-bit x86 Linux with its own OpenGL
renderer.

The loader maps the Mac executable into memory, binds its imports and starts
it. Where Mac OS X would supply CoreFoundation, Carbon, AGL or IOKit, this
project supplies the calls the game makes, over glibc, SDL 2 and the
system's OpenGL. No part of the game is included; you need your own copy.

## Status

Work in progress. Every one of the game's 724 imports binds. It runs its
startup checks (CPU, memory, QuickTime and OpenGL versions, an OpenGL context
probe, video memory, disk space), finds its disc, shows its EULA and asks for
its product key, which are answered without being shown (see
Configuration), loads its maps and shaders, and draws its main menu, with
its music. On a Pentium 4 with a GeForce 7600 GS and NVIDIA's 304 driver its
campaign has been played for an hour at a stretch, full screen and in a
window, and its time demo runs at 38 to 68 frames a second. Loading a new
part of a level still stalls a frame for a tenth of a second or more. The
intro movies are not implemented.

An import with no implementation is bound to a guard page, so its first use
stops the program with its name, its caller and the registers.

Halo has a fault of its own that the loader steps over. Before it saves a
checkpoint, it looks for dangerous effects near the player, and it reads the
node matrices of an effect's parent object even after that object has been
deleted. That read gets zeros instead of stopping the game, and stderr says
so; see `hle/game_faults.c`.

## Requirements

- 32-bit x86 Linux with glibc. The game's i386 code needs SSE2.
- SDL 2 (`libSDL2-2.0.so.0`) and an OpenGL driver (`libGL.so.1`). SDL's
  headers are in `third_party/`, so only the libraries are needed.
- `vm.mmap_min_addr` of 4096 or lower. Mac OS X i386 executables are not
  position-independent, and their `__TEXT` segment starts at 0x1000:

      sudo sysctl -w vm.mmap_min_addr=4096

## Build and run

    make ld-mac libmac.so
    cd /path/to/Halo.app/Contents/MacOS
    HLE_CD_PATH="/path/to/Halo Universal" /path/to/maloader-carbon/ld-mac ./Halo

`ld-mac` is linked `-no-pie`. A 32-bit kernel loads position-independent
executables at 0x400000, which is inside the game's image.

## Installer and launcher

    make install            # into ~/.local; PREFIX=/usr/local for everyone

puts the loader in `PREFIX/lib/maloader-carbon`, and two programs with menu
entries in `PREFIX/bin`. Both are Python 3 with GTK 3 (PyGObject): without
arguments they open a window, and with them they work on the command line.
`make test-frontend` checks them without a display.

- **halo-ce-installer** installs Halo: Combat Evolved from its Mac release,
  Halo Universal 2.0: choose the disc image (`.dmg`, `.img`, `.iso`, read
  with 7-Zip) or a folder with the disc's files, and where to install
  (`~/Games/HaloCE` by default). It unpacks the game's installer package with
  gzip and cpio, keeps the disc's own files in `Disc/Halo Universal` for the
  game's disc check, makes the game's Mac home in `Home`, and can bring over
  the saved games and settings of an earlier install. The game refuses a
  folder with a period in its name anywhere in its path, so the installer
  does too. It adds Halo to the applications menu, and to the desktop if
  asked, with the game's own icon.

      halo-ce-installer --source ~/Downloads/HaloMac.dmg --destination ~/Games/HaloCE

- **carbon-launcher** runs Carbon applications for Intel (a bundle with
  32-bit Intel code): add a `.app`, set its Mac home, a folder standing in for
  its disc, full screen or a window, sound, a detailed log and any of the
  settings below, then launch it, read its last log, or give it a menu entry.
  Each app's settings are a JSON file in `~/.config/maloader-carbon/apps`, and
  its last five logs are in `~/.local/state/maloader-carbon/logs`.

      carbon-launcher add ~/Applications/Some.app --display window --menu-entry
      carbon-launcher run some HLE_AUDIO_FRAMES=4096

  An app that asks for a product key (Halo does, the first time) is asked
  for it in a dialog, or on the terminal, only while its preferences hold no
  accepted key. The key goes to that one run as `HLE_DIALOG_<id>`; the game
  keeps it, and the launcher neither stores nor logs it.

## Configuration

- `HLE_CD_PATH`: the game checks that its disc is in the drive. Point this
  at a directory with the disc's contents, such as the disc image extracted.
  It appears as a mounted CD named after the directory (the Mac release's is
  `Halo Universal`), or after `HLE_CD_NAME` when that is set.
- `HLE_WINDOWED=1` plays in a window rather than changing the screen's mode,
  and ticks the game's own "Play in a window" setting; `HLE_WINDOWED=0`
  unticks it, for full screen. Without either, the game's saved setting
  stands.
- `HLE_VRAM_MB` is the video memory the renderer reports, 256 by default.
- `HLE_SOUND=0` keeps the game silent. Otherwise its sound goes to SDL's
  default audio device, or to the one `SDL_AUDIODRIVER` names.
  `HLE_AUDIO_FRAMES` is the device's buffer in frames, 2048 (46 ms) by
  default; a slow machine whose sound breaks up needs more, at the cost of
  delay.
- Dialogs are answered without being shown. When the game runs a dialog
  from its NIB, `HLE_CONTROL_<code>=<value>` first sets the control with that
  four-letter signature (`HLE_CONTROL_FSAA=2` picks the second FSAA setting),
  then the default button's command is sent, or the one `HLE_DIALOG_<name>`
  gives (`HLE_DIALOG_EULA=not!` declines the licence). stderr says what was
  chosen. Alerts print their text and take their default button.
- `HLE_CONTROL_Time=1` ticks "Run Time Demo" in the game's startup dialog: the
  game flies through four campaign levels with nobody at the controls and
  quits, which makes a repeatable benchmark. `HLE_HOLD_POINTER=0` leaves the
  desktop's pointer free even while the game's window has the focus, for
  runs like that.
- Classic dialogs from `DLOG` resources are answered the same way:
  `HLE_DIALOG_<id>` fills their text fields, split at dashes, and their first
  button is hit; without it, or when the game refuses the text, their second
  button (Cancel or Quit) is. The game asks for its product key, printed on
  the back of the Halo manual, in `DLOG` 10001 (10002 in German, 10003 in
  French): `HLE_DIALOG_10001=XXXX-XXXX-XXXX-XXXX`. The game saves a key it
  accepts with its preferences.
- `HLE_TRACE=1` logs lookups and decisions, and `LD_MAC_LIST_UNDEFINED=1`
  lists the imports with no implementation.
- `LD_MAC_TRACE_IMPORTS=1` logs each implemented import the first time the
  game calls it, with the address of the call, and `LD_MAC_TRACE_IMPORTS=all`
  logs every call.
- `HLE_FRAME_DUMP=<directory>` saves the first frame the game draws, and
  every 600th after it, to `frame-<n>.ppm` in that directory.
- Scroll Lock saves the next frame the game draws as a BMP file in the
  directory `HLE_SCREENSHOTS` names, `~/halo-screenshots` by default, and so
  does Print Screen where the desktop leaves it to the window (XFCE opens its
  own screenshot dialog on Print Screen). The game's own screenshots need a
  command-line switch its Mac startup never passes.
  `HLE_SCREENSHOT_FRAME=<n>` saves the nth frame, for runs with nobody at the
  keyboard. Print Screen, Scroll Lock and Pause reach the game as F13, F14 and
  F15, the keys a Mac keyboard has there.
- With `HLE_TRACE=1`, every 600th frame logs the frame rate over the last
  600, the longest frame and how many took over 50 and 100 ms, how a frame's
  time divides (the game's thread on the CPU, all threads on the CPU with the
  sound mixer's, and waiting in the swap), and how many times a GL context
  was made current somewhere new, each a round trip to the X server.
  `HLE_GL_STATS=1` adds the game's draw calls, vertices and indices, texture
  uploads and copies, `glFinish`, `glFlush` and `glReadPixels` calls, texture
  binds and ARB program parameters per frame.
- `HLE_PROFILE=<file>` samples, from the first frame, where each millisecond
  of CPU time goes and which of the game's functions led there, and writes
  the counts to that file at the exit, or at a crash.
  `tools/profile_report.py <file> <disassembly>` sums them up by thread,
  library and function.
- `HLE_LONG_FRAMES=<ms>` reports each frame that takes at least that long:
  how much of it the game's thread spent on the CPU and in the swap, where
  that CPU time went, grouped by the game's code it was called from, and
  what system calls the thread was blocked in, and from where. The blocked
  time is read every millisecond from `/proc/self/task/<id>/syscall` by a
  thread of its own, so the game's thread is not interrupted for it.
- `HLE_PROBE=<text>` reports the GL state at the draws made with a vertex
  program whose source holds `<text>` (`*` for every draw), once for each
  distinct state: the vertex attributes and their first values, where the
  first vertices land on the screen, the texture units with their combine
  functions and textures, blending, and the texture stage states of the
  game's Direct3D layer for the first two stages. `HLE_PROBE_COUNT` limits
  the states reported (64 by default), and
  `HLE_PROBE_TEXTURES=<directory>` saves each enabled texture as a PAM file
  and a screenshot of the frame.
- `HLE_VAR=0` leaves out the emulated vertex array extensions, and
  `HLE_COMBINE3=0` the emulated ATI combine functions (see Windows,
  graphics, input and sound).
- `HLE_RECOVER=0` lets the game's own fault (see Status) stop it with a crash
  report instead of stepping over it.
- `HLE_GAME_NICE` is how far the game's main thread gives way to the sound
  mixer's thread: a nice value added to it once the audio device is open, 4
  by default, 0 for none. With the kernel's autogroup scheduling it weighs
  only against the game's own threads. Without real-time priority for the
  mixer, a mixer that runs late while the game draws makes the sound pop.

## CoreFoundation

`hle/` implements the 77 CoreFoundation calls the game imports, with the
Darwin i386 calling convention: strings (including the compiler's constant
strings), arrays, dictionaries, numbers, data, XML property lists, URLs,
bundles and localized strings, preferences, UUIDs and character sets.

- Preferences are XML property lists in
  `~/.local/share/halo-mac-loader/home/Library/Preferences/`, or in
  `$HALO_MAC_HOME/Library/Preferences/` when that is set.
- `HLE_TRACE=1` logs bundle, resource and preference lookups.
- `make tests/cf_test && tests/cf_test /path/to/Halo.app` checks it,
  including against the game's own bundle.

## Carbon

- The File Manager puts the Mac's startup volume at / and, when
  `HLE_CD_PATH` is set, the disc as a second volume. FSRefs, FSSpecs with
  classic partial pathnames, catalog information, directory iteration, forks
  and the parameter-block calls the game uses are implemented. Carbon keeps
  68K structure alignment on Intel; `hle/carbon.h` asserts it against offsets
  read from the game's code.
- Paths the game opens through the C library are read the way a Mac reads
  them: without regard to case, since the game opens `shaders/vsh/...` where
  the folder is `Shaders`, and with `/Volumes/<disc name>` as the disc's
  directory and `/Volumes/Macintosh HD` as /.
- Folders a Mac keeps in the home, such as Preferences and Application
  Support, are under `~/.local/share/halo-mac-loader/home`. System folders are
  under `~/.local/share/halo-mac-loader/root`. A folder every Mac has, such as
  Documents, is made the first time a program looks it up.
- Resource forks come from AppleDouble companion files (`._name`). The game's
  `EULA.rsrc` keeps its resources that way, so extract the `._` files along
  with the rest of the application.
- The Resource Manager reads those forks, and the Memory Manager provides
  pointers and handles. Dates, clocks and the Gestalt selectors the game asks
  for are answered.
- `make tests/files_test && tests/files_test /path/to/Halo.app` checks it.

## Windows, graphics, input and sound

- CGL and AGL contexts are SDL OpenGL contexts. Code built with
  `aglMacro.h` calls through a context's dispatch table, which holds thunks
  generated from the 10.4 SDK's `gliDispatch.h` by `tools/gen_gl_dispatch.py`;
  the game's direct `gl` imports bind to the system's libGL. The renderer
  described is one accelerated NVIDIA renderer.
- AGL pbuffers are framebuffer objects: the texture `aglTexImagePBuffer`
  names is the pbuffer's image, and `aglSetPBuffer` draws into it. Without
  them Halo drew a 128x128 effect texture into a corner of the screen. After
  each pbuffer it draws in, the game detaches its window from the context and
  attaches it again. A detached context stays bound to its window meanwhile,
  as moving it cost two round trips to the X server each time, and a window
  destroyed first moves its contexts to a hidden window.
- The game copies `GL_EXTENSIONS` into a 4096-byte buffer, which a newer
  driver's list overruns, so it sees only the extensions whose names its
  executable contains, plus `GL_EXT_texture_rectangle` where the driver has
  the ARB extension of the same enumerants.
- Apple's ARB program assembler accepts an `ALIAS` of a binding, as in
  `ALIAS oPos = result.position;`, and all 90 of the game's vertex programs
  use one. The ARB grammar aliases only declared variables, so NVIDIA's
  driver refuses those programs. As each program is loaded, such an `ALIAS`
  becomes the declaration it stands for, `OUTPUT oPos = result.position;`
  (`ATTRIB` for a vertex binding). A program the driver still refuses is
  reported on stderr with the driver's message.
  `make tests/arb_test && tests/arb_test GameData/Shaders/vsh/*.vsh` checks
  the rewrite, with the game's own programs when they are named.
- Halo draws its vertex buffers the fast way only where OpenGL has
  `GL_APPLE_vertex_array_range` and `GL_APPLE_fence`; elsewhere it hands the
  driver client arrays, which NVIDIA's driver copies on the CPU for every
  draw. Those two and `GL_APPLE_vertex_array_object` are emulated with ARB
  buffer objects: a vertex array object keeps its own client arrays, what the
  game flushes from a range it marked for caching on the GPU goes to a buffer
  object for that span, and an array pointer into a flushed span reads from
  its buffer object. Ranges left to Apple's default or marked shared, whose
  memory the Mac's GPU read directly and the game did not always flush, stay
  client arrays; as buffer objects their effects smeared through walls. On a
  Pentium 4 the time demo runs 10 to 35% faster with them. Where an array
  pointer leads is looked up again only when a range, span or storage hint
  has changed. `HLE_VAR=0` leaves them out. `make tests/var_ranges_test && tests/var_ranges_test` checks the
  ranges they keep.
- Apple's OpenGL has `GL_ATI_texture_env_combine3` on every renderer, and
  Halo's Direct3D layer sets its `MODULATE_ADD_ATI` combine function for
  `D3DTOP_MULTIPLYADD` without looking for the extension, as it does for
  bullet marks. NVIDIA's driver lacks it and refuses the function, and the
  texture unit went on with the function it had before: the marks were drawn
  in their color alone, as squares. Where the driver has
  `GL_NV_texture_env_combine4`, a unit the game gives an ATI function
  combines in NVIDIA's four-argument form, `Arg0 * Arg1 + Arg2 * Arg3`, with
  the game's arguments rearranged to the same result, and goes back to the
  usual form when the game changes the function. `HLE_COMBINE3=0` leaves it
  out. `make tests/combine3_test && tests/combine3_test` checks the result
  against the driver; it needs a display.
- Displays, their modes and the main GDevice describe SDL's display 0. A mode
  switch resizes the game's window, and a window covering a captured display
  goes full screen unless `HLE_WINDOWED` is 1. Gamma tables are recorded,
  not applied.
- Windows, controls and menus come from the application's NIB
  (`objects.xib`) and are kept as records the game queries; only a window the
  game draws in with OpenGL is real. The Carbon Event Manager, window groups,
  the Process Manager and Multiprocessing Services are implemented. SDL input
  arrives as Carbon keyboard and mouse events with Mac key codes, taken in
  even by calls that do not wait for events. While the game's window has the
  focus and the game has taken the mouse (it hid the pointer, or it warps
  it), the pointer is held in relative mode: motion moves the position the
  game reads, and the game's warps move only that position.
- QuickDraw keeps ports, GWorlds with real pixels, colors and rectangles.
- Sound Manager channels play through one SDL audio device. A channel's
  buffers are mixed at their own rate times the channel's rate multiplier,
  with its volume and amplitude, and commands queued behind a buffer wait
  until it has played. A sum past three quarters of full scale bends toward
  full scale instead of clipping. Uncompressed 16-bit samples are
  little-endian, as on an Intel Mac; compressed formats are silent.
  `make tests/sound_test && tests/sound_test` checks the mixing.
- QuickTime reports no movies, so the intro is skipped. IOKit shows the
  disc and no HID devices.

## Tools

- `tools/asm_annotate.py BINARY DISASSEMBLY LO HI` prints part of an
  `llvm-objdump --macho -d` listing with the C strings, CFStrings and
  four-character codes its constants name.
- `tools/import_walk.py DISASSEMBLY ADDR [DEPTH] [IMPLEMENTED]` lists the
  imports a function reaches, in the order a walk meets them, marking the
  ones with no implementation.
- `tools/profile_report.py PROFILE DISASSEMBLY [ROWS]` sums up an
  `HLE_PROFILE` file: CPU time by thread and by mapping, the functions and
  library symbols that took the most, and for time spent outside the game,
  the game's functions that called out.

## Changes from maloader

- Pre-10.5 i386 images: `__IMPORT,__jump_table` stubs, external relocations,
  LOCAL and ABSOLUTE indirect symbols, and a Darwin initial stack with `envp`
  and `apple[]`.
- Pre-10.5 i386 libraries, such as the PhysX libraries a game bundles: their
  exports come from the symbol table, since they have no export trie; their
  local relocations and local non-lazy pointers slide with the library, which
  is linked at address 0; and every image is mapped before any is bound, since
  a library may import from the executable or from a library mapped after it.
  A library's initializers run before those of the image that loads it.
- Imports bind by the library each one names (the two-level namespace): a
  game that exports its own `towlower` still gets the system's where it
  imports it from libSystem. Lookups made at run time through
  `CFBundleGetFunctionPointerForName` or `dlsym` on a system library never
  return the game's own functions.
- A Linux library named by its development link (`libz.so`) is found by its
  runtime name (`libz.so.1`) when the development package is not installed.
- An undefined symbol marked `N_REF_TO_WEAK` binds to the library that defines
  it. The bit is `N_WEAK_DEF` on a defined symbol; taking it for that bound
  `operator new` and `delete` to address 0.
- `libmac` additions for 10.4-era executables: `__sF`, keymgr, the `errno`
  variable, `bootstrap_port`, a monotonic `mach_absolute_time`.
- An unimplemented import stops the program with its name, its caller, the
  registers and the frame-pointer chain. Any other fault is reported the same
  way, with the mappings that hold the faulting code and address and the code
  addresses found on the stack.
- A fixed `mmap` that replaces memory mapped by something other than the
  program says so on stderr, with the mappings it replaces; Linux replaces
  them silently.
- The program's `dynamic_cast` calls are answered from a cache for its first
  thread: the answer depends only on the object's vtable pointer and the two
  types, and the game's Direct3D layer casts thousands of times a frame, so
  libstdc++'s walk of the class hierarchy was over 3% of its CPU time.
  Together with the vertex array lookups above, the time demo runs about 2%
  faster. `HLE_CAST_CACHE=0` leaves the cache out, `HLE_CAST_STATS=1` counts
  its answers, and `make tests/cast_test && tests/cast_test` checks it across
  multiple, virtual and ambiguous bases.
- glibc's allocator keeps every block in the heap, never gives the heap back,
  and grows it 64 MB at a time. The game draws a little past the ends of its
  vertex buffers, and NVIDIA's driver, copying them, faulted where a buffer
  ended at the top of the heap.
- `sysctl` and `sysctlbyname` answer as a 10.4.9 Intel Mac with this
  machine's CPU and memory; maloader's `sysctl` aborted on most queries.
- Lookups the program makes at run time, through
  `CFBundleGetFunctionPointerForName` or `dlsym`, resolve the way its imports
  from system libraries do. `dlopen` of a Mac library that is not present as a Mach-O file returns
  a handle for exactly that, instead of exiting.

---

## maloader (upstream README)

This is a userland Mach-O loader for linux.

## Installation

```bash
$ make release
```
## Usage

```bash
$ ./ld-mac mac_binary [options...]
```
You need OpenCFLite (http://sourceforge.net/projects/opencflite/)
installed if you want to run some programs such as dsymutil.
opencflite-476.17.2 is recommended.

## How to use compiler toolchains of Xcode

Recent Xcode toolchains are built by clang/libc++. This means some C++
programs do not work with ld-mac linked against libstdc++. To build
ld-mac with clang/libc++, run

```bash
$ make clean
$ make USE_LIBCXX=1
```
You need compiler toolchain binaries on your Linux. unpack_xcode.sh in
this repository helps you to set up them if you have a dmg package of
Xcode. This script was checked with Xcode 4.3.3, 4.4.1, 4.5.2, 4.6.2,
and 5.0.1. It would work even on other Xcode releases, but you may
need to modify the script by yourself. How things are stored in dmg
packages heavily depend on the version of Xcode. You can use this like

```bash
$ ./unpack_xcode.sh ~/Downloads/xcode_5.0.1_command_line_tools*.dmg
```

This will create xcode_5.0.1_command_line_tools*/root. We will call
this directory as $ROOT.

```bash
$ ./ld-mac $ROOT/usr/bin/clang --sysroot=$ROOT -c mach/hello.c
$ file hello.o
hello.o: Mach-O 64-bit x86_64 object
```
To link binaries on Linux, you also need necessary dylibs in
root/usr/lib. For example, simple hello world program requires
/usr/lib/libSystem.dylib and /usr/lib/system. Do something like

```bash
(mac)$ tar -cvzf sys.tgz /usr/lib/libSystem* /usr/lib/system
(mac)$ scp sys.tgz $USER@linux:/tmp
(linux)$ cd $ROOT && tar -xvzf /tmp/sys.tgz
```
Also note that it seems ld does not like the version number of
Xcode. So you need to move xcode_5.0.1_command_line_tools*/root to
somewhere else. For example:

```bash
$ ln -sf xcode_5.0.1_command_line_tools_10.9_20131022/root
$ ./ld-mac $ROOT/usr/bin/clang --sysroot=$ROOT -g mach/hello.c
$ ./ld-mac ./a.out
Hello, 64bit world!
```
## How to use compiler toolchains of Xcode 3.2.6

Get xcode_3.2.6_and_ios_sdk_4.3__final.dmg (or another xcode package).

```bash
$ git clone git@github.com:shinh/maloader.git
$ ./maloader/unpack_xcode.sh xcode_3.2.6_and_ios_sdk_4.3__final.dmg
$ sudo cp -a xcode_3.2.6_and_ios_sdk_4.3__final/root /usr/i686-apple-darwin10
$ cd maloader
$ make release
$ ./ld-mac /usr/i686-apple-darwin10/usr/bin/gcc mach/hello.c
$ ./ld-mac a.out
```

## How to run Mach-O binaries using binfmt_misc
```bash
$ ./binfmt_misc.sh
$ /usr/i686-apple-darwin10/usr/bin/gcc mach/hello.c
$ ./a.out
```
To remove the entries, run the following command:

```bash
$ ./binfmt_misc.sh stop
```

## How to try 32bit support

```bash
$ make clean
$ make all BITS=32
```
If you see permission errors like:

```bash
ld-mac: ./mach/hello.c.bin mmap(file) failed: Operation not permitted
```
you should run the following command to allow users to mmap files to
addresses less than 0x10000.

```bash
$ sudo sh -c 'echo 4096 > /proc/sys/vm/mmap_min_addr'
```
Or, running ld-mac as a super user would also work.

## How to run both 64bit Mach-O and 32bit Mach-O binaries
```bash
$ make both
$ ./binfmt_misc.sh start `pwd`/ld-mac.sh
$ /usr/i686-apple-darwin10/usr/bin/gcc -arch i386 mach/hello.c -o hello32
$ /usr/i686-apple-darwin10/usr/bin/gcc -arch x86_64 mach/hello.c -o hello64
$ /usr/i686-apple-darwin10/usr/bin/gcc -arch i386 -arch x86_64 mach/hello.c -o hello
$ ./hello32
Hello, 32bit world!
$ ./hello64
Hello, 64bit world!
$ ./hello
Hello, 64bit world!
$ LD_MAC_BITS=32 ./hello
Hello, 32bit world!
```

## Which programs should work

OK

- gcc-4.2 (link with -g requires OpenCFLite)
- otool
- nm
- dyldinfo
- dwarfdump
- strip
- size
- dsymutil (need OpenCFLite)
- cpp-4.2
- clang

not OK

- llvm-gcc
- gnumake and bsdmake
- lex and flex
- ar
- m4
- gdb
- libtool
- nasm and ndisasm (i386)
- mpicc, mpicxx, and mpic++

## Notice

- Running all Mac binaries isn't my goal. Only command line tools such
  as compiler tool chain can be executed by this loader.
- A slide about this: http://shinh.skr.jp/slide/ldmac/000.html

## TODO

- read dwarf for better backtracing
- make llvm-gcc work
- improve 32bit support
- handle dwarf and C++ exception

## License

Simplified BSD License or GPLv3.

Note that all files in "include" directory and some files in "libmac"
were copied from Apple's Libc-594.9.1.
http://www.opensource.apple.com/release/mac-os-x-1064/

See http://www.gnu.org/licenses/gpl-3.0.txt for GPLv3.
