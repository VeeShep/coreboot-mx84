#!/usr/bin/env python3
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parents[1]
HELPER = HERE / "root/usr/lib/mx84-platform/mx84-pfsense-start"


def main():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        virsh = root / "virsh"
        virsh.write_text("""#!/bin/sh
printf '%s\\n' "$*" >> "$CALLS"
case "$1" in
    domstate) printf '%s\\n' "$STATE"; exit "$QUERY_RC" ;;
    start) exit "$START_RC" ;;
    *) exit 99 ;;
esac
""")
        virsh.chmod(0o755)
        for state, query, start, expected in (
            ("running", 0, 0, 0), ("shut off", 0, 0, 0),
            ("shut off", 0, 7, 7), ("paused", 0, 0, 1),
            ("in shutdown", 0, 0, 1), ("", 4, 0, 4),
        ):
            calls = root / "calls"
            calls.write_text("")
            environment = dict(os.environ, PATH=f"{root}:{os.environ['PATH']}",
                               STATE=state, QUERY_RC=str(query), START_RC=str(start),
                               CALLS=str(calls))
            result = subprocess.run(["sh", str(HELPER)], env=environment, capture_output=True)
            assert result.returncode == expected, (state, result.stderr)
            expected_calls = ["domstate pfsense"]
            if state == "shut off" and not query:
                expected_calls.append("start pfsense")
            assert calls.read_text().splitlines() == expected_calls
    unit = (HERE / "root/usr/lib/systemd/system/mx84-pfsense.service").read_text()
    assert "Requires=mx84-platform-ready.service\n" in unit
    assert "Wants=libvirtd.service\n" in unit
    assert "ExecStartPre=/usr/lib/mx84-platform/mx84-platform-ready\n" in unit
    assert "ExecStart=/usr/lib/mx84-platform/mx84-pfsense-start\n" in unit
    print("PASS: pfSense start/reuse/error states and non-propagating libvirt dependency")


if __name__ == "__main__":
    main()
