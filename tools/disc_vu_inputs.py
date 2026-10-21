"""Extract verified VU1 instruction ranges from a player's own PAL executable.

This module contains offsets and verification metadata, never instruction bytes.
Reconstructed buffers are translation inputs, not complete runtime VU1 images.
"""
import hashlib
from pathlib import Path

ELF_SHA256 = "f8f9f9987607713c809fb3ae9d04f213ff88326c4aa0c338ab99c4ac33655d1f"
# provenance key, entry PC, final pair PC, diagnostic name, (VU PC, ELF offset, bytes), SHA256
RANGES = [('1ad6eefff0c2e7e5',
  2048,
  2680,
  'lift_1ad6_0800',
  [(2048, 2283256, 640)],
  '2f3879c28eeebe36ce2d32c44c1bf6c37588de02df327e853f364434e815474a'),
 ('1ad6eefff0c2e7e5',
  4096,
  4784,
  'lift_1ad6_1000',
  [(4096, 2273016, 64), (4160, 2285080, 632)],
  '088206beaf43d8fffbe7cb8ef919d097201fb41179cc1721c00f9fe5c16dfdde'),
 ('da28291ffe0b3334',
  0,
  1096,
  'lift_da28_0000',
  [(0, 2239496, 1104)],
  'dc10cf1f223ce1363a23b96e4ef747cace63a0919bdcde6ec52631abf8d8953c'),
 ('da28291ffe0b3334',
  16,
  2384,
  'lift_da28_0010',
  [(16, 2239512, 2032), (2048, 2241552, 344)],
  '59ca3b1daec90b95eb05dc846c60a39d1da63bf88f54059aa4217407669cf800'),
 ('da28291ffe0b3334',
  80,
  4208,
  'lift_da28_0050',
  [(80, 2239576, 1968), (2048, 2241552, 2048), (4096, 2243608, 120)],
  '320391592e31df99432346a7a271ed862e6643f5cc1665ea11c3fcb363e6565b'),
 ('1b91674603e323f2',
  2048,
  3144,
  'lift_1b91_0800',
  [(2048, 2283256, 64), (2112, 2286296, 1040)],
  'da247708eac0b09232299dbe2510d911ccaa0ee2581ab80ce892341adc3271c9'),
 ('3249a2515e770fcc',
  2048,
  3144,
  'lift_battle_actor_skinning_0800',
  [(2048, 2283256, 64), (2112, 2286296, 1040)],
  'da247708eac0b09232299dbe2510d911ccaa0ee2581ab80ce892341adc3271c9'),
 ('baa8e61743cea7a2',
  2048,
  2840,
  'lift_battle_mesh_0800',
  [(2048, 2269080, 800)],
  '15ebd99f6cdbe21a445b4436e10c54f99baefa30239f2caa7cc04c28038d6e11'),
 ('baa8e61743cea7a2',
  9216,
  10208,
  'lift_battle_fx_2400',
  [(9216, 2273016, 1000)],
  '4ef7cb3fd9d72d359645ecb4004064c0f96a1b3edd63321e26bf50e05cdb9f5c'),
 ('64127817c106b85a',
  9216,
  10064,
  'lift_battle_particles_2400',
  [(9216, 2269080, 64), (9280, 2289832, 792)],
  'f36254a00630036972a9101702fcb6880752dffc01337aeed1dc01f46f75b1b2'),
 ('46d6ccb25f6b8d32',
  9216,
  10680,
  'lift_battle_terrain_2400',
  [(9216, 2251624, 1472)],
  'd10417eb414e98544cfd87fef4c16dacb2fd59879f40ca84e4c67d236bb4312f'),
 ('46d6ccb25f6b8d32',
  2048,
  3608,
  'lift_battle_water_0800',
  [(2048, 2245592, 1568)],
  'c69ed25baeae156fc0ea6b95a860db40a3b4a84a32ef52626742c3b939741f65'),
 ('60b13c6c3365e06a',
  9216,
  10360,
  'lift_battle_shadows_2400',
  [(9216, 2263048, 1152)],
  '2f08085bc29ef23277eb06e2c111c62e06e85768461d3797ab8a7d7d38743d72'),
 ('baa8e61743cea7a2',
  64,
  240,
  'lift_battle_init_0040',
  [(64, 2243912, 184)],
  '7ddd45d68a0746afc5307f934e2a0ad5bd9d94bd2af6c6ff0cea69306b61146e'),
 ('baa8e61743cea7a2',
  240,
  560,
  'lift_battle_stream_00f0',
  [(240, 2244088, 328)],
  '89d643fc24b56c1595ab8257dd44c1337fda1e4e9212d645f83e9501e797e56a'),
 ('84a67c1d4789e87b',
  560,
  880,
  'lift_battle_clip_0230',
  [(560, 2244088, 72), (632, 2244480, 256)],
  '3cdf07158dc5866bc30343d9fc1863c980968e9cfe7ff29c7d1bae585cc819f9'),
 ('60b13c6c3365e06a',
  2048,
  2952,
  'lift_battle_shadows_0800',
  [(2048, 2258152, 912)],
  'fcc64b6363242901c3e33cca9c0ac97f367f83c500fbd7cf27fd13104c189d7e'),
 ('84a67c1d4789e87b',
  0,
  40,
  'lift_battle_stream_0000',
  [(0, 2243848, 48)],
  '1864d0464d1b2b2fbd1d5c9110961e3c66dd6e7d68ee7ac0493884781aed48ce'),
 ('3a4f0cd1c30a3aef',
  880,
  1200,
  'lift_battle_clip_0370',
  [(880, 2244088, 104), (984, 2244832, 224)],
  '6caeb04641a0ccd09a519734660525cb4873b77307974b5fe86878f6e73dc27d'),
 ('3a4f0cd1c30a3aef',
  1200,
  1520,
  'lift_battle_clip_04b0',
  [(1200, 2244088, 104), (1304, 2244832, 104), (1408, 2245256, 120)],
  '864a25882178cf8072163a3b3313a7f2a66040dcdcb9e9c3b385e993edc46a1e'),
 ('da28291ffe0b3334',
  96,
  240,
  'lift_battle_init_0060',
  [(96, 2239592, 152)],
  'bb1bb97def016f2bc819a5b6aef403f7d7c6a993232709908b95a324789ce222'),
 ('87e0189b1bc04e8e',
  40,
  240,
  'lift_battle_init_0028',
  [(40, 2243888, 208)],
  '27875f73b35b92b53142afe04fdbb6bf80e39e98397a02977eaa1377e84567e6'),
 ('baa8e61743cea7a2',
  544,
  552,
  'flush_0220',
  [(544, 2155848, 16)],
  '5eb74410b7d656eb5a22ee9e60eef38c6896a22506b84d1277503a6641acc0df'),
 ('baa8e61743cea7a2',
  864,
  872,
  'flush_0360',
  [(864, 2155848, 16)],
  '5eb74410b7d656eb5a22ee9e60eef38c6896a22506b84d1277503a6641acc0df')]

