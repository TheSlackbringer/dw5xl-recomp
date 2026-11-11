// Issue 049 Part 2 (P2.4): see dw5xl_smooth_list.h.
#include "dw5xl_smooth_list.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace dw5xl::smooth {
namespace {
constexpr uint32_t kRamMask = 0x01FFFFF0u;
// The UNPACK code each builder writes in the last word of its block's second quadword (V4-32, count, address).
constexpr uint32_t kUnpackCode[3] = {0x6C058000u, 0x6C0C8000u, 0x6C150000u};

// Keys: what kind of thing in the top bits, which one below.
constexpr uint64_t kObjectKey = 1ull << 60, kBoneKey = 2ull << 60;

// One game frame is too short for a real motion to carry a thing this far, or to turn the camera this much: more is
// a cut or a respawn, and blending across it would draw a thing flying through the scene. The player runs about 14
// units a frame and the camera follows at about the same speed (measured in the battle of the test state).
constexpr double kJumpUnits = 600.0;
constexpr double kCutDegrees = 30.0;

using D16 = std::array<double, 16>; // as Matrix: element [column * 4 + row]

D16 toDouble(const Matrix &m)
{
    D16 r;
    for (int i = 0; i < 16; ++i)
        r[i] = m[i];
    return r;
}

Matrix toFloat(const D16 &m)
{
    Matrix r;
    for (int i = 0; i < 16; ++i)
        r[i] = static_cast<float>(m[i]);
    return r;
}

// a x b: the transform that applies b first.
D16 mul(const D16 &a, const D16 &b)
{
    D16 r;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
        {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
                sum += a[k * 4 + row] * b[column * 4 + k];
            r[column * 4 + row] = sum;
        }
    return r;
}

// The inverse of a rotation with a position.
D16 rigidInverse(const D16 &m)
{
    D16 r{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            r[column * 4 + row] = m[row * 4 + column];
    for (int row = 0; row < 3; ++row)
        r[12 + row] = -(m[row * 4 + 0] * m[12] + m[row * 4 + 1] * m[13] + m[row * 4 + 2] * m[14]);
    r[15] = 1.0;
    return r;
}

// A transform taken apart: a rotation (quaternion x, y, z, w), a scale per axis, a position, and the fourth number of
// each of its four vectors (kept as they are found).
struct Parts
{
    double q[4]{};
    double scale[3]{};
    double position[3]{};
    double fourth[4]{};
    bool mirrored = false;
};

bool decompose(const D16 &m, Parts &p)
{
    double r[3][3]; // r[axis][component]
    for (int axis = 0; axis < 3; ++axis)
    {
        const double x = m[axis * 4], y = m[axis * 4 + 1], z = m[axis * 4 + 2];
        const double length = std::sqrt(x * x + y * y + z * z);
        if (!(length > 1e-9) || !std::isfinite(length))
            return false;
        p.scale[axis] = length;
        r[axis][0] = x / length;
        r[axis][1] = y / length;
        r[axis][2] = z / length;
    }
    const auto dot = [&](int i, int j) { return r[i][0] * r[j][0] + r[i][1] * r[j][1] + r[i][2] * r[j][2]; };
    if (std::fabs(dot(0, 1)) > 1e-3 || std::fabs(dot(0, 2)) > 1e-3 || std::fabs(dot(1, 2)) > 1e-3)
        return false; // sheared: no rotation with scale
    const double cx = r[1][1] * r[2][2] - r[1][2] * r[2][1], cy = r[1][2] * r[2][0] - r[1][0] * r[2][2];
    const double cz = r[1][0] * r[2][1] - r[1][1] * r[2][0];
    p.mirrored = r[0][0] * cx + r[0][1] * cy + r[0][2] * cz < 0.0;
    if (p.mirrored)
    {
        p.scale[2] = -p.scale[2];
        for (double &v : r[2])
            v = -v;
    }
    for (int i = 0; i < 3; ++i)
        p.position[i] = m[12 + i];
    for (int i = 0; i < 4; ++i)
        p.fourth[i] = m[i * 4 + 3];
    if (!std::isfinite(p.position[0]) || !std::isfinite(p.position[1]) || !std::isfinite(p.position[2]))
        return false;

    // The rotation matrix has r[axis] as its columns: R[row][column] = r[column][row].
    const double m00 = r[0][0], m11 = r[1][1], m22 = r[2][2], trace = m00 + m11 + m22;
    if (trace > 0.0)
    {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        p.q[3] = 0.25 * s;
        p.q[0] = (r[1][2] - r[2][1]) / s;
        p.q[1] = (r[2][0] - r[0][2]) / s;
        p.q[2] = (r[0][1] - r[1][0]) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        p.q[3] = (r[1][2] - r[2][1]) / s;
        p.q[0] = 0.25 * s;
        p.q[1] = (r[1][0] + r[0][1]) / s;
        p.q[2] = (r[2][0] + r[0][2]) / s;
    }
    else if (m11 > m22)
    {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        p.q[3] = (r[2][0] - r[0][2]) / s;
        p.q[0] = (r[1][0] + r[0][1]) / s;
        p.q[1] = 0.25 * s;
        p.q[2] = (r[2][1] + r[1][2]) / s;
    }
    else
    {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        p.q[3] = (r[0][1] - r[1][0]) / s;
        p.q[0] = (r[2][0] + r[0][2]) / s;
        p.q[1] = (r[2][1] + r[1][2]) / s;
        p.q[2] = 0.25 * s;
    }
    return true;
}

D16 compose(const Parts &p)
{
    const double x = p.q[0], y = p.q[1], z = p.q[2], w = p.q[3];
    // R[row][column]
    const double rotation[3][3] = {{1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w)},
                                   {2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w)},
                                   {2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y)}};
    D16 m{};
    for (int axis = 0; axis < 3; ++axis)
    {
        for (int row = 0; row < 3; ++row)
            m[axis * 4 + row] = rotation[row][axis] * p.scale[axis];
        m[axis * 4 + 3] = p.fourth[axis];
    }
    for (int i = 0; i < 3; ++i)
        m[12 + i] = p.position[i];
    m[15] = p.fourth[3];
    return m;
}

