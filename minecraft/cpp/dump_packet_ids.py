#!/usr/bin/env python3
"""Dump Minecraft 26.3 packet ids from the deobfuscated common jar.

The jar is the one Fabric Loom already downloaded for cks-minecraft-mod.
Packet ids are the order of ProtocolInfoBuilder.addPacket in the protocol
classes. Play clientbound counts the bundle delimiter registered by
withBundlePacket as id 0 (empty unit packet). Unknown packets are skipped
by frame length; this header only needs to name the ones ck-mc-load answers.
"""
import argparse
import struct
import sys
import zipfile
from pathlib import Path

DEFAULT_JAR = Path(
    "/home/ubuntu/.gradle/caches/fabric-loom/minecraftMaven/net/minecraft/"
    "minecraft-common-deobf/26.3/minecraft-common-deobf-26.3.jar"
)


def parse_cp(data):
    cp_count = struct.unpack(">H", data[8:10])[0]
    i = 10
    idx = 1
    cp = [None]
    while idx < cp_count:
        tag = data[i]
        i += 1
        if tag == 1:
            ln = struct.unpack(">H", data[i : i + 2])[0]
            i += 2
            s = data[i : i + ln].decode("utf-8", "replace")
            i += ln
            cp.append(("u", s))
        elif tag in (7, 8, 16, 19, 20):
            cp.append((tag, struct.unpack(">H", data[i : i + 2])[0]))
            i += 2
        elif tag in (3, 4):
            cp.append((tag, struct.unpack(">i", data[i : i + 4])[0]))
            i += 4
        elif tag in (9, 10, 11, 12, 17, 18):
            a, b = struct.unpack(">HH", data[i : i + 4])
            i += 4
            cp.append((tag, a, b))
        elif tag in (5, 6):
            cp.append((tag, data[i : i + 8]))
            i += 8
            cp.append(None)
            idx += 1
        elif tag == 15:
            cp.append((15, data[i]))
            i += 3
        else:
            raise SystemExit(f"bad constant tag {tag}")
        idx += 1
    return cp, i


def utf(cp, i):
    t = cp[i]
    return t[1] if isinstance(t, tuple) and t[0] == "u" else None


def cls(cp, i):
    t = cp[i]
    return utf(cp, t[1]) if isinstance(t, tuple) and t[0] == 7 else None


def nat(cp, i):
    t = cp[i]
    if isinstance(t, tuple) and t[0] == 12:
        return utf(cp, t[1]), utf(cp, t[2])
    return None, None


def methods(data):
    cp, i = parse_cp(data)
    _access, _this, _super, ifaces = struct.unpack(">HHHH", data[i : i + 8])
    i += 8 + ifaces * 2
    fc = struct.unpack(">H", data[i : i + 2])[0]
    i += 2
    for _ in range(fc):
        i += 6
        ac = struct.unpack(">H", data[i : i + 2])[0]
        i += 2
        for __ in range(ac):
            i += 2
            ln = struct.unpack(">I", data[i : i + 4])[0]
            i += 4 + ln
    mc = struct.unpack(">H", data[i : i + 2])[0]
    i += 2
    out = []
    for _ in range(mc):
        _acc, name_i, desc_i, ac = struct.unpack(">HHHH", data[i : i + 8])
        i += 8
        name, desc = utf(cp, name_i), utf(cp, desc_i)
        code = None
        for __ in range(ac):
            an, ln = struct.unpack(">HI", data[i : i + 6])
            i += 6
            info = data[i : i + ln]
            i += ln
            if utf(cp, an) == "Code":
                code = info
        out.append((name, desc, code, cp))
    return out


