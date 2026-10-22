#!/usr/bin/env python3
"""Compare one pair of saved PNG/P6 frames exactly; never launches the game.

Exit 0: identical decoded RGBA; 1: different pixels; 2: input/output error.
See compare_frames.md for supported inputs, context and interpretation limits.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys

LIMIT = 64 * 1024 * 1024
PILLOW_VERSION = '13.0.0'
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
SPACE = b' \t\r\n\v\f'


class ComparisonError(ValueError):
    pass


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise ComparisonError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_snapshot(filename):
    """One disk read; all later inspection/decoding operates on these immutable bytes."""
    with Path(filename).open('rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise ComparisonError(f'{filename}: expected a completed regular file')
        if info.st_size > LIMIT:
            raise ComparisonError(f'{filename}: encoded input exceeds 64 MiB')
        data = stream.read(LIMIT + 1)
    if len(data) > LIMIT:
        raise ComparisonError(f'{filename}: encoded input exceeds 64 MiB')
    return data


def dimensions(width, height):
    if width <= 0 or height <= 0:
        raise ComparisonError('image dimensions must be positive')
    if width * height * 4 > LIMIT:
        raise ComparisonError('decoded RGBA exceeds 64 MiB per input')


def png_header(data):
    if len(data) < 33 or data[8:16] != b'\0\0\0\rIHDR':
        raise ComparisonError('invalid PNG header')
    width, height, depth, colour, compression, filtering, interlace = struct.unpack('>IIBBBBB', data[16:29])
    dimensions(width, height)
    if depth != 8 or colour not in (2, 6) or compression or filtering or interlace not in (0, 1):
        raise ComparisonError('PNG requires static 8-bit RGB/RGBA and interlace method 0 or 1')
    # Inspect chunk boundaries only; Pillow verifies CRCs and decodes pixels.
    offset = 8
    while offset + 12 <= len(data):
        length = struct.unpack_from('>I', data, offset)[0]
        kind = data[offset+4:offset+8]
        offset += 12 + length
        if offset > len(data):
            raise ComparisonError('truncated PNG chunk')
        if kind in (b'acTL', b'fcTL', b'fdAT'):
            raise ComparisonError('APNG is unsupported, including a single animated frame')
        if kind == b'IEND':
            if length or offset != len(data):
                raise ComparisonError('invalid PNG end or trailing data')
            return width, height
    raise ComparisonError('PNG has no complete end chunk')


def ppm_header(data):
    offset = 0
    tokens = []
    for _ in range(4):
        while offset < len(data):
            if data[offset] in SPACE:
                offset += 1
            elif data[offset] == 35:
                while offset < len(data) and data[offset] not in b'\r\n':
                    offset += 1
            else:
                break
        start = offset
        while offset < len(data) and data[offset] not in SPACE + b'#':
            offset += 1
        token = data[start:offset]
        if not token or len(token) > 10:
            raise ComparisonError('invalid P6 header token')
        tokens.append(token)
    if tokens[0] != b'P6' or any(not value.isdigit() for value in tokens[1:]) or int(tokens[3]) != 255:
        raise ComparisonError('only binary P6 PPM with maxval=255 is supported')
    width, height = int(tokens[1]), int(tokens[2])
    dimensions(width, height)
    if offset >= len(data) or data[offset] not in SPACE:
        raise ComparisonError('missing P6 raster separator')
    # Consume exactly the separator, not whitespace or '#' in the RGB raster.
    if len(data) - (offset + 1) != width * height * 3:
        raise ComparisonError('P6 requires exactly one complete raster ending at EOF')
    return width, height


def decode(filename, Image):
    data = read_snapshot(filename)
    if data.startswith(PNG_SIGNATURE):
        size = png_header(data)
        source_format = 'PNG'
        with Image.open(io.BytesIO(data)) as check:
            check.verify()
    elif data.startswith(b'P6'):
        size = ppm_header(data)
        source_format = 'PPM'
    else:
        raise ComparisonError(f'{filename}: expected static 8-bit RGB/RGBA PNG or P6 PPM')
    with Image.open(io.BytesIO(data)) as source:
        if source.format != source_format or source.mode not in ('RGB', 'RGBA') or source.size != size:
            raise ComparisonError('decoder format/mode/dimensions disagree with source header')
        source.load()
        alpha = ('stored' if source.mode == 'RGBA' else
                 'PNG transparent colour' if 'transparency' in source.info else 'synthesized opaque')
        image = source.convert('RGBA')
        record = dict(path=str(Path(filename).absolute()), source_mode=source.mode, source_format=source_format,
                      dimensions=list(size), alpha_source=alpha, file_sha256=sha(data),
                      pixel_sha256=sha(image.tobytes()))
    return image, record


def metrics(reference, candidate, ImageChops):
    bands = ImageChops.difference(reference, candidate).split()
    mask = bands[0]
    for band in bands[1:]:
        mask = ImageChops.lighter(mask, band)
    mask = mask.point([0] + [255]*255)
    bounds = mask.getbbox()
    first = None
    if bounds:
        y = bounds[1]
        x = mask.crop((0, y, mask.width, y+1)).tobytes().find(b'\xff')
        first = [x, y]
    changed = mask.histogram()[255]
    total = reference.width * reference.height
    return dict(total_pixels=total, changed_pixels=changed, changed_fraction=changed/total,
                first_changed_xy=first,
                bbox_inclusive=[bounds[0], bounds[1], bounds[2]-1, bounds[3]-1] if bounds else None,
                max_channel_delta=dict(zip('RGBA', [band.getextrema()[1] for band in bands]))), mask


def valid_hash(value):
    return isinstance(value, str) and re.fullmatch(r'[0-9a-fA-F]{64}', value) is not None


def integer(value, minimum=0):
    return type(value) is int and value >= minimum


def capture_options(options, kind, replay):
    if not isinstance(options, dict) or options.get('backend') not in ('vulkan', 'cpu') or type(options.get('weave')) is not bool:
        raise ComparisonError('context requires backend (vulkan/cpu) and boolean weave')
    keys = {'backend', 'weave', 'fb', 'stop_at', 'skip'}
    if set(options) - keys:
        raise ComparisonError('unknown capture option')
    if kind == 'framebuffer' and options['weave']:
        raise ComparisonError('framebuffer context requires weave=false (not applicable)')
    replay_keys = {'fb', 'stop_at', 'skip'}
    if replay or replay_keys & set(options) or kind == 'framebuffer':
        if not replay_keys <= set(options):
            raise ComparisonError('record fb, stop_at and skip choices/defaults together')
        fb = options['fb']
        if fb is not None and (not isinstance(fb, list) or len(fb) != 4
                               or not all(integer(v, 0 if i < 2 else 1) for i, v in enumerate(fb))):
            raise ComparisonError('fb must be null or [fbp,fbw,width,height] with integer values')
        if kind == 'framebuffer' and fb is None:
            raise ComparisonError('framebuffer context requires the actual fb selection')
        if options['stop_at'] is not None and not integer(options['stop_at']):
            raise ComparisonError('stop_at must be null or a nonnegative integer')
        skip = options['skip']
        if not isinstance(skip, list) or any(not isinstance(pair, list) or len(pair) != 2
                or not all(integer(v) for v in pair) or pair[0] > pair[1] for pair in skip):
            raise ComparisonError('skip must be a list of nonnegative inclusive [start,end] ranges')


def validate_context(filename, reference, candidate):
    context = json.loads(read_snapshot(filename))
    if not isinstance(context, dict) or set(context) != {'reference', 'candidate'}:
        raise ComparisonError('context must contain reference and candidate records')
    required = {'file_sha256', 'build_id', 'scene_id', 'frame_index', 'scale', 'ssaa', 'capture_kind', 'capture_options'}
    for side, actual in [('reference', reference), ('candidate', candidate)]:
        record = context[side]
        if not isinstance(record, dict) or not required <= set(record) or set(record) - required - {'replay_input_sha256'}:
            raise ComparisonError(f'context {side}: missing or unknown fields')
        if not valid_hash(record['file_sha256']) or record['file_sha256'].lower() != actual['file_sha256']:
            raise ComparisonError(f'context {side}: stale or invalid file_sha256')
        if any(not isinstance(record[k], str) or not record[k].strip() for k in ('build_id', 'scene_id')):
            raise ComparisonError('build_id and scene_id must be nonempty strings')
        if not integer(record['frame_index']) or not integer(record['scale'], 1) or not integer(record['ssaa'], 1):
            raise ComparisonError('invalid frame_index/scale/ssaa in context')
        if record['capture_kind'] not in ('presented', 'framebuffer'):
            raise ComparisonError('capture_kind must be presented or framebuffer')
        if 'replay_input_sha256' in record and not valid_hash(record['replay_input_sha256']):
            raise ComparisonError('invalid replay_input_sha256')
        capture_options(record['capture_options'], record['capture_kind'], 'replay_input_sha256' in record)
    a, b = context['reference'], context['candidate']
    if any(a[key] != b[key] for key in ('scene_id', 'frame_index', 'scale', 'ssaa', 'capture_kind', 'capture_options')):
        raise ComparisonError('capture context does not describe corresponding outputs')
    replay_a, replay_b = a.get('replay_input_sha256'), b.get('replay_input_sha256')
    if (replay_a is None) != (replay_b is None) or (replay_a and replay_a.lower() != replay_b.lower()):
        raise ComparisonError('both replay_input_sha256 values must be present and equal')
    return context


def main(argv=None):
    parser = Parser(description=__doc__)
    parser.add_argument('reference')
    parser.add_argument('candidate')
    parser.add_argument('--out', required=True, type=Path, help='new output directory; existing paths are refused')
    parser.add_argument('--heatmap', action='store_true', help='write opaque magenta/black difference.png')
    parser.add_argument('--context', type=Path, help='optional declared capture correspondence JSON')
    try:
        args = parser.parse_args(argv)
        try:
            import PIL
        except ImportError as exc:
            raise ComparisonError(f'Pillow {PILLOW_VERSION} is required; no automatic installation') from exc
        if PIL.__version__ != PILLOW_VERSION:
            raise ComparisonError(f'Pillow {PILLOW_VERSION} is required; found {PIL.__version__}')
        from PIL import Image, ImageChops, ImageFile
        ImageFile.LOAD_TRUNCATED_IMAGES = False
        reference, ref_info = decode(args.reference, Image)
        candidate, cand_info = decode(args.candidate, Image)
        if reference.size != candidate.size:
            raise ComparisonError('input dimensions do not match')
        context = validate_context(args.context, ref_info, cand_info) if args.context else None
        result, mask = metrics(reference, candidate, ImageChops)
        code = int(result['changed_pixels'] != 0)
        report = dict(schema_version=1, status='DIFFERENT' if code else 'IDENTICAL', exit_code=code,
                      reference=ref_info, candidate=cand_info, script_sha256=sha(Path(__file__).read_bytes()),
                      python_version=sys.version.split()[0], pillow_version=PIL.__version__,
                      decoding_policy='stored grid; straight RGBA; no colour management; RGB opaque unless PNG tRNS',
                      correspondence=dict(status='declared-context-match' if context else 'unverified',
                                          note='Declared provenance is not proof of equivalent game state.'), **result)
        if context is not None:
            report['context'] = context
        args.out.mkdir(parents=True, exist_ok=False)
        if args.heatmap:
            image = Image.new('RGBA', reference.size, (0, 0, 0, 255))
            image.paste((255, 0, 255, 255), mask=mask)
            with (args.out / 'difference.png').open('xb') as stream:
                image.save(stream, format='PNG')
        temporary = args.out / 'report.json.tmp'
        with temporary.open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write('\n')
        os.replace(temporary, args.out / 'report.json')
        print(f"{report['status']} {reference.width}x{reference.height}: "
              f"changed={result['changed_pixels']}/{result['total_pixels']} "
              f"fraction={result['changed_fraction']} first={result['first_changed_xy']} "
              f"bounds={result['bbox_inclusive']} max_delta={result['max_channel_delta']} "
              f"correspondence={report['correspondence']['status']}")
        return code
    except (OSError, ValueError, SyntaxError, ImportError, KeyError, TypeError, struct.error, MemoryError) as exc:
        print(f'ERROR: {exc}')
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