// The angle between two rotations, in degrees.
double degreesBetween(const Parts &a, const Parts &b)
{
    const double dot = std::fabs(a.q[0] * b.q[0] + a.q[1] * b.q[1] + a.q[2] * b.q[2] + a.q[3] * b.q[3]);
    return 2.0 * std::acos(std::min(1.0, dot)) * 180.0 / 3.14159265358979323846;
}

D16 blendD(const D16 &a, const D16 &b, double t)
{
    Parts pa, pb;
    if (!decompose(a, pa) || !decompose(b, pb) || pa.mirrored != pb.mirrored)
    {
        D16 r;
        for (int i = 0; i < 16; ++i)
            r[i] = a[i] + (b[i] - a[i]) * t;
        return r;
    }
    Parts p;
    double dot = pa.q[0] * pb.q[0] + pa.q[1] * pb.q[1] + pa.q[2] * pb.q[2] + pa.q[3] * pb.q[3];
    const double sign = dot < 0.0 ? -1.0 : 1.0; // the short way
    dot = std::fabs(dot);
    double wa = 1.0 - t, wb = t;
    if (dot < 0.9995)
    {
        const double angle = std::acos(dot), sine = std::sin(angle);
        wa = std::sin((1.0 - t) * angle) / sine;
        wb = std::sin(t * angle) / sine;
    }
    double length = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        p.q[i] = wa * pa.q[i] + wb * sign * pb.q[i];
        length += p.q[i] * p.q[i];
    }
    length = std::sqrt(length);
    for (double &v : p.q)
        v /= length;
    for (int i = 0; i < 3; ++i)
    {
        p.scale[i] = pa.scale[i] + (pb.scale[i] - pa.scale[i]) * t;
        p.position[i] = pa.position[i] + (pb.position[i] - pa.position[i]) * t;
    }
    for (int i = 0; i < 4; ++i)
        p.fourth[i] = pa.fourth[i] + (pb.fourth[i] - pa.fourth[i]) * t;
    return compose(p);
}

