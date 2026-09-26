#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate Firmware/manifest.json for the OpenDTU inverter firmware picker.

The webapp fetches this manifest straight from GitHub (raw.githubusercontent.com)
and lets the user pick a matching image for the connected inverter, so the
manifest carries everything the browser needs to filter, display and verify a
file without any help from the DTU:

  * path / name / family / folder  -- where the .hex lives below Firmware/
  * version / version_code         -- decoded from the file's identity row
  * identity                       -- the four identity nibble bytes as hex
  * serial_prefixes                -- inverter serial prefixes (top 16 bits of
                                      the upper serial half) whose identity rule
                                      matches this file; mirrors
                                      kFirmwareSerialRules in src/WebApi_devinfo.cpp
  * size / sha256 / rows           -- for integrity checks after download
  * date                           -- last git commit date of the file

Run from the repository root:

    python tools/gen_firmware_manifest.py [--ref BRANCH] [--repo OWNER/REPO]

The manifest is written to Firmware/manifest.json.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

# preSerial, newGen1, phase, inputType, dsp, newGen2, newGen3, bType
# Keep in sync with kFirmwareSerialRules in src/WebApi_devinfo.cpp.
FIRMWARE_SERIAL_RULES = [
    (0x1121, 0, 1, 0, 0, 0, 0, 0),  # HM 1T MI
    (0x1124, 0, 1, 0, 0, 0, 0, 0),  # HMS 1T MI
    (0x1125, 0, 1, 0, 0, 0, 0, 1),  # HMS 1T B
    (0x1126, 0, 1, 0, 0, 0, 0, 2),  # HMS 1T US
    (0x1400, 0, 1, 0, 0, 0, 0, 1),  # HMS 1T B
    (0x1141, 0, 1, 1, 0, 0, 0, 0),  # HM 2T MI
    (0x1143, 0, 1, 1, 1, 0, 0, 1),  # HMS 2T B
    (0x1144, 0, 1, 1, 1, 0, 0, 1),  # HMS 2T B
    (0x1146, 0, 1, 1, 1, 0, 0, 2),  # HMS 2T US
    (0x1410, 0, 1, 1, 1, 0, 0, 1),  # HMS 2T B
    (0x1161, 0, 1, 2, 0, 0, 0, 0),  # HM 4T MI
    (0x1162, 0, 4, 2, 0, 0, 0, 0),  # HME1 4T MI
    (0x1164, 0, 1, 2, 1, 0, 0, 0),  # HMS 4T MI
    (0x1165, 0, 1, 2, 2, 0, 0, 0),  # HMS 4T MI (2000B_T)
    (0x1166, 0, 1, 2, 1, 1, 0, 1),  # HMS 4T B (2000C_B)
    (0x1421, 0, 1, 2, 1, 1, 0, 1),  # HMS 4T B (2000C_B)
    (0x1620, 0, 1, 2, 3, 0, 0, 1),  # HMS 4T B (WB_B)
    (0x1361, 0, 3, 5, 0, 0, 0, 0),  # HMT 4T MI
    (0x1362, 0, 3, 6, 0, 0, 0, 0),  # HMT 4T MI (NA R)
    (0x1382, 0, 3, 3, 0, 0, 0, 0),  # HMT 6T MI
    (0x1520, 0, 1, 6, 0, 0, 0, 0),  # MIT-5000 MI
]

# Serial prefixes the DTU accepts for an update at all
# (kAllowedFirmwareUpdateSerialPrefixes in src/WebApi_devinfo.cpp).
ALLOWED_SERIAL_PREFIXES = {
    0x1121, 0x1141, 0x1161, 0x1162, 0x1124, 0x1126, 0x1400,
    0x1125, 0x1143, 0x1144, 0x1146, 0x1410, 0x1361, 0x1362,
    0x1164, 0x1165, 0x1166, 0x1421, 0x1620, 0x1382,
}

MAX_FIRMWARE_UPLOAD_SIZE = 800 * 1024  # MAX_FIRMWARE_UPLOAD_SIZE in src/WebApi_file.cpp

DEFAULT_REPO = "xX-nichtlachen-Xx/OpenDTU"
DEFAULT_REF = "FirmwareUpdate_HM_IV"


def decode_row(line: str) -> tuple[list[int], int] | None:
    """Mirror IntelHex::decodeRow(): returns (all bytes incl. checksum, record type)."""
    line = line.strip()
    if not line.startswith(":"):
        return None
    body = line[1:]
    if len(body) < 2 or len(body) % 2 != 0:
        raise ValueError("odd hex length")
    data = bytes.fromhex(body)
    if sum(data) & 0xFF != 0:
        raise ValueError("line checksum mismatch")
    if data[0] != len(data) - 5:
        raise ValueError("byte count field does not match line length")
    record_type = data[3] if len(data) >= 4 else 0xFF
    return list(data), record_type


def identity_matches_rule(identity: list[int], rule: tuple) -> bool:
    """Mirror identityRowMatchesRule() (identity = row bytes [4..7])."""
    _, new_gen1, phase, input_type, dsp, new_gen2, new_gen3, b_type = rule
    return (
        (identity[0] & 0x0F) == new_gen1
        and (identity[1] >> 4) == phase
        and (identity[1] & 0x0F) == input_type
        and (identity[2] >> 4) == dsp
        and (identity[2] & 0x0F) == new_gen2
        and (identity[3] >> 4) == new_gen3
        and (identity[3] & 0x0F) == b_type
    )