# Other disc files a chunk may name as its source (4th chunk element), next to the executable.
SOURCE_SHA256 = {'OVL_00.BIN': '1cb303e389c215412c3a104e26b8e43d9105f230b3b8a76564ed1a0eb92da50f'}

def load_disc_bins(path):
    return _load_bins(path, RANGES, 16384)

def load_vu0_bins(path):
    """VU0 microprogram images (4 KB micro memory, issue 025) rebuilt from VU0_RANGES. The VU0 programs share
    code, so each range's SHA256 is checked over that range's own chunks (as tools/vu1_find_disc_range.py
    computes it), not over the image the earlier ranges already filled."""
    return _load_bins(path, VU0_RANGES, 4096, own_digest=True)

def _load_bins(path, ranges, image_size, own_digest=False):
    elf = Path(path).read_bytes()
    actual = hashlib.sha256(elf).hexdigest()
    if actual != ELF_SHA256:
        raise ValueError(f"Unsupported PAL executable SHA256: {actual}")
    sources = {None: elf}
    def source(name):
        if name not in sources:
            blob = (Path(path).parent / name).read_bytes()
            if hashlib.sha256(blob).hexdigest() != SOURCE_SHA256.get(name):
                raise ValueError(f"Unsupported disc file SHA256: {name}")
            sources[name] = blob
        return sources[name]
    bins = {}
    covered = {}
    for key, start, end, name, chunks, digest in ranges:
        image = bins.setdefault(key, bytearray(image_size))
        occupied = covered.setdefault(key, bytearray(image_size))
        own = bytearray(image_size)
        for chunk in chunks:
            pc, offset, size = chunk[:3]
            data = source(chunk[3] if len(chunk) > 3 else None)[offset:offset + size]
            if len(data) != size or pc + size > len(image):
                raise ValueError(f"Out-of-bounds VU input: {name}")
            own[pc:pc + size] = data
            for i, byte in enumerate(data):
                if occupied[pc+i] and image[pc+i] != byte:
                    raise ValueError(f"Overlapping VU inputs disagree: {name}")
                image[pc+i] = byte
                occupied[pc+i] = 1
        checked = own if own_digest else image
        if hashlib.sha256(checked[start:end+8]).hexdigest() != digest:
            raise ValueError(f"VU instruction range mismatch: {name}")
    return {key: bytes(image) for key, image in bins.items()}, covered

