#!/usr/bin/env python3
import pathlib
import sys


source = pathlib.Path(sys.argv[1]).read_bytes()
if not source or len(source) > 16 * 1024 * 1024:
    raise SystemExit("shellcode.bin must be between 1 byte and 16 MiB")

output = [
    "#ifndef EMBEDDED_STAGE_H",
    "#define EMBEDDED_STAGE_H",
    "",
    "static unsigned char stage_data[] = {",
]
for offset in range(0, len(source), 12):
    chunk = source[offset:offset + 12]
    output.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
output.extend([
    "};",
    f"static const unsigned int stage_size = {len(source)}u;",
    "",
    "#endif",
    "",
])

pathlib.Path(sys.argv[2]).write_text("\n".join(output), encoding="ascii")