double distance(const D16 &a, const D16 &b)
{
    const double x = a[12] - b[12], y = a[13] - b[13], z = a[14] - b[14];
    return std::sqrt(x * x + y * y + z * z);
}

// A cut: the camera is somewhere else or looks somewhere else than one frame of motion can explain.
bool cut(const Camera &before, const Camera &now)
{
    const D16 wb = rigidInverse(toDouble(before.worldToView)), wn = rigidInverse(toDouble(now.worldToView));
    if (!(distance(wb, wn) <= kJumpUnits))
        return true;
    Parts pb, pn;
    if (!decompose(wb, pb) || !decompose(wn, pn))
        return true;
    return degreesBetween(pb, pn) > kCutDegrees;
}

Matrix matrixAt(const std::vector<uint8_t> &stream, uint32_t offset)
{
    Matrix m;
    std::memcpy(m.data(), stream.data() + offset, 64);
    return m;
}

// A camera matrix is looked for in the stream only if it cannot be mistaken for something else: not empty.
bool distinctive(const Matrix &m)
{
    int numbers = 0;
    for (const float v : m)
        numbers += v != 0.0f ? 1 : 0;
    return numbers >= 6;
}
} // namespace

bool flatten(const uint8_t *ram, uint32_t start, bool tte, List &out)
{
    out = List{};
    out.start = start & kRamMask;
    out.tagHalves = tte;
    uint32_t tadr = out.start, stack[2] = {};
    int sp = 0;
    for (uint32_t steps = 0; steps < 100000u; ++steps)
    {
        if (tadr + 16u > kRamBytes)
            break;
        uint64_t tag;
        std::memcpy(&tag, ram + tadr, 8);
        const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu), id = static_cast<uint32_t>((tag >> 28) & 7u);
        const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFF0u);
        if ((tag >> 63) != 0u)
            break; // scratchpad data: not copied here
        const uint32_t thisTag = tadr;
        if (tte)
            out.stream.insert(out.stream.end(), ram + tadr + 8u, ram + tadr + 16u);
        uint32_t data = tadr + 16u;
        bool end = false;
        switch (id)
        {
        case 0: data = addr; end = true; break;                 // refe
        case 1: tadr = data + qwc * 16u; break;                 // cnt
        case 2: tadr = addr; break;                             // next
        case 3: case 4: data = addr; tadr += 16u; break;        // ref, refs
        case 5:                                                 // call
            if (sp < 2)
                stack[sp++] = data + qwc * 16u;
            tadr = addr;
            break;
        case 6:                                                 // ret
            if (sp == 0)
                end = true;
            else
                tadr = stack[--sp];
            break;
        default: end = true; break;                             // end
        }
        data &= kRamMask;
        if (data + qwc * 16u > kRamBytes)
            break;
        if (qwc != 0u)
        {
            out.spans.push_back(Span{data, static_cast<uint32_t>(out.stream.size()), qwc * 16u, thisTag,
                                     static_cast<uint8_t>(id)});
            out.stream.insert(out.stream.end(), ram + data, ram + data + qwc * 16u);
        }
        if (end)
            return true;
        tadr &= kRamMask;
    }
    out = List{};
    return false;
}

bool offsetOf(const List &list, uint32_t address, uint32_t &offset)
{
    for (const Span &span : list.spans)
    {
        if (address >= span.source && address - span.source < span.bytes)
        {
            offset = span.offset + (address - span.source);
            return true;
        }
    }
    return false;
}

