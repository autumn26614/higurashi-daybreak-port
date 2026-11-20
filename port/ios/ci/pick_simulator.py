"""Print the UDID of an available iPhone simulator on the newest installed iOS runtime.

Reads `xcrun simctl list devices available -j` on stdin. The chosen device name and
runtime go to stderr for the CI log. Exits non-zero if none is found."""
import json
import re
import sys


def runtime_version(key):
    m = re.search(r"iOS-(\d+)(?:-(\d+))?", key)
    return (int(m.group(1)), int(m.group(2) or 0)) if m else (0, 0)


def main():
    devices = json.load(sys.stdin)["devices"]
    best = None
    for runtime, devs in devices.items():
        if "iOS" not in runtime:
            continue
        for dev in devs:
            if dev.get("isAvailable", True) and dev["name"].startswith("iPhone"):
                cand = (runtime_version(runtime), dev["name"], dev["udid"], runtime)
                if best is None or cand[:2] > best[:2]:
                    best = cand
    if best is None:
        sys.exit("no available iPhone simulator found")
    print(f"using {best[1]} ({best[3]})", file=sys.stderr)
    print(best[2])


if __name__ == "__main__":
    main()
