import hashlib
import json
import os
import re
import struct
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import asdict, dataclass, field
from typing import Callable, Dict, Iterator, List, Optional, Tuple

import xxhash
from PIL import Image

from texture_studio import archives

GUEST_FORMATS = {
    'DXT1': (18, 8, 4),
    'DXT1c': (18, 8, 4),
    'DXT1a': (18, 8, 4),
    'DXT5': (20, 16, 4),
    'DXT5_NM': (20, 16, 4),
    'RGBA8': (6, 4, 1),
    'RGBX8': (6, 4, 1),
    'L8': (2, 1, 1),
    'A8': (2, 1, 1),
}
FLAG_TOKENS = {'sRGB', 'aniso', 'cube', '3d', 'clamp'}
TEXTURE_TYPES = {'tg4h', 'tg4d'}
INDEX_FILE = 'index.json'
MANIFEST_FILE = 'textures.json'

Progress = Callable[[int, int, str], bool]


@dataclass
class TextureInfo:
    name: str
    group: str
    width: int
    height: int
    mips: int
    data_size: int
    format: str
    flags: List[str] = field(default_factory=list)

    @property
    def is_2d(self) -> bool:
        return 'cube' not in self.flags and '3d' not in self.flags


def parse_tg4h(data: bytes) -> Optional[TextureInfo]:
    if len(data) < 0x30:
        return None
    data_size, = struct.unpack('>I', data[0x1C:0x20])
    width, height = struct.unpack('>HH', data[0x20:0x24])
    tokens = [t.decode('ascii') for t in re.findall(rb'[\x20-\x7e]{2,}', data[0x30:])]
    fmt_index = next((i for i, t in enumerate(tokens) if t in GUEST_FORMATS), None)
    if fmt_index is None or len(tokens) < 2:
        return None
    flags = [t for t in tokens[fmt_index + 1:-2] if t in FLAG_TOKENS]
    return TextureInfo(tokens[-1], tokens[-2], width, height, max(1, data[0x26]), data_size,
                       tokens[fmt_index], flags)


def level0_size(info: TextureInfo) -> int:
    _, unit, block = GUEST_FORMATS[info.format]
    return ((info.width + block - 1) // block) * ((info.height + block - 1) // block) * unit


def host_level0(info: TextureInfo, tg4d: bytes) -> bytes:
    data = tg4d[:level0_size(info)]
    if GUEST_FORMATS[info.format][1] not in (8, 16):
        return data
    out = bytearray(data)
    out[0::2], out[1::2] = data[1::2], data[0::2]
    return bytes(out)


def runtime_hash(info: TextureInfo, tg4d: bytes) -> str:
    seed = info.width | (info.height << 16) | (GUEST_FORMATS[info.format][0] << 32)
    return f'{xxhash.xxh3_64_intdigest(host_level0(info, tg4d), seed=seed):016X}'


def decode(info: TextureInfo, tg4d: bytes) -> Image.Image:
    import texture2ddecoder
    level0 = host_level0(info, tg4d)
    size = (info.width, info.height)
    unit = GUEST_FORMATS[info.format][1]
    if unit in (8, 16):
        bc = texture2ddecoder.decode_bc1 if unit == 8 else texture2ddecoder.decode_bc3
        image = Image.frombytes('RGBA', size, bc(level0, *size), 'raw', 'BGRA')
        if info.format == 'DXT1c':
            image.putalpha(255)
        return image
    if unit == 4:
        return Image.frombytes('RGBA', size, level0, 'raw', 'ARGB')
    return Image.frombytes('L', size, level0).convert('RGBA')


def iter_textures(str_data: bytes) -> Iterator[Tuple[int, TextureInfo, bytes]]:
    pending = None
    ordinal = 0
    for type_name, _, payload in archives.iter_str_files(str_data, TEXTURE_TYPES):
        if type_name == 'tg4h':
            pending = parse_tg4h(payload)
        elif pending:
            if len(payload) >= pending.data_size and pending.format in GUEST_FORMATS:
                yield ordinal, pending, payload
            ordinal += 1
            pending = None


def _scan_entry(viv_path: str, entry: archives.BigEntry) -> List[dict]:
    with open(viv_path, 'rb') as f:
        data = archives.read_big_entry(f, entry)
    if data[:4] not in archives.STR_MAGICS:
        return []
    found = []
    try:
        for ordinal, info, tg4d in iter_textures(data):
            if not info.is_2d:
                continue
            record = asdict(info)
            record['hash'] = runtime_hash(info, tg4d)
            record['location'] = [os.path.basename(viv_path), entry.index, ordinal]
            found.append(record)
    except (ValueError, IndexError, struct.error):
        pass
    return found


def scan_game(game_dir: str, progress: Progress, workers: Optional[int] = None) -> Dict[str, dict]:
    jobs = []
    for name in sorted(os.listdir(game_dir)):
        if re.fullmatch(r'bigfile\d+\.viv', name.lower()):
            path = os.path.join(game_dir, name)
            with open(path, 'rb') as f:
                jobs += [(path, e) for e in archives.read_big_entries(f)]
    index: Dict[str, dict] = {}
    done = 0
    with ProcessPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(_scan_entry, path, entry) for path, entry in jobs]
        for future in as_completed(futures):
            for record in future.result():
                location = record.pop('location')
                existing = index.setdefault(record['hash'], record)
                existing.setdefault('locations', []).append(location)
                existing.setdefault('names', set()).add((record['name'], record['group']))
            done += 1
            if not progress(done, len(futures), f'{len(index)} textures'):
                for f in futures:
                    f.cancel()
                break
    for record in index.values():
        names = sorted(record.pop('names'))
        record['name'], record['group'] = names[0]
        record['aliases'] = sorted({n for n, _ in names[1:]} - {record['name']})
        record['locations'].sort()
    return dict(sorted(index.items(), key=lambda item: item[1]['name']))


