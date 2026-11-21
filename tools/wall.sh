#!/usr/bin/env bash
# Runs a command the way every test run of the application must run: behind a
# wall, so that it cannot reach the desktop, the sound devices or the network of
# the machine it is started on.
#
#   tools/wall.sh COMMAND [ARGS...]
#   tools/wall.sh --check     the control: proves each point below from inside
#
# Inside the wall (bubblewrap):
#   - no sound is possible: there is no /dev/snd, and the folder of the PulseAudio
#     and PipeWire sockets is empty; SDL's dummy audio driver is a second layer only
#   - none of the machine's devices can be opened: no controller, keyboard or
#     other input device, no USB device, no camera, no graphics card
#   - no network but loopback: nothing started here can reach a console on the
#     home network, or anything else. The network inside is the wall's own, so a
#     test may open the consoles' real ports and add a test interface to it
#   - no process of the desktop exists (own process numbering), and everything
#     started here ends when the wall does
#   - the window servers of the desktop cannot be reached; windows open on a
#     virtual X server whose screen is kept in a file (PSVP_TEST_FB)
#   - a private home, private XDG folders and a private /tmp: never the real profile
#   - a session bus of its own that can start nothing
#   - Vulkan runs on the software driver only, without the desktop's layers
#
# Where bubblewrap cannot run (a container that forbids it), the command runs
# without it only if the machine has no sound, input, USB, camera or graphics
# device at all. Tests that use the
# network then need PSVP_TEST_ISOLATED_NET set by whoever started the container,
# as a statement that its network is not anyone's home network.
#
#   PSVP_TEST_HOME=DIR      keep the private folders in DIR (default: temporary, removed)
#   PSVP_TEST_SCREEN=WxHxD  the virtual screen (default 1280x1024x24)
set -euo pipefail
[ $# -ge 1 ] || { sed -n '2,7p' "$0"; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
RT="/run/user/$(id -u)"

# A mistake in sizing a test run should cost the run, never someone's desktop programs.
echo 1000 > /proc/self/oom_score_adj 2>/dev/null || true

# The machine's files, but not its devices: /dev inside holds only what every program needs
# (null, zero, random, a terminal, shared memory). No sound card, no controller or keyboard,
# no USB device, no camera and no graphics card of this machine can be opened from inside.
# (Built by hand and not with bubblewrap's own --dev: for that one it moves the command into a second
# user namespace, where the rights over the wall's network, asked for below, no longer count.)
BWRAP=(--dev-bind / / --tmpfs /dev --tmpfs /dev/shm --unshare-user --unshare-pid --unshare-net --proc /proc --die-with-parent
       --symlink /proc/self/fd /dev/fd --symlink /proc/self/fd/0 /dev/stdin --symlink /proc/self/fd/1 /dev/stdout
       --symlink /proc/self/fd/2 /dev/stderr
       --cap-add CAP_NET_BIND_SERVICE --cap-add CAP_NET_ADMIN)
for node in null zero full random urandom tty; do
    [ -e "/dev/$node" ] && BWRAP+=(--dev-bind "/dev/$node" "/dev/$node")
done
# PSVP_WALL_GPU=1: the graphics card's compute and video nodes too (render nodes, NVIDIA's device
# nodes), for headless decoding on it. Never its display nodes (/dev/dri/card*), never input or sound.
GPU_NODES=()
if [ "${PSVP_WALL_GPU:-}" = 1 ]; then
    for node in /dev/dri/renderD* /dev/nvidia0 /dev/nvidia1 /dev/nvidiactl /dev/nvidia-uvm /dev/nvidia-uvm-tools; do
        [ -e "$node" ] && GPU_NODES+=("$node")
    done
    for node in "${GPU_NODES[@]}"; do BWRAP+=(--dev-bind "$node" "$node"); done
fi
[ -d "$RT" ] && BWRAP+=(--tmpfs "$RT")
[ -d /tmp/.X11-unix ] && BWRAP+=(--perms 1777 --tmpfs /tmp/.X11-unix)
WALL=1
if ! command -v bwrap >/dev/null 2>&1 || ! bwrap "${BWRAP[@]}" true 2>/dev/null; then WALL=0; fi
has_sound() { [ -n "$(ls -A /dev/snd 2>/dev/null)" ]; }
# Controllers, keyboards and other HID devices, USB devices, cameras, graphics cards.
devices_here='ls -d /dev/input/* /dev/hidraw* /dev/uinput /dev/uhid /dev/bus/usb/*/* /dev/video* /dev/dri/* 2>/dev/null; true'
has_devices() { [ -n "$(sh -c "$devices_here")" ]; }

if [ "$1" = "--check" ]; then
    if [ "$WALL" = 0 ]; then
        if has_sound; then echo "wall: CONTROL FAILED, bubblewrap cannot run and this machine has sound devices"; exit 1; fi
        if has_devices; then echo "wall: CONTROL FAILED, bubblewrap cannot run and this machine has input, USB, camera or graphics devices"; exit 1; fi
        echo "wall: control ok (no bubblewrap here, and no sound, input, USB, camera or graphics device at all)"; exit 0
    fi
    inside() { bwrap "${BWRAP[@]}" -- "$@"; }
    out="$(inside sh -c 'ls -A /dev/snd 2>/dev/null; for s in "'"$RT"'"/pulse "'"$RT"'"/pipewire-0 "'"$RT"'"/bus; do [ -e "$s" ] && echo "$s"; done; true')"
    if [ -n "$out" ]; then echo "wall: CONTROL FAILED, reachable inside the wall:"; echo "$out"; exit 1; fi
    if has_sound; then echo "wall: control ok (sound: $(ls -A /dev/snd | wc -l) devices outside, none inside)"
    else echo "wall: control ok (sound: this machine has no devices at all)"; fi

    reachable="$(inside sh -c "$devices_here" | grep -v -x -F "$(printf '%s\n' "${GPU_NODES[@]:-/nonexistent}")" || true)"
    if [ -n "$reachable" ]; then echo "wall: CONTROL FAILED, devices reachable inside the wall:"; echo "$reachable" | head -5; exit 1; fi
    echo "wall: control ok (devices: $(sh -c "$devices_here" | wc -l) input, USB, camera and graphics devices outside, none inside)"

    # /proc/net/dev lists the links of the network a process is in (the wall has its own /proc).
    links_of='awk "NR > 2 { sub(/:.*/, \"\"); gsub(/ /, \"\"); print }" /proc/net/dev | sort | tr "\n" " "'
    links="$(inside sh -c "$links_of")"
    if [ "$links" != "lo " ]; then echo "wall: CONTROL FAILED, network links inside the wall: $links"; exit 1; fi
    echo "wall: control ok (network: only loopback inside, $(sh -c "$links_of" | wc -w) links outside)"

    seen="$(inside sh -c 'ls -d /proc/[0-9]* | wc -l')"
    if [ "$seen" -gt 8 ]; then echo "wall: CONTROL FAILED, $seen processes are visible inside the wall"; exit 1; fi
    echo "wall: control ok (processes: $seen visible inside, $(ls -d /proc/[0-9]* | wc -l) outside)"

    servers="$(inside sh -c 'ls -A /tmp/.X11-unix 2>/dev/null; true')"
    if [ -n "$servers" ]; then echo "wall: CONTROL FAILED, window server sockets inside the wall: $servers"; exit 1; fi
    echo "wall: control ok (window servers: none reachable inside, $(ls -A /tmp/.X11-unix 2>/dev/null | wc -l) outside)"
    exit 0
fi

# Vulkan: the software driver, named explicitly, so a test never loads the desktop's GPU stack
# (with PSVP_WALL_GPU=1 the machine's own drivers, and no window server is used by such a run).
icd="${PSVP_TEST_ICD:-}"
if [ "${PSVP_WALL_GPU:-}" = 1 ] && [ -z "$icd" ]; then
    icd="$( (ls /usr/share/vulkan/icd.d/*.json /etc/vulkan/icd.d/*.json 2>/dev/null || true) | grep -v lvp_icd | tr '\n' ':' | sed 's/:$//')"
fi
if [ -z "$icd" ]; then
    for candidate in /usr/share/vulkan/icd.d/lvp_icd*.json /etc/vulkan/icd.d/lvp_icd*.json; do
        [ -f "$candidate" ] && { icd="$candidate"; break; }
    done
fi
if [ -z "$icd" ]; then
    echo "wall: no software Vulkan driver found (Arch: vulkan-swrast, Debian: mesa-vulkan-drivers, Alpine: mesa-vulkan-swrast)" >&2
    exit 5
fi

if [ -n "${PSVP_TEST_HOME:-}" ]; then
    TMP="$(realpath -m "$PSVP_TEST_HOME")"; mkdir -p "$TMP"
else
    TMP="$(mktemp -d "${TMPDIR:-/tmp}/psvp-wall.XXXXXX")"
    trap 'rm -rf "$TMP"' EXIT
fi
mkdir -p "$TMP/home" "$TMP/config" "$TMP/data" "$TMP/state" "$TMP/cache" "$TMP/rt" "$TMP/fb"
chmod 700 "$TMP/rt"
unset WAYLAND_DISPLAY WAYLAND_SOCKET DISPLAY DBUS_SESSION_BUS_ADDRESS
ENVS=(HOME="$TMP/home" XDG_CONFIG_HOME="$TMP/config" XDG_DATA_HOME="$TMP/data" XDG_STATE_HOME="$TMP/state"
      XDG_CACHE_HOME="$TMP/cache" XDG_RUNTIME_DIR="$TMP/rt" XDG_SESSION_TYPE=x11
      PULSE_SERVER=unix:/nonexistent/psvp-wall PIPEWIRE_REMOTE=/nonexistent/psvp-wall
      SDL_AUDIO_DRIVER=dummy SDL_AUDIODRIVER=dummy SDL_VIDEO_DRIVER=x11 SDL_VIDEODRIVER=x11
      GDK_BACKEND=x11 NO_AT_BRIDGE=1
      VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES="$icd" "VK_LOADER_LAYERS_DISABLE=~implicit~"
      PSVP_TEST_TMP="$TMP" PSVP_TEST_FB="$TMP/fb/Xvfb_screen0" PSVP_TEST_WALL="$WALL"
      PSVP_TEST_ISOLATED_NET="$([ "$WALL" = 1 ] && echo 1 || echo "${PSVP_TEST_ISOLATED_NET:-}")"
      PSVP_TEST_SCREEN="${PSVP_TEST_SCREEN:-1280x1024x24}" PSVP_TEST_BUS="$HERE/isolated-bus.conf")

# The X server picks a free display number itself and reports it, so runs started
# at the same moment never share one. It is ours ($!), and it is stopped by us.
inner='
  # Up to three tries, and what the server said is kept: a start that fails is told with its reason.
  server=""
  for attempt in 1 2 3; do
    rm -f "$PSVP_TEST_TMP/display"
    # Without GLX: nothing here draws with OpenGL, and an X server that offers it loads the
    # graphics drivers of the machine, which have no device to find behind the wall.
    Xvfb -displayfd 3 -screen 0 "$PSVP_TEST_SCREEN" -nolisten tcp -extension GLX -fbdir "$PSVP_TEST_TMP/fb" \
        3>"$PSVP_TEST_TMP/display" 2>"$PSVP_TEST_TMP/xvfb-$attempt.err" &
    server=$!
    for _ in $(seq 200); do [ -s "$PSVP_TEST_TMP/display" ] && break; kill -0 $server 2>/dev/null || break; sleep 0.05; done
    [ -s "$PSVP_TEST_TMP/display" ] && break
    kill $server 2>/dev/null; wait $server 2>/dev/null
    echo "wall: the virtual X server did not start (try $attempt): $(tail -2 "$PSVP_TEST_TMP/xvfb-$attempt.err" | tr "\n" " ")" >&2
    server=""
  done
  if [ -z "$server" ]; then
    echo "wall: the virtual X server did not start" >&2; exit 4
  fi
  export DISPLAY=":$(cat "$PSVP_TEST_TMP/display")"
  if command -v dbus-run-session >/dev/null 2>&1; then
    dbus-run-session --config-file="$PSVP_TEST_BUS" -- "$@"; rc=$?
  else
    DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/psvp-wall "$@"; rc=$?
  fi
  kill $server 2>/dev/null; wait $server 2>/dev/null
  exit $rc
'

if [ "$WALL" = 0 ]; then
    if has_sound; then echo "wall: bubblewrap cannot run and this machine has sound devices; refusing to run" >&2; exit 3; fi
    if has_devices; then echo "wall: bubblewrap cannot run and this machine has input, USB, camera or graphics devices; refusing to run" >&2; exit 3; fi
    env "${ENVS[@]}" bash -c "$inner" wall "$@"
else
    # A /tmp of its own, with only this run's folder in it: X servers that start at the same moment
    # in different walls would otherwise write the same file there (the keyboard map of display 0),
    # and now and then one of them failed to start.
    bwrap "${BWRAP[@]}" --perms 1777 --tmpfs /tmp --bind "$TMP" "$TMP" -- env "${ENVS[@]}" bash -c "$inner" wall "$@"
fi
