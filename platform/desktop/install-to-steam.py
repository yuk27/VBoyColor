#!/usr/bin/env python3
"""Adds VBoy Color to Steam as a non-Steam game, with its name, icon and
library artwork (steam/ next to this script) - what the Steam Frame needs to
show it like any other game. Run it from the unpacked folder:

    ./install-to-steam.py              the VR app (VBoyColorVR), as "VBoy Color"
    ./install-to-steam.py --window     the window app (VBoyColor) instead
    ./install-to-steam.py --uninstall  take it out of Steam again

Steam only reads its list of non-Steam games when it starts and writes it
back when it quits, so it has to be closed while this runs: on SteamOS (the
Frame, the Deck) the script stops Steam's service and starts it again, on a
desktop it asks Steam to quit and starts it again afterwards. Running it
again updates the entry instead of adding a second one. shortcuts.vdf is
backed up next to itself (shortcuts.vdf.bak) before it's changed.
"""
import argparse
import contextlib
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib

NAME = "VBoy Color"
# Steam's library artwork: the file in steam/ -> its name in Steam's grid folder.
ARTWORK = {"capsule.png": "{id}p.png", "header.png": "{id}.png", "hero.png": "{id}_hero.png", "logo.png": "{id}_logo.png"}


# --- shortcuts.vdf: Steam's binary key-value format ---------------------------
# Each value is a type byte (0 a nested map, 1 a string, 2 a 32-bit int), its
# key and the value; 8 ends a map. Kept as ordered (key, value) lists so a
# file is written back exactly as it was read, apart from what's changed.

def parse_vdf(data):
    pos = 0

    def cstring():
        nonlocal pos
        end = data.index(b"\0", pos)
        s = data[pos:end].decode("utf-8", "surrogateescape")
        pos = end + 1
        return s

    def mapping():
        nonlocal pos
        items = []
        while True:
            kind = data[pos]
            pos += 1
            if kind == 8:
                return items
            key = cstring()
            if kind == 0:
                items.append((key, mapping()))
            elif kind == 1:
                items.append((key, cstring()))
            elif kind == 2:
                items.append((key, struct.unpack_from("<i", data, pos)[0]))
                pos += 4
            else:
                raise ValueError("unknown value type %d in shortcuts.vdf" % kind)

    root = mapping()
    return root


def write_vdf(items):
    out = bytearray()
    for key, value in items:
        k = key.encode("utf-8", "surrogateescape") + b"\0"
        if isinstance(value, list):
            out += b"\0" + k + write_vdf(value) + b"\x08"
        elif isinstance(value, str):
            out += b"\x01" + k + value.encode("utf-8", "surrogateescape") + b"\0"
        else:
            out += b"\x02" + k + struct.pack("<i", value)
    return bytes(out)


def get(items, key, default=None):
    for k, v in items:
        if k.lower() == key.lower():
            return v
    return default


def put(items, key, value):
    for i, (k, _) in enumerate(items):
        if k.lower() == key.lower():
            items[i] = (k, value)
            return
    items.append((key, value))


# --- Steam -------------------------------------------------------------------

def steam_root():
    for path in ("~/.local/share/Steam", "~/.steam/steam", "~/.var/app/com.valvesoftware.Steam/data/Steam"):
        path = os.path.realpath(os.path.expanduser(path))
        if os.path.isdir(os.path.join(path, "userdata")):
            return path
    sys.exit("Couldn't find Steam (no userdata folder in ~/.local/share/Steam or ~/.steam/steam).")


def user_configs(root):
    """The config folder of every Steam account that's signed in here."""
    base = os.path.join(root, "userdata")
    return [os.path.join(base, d, "config") for d in sorted(os.listdir(base))
            if d.isdigit() and d != "0" and os.path.isdir(os.path.join(base, d, "config"))]


def steam_service_active():
    return subprocess.run(["systemctl", "--user", "is-active", "--quiet", "steam.service"],
                          stderr=subprocess.DEVNULL).returncode == 0


def steam_running():
    return subprocess.run(["pgrep", "-x", "steam"], stdout=subprocess.DEVNULL).returncode == 0


