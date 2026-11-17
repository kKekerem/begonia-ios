#!/usr/bin/env python3
"""
Apple Device Tree (ADT) binary serializer, parser, and IM4P unwrapper.

ADT binary layout (little-endian):
  node = { u32 nprops, u32 nchildren, props..., children... }
  prop = { char name[32], u32 size_and_flags (bit 31 = placeholder), u8 value[padded_to_4] }
"""
from __future__ import annotations

import struct
import subprocess
from typing import List, Optional, Tuple


class Node:
    """In-memory representation of a single Apple Device Tree node."""

    def __init__(self, name: Optional[str] = None) -> None:
        self.props: List[Tuple[str, int, bytes]] = []
        self.children: List[Node] = []
        if name is not None:
            self.props.append(("name", 0, name.encode("utf-8") + b"\0"))

    @property
    def name(self) -> str:
        val = self.prop("name")
        if val is None:
            return "?"
        return val.split(b"\0", 1)[0].decode("utf-8", errors="replace")

    def prop(self, key: str) -> Optional[bytes]:
        for name, _, val in self.props:
            if name == key:
                return val
        return None

    def child(self, name: str) -> Optional[Node]:
        for c in self.children:
            if c.name == name:
                return c
        return None


def u32(*values: int) -> bytes:
    return struct.pack(f"<{len(values)}I", *values)


def u64(*values: int) -> bytes:
    return struct.pack(f"<{len(values)}Q", *values)


def cstr(text: str) -> bytes:
    return text.encode("utf-8") + b"\0"


def make_node(
    name: str,
    props: Tuple[Tuple[str, bytes], ...] | List[Tuple[str, bytes]] = (),
    children: Tuple[Node, ...] | List[Node] = (),
) -> Node:
    n = Node(name)
    for key, val in props:
        n.props.append((key, 0, val))
    n.children.extend(children)
    return n


def parse(buf: bytes, off: int = 0) -> Tuple[Node, int]:
    """Deserialize an ADT buffer starting at offset `off`."""
    node = Node()
    nprops, nchildren = struct.unpack_from("<II", buf, off)
    off += 8
    for _ in range(nprops):
        raw_name = buf[off : off + 32].split(b"\0", 1)[0].decode("utf-8", errors="replace")
        raw_size, = struct.unpack_from("<I", buf, off + 32)
        flags = (raw_size >> 31) & 1
        length = raw_size & 0x7FFFFFFF
        val = buf[off + 36 : off + 36 + length]
        node.props.append((raw_name, flags, val))
        off += 36 + ((length + 3) & ~3)
    for _ in range(nchildren):
        child, off = parse(buf, off)
        node.children.append(child)
    return node, off


def serialize(node: Node) -> bytes:
    """Serialize an ADT node hierarchy into binary format."""
    out = bytearray(struct.pack("<II", len(node.props), len(node.children)))
    for name, flags, val in node.props:
        encoded = name.encode("utf-8")[:31].ljust(32, b"\0")
        out += encoded
        out += struct.pack("<I", (len(val) & 0x7FFFFFFF) | ((flags & 1) << 31))
        out += val
        pad = (-len(val)) & 3
        if pad:
            out += b"\0" * pad
    for child in node.children:
        out += serialize(child)
    return bytes(out)


def _der_elem(buf: bytes, off: int) -> Tuple[int, int, int]:
    tag = buf[off]
    length = buf[off + 1]
    off += 2
    if length & 0x80:
        nbytes = length & 0x7F
        length = int.from_bytes(buf[off : off + nbytes], "big")
        off += nbytes
    return tag, off, off + length


def unwrap_im4p(buf: bytes) -> bytes:
    """Extract raw payload from an unencrypted IM4P DeviceTree container."""
    if not (buf[:1] == b"\x30" and b"IM4P" in buf[:16]):
        return buf
    tag, start, end = _der_elem(buf, 0)
    if tag != 0x30:
        raise ValueError("not a DER SEQUENCE")
    items = []
    pos = start
    while pos < end:
        t, vs, ve = _der_elem(buf, pos)
        items.append((t, buf[vs:ve]))
        pos = ve
    if not items or items[0][1] != b"IM4P":
        raise ValueError("missing IM4P magic")
    if any(t == 0x04 for t, _ in items[4:]):
        raise ValueError("IM4P payload is encrypted (KBAG present)")
    payload = items[3][1]
    if payload[:4] == b"bvx2":
        payload = subprocess.run(["lzfse", "-decode"], input=payload, capture_output=True, check=True).stdout
    return payload