# Observed runtime image identities: validation/dispatch metadata, never instruction payload.
IMAGE_REGISTRATIONS = [(1933995873025583077, 2048, 'lift_1ad6_0800'),
 (1933995873025583077, 4096, 'lift_1ad6_1000'),
 (1986482461057688562, 2048, 'lift_1b91_0800'),
 (15719859716719588148, 0, 'lift_da28_0000'),
 (15719859716719588148, 16, 'lift_da28_0010'),
 (15719859716719588148, 80, 'lift_da28_0050'),
 (10200758193508629149, 0, 'lift_da28_0000'),
 (10200758193508629149, 16, 'lift_da28_0010'),
 (10200758193508629149, 80, 'lift_da28_0050'),
 (2401148097440195882, 0, 'lift_da28_0000'),
 (2401148097440195882, 16, 'lift_da28_0010'),
 (2401148097440195882, 80, 'lift_da28_0050'),
 (7159781944600618902, 0, 'lift_da28_0000'),
 (7159781944600618902, 16, 'lift_da28_0010'),
 (7159781944600618902, 80, 'lift_da28_0050'),
 (18117636521138529375, 0, 'lift_da28_0000'),
 (18117636521138529375, 16, 'lift_da28_0010'),
 (18117636521138529375, 80, 'lift_da28_0050'),
 (37676886771247015, 240, 'lift_battle_stream_00f0'),
 (37676886771247015, 2048, 'lift_battle_shadows_0800'),
 (37676886771247015, 9216, 'lift_battle_shadows_2400'),
 (507773364147557335, 0, 'lift_battle_stream_0000'),
 (1933995873025583077, 0, 'lift_battle_stream_0000'),
 (1933995873025583077, 64, 'lift_battle_init_0040'),
 (1933995873025583077, 240, 'lift_battle_stream_00f0'),
 (1933995873025583077, 560, 'lift_battle_clip_0230'),
 (1986482461057688562, 64, 'lift_battle_init_0040'),
 (1986482461057688562, 560, 'lift_battle_clip_0230'),
 (1986482461057688562, 2048, 'lift_battle_actor_skinning_0800'),
 (2401148097440195882, 96, 'lift_battle_init_0060'),
 (3507279194603627583, 880, 'lift_battle_clip_0370'),
 (3623605845557186508, 64, 'lift_battle_init_0040'),
 (3623605845557186508, 560, 'lift_battle_clip_0230'),
 (3623605845557186508, 2048, 'lift_battle_actor_skinning_0800'),
 (4201591072419887855, 64, 'lift_battle_init_0040'),
 (4201591072419887855, 880, 'lift_battle_clip_0370'),
 (4201591072419887855, 1200, 'lift_battle_clip_04b0'),
 (4437332246463860240, 0, 'lift_battle_stream_0000'),
 (4437332246463860240, 64, 'lift_battle_init_0040'),
 (4437332246463860240, 240, 'lift_battle_stream_00f0'),
 (4437332246463860240, 2048, 'lift_battle_water_0800'),
 (4437332246463860240, 9216, 'lift_battle_terrain_2400'),
 (5104492294148164914, 240, 'lift_battle_stream_00f0'),
 (5104492294148164914, 2048, 'lift_battle_water_0800'),
 (5104492294148164914, 9216, 'lift_battle_terrain_2400'),
 (5329447496468832601, 0, 'lift_battle_stream_0000'),
 (5329447496468832601, 40, 'lift_battle_init_0028'),
 (5329447496468832601, 64, 'lift_battle_init_0040'),
 (5329447496468832601, 560, 'lift_battle_clip_0230'),
 (5329447496468832601, 9216, 'lift_battle_particles_2400'),
 (6214707764247923172, 0, 'lift_battle_stream_0000'),
 (6214707764247923172, 64, 'lift_battle_init_0040'),
 (6214707764247923172, 240, 'lift_battle_stream_00f0'),
 (6214707764247923172, 560, 'lift_battle_clip_0230'),
 (6214707764247923172, 2048, 'lift_battle_mesh_0800'),
 (6214707764247923172, 9216, 'lift_battle_fx_2400'),
 (6803241987644672792, 64, 'lift_battle_init_0040'),
 (6803241987644672792, 560, 'lift_battle_clip_0230'),
 (6803241987644672792, 2048, 'lift_battle_actor_skinning_0800'),
 (6967416533935317098, 240, 'lift_battle_stream_00f0'),
 (6967416533935317098, 2048, 'lift_battle_shadows_0800'),
 (6967416533935317098, 9216, 'lift_battle_shadows_2400'),
 (7159781944600618902, 96, 'lift_battle_init_0060'),
 (7184570901524038838, 240, 'lift_battle_stream_00f0'),
 (7184570901524038838, 2048, 'lift_battle_water_0800'),
 (7184570901524038838, 9216, 'lift_battle_terrain_2400'),
 (7210957996791609434, 0, 'lift_battle_stream_0000'),
 (7210957996791609434, 40, 'lift_battle_init_0028'),
 (7210957996791609434, 64, 'lift_battle_init_0040'),
 (7210957996791609434, 560, 'lift_battle_clip_0230'),
 (7210957996791609434, 9216, 'lift_battle_particles_2400'),
 (7932518408468101859, 0, 'lift_battle_stream_0000'),
 (7932518408468101859, 64, 'lift_battle_init_0040'),
 (7932518408468101859, 240, 'lift_battle_stream_00f0'),
 (7932518408468101859, 560, 'lift_battle_clip_0230'),
 (7932518408468101859, 2048, 'lift_battle_mesh_0800'),
 (7932518408468101859, 9216, 'lift_battle_fx_2400'),
 (8021513296740625458, 0, 'lift_battle_stream_0000'),
 (8514532620255434139, 240, 'lift_battle_stream_00f0'),
 (8514532620255434139, 2048, 'lift_battle_shadows_0800'),
 (8514532620255434139, 9216, 'lift_battle_shadows_2400'),
 (9544803665684009766, 0, 'lift_battle_stream_0000'),
 (9544803665684009766, 40, 'lift_battle_init_0028'),
 (9544803665684009766, 64, 'lift_battle_init_0040'),
 (9544803665684009766, 560, 'lift_battle_clip_0230'),
 (9544803665684009766, 9216, 'lift_battle_particles_2400'),
 (9558463724336572539, 0, 'lift_battle_stream_0000'),
 (9558463724336572539, 64, 'lift_battle_init_0040'),
 (9558463724336572539, 240, 'lift_battle_stream_00f0'),
 (9558463724336572539, 560, 'lift_battle_clip_0230'),
 (9558463724336572539, 2048, 'lift_battle_mesh_0800'),
 (9558463724336572539, 9216, 'lift_battle_fx_2400'),
 (9790852644368043662, 0, 'lift_battle_stream_0000'),
 (9790852644368043662, 40, 'lift_battle_init_0028'),
 (10200758193508629149, 96, 'lift_battle_init_0060'),
 (11029770662716130715, 0, 'lift_battle_stream_0000'),
 (11029770662716130715, 64, 'lift_battle_init_0040'),
 (11029770662716130715, 880, 'lift_battle_clip_0370'),
 (12522403273703794968, 64, 'lift_battle_init_0040'),
 (12522403273703794968, 560, 'lift_battle_clip_0230'),
 (12522403273703794968, 2048, 'lift_battle_actor_skinning_0800'),
 (12711047209976807277, 0, 'lift_battle_stream_0000'),
 (12711047209976807277, 64, 'lift_battle_init_0040'),
 (12711047209976807277, 240, 'lift_battle_stream_00f0'),
 (12915579896576408263, 880, 'lift_battle_clip_0370'),
 (13053105572338147423, 64, 'lift_battle_init_0040'),
 (13053105572338147423, 240, 'lift_battle_stream_00f0'),
 (13053105572338147423, 560, 'lift_battle_clip_0230'),
 (13053105572338147423, 2048, 'lift_battle_mesh_0800'),
 (13053105572338147423, 9216, 'lift_battle_fx_2400'),
 (13450253274738239394, 64, 'lift_battle_init_0040'),
 (13450253274738239394, 240, 'lift_battle_stream_00f0'),
 (13450253274738239394, 560, 'lift_battle_clip_0230'),
 (13450253274738239394, 2048, 'lift_battle_mesh_0800'),
 (13450253274738239394, 9216, 'lift_battle_fx_2400'),
 (13877360326691393880, 0, 'lift_battle_stream_0000'),
 (14289388070734350280, 0, 'lift_battle_stream_0000'),
 (14500297400081962351, 64, 'lift_battle_init_0040'),
 (14500297400081962351, 560, 'lift_battle_clip_0230'),
 (14500297400081962351, 2048, 'lift_battle_actor_skinning_0800'),
 (15183910716862634143, 0, 'lift_battle_stream_0000'),
 (15183910716862634143, 64, 'lift_battle_init_0040'),
 (15183910716862634143, 240, 'lift_battle_stream_00f0'),
 (15183910716862634143, 2048, 'lift_battle_shadows_0800'),
 (15183910716862634143, 9216, 'lift_battle_shadows_2400'),
 (15719859716719588148, 96, 'lift_battle_init_0060'),
 (15747464986114785014, 0, 'lift_battle_stream_0000'),
 (15747464986114785014, 40, 'lift_battle_init_0028'),
 (15747464986114785014, 64, 'lift_battle_init_0040'),
 (15747464986114785014, 560, 'lift_battle_clip_0230'),
 (15747464986114785014, 9216, 'lift_battle_particles_2400'),
 (16706044886933806842, 240, 'lift_battle_stream_00f0'),
 (16706044886933806842, 2048, 'lift_battle_water_0800'),
 (16706044886933806842, 9216, 'lift_battle_terrain_2400'),
 (16941762177108264386, 880, 'lift_battle_clip_0370'),
 (17278749103648695599, 0, 'lift_battle_stream_0000'),
 (18117636521138529375, 96, 'lift_battle_init_0060')]