size_t resolve(const std::vector<Tag> &recorded, List &list)
{
    list.tags.clear();
    // The inline blocks by the address of their DMA tag.
    std::vector<std::pair<uint32_t, uint32_t>> inlineBlocks; // tag address, span index
    inlineBlocks.reserve(list.spans.size());
    for (uint32_t i = 0; i < list.spans.size(); ++i)
    {
        if (list.spans[i].id == 1u)
            inlineBlocks.emplace_back(list.spans[i].tag, i);
    }
    std::sort(inlineBlocks.begin(), inlineBlocks.end());

    size_t missing = 0;
    for (const Tag &tag : recorded)
    {
        const auto kind = static_cast<size_t>(tag.kind);
        const auto at = std::lower_bound(inlineBlocks.begin(), inlineBlocks.end(), std::make_pair(tag.block, 0u));
        if (kind >= 3u || at == inlineBlocks.end() || at->first != tag.block)
        {
            ++missing;
            continue;
        }
        const Span &span = list.spans[at->second];
        uint32_t code = 0;
        const bool shaped = span.source == tag.block + 16u && span.bytes == (kBlockQuadwords[kind] - 1u) * 16u;
        if (shaped)
            std::memcpy(&code, list.stream.data() + span.offset + 12u, 4);
        if (!shaped || code != kUnpackCode[kind])
        {
            ++missing; // the game wrote something else there since (an older recording)
            continue;
        }
        Tag resolved = tag;
        resolved.offset = span.offset;
        const auto same = std::find_if(list.tags.begin(), list.tags.end(),
                                       [&](const Tag &other) { return other.block == tag.block; });
        if (same == list.tags.end())
            list.tags.push_back(resolved);
        else if (same->serial < resolved.serial)
            *same = resolved;
    }
    std::sort(list.tags.begin(), list.tags.end(), [](const Tag &a, const Tag &b) { return a.offset < b.offset; });
    return missing;
}

Matrix blend(const Matrix &a, const Matrix &b, float t)
{
    if (!(t > 0.0f))
        return a;
    if (t >= 1.0f)
        return b;
    return toFloat(blendD(toDouble(a), toDouble(b), t));
}

Camera blend(const Camera &before, const Camera &now, float t)
{
    if (!(t > 0.0f))
        return before;
    if (t >= 1.0f)
        return now;
    const D16 wb = rigidInverse(toDouble(before.worldToView)), wn = rigidInverse(toDouble(now.worldToView));
    const D16 wt = blendD(wb, wn, t), vt = rigidInverse(wt);
    const auto rebuilt = [&](const Matrix &xb, const Matrix &xn)
    {
        // The matrix without the view (its projection part) changes only with a zoom: blended number by number.
        const D16 pb = mul(toDouble(xb), wb), pn = mul(toDouble(xn), wn);
        D16 pt;
        for (int i = 0; i < 16; ++i)
            pt[i] = pb[i] + (pn[i] - pb[i]) * t;
        return toFloat(mul(pt, vt));
    };
    Camera out = now;
    out.worldToView = toFloat(vt);
    out.screenProjView = rebuilt(before.screenProjView, now.screenProjView);
    out.clip = rebuilt(before.clip, now.clip);
    out.projView = rebuilt(before.projView, now.projView);
    out.wideClip = rebuilt(before.wideClip, now.wideClip);
    out.viewToWorld = toFloat(wt);
    for (int vector = 0; vector < 4; ++vector) // the game leaves its own numbers in the fourth places
        out.viewToWorld[vector * 4 + 3] = now.viewToWorld[vector * 4 + 3];
    return out;
}

bool jumped(const Matrix &a, const Matrix &b)
{
    return !(distance(toDouble(a), toDouble(b)) <= kJumpUnits);
}