def format_version(code: int) -> str:
    # Hoymiles encodes the version as a decimal number MMmmpp, e.g. 0x2904 =
    # 10500 -> 1.5.0 and 0x4E22 = 20002 -> 2.0.2.
    return f"{code // 10000}.{(code // 100) % 100}.{code % 100}"


def git_commit_date(path: Path, repo_root: Path) -> str | None:
    try:
        out = subprocess.run(
            ["git", "log", "-1", "--format=%cI", "--", str(path.relative_to(repo_root))],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
        return out or None
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def git_head(repo_root: Path) -> str | None:
    try:
        return subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=repo_root, capture_output=True, text=True, check=True
        ).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def analyze_hex(path: Path) -> dict:
    raw = path.read_bytes()
    text = raw.decode("ascii", errors="strict")
    rows = 0
    has_eof = False
    identity: list[int] | None = None
    version_code: int | None = None

    for line_no, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            decoded = decode_row(line)
        except ValueError as exc:
            raise ValueError(f"{path}: line {line_no}: {exc}") from exc
        if decoded is None:
            raise ValueError(f"{path}: line {line_no}: does not start with ':'")
        data, record_type = decoded
        if has_eof:
            raise ValueError(f"{path}: line {line_no}: data after EOF record")
        rows += 1
        if record_type == 0x01:
            has_eof = True
        if identity is None:
            # First row is the vendor identity row: LL=06, type 0x11, then
            # [4]=const 0x10 [5]=phase/inputType [6]=dsp/newGen2 [7]=newGen3/bType
            # [8..9]=version (big endian).
            if record_type != 0x11 or len(data) < 11:
                raise ValueError(f"{path}: first row is not an identity row")
            identity = data[4:8]
            version_code = (data[8] << 8) | data[9]

    if not has_eof:
        raise ValueError(f"{path}: no EOF record")
    if identity is None or version_code is None:
        raise ValueError(f"{path}: empty file")

    prefixes = sorted(
        f"{rule[0]:04X}"
        for rule in FIRMWARE_SERIAL_RULES
        if rule[0] in ALLOWED_SERIAL_PREFIXES and identity_matches_rule(identity, rule)
    )

    return {
        "size": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "rows": rows,
        "identity": "".join(f"{b:02X}" for b in identity),
        "version_code": version_code,
        "version": format_version(version_code),
        "serial_prefixes": prefixes,
    }


def family_of(rel: Path) -> str:
    return rel.parts[0] if len(rel.parts) > 1 else "Unknown"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", default=DEFAULT_REPO, help="GitHub owner/repo (default: %(default)s)")
    parser.add_argument("--ref", default=DEFAULT_REF, help="branch, tag or commit used in download URLs (default: %(default)s)")
    parser.add_argument("--firmware-dir", default=None, help="Firmware directory (default: <repo>/Firmware)")
    parser.add_argument("--output", default=None, help="output file (default: <firmware-dir>/manifest.json)")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    fw_dir = Path(args.firmware_dir).resolve() if args.firmware_dir else repo_root / "Firmware"
    output = Path(args.output).resolve() if args.output else fw_dir / "manifest.json"

    if not fw_dir.is_dir():
        print(f"error: firmware directory not found: {fw_dir}", file=sys.stderr)
        return 1

    base_url = f"https://raw.githubusercontent.com/{args.repo}/{args.ref}/Firmware/"

    entries: list[dict] = []
    seen_sha: dict[str, dict] = {}
    errors = 0

    for path in sorted(fw_dir.rglob("*.hex"), key=lambda p: (len(p.relative_to(fw_dir).parts), str(p).lower())):
        rel = path.relative_to(fw_dir)
        try:
            info = analyze_hex(path)
        except (ValueError, UnicodeDecodeError) as exc:
            print(f"skip: {exc}", file=sys.stderr)
            errors += 1
            continue

        if info["size"] > MAX_FIRMWARE_UPLOAD_SIZE:
            print(f"skip: {rel}: {info['size']} bytes exceeds DTU upload limit", file=sys.stderr)
            continue

        # Identical images stored under several names (e.g. the per-serial
        # capture folders next to the plain HM_*.hex) are listed once, under
        # the shallowest / shortest path thanks to the sort order above.
        dup = seen_sha.get(info["sha256"])
        if dup is not None:
            dup.setdefault("aliases", []).append(rel.as_posix())
            continue

        entry = {
            "path": rel.as_posix(),
            "name": path.stem,
            "family": family_of(rel),
            "folder": rel.parent.as_posix() if str(rel.parent) != "." else "",
            "date": git_commit_date(path, repo_root)
            or datetime.fromtimestamp(path.stat().st_mtime, tz=timezone.utc).isoformat(timespec="seconds"),
            **info,
        }
        seen_sha[info["sha256"]] = entry
        entries.append(entry)

    manifest = {
        "schema": 1,
        "generated": datetime.now(tz=timezone.utc).isoformat(timespec="seconds"),
        "repo": args.repo,
        "ref": args.ref,
        "commit": git_head(repo_root),
        "base_url": base_url,
        "changelog_url": base_url + "Hoymiles_Inverter_Firmware_Changelog.txt",
        "mapping_url": base_url + "Inverter_Serial_to_File_Mapping.txt",
        "max_size": MAX_FIRMWARE_UPLOAD_SIZE,
        "files": entries,
    }

    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {output} ({len(entries)} files, {errors} skipped)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
