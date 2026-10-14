"""Short DT_NEEDED names resolving to `<stem>-<variant>.prx` guest files.

Dumps (notably Unreal Engine titles) ship `kitt_support-ps5-shipping.prx`
while DT_NEEDED names `kitt_support.prx`. Exact and case-folded lookups win;
a single stem-prefixed candidate is accepted, several are an error.
"""

from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile

from test_guest_intel_trampolines import main_fixture
from test_guest_module_directories import module_with_symbol, needed_libraries


def executable_with_needed(needed):
    image = main_fixture()
    strings = b"\0" + needed + b"\0"
    image[0x4800:0x4800 + len(strings)] = strings
    tags = []
    for position in range(0x4600, 0x4600 + 0x200, 16):
        tag, value = struct.unpack_from("<qQ", image, position)
        if tag == 0:
            break
        tags.append((tag, value))
    tags = [(5, 0x4800) if tag == 5 else (10, len(strings)) if tag == 10 else (tag, value) for tag, value in tags]
    tags += [(1, 1), (0, 0)]
    for index, tag in enumerate(tags):
        struct.pack_into("<qQ", image, 0x4600 + index * 16, *tag)
    struct.pack_into("<QQ", image, 120 + 32, len(tags) * 16, len(tags) * 16)
    return image


def main():
    relinker = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-module-alias-") as directory:
        work = Path(directory)

        def convert(case, windows, needed):
            (case / "sce_module").mkdir(parents=True, exist_ok=True)
            source = case / "input.elf"
            source.write_bytes(executable_with_needed(needed))
            output = case / ("output.exe" if windows else "output.elf")
            result = subprocess.run([str(relinker), *(["--windows"] if windows else []), str(source), str(output)],
                                    capture_output=True, text=True, timeout=30)
            return result, output

        for windows in (False, True):
            case = work / f"{windows}-alias"
            (case / "sce_module").mkdir(parents=True)
            (case / "sce_module" / "party-ps5-shipping.prx").write_bytes(module_with_symbol(True))
            result, output = convert(case, windows, b"party.prx")
            assert result.returncode == 0, (result.stdout, result.stderr)
            artifact = case / "app0" / "sce_module" / "party-ps5-shipping.prx.guest.prx"
            assert artifact.is_file(), list((case / "app0").rglob("*"))
            if windows and os.name == "nt":
                run = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
                assert run.returncode == 42, (run.returncode, run.stdout, run.stderr)
            if not windows:
                assert needed_libraries(output.read_bytes()) == [
                    "$ORIGIN/app0/sce_module/party-ps5-shipping.prx.guest.prx"], needed_libraries(output.read_bytes())

            case = work / f"{windows}-debug-suffix"
            (case / "sce_module").mkdir(parents=True)
            (case / "sce_module" / "eossdk-ps5-shipping.prx").write_bytes(module_with_symbol(True))
            result, output = convert(case, windows, b"EOSSDK-PS5-Shipping.debug_prx")
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert (case / "app0" / "sce_module" / "eossdk-ps5-shipping.prx.guest.prx").is_file()

            case = work / f"{windows}-exact-wins"
            (case / "sce_module").mkdir(parents=True)
            (case / "sce_module" / "party.prx").write_bytes(module_with_symbol(True))
            (case / "sce_module" / "party-ps5-shipping.prx").write_bytes(module_with_symbol(True))
            result, output = convert(case, windows, b"party.prx")
            assert result.returncode == 0, (result.stdout, result.stderr)

            case = work / f"{windows}-ambiguous"
            (case / "sce_module").mkdir(parents=True)
            (case / "sce_module" / "party-ps5-a.prx").write_bytes(module_with_symbol(True))
            (case / "sce_module" / "party-ps5-b.prx").write_bytes(module_with_symbol(True))
            result, output = convert(case, windows, b"party.prx")
            assert result.returncode == 2 and "Ambiguous guest dependency name: party.prx" in result.stderr, (result.returncode, result.stderr)
            assert not output.exists(), output

            case = work / f"{windows}-no-false-positive"
            (case / "sce_module").mkdir(parents=True)
            (case / "sce_module" / "otherness.prx").write_bytes(module_with_symbol(True))
            result, output = convert(case, windows, b"other.prx")
            assert result.returncode == 0, (result.stdout, result.stderr)
            if not windows:
                needed = needed_libraries(output.read_bytes())
                assert "other.prx" in needed, needed
                assert any("otherness.prx.guest.prx" in entry for entry in needed), needed
    print("Guest module alias tests passed")


if __name__ == "__main__":
    main()