void extract(const List &list, const std::vector<BoneBuffer> &buffers, Values &out)
{
    out.items.clear();
    out.cameras = list.cameras;
    for (const Tag &tag : list.tags)
    {
        if (tag.kind != TagKind::Matrix && tag.kind != TagKind::LitMatrix)
            continue;
        Values::Item item;
        item.key = kObjectKey | tag.key;
        item.offset = tag.offset + 0x10u;
        item.model = tag.model;
        item.matrix = matrixAt(list.stream, item.offset);
        item.extraQuadwords = tag.kind == TagKind::LitMatrix ? 8u : 1u;
        std::memcpy(item.extra.data(), list.stream.data() + item.offset + 64u, item.extraQuadwords * 16u);
        out.items.push_back(item);
    }
    for (const Span &span : list.spans)
    {
        if (span.id != 3u && span.id != 4u)
            continue;
        const auto buffer = std::find_if(buffers.begin(), buffers.end(),
                                         [&](const BoneBuffer &b) { return b.base == span.source; });
        if (buffer == buffers.end())
            continue;
        // The buffer is a ready VIF packet: UNPACK V4-32 runs of whole matrices, each behind its code.
        for (uint32_t at = 0; at + 4u <= span.bytes;)
        {
            uint32_t code;
            std::memcpy(&code, list.stream.data() + span.offset + at, 4);
            const uint32_t command = (code >> 24) & 0x7Fu, count = ((code >> 16) & 0xFFu) ? ((code >> 16) & 0xFFu) : 256u;
            if (command == 0x00u || (command == 0x01u && (code & 0xFFFFu) == 0x0404u)) // NOP, STCYCL 4 of 4
            {
                at += 4u;
                continue;
            }
            if ((command & 0x6Fu) != 0x6Cu || at + 4u + count * 16u > span.bytes)
                break; // anything else: the rest is left alone
            if (count % 4u == 0u)
            {
                for (uint32_t k = 0; k < count / 4u; ++k)
                {
                    Values::Item item;
                    const uint32_t place = at + 4u + k * 64u;
                    item.key = kBoneKey | (uint64_t(buffer->slot) << 40) | (uint64_t(buffer->pair) << 36) | place;
                    item.offset = span.offset + place;
                    item.matrix = matrixAt(list.stream, item.offset);
                    out.items.push_back(item);
                }
            }
            at += 4u + count * 16u;
        }
    }
    std::sort(out.items.begin(), out.items.end(),
              [](const Values::Item &a, const Values::Item &b) { return a.key < b.key; });
}