class SteamClosed:
    """Steam closed for the edit, and started again after it (if it was running)."""

    def __init__(self, ask):
        self.ask = ask
        self.how = None

    def __enter__(self):
        if steam_service_active():
            self.how = "service"
        elif steam_running():
            self.how = "desktop"
        else:
            return self
        if self.ask and input("Steam has to close for a moment while VBoy Color is added. Close it now? [Y/n] ").strip().lower() not in ("", "y", "yes"):
            sys.exit("Nothing changed.")
        print("Closing Steam...")
        if self.how == "service":
            subprocess.run(["systemctl", "--user", "stop", "steam.service"], check=True)
        else:
            subprocess.run(["steam", "-shutdown"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for _ in range(60):
                if not steam_running():
                    break
                time.sleep(0.5)
            else:
                sys.exit("Steam didn't close - quit it and run this again.")
        return self

    def __exit__(self, *exc):
        if self.how == "service":
            print("Starting Steam again...")
            subprocess.run(["systemctl", "--user", "start", "steam.service"])
        elif self.how == "desktop":
            print("Starting Steam again...")
            subprocess.Popen(["steam"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


# --- the entry ---------------------------------------------------------------

def is_ours(entry, exe):
    return get(entry, "Exe", "").strip('"') == exe


def update_config(config, here, exe, vr, uninstall):
    vdf = os.path.join(config, "shortcuts.vdf")
    root = parse_vdf(open(vdf, "rb").read()) if os.path.exists(vdf) else [("shortcuts", [])]
    shortcuts = get(root, "shortcuts")
    if shortcuts is None:
        shortcuts = []
        root.append(("shortcuts", shortcuts))
    grid = os.path.join(config, "grid")

    ours = [(k, e) for k, e in shortcuts if is_ours(e, exe)]
    if uninstall:
        for _, entry in ours:
            appid = get(entry, "appid", 0) & 0xFFFFFFFF
            for pattern in ARTWORK.values():
                path = os.path.join(grid, pattern.format(id=appid))
                if os.path.exists(path):
                    os.remove(path)
        kept = [e for _, e in shortcuts if not is_ours(e, exe)]
        shortcuts[:] = [(str(i), e) for i, e in enumerate(kept)]
    else:
        if ours:
            entry = ours[0][1]
            appid = get(entry, "appid", 0) & 0xFFFFFFFF
        else:
            # Steam's own id for a non-Steam game: from the path and the name.
            appid = (zlib.crc32(('"%s"%s' % (exe, NAME)).encode()) & 0xFFFFFFFF) | 0x80000000
            # (the fields, spelled and ordered as Steam writes them itself)
            entry = [("appid", struct.unpack("<i", struct.pack("<I", appid))[0]), ("appname", ""), ("Exe", ""),
                     ("StartDir", ""), ("icon", ""), ("ShortcutPath", ""), ("LaunchOptions", ""), ("IsHidden", 0),
                     ("AllowDesktopConfig", 1), ("AllowOverlay", 1), ("OpenVR", 0), ("Devkit", 0),
                     ("DevkitGameID", ""), ("DevkitOverrideAppID", 0), ("LastPlayTime", 0), ("FlatpakAppID", ""),
                     ("sortas", ""), ("tags", [])]
            shortcuts.append((str(len(shortcuts)), entry))
        put(entry, "appname", NAME)
        put(entry, "Exe", '"%s"' % exe)
        put(entry, "StartDir", '"%s"' % here)
        put(entry, "icon", os.path.join(here, "steam", "icon.png"))
        put(entry, "OpenVR", 1 if vr else 0)
        os.makedirs(grid, exist_ok=True)
        for source, pattern in ARTWORK.items():
            shutil.copyfile(os.path.join(here, "steam", source), os.path.join(grid, pattern.format(id=appid)))

    if os.path.exists(vdf):
        shutil.copyfile(vdf, vdf + ".bak")
    with open(vdf + ".tmp", "wb") as f:
        f.write(write_vdf(root) + b"\x08")
    os.replace(vdf + ".tmp", vdf)
    return len(ours)


def main():
    parser = argparse.ArgumentParser(description="Add VBoy Color to Steam with its name, icon and artwork.")
    parser.add_argument("--window", action="store_true", help="add the window app (VBoyColor) instead of the VR app")
    parser.add_argument("--uninstall", action="store_true", help="take VBoy Color out of Steam")
    parser.add_argument("--yes", "-y", action="store_true", help="don't ask before closing Steam")
    parser.add_argument("--steam-root", help=argparse.SUPPRESS)  # (tests: a Steam folder that isn't the real one)
    args = parser.parse_args()

    here = os.path.dirname(os.path.realpath(__file__))
    exe = os.path.join(here, "VBoyColor" if args.window else "VBoyColorVR")
    if not os.path.isfile(exe):
        sys.exit("%s isn't here - run this from the unpacked VBoy Color folder." % os.path.basename(exe))
    root = args.steam_root or steam_root()
    configs = user_configs(root)
    if not configs:
        sys.exit("No Steam account found in %s - start Steam and sign in once first." % root)

    # (a test's Steam folder isn't the running Steam's: leave Steam alone)
    with SteamClosed(ask=not args.yes) if not args.steam_root else contextlib.nullcontext():
        for config in configs:
            found = update_config(config, here, exe, vr=not args.window, uninstall=args.uninstall)
            account = os.path.basename(os.path.dirname(config))
            if args.uninstall:
                print("Steam account %s: %s" % (account, "removed VBoy Color" if found else "VBoy Color wasn't there"))
            else:
                print("Steam account %s: %s VBoy Color" % (account, "updated" if found else "added"))


if __name__ == "__main__":
    main()