def load_index(project: str) -> Dict[str, dict]:
    path = os.path.join(project, INDEX_FILE)
    if not os.path.exists(path):
        return {}
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def save_json(path: str, data) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=1)
    os.replace(tmp, path)


def load_manifest(project: str) -> Dict[str, dict]:
    path = os.path.join(project, MANIFEST_FILE)
    if not os.path.exists(path):
        return {}
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def safe_part(s: str) -> str:
    return re.sub(r'[^A-Za-z0-9_.-]+', '_', s) or 'unnamed'


def image_digest(image: Image.Image) -> str:
    return hashlib.sha1(image.convert('RGBA').tobytes()).hexdigest()


def relative_png(record: dict) -> str:
    return f"{safe_part(record['group'])}/{safe_part(record['name'])}_{record['hash'][:8]}.png"


def read_texture(game_dir: str, record: dict) -> Tuple[TextureInfo, bytes]:
    viv, entry_index, ordinal = record['locations'][0]
    data = archives.open_entry(os.path.join(game_dir, viv), entry_index)
    for o, info, tg4d in iter_textures(data):
        if o == ordinal:
            return info, tg4d
    raise ValueError(f"texture {record['name']} not found in {viv} entry {entry_index}")


def export_textures(game_dir: str, project: str, records: List[dict],
                    progress: Progress) -> int:
    manifest = load_manifest(project)
    by_entry: Dict[Tuple[str, int], List[dict]] = {}
    for record in records:
        viv, entry_index, _ = record['locations'][0]
        by_entry.setdefault((viv, entry_index), []).append(record)
    done = exported = 0
    for (viv, entry_index), group in by_entry.items():
        data = archives.open_entry(os.path.join(game_dir, viv), entry_index)
        wanted = {r['locations'][0][2]: r for r in group}
        for ordinal, info, tg4d in iter_textures(data):
            record = wanted.get(ordinal)
            if not record:
                continue
            rel = relative_png(record)
            image = decode(info, tg4d)
            path = os.path.join(project, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            image.save(path)
            manifest[rel] = {
                'hash': record['hash'],
                'name': record['name'],
                'group': record['group'],
                'width': record['width'],
                'height': record['height'],
                'format': record['format'],
                'original_digest': image_digest(image),
            }
            exported += 1
            done += 1
            if not progress(done, len(records), rel):
                save_json(os.path.join(project, MANIFEST_FILE), manifest)
                return exported
    save_json(os.path.join(project, MANIFEST_FILE), manifest)
    return exported


def changed_textures(project: str) -> List[Tuple[str, dict]]:
    changed = []
    for rel, entry in load_manifest(project).items():
        path = os.path.join(project, rel)
        if not os.path.exists(path):
            continue
        with Image.open(path) as image:
            if image_digest(image) != entry['original_digest']:
                changed.append((rel, entry))
    return changed


def mip_chain(image: Image.Image) -> List[Image.Image]:
    levels = [image]
    while image.width > 1 or image.height > 1:
        image = image.resize((max(image.width // 2, 1), max(image.height // 2, 1)),
                             Image.LANCZOS)
        levels.append(image)
    return levels


def write_dds(path: str, image: Image.Image) -> None:
    levels = mip_chain(image.convert('RGBA'))
    w, h = image.size
    header = struct.pack('<4s7I44x', b'DDS ', 124, 0x100F | 0x20000 | 0x8, h, w, w * 4, 0,
                         len(levels))
    pixel_format = struct.pack('<2I4s5I', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)
    caps = struct.pack('<5I', 0x401008, 0, 0, 0, 0)
    dx10 = struct.pack('<5I', 28, 3, 0, 1, 0)
    with open(path, 'wb') as f:
        f.write(header + pixel_format + caps + dx10)
        for level in levels:
            f.write(level.tobytes())


def build_mod(project: str, mod_dir: str, name: str, author: str, version: str,
              progress: Progress) -> Tuple[int, List[str]]:
    changed = changed_textures(project)
    out_dir = os.path.join(mod_dir, 'textures')
    os.makedirs(out_dir, exist_ok=True)
    warnings = []
    for i, (rel, entry) in enumerate(changed, 1):
        with Image.open(os.path.join(project, rel)) as source:
            image = source.convert('RGBA')
        if image.width * entry['height'] != image.height * entry['width']:
            warnings.append(f"{rel}: {image.width}x{image.height} changes the "
                            f"{entry['width']}x{entry['height']} aspect ratio")
        write_dds(os.path.join(out_dir, f"{safe_part(entry['name'])}_{entry['hash']}.dds"), image)
        if not progress(i, len(changed), rel):
            break
    with open(os.path.join(mod_dir, 'mod.toml'), 'w', encoding='utf-8') as f:
        f.write(f'name = {json.dumps(name)}\nauthor = {json.dumps(author)}\n'
                f'version = {json.dumps(version)}\n')
    return len(changed), warnings
