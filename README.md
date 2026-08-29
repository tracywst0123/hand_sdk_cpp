# hand_sdk_cpp

User-space EtherCAT SDK for the `hand_200` dexterous hands (21 joints per hand,
left and right). It owns a real-time thread that exchanges process data with the
hands every millisecond on a Distributed-Clocks grid, and exposes a small,
thread-safe API that your code can call at any rate from any thread.

- **Public API**: `hand_sdk::Api` — connect, read joint states, send joint
  commands, and make request/response service calls (mode, gains, motor state).
- **No EtherCAT in your code**: the public headers contain no SOEM or EtherCAT
  types. You never link SOEM yourself.
- **Design notes**: see [notes.md](notes.md) for the wire protocol, DC bring-up,
  and internal layering.

Ways to use it, covered below:

| # | You want to… | Go to |
|---|---|---|
| 1 | Get the source, build it, run the examples | [Approach 1](#approach-1-build-the-whole-package) |
| 2 | Link it into your own C++ project | [Approach 2](#approach-2-use-it-from-a-c-project) |
| 3 | Link it into a ROS 2 C++ node | [Approach 3](#approach-3-use-it-from-a-ros-2-package) |
| — | Hand the built library to someone else | [Distributing](#distributing-the-built-sdk) |

---

## Prerequisites

Common to all three approaches.

- Linux, CMake ≥ 3.28, a C++17 compiler.
- **SOEM** (Simple Open EtherCAT Master) built as a static library. It is the
  only external dependency, and it is needed **only to build this SDK** — not by
  anything that consumes it.

```bash
git clone https://github.com/OpenEtherCATsociety/SOEM.git ~/tools/SOEM
cmake -S ~/tools/SOEM -B ~/tools/SOEM/build/default
cmake --build ~/tools/SOEM/build/default
# produces ~/tools/SOEM/build/default/libsoem.a
```

This package looks for SOEM at `$HOME/Documents/tools/SOEM` by default. Point it
elsewhere **without editing any file** using either:

```bash
cmake -B build -DSOEM_ROOT=/path/to/SOEM     # CMake option
export SOEM_ROOT=/path/to/SOEM               # or environment variable
```

If SOEM is missing, configuration fails with a message telling you exactly this.

### Runtime permissions (required to talk to hardware)

EtherCAT needs a raw socket, and the cyclic thread needs real-time scheduling.
Grant both to **your final executable** (not to the SDK, and not to the system
Python):

```bash
sudo setcap cap_net_admin,cap_net_raw,cap_sys_nice+ep ./your_program
```

Without `cap_net_admin` or `cap_net_raw`, `connect()` fails to open the
interface. Without `cap_sys_nice`, everything still runs but the cycle jitters
more and logs a warning. Running as root also works but is not recommended.

---

## Approach 1: build the whole package

Use this to develop the SDK itself, or to run the bundled examples against real
hardware.

```bash
cd hand_sdk_cpp
cmake -B build                    # add -DSOEM_ROOT=... if SOEM is elsewhere
cmake --build build
```

This produces:

| Artifact | What it is |
|---|---|
| `build/libhand_sdk.a` | the SDK, for local linking |
| `build/libhand_sdk_bundled.a` | the SDK **with SOEM merged in** (this is what gets installed) |
| `build/hand_sdk_example` | example: connect, bring up both hands, stream states |
| `build/bit_io_test` | diagnostic for a simple bit-IO EtherCAT slave |

Run the example (edit `interface_name` in `examples/main.cpp` first, or copy it
as a starting point for your own program):

```bash
sudo setcap cap_net_admin,cap_net_raw,cap_sys_nice+ep build/hand_sdk_example
./build/hand_sdk_example
```

> **Safety**: `examples/main.cpp` enables control and streams zero position
> commands, so the hands will actively hold position 0. Keep them free to move
> on a first run.

---

## Distributing the built SDK

You do **not** need to give anyone the source. Building once produces a
self-contained prefix that other projects consume with `find_package`.

### Step 1 — install to a prefix

Run this **from the package root** (`build` is a relative path to the build
directory you created above):

```bash
cd hand_sdk_cpp
cmake --install build --prefix /opt/hand_sdk        # needs sudo, see below
```

Pick the prefix to match the audience:

| Prefix | Who can use it | sudo? |
|---|---|---|
| `~/.local` | just you, on this machine | no |
| `/opt/hand_sdk` | everyone on this machine | **yes** — `/opt` is root-owned |
| `/tmp/hand_sdk-1.0.1` | staging for a tarball | no |

For `/opt`, prefix the command with `sudo`:

```bash
sudo cmake --install build --prefix /opt/hand_sdk
```

Whatever you choose, the result is the same layout:

```
<prefix>/
├── include/hand_sdk/       api.hpp config.hpp hand_sdk.hpp
│                           messages.hpp service.hpp status.hpp
└── lib/
    ├── libhand_sdk.a       SDK + SOEM merged into one archive (~1 MB)
    └── cmake/hand_sdk/     package config for find_package()
```

> **Can't find `/opt/hand_sdk` in your editor?** That is expected — `/opt` lives
> outside your workspace folder, so the VSCode Explorer will not show it. Check
> from a terminal instead: `ls -R /opt/hand_sdk`.

Consumers need **no SOEM checkout and no SOEM include paths** — the archive
already contains SOEM's object files.

### Step 2 — package it for another machine

The prefix is relocatable, so a plain tarball is enough. Copy the README in
first so the usage instructions travel with the library:

```bash
sudo cp README.md /opt/hand_sdk/
tar czf ~/hand_sdk-1.0.1.tar.gz -C /opt hand_sdk
```

`-C /opt hand_sdk` means "change into `/opt`, then archive the `hand_sdk`
directory", which keeps the paths inside the archive relative (`hand_sdk/lib/…`)
so it can be unpacked anywhere. The result lands in your home directory:
`~/hand_sdk-1.0.1.tar.gz`, roughly 300 KB.

> **Do not use `sudo tar`.** Everything under `/opt/hand_sdk` is world-readable,
> so `tar` needs no privileges to read it — and running it as root leaves the
> tarball owned by `root`, which you then cannot overwrite on the next rebuild
> without `sudo`. If you already made that mistake:
> `sudo chown $USER:$USER ~/hand_sdk-1.0.1.tar.gz`.
>
> Note that `sudo tar czf ~/file.tar.gz …` still writes to *your* home, not
> root's: the shell expands `~` before `sudo` ever runs.

### Step 3 — what the recipient does

```bash
tar xzf hand_sdk-1.0.1.tar.gz -C ~/libs      # unpack anywhere
cmake -B build -DCMAKE_PREFIX_PATH=~/libs/hand_sdk
cmake --build build
sudo setcap cap_net_admin,cap_net_raw,cap_sys_nice+ep build/their_app
```

Their CMakeLists needs only the two lines from
[Approach 2](#approach-2-use-it-from-a-c-project).

Tell them two things that are not obvious from the tarball:

- It is a **compiled archive for Linux x86-64**, not source. A different
  architecture needs a rebuild from source.
- Their **final executable** still needs
  `sudo setcap cap_net_admin,cap_net_raw,cap_sys_nice+ep` to reach the hardware. The
  capability attaches to their binary, not to the library.

### Updating a shared install

Re-running the install overwrites the prefix in place, so consumers pick up the
new version on their next build with no changes on their side:

```bash
cmake --build build && sudo cmake --install build --prefix /opt/hand_sdk
```

Bump `project(hand_sdk_cpp VERSION …)` in `CMakeLists.txt` when the public API
changes — `find_package(hand_sdk 1.2 REQUIRED)` then fails loudly on a too-old
install instead of failing at link time.

---

## Approach 2: use it from a C++ project

Your project needs only the installed prefix from above. Two lines of CMake:

```cmake
cmake_minimum_required(VERSION 3.22)
project(my_app CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(hand_sdk REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE hand_sdk::hand_sdk)
```

The imported target `hand_sdk::hand_sdk` carries its include directory and link
dependencies (`pthread`, `rt`) with it, so nothing else needs configuring.

```cpp
#include <hand_sdk.hpp>     // pulls in the whole public API
#include <iostream>

int main()
{
    hand_sdk::Config config;
    config.interface_name  = "eth0";        // your NIC
    config.left_hand_name  = "hand_200_L";  // EtherCAT device names
    config.right_hand_name = "hand_200_R";

    auto hand = hand_sdk::createEthercat(config);

    const auto connected = hand.connect();
    if (!connected.success) {
        std::cerr << connected.msg << "\n";
        return 1;
    }

    // Read the newest state (never blocks, safe from any thread)
    if (const auto state = hand.readState(); state.success) {
        std::cout << "left q[0] = " << state.value.left.q[0] << " rad"
                  << ", errors = 0x" << std::hex
                  << state.value.left.error_bits << "\n";
    }

    // Send a joint command (delivered on the next bus cycle)
    hand_sdk::frames::DualHandCommand cmd;
    cmd.has_left = true;
    cmd.left.q[0] = 0.1f;                   // rad
    hand.sendCommand(cmd);

    hand.disconnect();
}
```

Build and grant capabilities:

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/opt/hand_sdk
cmake --build build
sudo setcap cap_net_admin,cap_net_raw,cap_sys_nice+ep build/my_app
./build/my_app
```

If you installed to a standard location (`/usr/local`), you can drop
`CMAKE_PREFIX_PATH` entirely.

---

## Approach 3: use it from a ROS 2 package

Tested on ROS 2 Jazzy. Put `hand_sdk_cpp` in your workspace `src/` — it ships a
`package.xml` and builds as a plain CMake package under colcon.

```
ros2_ws/
└── src/
    ├── hand_sdk_cpp/      <- this package
    └── my_hand_node/      <- your node
```

`my_hand_node/package.xml`:

```xml
<depend>rclcpp</depend>
<depend>hand_sdk_cpp</depend>
```

`my_hand_node/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_hand_node)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(hand_sdk REQUIRED)

add_executable(my_hand_node src/node.cpp)
target_link_libraries(my_hand_node PRIVATE hand_sdk::hand_sdk rclcpp::rclcpp)

install(TARGETS my_hand_node DESTINATION lib/${PROJECT_NAME})
ament_package()
```

> ⚠️ **Use `target_link_libraries`, not `ament_target_dependencies`.** Mixing the
> two on one target is a CMake error ("keyword and plain signature"), because
> `hand_sdk::hand_sdk` must be linked with the keyword (`PRIVATE`) form. Link
> `rclcpp::rclcpp` as a normal imported target alongside it, as shown above.

Note the asymmetry in names, which is intentional:
`find_package(**hand_sdk**)` — the CMake package — while the ROS package (and the
`<depend>`) is **`hand_sdk_cpp`**, the directory name.

Build and run:

```bash
cd ros2_ws
colcon build --packages-select hand_sdk_cpp my_hand_node
source install/setup.bash

# if SOEM is not at the default path, pass it through colcon:
#   colcon build --packages-select hand_sdk_cpp \
#                --cmake-args -DSOEM_ROOT=/path/to/SOEM

# capabilities go on the installed binary
sudo setcap cap_net_admin,cap_net_raw,cap_sys_nice+ep install/my_hand_node/lib/my_hand_node/my_hand_node
ros2 run my_hand_node my_hand_node
```

Colcon builds `hand_sdk_cpp` first and adds its install prefix to the
`CMAKE_PREFIX_PATH` for your node automatically, so `find_package(hand_sdk)`
just works — no manual paths.

`setcap` makes the binary a secure-execution file, and the loader ignores
`LD_LIBRARY_PATH` for those — which is exactly how a sourced ROS workspace
advertises `/opt/ros/<distro>/lib`. A capability-carrying ROS node therefore
needs an absolute `RPATH` or it fails at exec with `librclcpp.so: cannot open
shared object file`. Give your node one:

```cmake
get_filename_component(_ros_lib_dir "${rclcpp_DIR}/../../../lib" ABSOLUTE)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib;${_ros_lib_dir}")
target_link_options(my_hand_node PRIVATE -Wl,--disable-new-dtags)
```

The `--disable-new-dtags` matters: it emits `DT_RPATH` instead of `DT_RUNPATH`,
and only `DT_RPATH` is inherited by the RMW implementation that `rcl` opens with
`dlopen` at startup.

### Node structure hint

`Api` is not copyable and owns a real-time thread, so hold it by value or
`unique_ptr` as a node member, call `connect()` once in the constructor, and
drive `readState()` / `sendCommand()` from an ordinary ROS timer. The bus keeps
running at 1 kHz regardless of your timer rate — a 50 Hz timer is fine.

---

## API at a glance

All public types live in `hand_sdk`; see the headers for full documentation.

| Call | Blocking? | Purpose |
|---|---|---|
| `connect()` / `disconnect()` | yes, once | bring the bus up / down |
| `readState()` | no | newest joint states + per-motor `error_bits` |
| `sendCommand()` | no | joint position/velocity/torque commands |
| `stateSequence()` / `commandSequence()` | no | detect fresh vs. repeated data |
| `serviceComm()` | ~10 ms | **write** — start/stop broadcast responses |
| `serviceMode()` | ~10 ms | **write** — set loop mode + control enable |
| `serviceGains()` | ~10 ms | **write** — set force-mode position loop gains |
| `serviceZeroCalibrate()` | ~10 ms | **write** — start the position zero-point calibration |
| `serviceModeQuery()` | ~10 ms | read — the mode/enable each joint is already in |
| `serviceMcState()` | ~10 ms | read — per-joint state and fault codes |
| `serviceGainsQuery()` | ~10 ms | read — the gains each joint is already running |
| `serviceZeroCalibrateState()` | ~10 ms | read — per-joint zero-calibration state |
| `cycleStats()`, `leftStatus()`, `leftDiagnostics()` | no | health and diagnostics |

Every call returns `Result<T>` with `.success`, `.msg`, and `.value` — no
exceptions are thrown across the API boundary.

Units are SI: positions in rad, velocities in rad/s, torques in N·m. Only one
service call may be in flight at a time; a second concurrent call fails
immediately with `"another service call is in progress"` rather than queueing.

### Joint order and direction

Every index the API takes or returns -- `q[i]`, `JointMask` bit `i`, `error_bits`
bit `i` -- is a **logical** joint, `finger * 4 + joint`, with the reserved 21st
slot last. Two things sit between that and the device, both applied at the
boundary and nowhere else:

- **Order.** The device reads each finger's four joints distal-first, so logical
  0,1,2,3 go out as slots 3,2,1,0, and likewise for every other finger. This
  lives in `wireSlot()` (`frames/pdo_layout.hpp`) and is applied by the codecs
  in both directions. The only place device slot numbers surface is the
  `[service]` request/reply dump, which prints the raw wire.
- **Direction.** `kLeftJointDirection` / `kRightJointDirection` in `config.hpp`
  carry one `+1` / `-1` per joint per hand, and `Config::joint_direction`
  defaults to them. `-1` means that motor is fitted the other way round from the
  model, and Api negates `q`, `dq` and `tau` on the way out and back again on
  the way in. Gains, fault bits and the service channel are never flipped. The
  tables are fixed at build time -- how a motor is fitted is a property of the
  hand, not a launch option -- and a `static_assert` rejects any entry that is
  not `+1` or `-1`.

### Write services are verified per joint

A read service succeeds once a reply block for that hand comes back carrying the
right cmd id. There is nothing else to check — nothing was asked to change.

A write service puts that same transport check first, then holds every joint the
request addressed to what it was told. The device echoes its resulting state in
the same slots the request was written into, so the reply says, per joint,
whether it took:

| Service | Checked against | How |
|---|---|---|
| `serviceComm()` | the `ComCmd` sent | exact — cmd 4 has no fields, the echo *is* the command |
| `serviceMode()` | mode + enable | exact |
| `serviceGains()` | kp / kd | within `Config::service_tolerance` |
| `serviceZeroCalibrate()` | — | not verified: the routine outlives the window, so nothing has an outcome to echo yet. Poll `serviceZeroCalibrateState()` for the result |

One addressed joint disagreeing makes `.success` false; `.msg` then names the
joints on both sides with what came back, and `.value` still carries the decoded
per-joint reply so a caller can render its own view rather than parse prose.
`service::took()` is that same per-joint predicate, exposed so a caller counting
joints and the SDK judging them cannot drift apart.

```
gains: 18/20 joints took it | pass: 0-17 | fail: j18 kp=0.0000 kd=0.0000 (wanted kp=3.0000 kd=3.0000 +-0.0005)
```

`Config::service_tolerance` defaults to `1/2048` — one fixed-point step. The
application side is float and the electronics are fixed point at a scale of
2048, so a request of `(1/2048 − δ)` is a number the electronics cannot hold and
it runs `0` instead. One step of disagreement is that conversion; more than one
step is the device substituting a value of its own.

Joints outside a request's `JointMask` are sent the device's "no action" word,
are never asked for anything, and so can neither pass nor fail. Cmd 10 is the
exception: its unselected slots carry `0x0000` (`ZeroCalCmd::kNone`), because the
ignore word's low byte is `0xFF` and only `0x00` is defined as "no calibration
starts" — sending the ignore word there started a calibration on the joints the
mask meant to skip. The reserved
21st slot is written like the others but never verified — nothing is behind it to
answer (`JointMask::active()` is the 20 that are).

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `SOEM not found` at configure time | Build SOEM, then `-DSOEM_ROOT=/path/to/SOEM` |
| `ecx_init failed: could not open interface` | Wrong `interface_name`, or missing `cap_net_raw` |
| `no matching hand slaves found` | Device names differ — the error lists what was found on the bus; set `left_hand_name` / `right_hand_name` to match |
| `SCHED_FIFO failed` in the log | Missing `cap_sys_nice`; runs anyway with more jitter |
| `failed to reach OPERATIONAL` | Check the AL status codes in the error text; see [notes.md](notes.md) "DC-Synchron bring-up" |
| CMake "keyword and plain signature" error | ROS only — use `target_link_libraries`, not `ament_target_dependencies` (see Approach 3) |
| Python bindings | Not supported yet; requires rebuilding SOEM with `-DCMAKE_POSITION_INDEPENDENT_CODE=ON` |