# Proposed native subroutine targets: offsets/hashes only.
RANGES.extend([('99119dfac138459b', 2048, 5200, 'lift_battle_calls_99119dfa_0800', [(2048, 2259864, 2048), (4096, 2261920, 1112)], '008dfb87a69648975f6fa618bfa17afba7d83f0ccdcbb267566bf8ef2915f0b9'), ('99119dfac138459b', 9216, 12976, 'lift_battle_calls_99119dfa_2400', [(9216, 2265288, 2048), (11264, 2267344, 1720)], 'a98f85559db802832f30e3243fadae7b5154c9ccabcade24353f3e65e4ee2ac7'), ('eb1d3af053afc5c2', 2048, 5128, 'lift_battle_calls_eb1d3af0_0800', [(2048, 2248520, 2048), (4096, 2250576, 1040)], 'b5c60b77d67ea778deb5281fe46902630687bc041220bbe6b2bc435e900009dd'), ('eb1d3af053afc5c2', 9216, 12904, 'lift_battle_calls_eb1d3af0_2400', [(9216, 2254440, 2048), (11264, 2256496, 1648)], 'b1ada5f32533e01acdc262d64f1b8476d8013786a8a8ab10db05264013fbfbcb')])
IMAGE_REGISTRATIONS.extend([(11029770662716130715, 2048, 'lift_battle_calls_99119dfa_0800'), (11029770662716130715, 9216, 'lift_battle_calls_99119dfa_2400'), (16941762177108264386, 2048, 'lift_battle_calls_eb1d3af0_0800'), (16941762177108264386, 9216, 'lift_battle_calls_eb1d3af0_2400')])


