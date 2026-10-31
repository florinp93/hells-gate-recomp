import struct
from dataclasses import dataclass
from typing import BinaryIO, Iterator, List, Tuple

BIG_MAGIC = b'BIGH'
STR_MAGICS = (b'ols3', b'3slo')
BLOCK_CONTENT = 0x53484F43
CONTENT_HEADER = 0x53484452
CONTENT_DATA = 0x53444154
CONTENT_COMPRESSED = 0x5270616B


@dataclass
class BigEntry:
    index: int
    offset: int
    size: int
    name_hash: int


def read_big_entries(f: BinaryIO) -> List[BigEntry]:
    f.seek(0)
    header = f.read(16)
    if header[:4] != BIG_MAGIC:
        raise ValueError('not a BIGH archive')
    count, = struct.unpack('>I', header[8:12])
    table = f.read(12 * count)
    return [BigEntry(i, *struct.unpack_from('>III', table, 12 * i)) for i in range(count)]


def read_big_entry(f: BinaryIO, entry: BigEntry) -> bytes:
    f.seek(entry.offset)
    return f.read(entry.size)


def refpack_decompress(data: bytes) -> bytes:
    header = (data[0] << 8) | data[1]
    if header & 0x1FFF != 0x10FB:
        raise ValueError('not RefPack data')
    pos = 2
    if header & 0x8000:
        size, = struct.unpack_from('>I', data, pos)
        pos += 4
    else:
        size = (data[pos] << 16) | (data[pos + 1] << 8) | data[pos + 2]
        pos += 3
    out = bytearray(size)
    o = 0
    end = len(data)
    while pos < end:
        prefix = data[pos]
        pos += 1
        copy = 0
        stop = False
        if prefix < 0x80:
            b0 = data[pos]
            pos += 1
            plain = prefix & 3
            copy = ((prefix & 0x1C) >> 2) + 3
            distance = (((prefix & 0x60) << 3) | b0) + 1
        elif prefix < 0xC0:
            b0, b1 = data[pos], data[pos + 1]
            pos += 2
            plain = b0 >> 6
            copy = (prefix & 0x3F) + 4
            distance = (((b0 & 0x3F) << 8) | b1) + 1
        elif prefix < 0xE0:
            b0, b1, b2 = data[pos], data[pos + 1], data[pos + 2]
            pos += 3
            plain = prefix & 3
            copy = (((prefix & 0x0C) << 6) | b2) + 5
            distance = (((((prefix & 0x10) << 4) | b0) << 8) | b1) + 1
        elif prefix < 0xFC:
            plain = ((prefix & 0x1F) + 1) * 4
        else:
            plain = prefix & 3
            stop = True
        if plain:
            plain = min(plain, end - pos, size - o)
            out[o:o + plain] = data[pos:pos + plain]
            pos += plain
            o += plain
        if copy:
            copy = min(copy, size - o)
            start = o - distance
            if start < 0:
                break
            if distance >= copy:
                out[o:o + copy] = out[start:start + copy]
            else:
                for i in range(copy):
                    out[o + i] = out[start + i]
            o += copy
        if stop:
            break
    return bytes(out[:o])


def _read_cstring(data: bytes, pos: int) -> Tuple[str, int]:
    end = data.index(b'\0', pos)
    return data[pos:end].decode('ascii', 'replace'), end + 1


def iter_str_files(data: bytes, wanted=None) -> Iterator[Tuple[str, str, bytes]]:
    if data[:4] not in STR_MAGICS:
        raise ValueError('not a StreamSet')
    fmt = '>' if data[:4] == b'ols3' else '<'
    blocks = []
    pos = 12
    end = len(data)
    while pos + 8 <= end:
        kind, size = struct.unpack_from(fmt + 'II', data, pos)
        if size < 8 or pos + size > end:
            break
        if kind == BLOCK_CONTENT and size >= 12:
            content, = struct.unpack_from(fmt + 'I', data, pos + 8)
            blocks.append((content, pos + 12, size - 12))
        pos += size
    i = 0
    while i < len(blocks):
        content, offset, _ = blocks[i]
        i += 1
        if content != CONTENT_HEADER:
            continue
        total, = struct.unpack_from(fmt + 'I', data, offset + 28)
        _, p = _read_cstring(data, offset + 32)
        file_name, p = _read_cstring(data, p)
        type_name, _ = _read_cstring(data, p)
        type_name = type_name.lower()
        if wanted is not None and type_name not in wanted:
            while i < len(blocks) and blocks[i][0] in (CONTENT_DATA, CONTENT_COMPRESSED):
                i += 1
            continue
        payload = bytearray()
        while len(payload) < total and i < len(blocks):
            content, offset, size = blocks[i]
            if content == CONTENT_COMPRESSED:
                packed, = struct.unpack_from(fmt + 'I', data, offset)
                payload += refpack_decompress(data[offset + 4:offset + 4 + packed])
            elif content == CONTENT_DATA:
                payload += data[offset:offset + size]
            else:
                break
            i += 1
        yield type_name, file_name, bytes(payload[:total])


def open_entry(path: str, entry_index: int) -> bytes:
    with open(path, 'rb') as f:
        entries = read_big_entries(f)
        return read_big_entry(f, entries[entry_index])

