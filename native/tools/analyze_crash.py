#!/usr/bin/env python3
"""Read a Portal2Compat run log, verify local image UUIDs, symbolize guest frames.

No game launch, debugger attachment, network access, or third-party Python modules.
"""
import argparse
import bisect
from functools import lru_cache
from pathlib import Path
import re
import subprocess

KINDS = dict(enumerate(("alloc", "free", "realloc", "invalid-free", "animation-before",
    "animation-after", "tick-before", "tick-after", "trigger-before", "trigger-after",
    "queue", "queued-entity", "destroy-before", "destroy-after", "queue-invalidate", "queue-compact"), 1))
FIELDS = re.compile(r"([\w-]+)=(0x[0-9a-f]+)")


def parse(text):
    images, frames, events = [], [], []
    registers, pc, stack_return = {}, None, None
    complete = False
    for line in text.splitlines():
        values = {key: int(value, 16) for key, value in FIELDS.findall(line)}
        if line.startswith("capture image "):
            values["path"] = line.split(" path=", 1)[1]
            values["uuid"] = re.search(r" uuid=([0-9a-f]{32})", line).group(1)
            images.append(values)
        elif line.startswith("capture pc="):
            pc = values["pc"]
        elif line.startswith("capture register "):
            registers[values["index"]] = values["value"]
        elif line.startswith("capture frame "):
            frames.append(values["return"])
        elif line.startswith("capture memory stack ") and stack_return is None:
            words = re.findall(r"0x[0-9a-f]+", line)
            if len(words) > 1:
                stack_return = int(words[1], 16)
        elif line.startswith("capture event "):
            events.append(values)
        elif line == "capture end":
            complete = True
    return dict(images=images, frames=frames, events=sorted(events, key=lambda e: e["seq"]),
                registers=registers, pc=pc, stack_return=stack_return, complete=complete)


@lru_cache(maxsize=64)
def symbols(path, expected_uuid):
    """Never apply symbols from a different build to an old crash address."""
    if not Path(path).is_file():
        return [], [], "image not found"
    try:
        identity = subprocess.check_output(["dwarfdump", "--uuid", path], text=True, stderr=subprocess.DEVNULL)
        uuid = re.search(r"UUID: ([A-Fa-f0-9-]+) \(i386\)", identity)
        if not uuid or uuid.group(1).replace("-", "").lower() != expected_uuid:
            return [], [], "UUID mismatch; symbols refused"
        output = subprocess.check_output(["nm", "-arch", "i386", "-n", path], text=True, stderr=subprocess.DEVNULL)
        entries = []
        for line in output.splitlines():
            match = re.match(r"([0-9a-fA-F]+) [tT] (.+)", line)
            if match:
                entries.append((int(match.group(1), 16), match.group(2)))
        entries.sort()
        names = subprocess.check_output(["c++filt"], input="\n".join(n for _, n in entries), text=True).splitlines()
        return [a for a, _ in entries], names, ""
    except (OSError, subprocess.CalledProcessError) as error:
        return [], [], str(error)


def describe(address, images, image_dir=None):
    for image in images:
        if image["base"] <= address < image["end"]:
            offset = address-image["slide"]
            path = Path(image["path"])
            if image_dir:
                path = image_dir/path.name
            starts, names, error = symbols(str(path), image["uuid"])
            index = bisect.bisect_right(starts, offset)-1
            result = f"{path.name}+0x{offset:x}"
            if index >= 0:
                result += f"  {names[index]}+0x{offset-starts[index]:x}"
            elif error:
                result += f"  [{error}]"
            return result
    return "unmapped/bridge address"


def related(event, address):
    # Allocator events describe an allocation span; physics events describe a
    # single entity/manager, and their 'size' is not an allocation extent.
    if event["kind"] in (1, 2, 3):
        return event["address"] <= address < event["address"]+event["size"]
    return event["address"] == address


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("log", type=Path)
    cli.add_argument("--address", type=lambda s: int(s, 0), help="entity/interior pointer whose lifetime to inspect")
    cli.add_argument("--image-dir", type=Path, help="directory containing matching i386 images moved since the crash")
    args = cli.parse_args()
    data = parse(args.log.read_text(errors="replace"))
    print(f"Capture: {'complete' if data['complete'] else 'missing or incomplete'}")
    if data["pc"] is not None:
        print(f"Fault PC 0x{data['pc']:08x}: {describe(data['pc'], data['images'], args.image_dir)}")
    addresses = ([data["stack_return"]] if data["stack_return"] else [])+data["frames"]
    print("Guest return addresses (stack top is a candidate; frame-chain entries follow):")
    for address in addresses[:65]:
        print(f"  0x{address:08x}  {describe(address, data['images'], args.image_dir)}")
    address = args.address
    if address is None and data["pc"] == 0:
        address = data["registers"].get(0)
        if address:
            print("Using saved EAX as an object candidate for this null-PC crash; confirm against the call instruction.")
    if address is not None:
        matches = [event for event in data["events"] if related(event, address)]
        print(f"Retained history for 0x{address:08x}: {len(matches)} events (last 60 shown)")
    else:
        matches = [event for event in data["events"] if event["kind"] >= 5]
        print("Last 60 retained physics observations; use --address to inspect a specific object's allocation history:")
    for event in matches[-60:]:
        print(f"  #{event['seq']} {KINDS.get(event['kind'], 'unknown')} "
              f"object=0x{event['address']:08x} size/count=0x{event['size']:x} "
              f"site/stage=0x{event['site']:08x} detail/vtable=0x{event['detail']:08x}")
    print("History is bounded; an absent allocation/free event does not prove it never happened.")


if __name__ == "__main__":
    main()