# Opening (OVL00) VU1 programs: offsets in OVL_00.BIN and hashes only.
RANGES.extend([('dd3d1361f34c85a3', 672, 3120, 'lift_opening_dd3d_02a0', [(24, 409848, 2024, 'OVL_00.BIN'), (2048, 411880, 1080, 'OVL_00.BIN')], '851fc55e5a3239c8760e5780662fa06b1afb92d37bff2e53326a5bee82e24621'), ('4a6dbc6bbd3f1fbb', 784, 1352, 'lift_opening_4a6d_0310', [(256, 408688, 1104, 'OVL_00.BIN')], '8f20f6c648e0a0cc41317a896964e42f5cb04e4ebbea05bcace27af86315ff25'), ('4a6dbc6bbd3f1fbb', 256, 1352, 'lift_opening_4a6d_0100', [(256, 408688, 1104, 'OVL_00.BIN')], '1e763081903f202f62507d68d52d38905a53a4c3531147ae34417da246b9e012'), ('4a6dbc6bbd3f1fbb', 0, 248, 'lift_opening_4a6d_0000', [(0, 408432, 256, 'OVL_00.BIN')], '1fa7b2429eaecbc1e5a11487748ad7f1b6c3446715b39ef1440b263139b24dbc'), ('4dedc9b811a016b1', 680, 2480, 'lift_opening_4ded_02a8', [(184, 417704, 1864, 'OVL_00.BIN'), (2048, 419576, 440, 'OVL_00.BIN')], '25089d5f1079ca29292daaa4ac41732cf8e3fa68262e8631c0633df797a07fee'), ('881c2abb30cea5ce', 2352, 5944, 'lift_opening_881c_0930', [(112, 438864, 1936, 'OVL_00.BIN'), (2048, 440808, 2048, 'OVL_00.BIN'), (4096, 442864, 1856, 'OVL_00.BIN')], '7e26fcf776dae17eac7578a30be176e14a4e2fe4a772320c5ae83eef450a611c'), ('881c2abb30cea5ce', 0, 104, 'lift_opening_881c_0000', [(0, 438752, 112, 'OVL_00.BIN')], 'f25eaa23f29cffc494aefa5c9fbb8ba8aa07155dce19441b5bcdeef20b4424a4')])