void build(const List &list, const Values &now, const Values &before, float t, std::vector<uint8_t> &stream,
           BuildStats *stats)
{
    BuildStats counted;
    stream = list.stream;
    const auto finish = [&] { if (stats) *stats = counted; };
    if (t >= 1.0f)
        return finish();

    // The cameras with a partner; a cut in any of them leaves the frame as it is. A frame can hold several cameras of
    // one view (a pass with another depth range): the partner is the one of that view whose projection part (the
    // matrix without the view) is nearest, since that part does not move with the camera.
    std::vector<std::pair<const Camera *, const Camera *>> cameras; // now, before
    const auto projectionPart = [](const Camera &c)
    { return mul(toDouble(c.screenProjView), rigidInverse(toDouble(c.worldToView))); };
    for (const Camera &camera : now.cameras)
    {
        const D16 part = projectionPart(camera);
        const Camera *partner = nullptr;
        double nearest = 0.0;
        for (const Camera &candidate : before.cameras)
        {
            if (candidate.view != camera.view)
                continue;
            const D16 other = projectionPart(candidate);
            double apart = 0.0;
            for (int i = 0; i < 16; ++i)
                apart += std::fabs(part[i] - other[i]) / (1.0 + std::fabs(part[i]) + std::fabs(other[i]));
            if (!partner || apart < nearest)
            {
                partner = &candidate;
                nearest = apart;
            }
        }
        if (!partner)
            continue;
        if (cut(*partner, camera))
        {
            counted.cut = true;
            return finish();
        }
        cameras.emplace_back(&camera, partner);
    }

    for (const Values::Item &item : now.items)
    {
        const auto partner = std::lower_bound(before.items.begin(), before.items.end(), item.key,
                                              [](const Values::Item &a, uint64_t key) { return a.key < key; });
        if (partner == before.items.end() || partner->key != item.key || partner->model != item.model)
        {
            ++counted.unmatched;
            continue;
        }
        if (jumped(partner->matrix, item.matrix))
        {
            ++counted.jumped;
            continue;
        }
        const Matrix value = blend(partner->matrix, item.matrix, t);
        std::memcpy(stream.data() + item.offset, value.data(), 64);
        if (item.extraQuadwords != 0u && item.extraQuadwords == partner->extraQuadwords)
        {
            std::array<float, 32> extra = partner->extra; // t <= 0: the frame before's, bit for bit
            if (t > 0.0f)
            {
                for (uint32_t i = 0; i < item.extraQuadwords * 4u; ++i)
                    extra[i] = partner->extra[i] + (item.extra[i] - partner->extra[i]) * t;
            }
            std::memcpy(stream.data() + item.offset + 64u, extra.data(), item.extraQuadwords * 16u);
        }
        ++((item.key & kBoneKey) ? counted.bones : counted.objects);
    }

    // Every copy of a camera matrix, and of the camera position, in the inline data.
    for (const auto &[camera, partner] : cameras)
    {
        const Camera between = blend(*partner, *camera, t);
        const Matrix *const found[6] = {&camera->screenProjView, &camera->clip, &camera->projView, &camera->wideClip,
                                        &camera->viewToWorld, &camera->worldToView};
        const Matrix *const replaced[6] = {&between.screenProjView, &between.clip, &between.projView, &between.wideClip,
                                           &between.viewToWorld, &between.worldToView};
        bool looked[6];
        for (int i = 0; i < 6; ++i)
            looked[i] = distinctive(*found[i]);
        const float *const position = camera->viewToWorld.data() + 12;
        const bool lookForPosition = position[0] != 0.0f || position[1] != 0.0f || position[2] != 0.0f;
        for (const Span &span : list.spans)
        {
            if (span.id != 1u)
                continue;
            for (uint32_t at = 0; at + 16u <= span.bytes; at += 16u)
            {
                const uint8_t *const bytes = list.stream.data() + span.offset + at;
                bool matched = false;
                for (int i = 0; i < 6 && !matched && at + 64u <= span.bytes; ++i)
                {
                    if (!looked[i] || std::memcmp(bytes, found[i]->data(), 64) != 0)
                        continue;
                    std::memcpy(stream.data() + span.offset + at, replaced[i]->data(), 64);
                    ++counted.cameraCopies;
                    matched = true;
                }
                if (matched)
                {
                    at += 48u;
                    continue;
                }
                if (lookForPosition && std::memcmp(bytes, position, 16) == 0)
                {
                    std::memcpy(stream.data() + span.offset + at, between.viewToWorld.data() + 12, 16);
                    ++counted.cameraCopies;
                }
            }
        }
    }
    finish();
}