def registered(code, cp, bundle_delimiter=False):
    """Packet-type field names in registration order."""
    code_len = struct.unpack(">I", code[4:8])[0]
    bc = code[8 : 8 + code_len]
    j = 0
    pending = None
    names = []
    saw_bundle = False

    def field_name(ix):
        t = cp[ix]
        if isinstance(t, tuple) and t[0] == 9:
            _c = cls(cp, t[1])
            n, _d = nat(cp, t[2])
            if n and ("CLIENTBOUND" in n or "SERVERBOUND" in n or n == "CLIENT_INTENTION"):
                return n
        return None

    while j < len(bc):
        op = bc[j]
        if op == 0xB2:
            ix = struct.unpack(">H", bc[j + 1 : j + 3])[0]
            name = field_name(ix)
            if name:
                pending = name
            j += 3
        elif op in (0xB6, 0xB7, 0xB8):
            ix = struct.unpack(">H", bc[j + 1 : j + 3])[0]
            t = cp[ix]
            called = None
            if isinstance(t, tuple) and t[0] in (10, 11):
                _c = cls(cp, t[1])
                called, _d = nat(cp, t[2])
            if called == "withBundlePacket":
                names.append("CLIENTBOUND_BUNDLE_DELIMITER")
                saw_bundle = True
                pending = None
            elif called == "addPacket" and pending:
                names.append(pending)
                pending = None
            j += 3
        elif op == 0xB9:
            j += 5
        elif op in (0xB3, 0xB4, 0xB5, 0xBB, 0xC0, 0xC1):
            j += 3
        elif op == 0x12:
            j += 2
        elif op in (0x13, 0x14, 0x10):
            j += 2 if op == 0x10 else 3
        elif op == 0x11:
            j += 3
        elif op == 0xAA:
            pad = (4 - (j + 1) % 4) % 4
            k = j + 1 + pad
            _default, low, high = struct.unpack(">iii", bc[k : k + 12])
            j = k + 12 + (high - low + 1) * 4
        elif op == 0xAB:
            pad = (4 - (j + 1) % 4) % 4
            k = j + 1 + pad
            _default, npairs = struct.unpack(">ii", bc[k : k + 8])
            j = k + 8 + npairs * 8
        else:
            j += 1
    if bundle_delimiter and not saw_bundle:
        raise SystemExit("expected a bundle delimiter registration")
    return names


def lambda_code(z, path, name):
    data = z.read(path)
    for n, _d, code, cp in methods(data):
        if n == name:
            return code, cp
    raise SystemExit(f"{name} missing in {path}")


def cpp_enum(name, items):
    lines = [f"enum class {name} : int {{"]
    for i, item in enumerate(items):
        ident = item
        for prefix in ("CLIENTBOUND_", "SERVERBOUND_"):
            if ident.startswith(prefix):
                ident = ident[len(prefix) :]
                break
        lines.append(f"  {ident} = {i},")
    lines.append("};")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jar", type=Path, default=DEFAULT_JAR)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    if not args.jar.is_file():
        print(f"jar not found: {args.jar}", file=sys.stderr)
        return 1
    z = zipfile.ZipFile(args.jar)
    groups = {
        "SbHandshake": ("net/minecraft/network/protocol/handshake/HandshakeProtocols.class", "lambda$static$0", False),
        "CbLogin": ("net/minecraft/network/protocol/login/LoginProtocols.class", "lambda$static$1", False),
        "SbLogin": ("net/minecraft/network/protocol/login/LoginProtocols.class", "lambda$static$0", False),
        "CbConfig": ("net/minecraft/network/protocol/configuration/ConfigurationProtocols.class", "lambda$static$1", False),
        "SbConfig": ("net/minecraft/network/protocol/configuration/ConfigurationProtocols.class", "lambda$static$0", False),
        "CbPlay": ("net/minecraft/network/protocol/game/GameProtocols.class", "lambda$static$3", True),
        "SbPlay": ("net/minecraft/network/protocol/game/GameProtocols.class", "lambda$static$1", False),
    }
    parts = [
        "#pragma once",
        "// Generated by dump_packet_ids.py from minecraft-common-deobf 26.3.",
        "// Protocol version is SharedConstants of that jar (777). Do not edit by hand.",
        "namespace ckmc {",
        "inline constexpr int kProtocolVersion = 777;",
        "",
    ]
    for enum_name, (path, lam, bundle) in groups.items():
        code, cp = lambda_code(z, path, lam)
        items = registered(code, cp, bundle_delimiter=bundle)
        if not items:
            raise SystemExit(f"no packets in {enum_name}")
        parts.append(cpp_enum(enum_name, items))
        parts.append("")
    parts.append("}  // namespace ckmc")
    parts.append("")
    text = "\n".join(parts)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text)
    print(f"wrote {args.out} ({text.count(chr(10))} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