# Issue 001: battle programs of image 3a4f0cd1c30a3aef that were still interpreted (~27–30 ns/VU cycle);
# located with tools/vu1_find_disc_range.py.
RANGES.extend([
 ('3a4f0cd1c30a3aef',
  2048,
  5136,
  'lift_battle_3a4f_0800',
  [(2048, 2269896, 2048), (4096, 2271952, 456), (4568, 2272424, 576)],
  '89287eb97c5bfa17ee846b280bcc5a8669d6537ca851d57d69913d55d9b3d93c'),
 ('3a4f0cd1c30a3aef',
  9216,
  12416,
  'lift_battle_3a4f_2400',
  [(9216, 2274040, 2048), (11264, 2276096, 568), (11848, 2276680, 576)],
  '57440b64b70260c64c9798ed25c1cbb6c7a143dfe3627e39acd21faa2ee2f14d'),
])
IMAGE_REGISTRATIONS.extend([(4201591072419887855, 2048, 'lift_battle_3a4f_0800'),
                            (4201591072419887855, 9216, 'lift_battle_3a4f_2400')])

# VU0 microprograms started by VCALLMS (issue 025): one 4 KB image (census hash fd5ecac9d295d513, Si Shui Gate),
# entries found with tools/vu1_find_disc_range.py. Same format as RANGES; offsets, sizes and SHA256 only.
VU0_RANGES = [
    ('fd5ecac9d295d513', 0x0050, 0x0b90, 'vu0_fd5e_0050',
     [(80, 2236040, 16), (232, 2236192, 568), (2504, 2238472, 376), (2912, 2238880, 56)],
     'b3013fe234c46de37d0aebe7320c96cf2dd8f5b0dec95b18634f92335b3e0ed9'),
    ('fd5ecac9d295d513', 0x0090, 0x0318, 'vu0_fd5e_0090',
     [(144, 2236104, 16), (192, 2236152, 40), (368, 2236328, 432)],
     'c791747009b7f705d2cb42ec6a1a5b882da3c6cb7f144d652c518083da0e7883'),
    ('fd5ecac9d295d513', 0x0020, 0x0318, 'vu0_fd5e_0020',
     [(32, 2235992, 16), (160, 2236120, 32), (232, 2236192, 568)],
     'b8a490854fc83519f3c117157d53681438ff3307ee31dbc2c0fac44fb6f07059'),
    ('fd5ecac9d295d513', 0x0030, 0x09c0, 'vu0_fd5e_0030',
     [(48, 2236008, 16), (1824, 2237784, 224), (2048, 2238016, 456)],
     '1edd2693880f3f65c74d7cbf0093acaa2cf936ff17d7c76f40beef9841b91f93'),
    # The Opening's VU0 image (census hash 76d23035e908a12e, in OVL_00.BIN); 0x0000 builds the bone rotation
    # matrices (sin/cos subroutine), 93.5 % of the Opening's VU0 cycles.
    ('76d23035e908a12e', 0x0000, 0x0370, 'vu0_76d2_0000',
     [(0, 406424, 360, 'OVL_00.BIN'), (392, 406816, 496, 'OVL_00.BIN')],
     '9a8a0e523d870a09743066e86b60c43d9f6a452b4d65546f0f8bae3d0929987b'),
    ('76d23035e908a12e', 0x0168, 0x0370, 'vu0_76d2_0168',
     [(360, 406784, 528, 'OVL_00.BIN')],
     '428d6370f12e1c2650e09ddac01d10d5e77356b5512329fe29e11cc65261bf85'),
    ('76d23035e908a12e', 0x0378, 0x0448, 'vu0_76d2_0378',
     [(888, 407312, 216, 'OVL_00.BIN')],
     '951cdb8984f0771d954b45a50b2084ee6582b07ac2f8550e3440b25a15691f7e'),
    ('76d23035e908a12e', 0x0450, 0x07b0, 'vu0_76d2_0450',
     [(1104, 407528, 872, 'OVL_00.BIN')],
     '49cb72b51cc7961bb740f53762d66b8b60810a0e66f72fe199e9578a33e1f586'),
]