void sharedOnly(const List &before, const List &now, std::vector<uint8_t> &nowStream, std::vector<uint8_t> &beforeStream,
                SharedStats *stats, const Values *nowValues, const Values *beforeValues)
{
    SharedStats counted;
    beforeStream = before.stream;
    struct Part
    {
        uint32_t offset = 0, bytes = 0; // in its stream, the tag's upper half included
        uint64_t hash = 0;
        bool shared = false;
    };
    const auto parts = [](const List &list, const std::vector<uint8_t> &stream)
    {
        std::vector<Part> out;
        out.reserve(list.spans.size());
        const uint32_t half = list.tagHalves ? 8u : 0u;
        for (const Span &span : list.spans)
        {
            Part part;
            part.offset = span.offset - half;
            part.bytes = span.bytes + half;
            uint64_t h = 1469598103934665603ull; // FNV-1a over 8-byte words (a part is a whole number of them)
            for (uint32_t i = 0; i + 8u <= part.bytes; i += 8u)
            {
                uint64_t word;
                std::memcpy(&word, stream.data() + part.offset + i, 8);
                h = (h ^ word) * 1099511628211ull;
            }
            part.hash = h;
            out.push_back(part);
        }
        return out;
    };
    std::vector<Part> newer = parts(now, nowStream), older = parts(before, beforeStream);

    const auto same = [&](const Part &a, const Part &b)
    {
        return a.hash == b.hash && a.bytes == b.bytes &&
               std::memcmp(nowStream.data() + a.offset, beforeStream.data() + b.offset, a.bytes) == 0;
    };
    const auto share = [&](Part &a, Part &b)
    {
        a.shared = b.shared = true;
        ++counted.shared;
        counted.sharedBytes += a.bytes;
    };
    for (const Part &part : newer)
        counted.bytesNow += part.bytes;
    // Two consecutive lists are mostly the same: the equal parts at both ends first, then the longest common
    // subsequence of what is between them.
    size_t head = 0, tailNow = newer.size(), tailBefore = older.size();
    while (head < tailNow && head < tailBefore && same(newer[head], older[head]))
    {
        share(newer[head], older[head]);
        ++head;
    }
    while (tailNow > head && tailBefore > head && same(newer[tailNow - 1u], older[tailBefore - 1u]))
    {
        share(newer[tailNow - 1u], older[tailBefore - 1u]);
        --tailNow;
        --tailBefore;
    }
    const size_t n = tailNow - head, m = tailBefore - head;
    if (n != 0u && m != 0u)
    {
        // length[i][j]: the longest common subsequence of newer[head + i ...] and older[head + j ...].
        std::vector<uint32_t> length((n + 1u) * (m + 1u), 0u);
        const auto at = [&](size_t i, size_t j) -> uint32_t & { return length[i * (m + 1u) + j]; };
        for (size_t i = n; i-- > 0u;)
            for (size_t j = m; j-- > 0u;)
                at(i, j) = same(newer[head + i], older[head + j]) ? at(i + 1u, j + 1u) + 1u
                                                                  : std::max(at(i + 1u, j), at(i, j + 1u));
        for (size_t i = 0, j = 0; i < n && j < m;)
        {
            if (same(newer[head + i], older[head + j]) && at(i, j) == at(i + 1u, j + 1u) + 1u)
            {
                share(newer[head + i], older[head + j]);
                ++i;
                ++j;
            }
            else if (at(i + 1u, j) >= at(i, j + 1u))
                ++i;
            else
                ++j;
        }
    }
    // The partner of a keyed value in the frame before (as build() finds it).
    const auto partnerOf = [&](const Values::Item &item) -> const Values::Item *
    {
        if (!beforeValues)
            return nullptr;
        const auto at = std::lower_bound(beforeValues->items.begin(), beforeValues->items.end(), item.key,
                                         [](const Values::Item &a, uint64_t key) { return a.key < key; });
        return at != beforeValues->items.end() && at->key == item.key && at->model == item.model ? &*at : nullptr;
    };
    const uint32_t half = now.tagHalves ? 8u : 0u;
    for (size_t i = 0; i < newer.size(); ++i)
    {
        const Part &part = newer[i];
        if (part.shared)
            continue;
        // What kind of part is left out, and why (before its bytes are cleared).
        const Span &span = now.spans[i];
        const auto tag = std::find_if(now.tags.begin(), now.tags.end(),
                                      [&](const Tag &t) { return t.offset == span.offset; });
        bool bones = false, fresh = false, moved = false;
        const Values::Item *anyPartner = nullptr;
        if (nowValues)
        {
            for (const Values::Item &item : nowValues->items)
            {
                if (item.offset < span.offset || item.offset - span.offset >= span.bytes)
                    continue;
                bones = bones || (item.key & kBoneKey) != 0u;
                const Values::Item *partner = partnerOf(item);
                fresh = fresh || partner == nullptr;
                moved = moved || (partner != nullptr && jumped(partner->matrix, item.matrix));
                anyPartner = anyPartner ? anyPartner : partner;
            }
        }
        const bool elsewhere = std::any_of(older.begin(), older.end(), [&](const Part &other) { return same(part, other); });
        SharedStats::Why *why = tag != now.tags.end() ? (tag->kind == TagKind::Camera ? &counted.cameras : &counted.objects)
                                : bones ? &counted.bones : nullptr;
        if (why && beforeValues)
        {
            if (elsewhere)
                ++why->outOfPlace;
            else if (why != &counted.cameras && fresh)
                ++why->fresh;
            else if (why != &counted.cameras && moved)
                ++why->jumped;
            else
            {
                // The part of the list before that holds the partner (a camera: the camera block in the same place
                // among the camera blocks), and the words in which the two differ.
                const Part *other = nullptr;
                if (anyPartner)
                {
                    for (size_t j = 0; j < older.size() && !other; ++j)
                        if (anyPartner->offset >= before.spans[j].offset &&
                            anyPartner->offset - before.spans[j].offset < before.spans[j].bytes)
                            other = &older[j];
                }
                else if (why == &counted.cameras)
                {
                    size_t ordinal = 0;
                    for (const Tag &t : now.tags)
                        ordinal += t.kind == TagKind::Camera && t.offset < span.offset ? 1u : 0u;
                    for (const Tag &t : before.tags)
                    {
                        if (t.kind != TagKind::Camera)
                            continue;
                        if (ordinal-- == 0u)
                        {
                            for (size_t j = 0; j < older.size() && !other; ++j)
                                if (before.spans[j].offset == t.offset)
                                    other = &older[j];
                            break;
                        }
                    }
                }
                uint32_t differing = other && part.bytes == other->bytes ? 0u : 1u, firstWord = 0u, firstNow = 0u,
                         firstBefore = 0u;
                if (other)
                {
                    const uint32_t words = std::min(part.bytes, other->bytes) / 4u;
                    for (uint32_t w = 0; w < words; ++w)
                    {
                        uint32_t a, b;
                        std::memcpy(&a, nowStream.data() + part.offset + w * 4u, 4);
                        std::memcpy(&b, beforeStream.data() + other->offset + w * 4u, 4);
                        if (a != b && differing++ == 0u)
                        {
                            firstWord = w - half / 4u; // counted from the block's data, behind the tag's upper half
                            firstNow = a;
                            firstBefore = b;
                        }
                    }
                }
                // An object block ends with the quadword that starts its VU1 program (MSCAL in its first word).
                const bool programOnly = why == &counted.objects && other && differing == 1u &&
                                         firstWord == (span.bytes - 16u) / 4u;
                if (programOnly)
                {
                    ++why->program;
                }
                else
                {
                    ++why->different;
                    if (!counted.hasExample && other)
                    {
                        counted.hasExample = true;
                        counted.exampleKind = why == &counted.objects ? 0u : why == &counted.cameras ? 1u : 2u;
                        counted.exampleWords = differing;
                        counted.exampleWord = firstWord;
                        counted.exampleNow = firstNow;
                        counted.exampleBefore = firstBefore;
                    }
                }
            }
        }
        std::memset(nowStream.data() + part.offset, 0, part.bytes);
        if (tag != now.tags.end())
            ++(tag->kind == TagKind::Camera ? counted.cameraBlocks : counted.objectBlocks);
        else if (bones)
            ++counted.bonePackets;
        else if (span.id == 1u)
        {
            ++counted.otherInline;
            counted.otherInlineBytes += span.bytes;
        }
        else
        {
            ++counted.otherReferences;
            counted.otherReferenceBytes += span.bytes;
        }
    }
    for (const Part &part : older)
    {
        counted.bytesBefore += part.bytes;
        if (!part.shared)
            std::memset(beforeStream.data() + part.offset, 0, part.bytes);
    }
    counted.partsNow = static_cast<uint32_t>(newer.size());
    counted.partsBefore = static_cast<uint32_t>(older.size());
    if (stats)
        *stats = counted;
}

} // namespace dw5xl::smooth