# Issue 025 (spec v3): VU1 programs of the Si Shui Gate fight that ran interpreted (census 2026-10-21). One key per
# program (image hash + entry) so each range is checked on its own; offsets and hashes only.
RANGES.extend([('d29a38db75ab7cca_0000', 0, 688, 'lift_sishui_d29a_0000', [(0, 2158264, 696)], 'ad9ba2b0b885d3085560ff64cd7d652ad18f8ee9f4cf17b7704585090c1f75ec'), ('1ba40975a14161fc_0000', 0, 864, 'lift_sishui_1ba4_0000', [(0, 2158984, 872)], 'dda311869bcc9fe9f17def8f1ec370d7fa6e70f0e7696f78200ba7905e68469c'), ('49f60042ec1ea159_0800', 2048, 2736, 'lift_sishui_49f6_0800', [(2048, 2289048, 696)], 'c79fe520b2fac464cfe849193e97feb7ae0b83b47d4df03950b5d78d9c564ce9'), ('49f60042ec1ea159_05f0', 1520, 2736, 'lift_sishui_49f6_05f0', [(1520, 2245368, 16), (1544, 2245392, 40), (2048, 2289048, 696)], 'c5699ccfe6f1cbb48ac5cd2c57f347714b5244ad9dfd5b8b300195f319a7fcea'), ('563f13c8dedc09e4_05f0', 1520, 2832, 'lift_sishui_563f_05f0', [(1520, 2245368, 16), (1544, 2245392, 40), (2048, 2269080, 792)], '026302f28965d941f5eae6d63aad1330a906121a73da17e8cbf6d6a276c23c4c'), ('49f60042ec1ea159_0600', 1536, 10056, 'lift_sishui_49f6_0600', [(1536, 2245384, 48), (9216, 2289768, 848)], 'df4879779345772db6940dd27b04acd0dccafb86dfe877c2ae422d6f04c2941a'), ('6621289d99458305_0000', 0, 624, 'lift_sishui_6621_0000', [(0, 2290696, 584), (600, 2291296, 32)], '598a7c6fe3bcbf3c411d520f12e21d07e8947165dbe2206fdb69fcfa6723add2'), ('fae9165130cc72ff_0000', 0, 712, 'lift_sishui_fae9_0000', [(0, 2155144, 720)], '80e7c3ebf6a371fdbdb3842d132a95abde66a4e0970ba690f26037e9b5d4a0e6'), ('8d905f87f3bcba9d_0040', 64, 4208, 'lift_sishui_8d90_0040', [(64, 2239560, 16), (3040, 2242544, 1056), (4096, 2243608, 120)], '2f05fc9977a4a90cf990ffeaecc1bbd0125a3d263eec53d1ebcd41ee82e61035'), ('5e225d7fd5bf037b_0000', 0, 1264, 'lift_sishui_5e22_0000', [(0, 2156968, 1272)], '99e480d5d3c54777be23c52050d622c6e7372f0a85e9511c8faa37d350a42467'), ('336695fe491687e6_0800', 2048, 5136, 'lift_sishui_3366_0800', [(2048, 2269896, 2048), (4096, 2271952, 456), (4568, 2272424, 576)], '89287eb97c5bfa17ee846b280bcc5a8669d6537ca851d57d69913d55d9b3d93c'), ('3cb47dd9f41da6ae_0020', 32, 3032, 'lift_sishui_3cb4_0020', [(32, 2239528, 16), (2384, 2241888, 656)], '428cdeb5826e4a84c9dbb066cc6aa0a05f828c7862748677fd65e7aeab65db95')])
IMAGE_REGISTRATIONS.extend([(15175504409601473738, 0, 'lift_sishui_d29a_0000'), (1991727336025842172, 0, 'lift_sishui_1ba4_0000'), (5329447496468832601, 2048, 'lift_sishui_49f6_0800'), (5329447496468832601, 1520, 'lift_sishui_49f6_05f0'), (6214707764247923172, 1520, 'lift_sishui_563f_05f0'), (5329447496468832601, 1536, 'lift_sishui_49f6_0600'), (7359207923446547205, 0, 'lift_sishui_6621_0000'), (18080006717022434047, 0, 'lift_sishui_fae9_0000'), (10200758193508629149, 64, 'lift_sishui_8d90_0040'), (6783086792401683323, 0, 'lift_sishui_5e22_0000'), (3703812662939256806, 2048, 'lift_sishui_3366_0800'), (4374259513140553390, 32, 'lift_sishui_3cb4_0020')])
# Issue 025: programs the game continues (MSCNT) after a stop, at their continuation pc.
RANGES.extend([('1ba40975a14161fc_0368', 872, 880, 'lift_sishui_1ba4_0368', [(0, 2158984, 888)], '3b652ffdb2e8cb3328443a9e764c1b4c846a31951b257ff690cab7f615e57a83'), ('eb1d3af053afc5c2_04a0', 1184, 1192, 'lift_sishui_eb1d_04a0', [(1184, 2155848, 16)], '5eb74410b7d656eb5a22ee9e60eef38c6896a22506b84d1277503a6641acc0df')])
IMAGE_REGISTRATIONS.extend([(1991727336025842172, 872, 'lift_sishui_1ba4_0368'), (16941762177108264386, 1184, 'lift_sishui_eb1d_04a0')])
RANGES.extend([('3a4f0cd1c30a3aef_05f0', 1520, 5136, 'lift_sishui_3a4f_05f0', [(1520, 2245368, 16), (1544, 2245392, 40), (2048, 2269896, 2048), (4096, 2271952, 456), (4568, 2272424, 576)], 'bb955c614f07dc75a13926e9fa7c8be5364243ed97a9f085f55ab18ce0cebf82')])
IMAGE_REGISTRATIONS.extend([(4201591072419887855, 1520, 'lift_sishui_3a4f_05f0')])
# Issue 025: interpreted entries from the Opening and heavy-fight censuses (2026-10-21).
RANGES.extend([('dd3d1361f34c85a3_0000', 0, 8, 'lift_opening_dd3d_0000', [(0, 406768, 16, 'OVL_00.BIN')], '5eb74410b7d656eb5a22ee9e60eef38c6896a22506b84d1277503a6641acc0df'), ('881c2abb30cea5ce_0070', 112, 5944, 'lift_opening_881c_0070', [(112, 438864, 1936, 'OVL_00.BIN'), (2048, 440808, 304, 'OVL_00.BIN'), (2368, 441128, 1728, 'OVL_00.BIN'), (4096, 442864, 1856, 'OVL_00.BIN')], '4d6c00440928cd410fce0ee185285937694c2beb81caf87efc89d9f7e333b100'), ('4dedc9b811a016b1_0000', 0, 176, 'lift_opening_4ded_0000', [(0, 417520, 184, 'OVL_00.BIN')], '6b58bcdba4b00a4f0505ac4432c08a285defa93bd21c0290aa9cf9ddb5a51e75'), ('dd3d1361f34c85a3_0010', 16, 3120, 'lift_opening_dd3d_0010', [(16, 409840, 656, 'OVL_00.BIN'), (688, 410512, 1360, 'OVL_00.BIN'), (2048, 411880, 1080, 'OVL_00.BIN')], 'e092ee754418fca7c06c404ab71ada70a99313d1ea4f779f0a04a97b9929a6da'), ('4dedc9b811a016b1_00b8', 184, 2480, 'lift_opening_4ded_00b8', [(184, 417704, 496, 'OVL_00.BIN'), (696, 418216, 1352, 'OVL_00.BIN'), (2048, 419576, 440, 'OVL_00.BIN')], 'b4eb54528f77a27671d9bbe01e1af6286044b8fa9956ab37fd01b400c109ad94'), ('baa8e61743cea7a2_0600', 1536, 10200, 'lift_battle_baa8_0600', [(1536, 2245384, 48), (9216, 2273016, 992)], '59a42e029f333b93580aa184c2805f0f8557b8f44a6af8e545b7bcfd5f16876d'), ('64c44bbb44892dc9_0800', 2048, 3016, 'lift_battle_64c4_0800', [(2048, 2279112, 976)], '5844121311d241bfa214d908d4a07fc415375be630e74adfb63473d47f2fe090')])
IMAGE_REGISTRATIONS.extend([(15941919567376057763, 0, 'lift_opening_dd3d_0000'), (9807761071972197838, 112, 'lift_opening_881c_0070'), (5615366102807746225, 0, 'lift_opening_4ded_0000'), (15941919567376057763, 16, 'lift_opening_dd3d_0010'), (5615366102807746225, 184, 'lift_opening_4ded_00b8'), (13450253274738239394, 1536, 'lift_battle_baa8_0600'), (7261011766908890569, 2048, 'lift_battle_64c4_0800')])
