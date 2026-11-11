// Issue 049 Part 2 (P2.4): in-between values and the builder (app/src/dw5xl_smooth_list.h). No game data.
//  - blend(Matrix): the end points bit for bit; a rotation turns the short way and keeps its length; position and
//    scale move straight; mirrored transforms; a sheared matrix is blended number by number;
//  - blend(Camera): the end points bit for bit; half way between two poses it is the camera at the half-way pose with
//    the same projection; a zoom moves the projection straight;
//  - extract() / build(): object matrices by their tag, bone matrices by slot and place (whatever the buffer's
//    address), every copy of a camera matrix and of the camera's position; at t = 1 the stream is the list's own,
//    at t = 0 every blended value is the frame before's, bit for bit, and no other byte changes; a thing without a
//    partner, with another model or that jumped stays at its newest value; a camera cut leaves the whole stream.
#include "dw5xl_smooth_list.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dw5xl::smooth;

namespace {
int failures = 0;
void check(bool ok, const std::string &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

constexpr double kPi = 3.14159265358979323846;

// The test's own matrix arithmetic, in double, row-major (m[row][column]); the game stores columns.
struct M4
{
    double m[4][4]{};
};
M4 identity()
{
    M4 r;
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0;
    return r;
}
M4 mul(const M4 &a, const M4 &b)
{
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
M4 rotY(double degrees)
{
    M4 r = identity();
    const double c = std::cos(degrees * kPi / 180.0), s = std::sin(degrees * kPi / 180.0);
    r.m[0][0] = c; r.m[0][2] = s; r.m[2][0] = -s; r.m[2][2] = c;
    return r;
}
M4 scaled(const M4 &a, double x, double y, double z)
{
    M4 r = a;
    for (int i = 0; i < 3; ++i) { r.m[i][0] *= x; r.m[i][1] *= y; r.m[i][2] *= z; }
    return r;
}
M4 at(const M4 &a, double x, double y, double z)
{
    M4 r = a;
    r.m[0][3] = x; r.m[1][3] = y; r.m[2][3] = z;
    return r;
}
M4 rigidInverse(const M4 &a)
{
    M4 r = identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    for (int i = 0; i < 3; ++i)
        r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    return r;
}
Matrix game(const M4 &a)
{
    Matrix r{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) r[column * 4 + row] = static_cast<float>(a.m[row][column]);
    return r;
}
bool same(const Matrix &a, const Matrix &b) { return std::memcmp(a.data(), b.data(), sizeof(float) * 16) == 0; }
double apart(const Matrix &a, const Matrix &b)
{
    double worst = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        const double scale = std::max(1.0, std::max(std::fabs(double(a[i])), std::fabs(double(b[i]))));
        worst = std::max(worst, std::fabs(double(a[i]) - double(b[i])) / scale);
    }
    return worst;
}
double columnLength(const Matrix &a, int column)
{
    return std::sqrt(double(a[column * 4]) * a[column * 4] + double(a[column * 4 + 1]) * a[column * 4 + 1] +
                     double(a[column * 4 + 2]) * a[column * 4 + 2]);
}
double determinant3(const Matrix &a)
{
    const double x0 = a[0], y0 = a[1], z0 = a[2], x1 = a[4], y1 = a[5], z1 = a[6], x2 = a[8], y2 = a[9], z2 = a[10];
    return x0 * (y1 * z2 - z1 * y2) - x1 * (y0 * z2 - z0 * y2) + x2 * (y0 * z1 - z0 * y1);
}

// A projection and a screen matrix like the game's.
M4 projection(double fx)
{
    M4 p;
    p.m[0][0] = fx; p.m[1][1] = 3.5; p.m[2][2] = 1.01432; p.m[2][3] = -128.916; p.m[3][2] = 1.0;
    return p;
}
M4 screen()
{
    M4 s = identity();
    s.m[0][0] = 256.0; s.m[0][3] = 2048.0; s.m[1][1] = 112.0; s.m[1][3] = 2048.0; s.m[2][2] = -32735.5; s.m[2][3] = 32735.5;
    return s;
}
// The camera standing at (x, y, z), turned by `yaw` degrees.
Camera camera(double x, double y, double z, double yaw, double fx)
{
    const M4 world = at(rotY(yaw), x, y, z), view = rigidInverse(world);
    Camera c;
    c.view = 0;
    c.worldToView = game(view);
    c.viewToWorld = game(world);
    c.viewToWorld[7] = -2.2939f; // the game leaves another number in an unused place
    c.projView = game(mul(projection(fx), view));
    c.screenProjView = game(mul(screen(), mul(projection(fx), view)));
    c.clip = game(mul(projection(fx * 0.157), view));
    c.wideClip = game(mul(projection(fx * 0.667), view));
    return c;
}

void put32(std::vector<uint8_t> &ram, uint32_t at, uint32_t value) { std::memcpy(ram.data() + at, &value, 4); }
void putMatrix(std::vector<uint8_t> &ram, uint32_t at, const Matrix &m) { std::memcpy(ram.data() + at, m.data(), 64); }
uint32_t get32(const std::vector<uint8_t> &bytes, size_t at)
{
    uint32_t v;
    std::memcpy(&v, bytes.data() + at, 4);
    return v;
}
Matrix getMatrix(const std::vector<uint8_t> &bytes, size_t at)
{
    Matrix m;
    std::memcpy(m.data(), bytes.data() + at, 64);
    return m;
}
void tag(std::vector<uint8_t> &ram, uint32_t at, uint32_t id, uint32_t qwc, uint32_t addr)
{
    put32(ram, at, (id << 28) | qwc);
    put32(ram, at + 4u, addr);
}

// One frame's list in an empty memory image:
//   a matrix block (object A), a second matrix block (object B), a camera block, a free inline block holding a copy
//   of the camera's projection x view and of its position, a reference to a bone buffer of two matrices, the end.
constexpr uint32_t kObjectA = 0x100000u, kObjectB = 0x100080u, kCameraBlock = 0x100100u, kFree = 0x100280u;
constexpr uint32_t kBoneRef = 0x100300u, kEnd = 0x100310u;
constexpr uint32_t kSourceA = 0x00BBC5C0u, kSourceB = 0x00BBC440u;

struct Scene
{
    Matrix objectA, objectB, bone0, bone1;
    float colourA[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // the colour scale behind object A's matrix
    uint32_t programA = 0x1400001Eu;             // the VIF code that starts object A's VU1 program
    Camera cam;
    Camera second;             // a second camera of the same view (another projection), when `two` is set:
    bool two = false;          // the free block then holds its projection x view
    bool effect = false;       // one more inline block behind the bone packet (something only this frame draws)
    uint32_t boneBase = 0, modelA = 7u;
};

List makeList(const Scene &scene, std::vector<BoneBuffer> &buffers)
{
    std::vector<uint8_t> ram(kRamBytes, 0u);
    tag(ram, kObjectA, 1u, 7u, 0u);
    put32(ram, kObjectA + 0x1Cu, 0x6C058000u);
    putMatrix(ram, kObjectA + 0x20u, scene.objectA);
    std::memcpy(ram.data() + kObjectA + 0x60u, scene.colourA, 16);
    put32(ram, kObjectA + 0x70u, scene.programA);
    tag(ram, kObjectB, 1u, 7u, 0u);
    put32(ram, kObjectB + 0x1Cu, 0x6C058000u);
    putMatrix(ram, kObjectB + 0x20u, scene.objectB);
    tag(ram, kCameraBlock, 1u, 0x17u, 0u);
    put32(ram, kCameraBlock + 0x1Cu, 0x6C150000u);
    putMatrix(ram, kCameraBlock + 0x20u, scene.cam.screenProjView);
    putMatrix(ram, kCameraBlock + 0x60u, scene.cam.clip);
    putMatrix(ram, kCameraBlock + 0xA0u, scene.cam.projView);
    putMatrix(ram, kCameraBlock + 0xE0u, game(screen()));
    tag(ram, kFree, 1u, 7u, 0u);
    put32(ram, kFree + 0x10u, 0x01000404u);
    std::memcpy(ram.data() + kFree + 0x20u, scene.cam.viewToWorld.data() + 12, 16); // the camera's position
    putMatrix(ram, kFree + 0x30u, scene.two ? scene.second.projView : scene.cam.projView);
    tag(ram, kBoneRef, 3u, 9u, scene.boneBase);
    put32(ram, scene.boneBase + 0x0Cu, 0x6C080010u);                                // UNPACK V4-32 x 8 to 0x10
    putMatrix(ram, scene.boneBase + 0x10u, scene.bone0);
    putMatrix(ram, scene.boneBase + 0x50u, scene.bone1);
    if (scene.effect)
    {
        tag(ram, kEnd, 1u, 2u, 0u);
        put32(ram, kEnd + 0x10u, 0x01000404u);
        put32(ram, kEnd + 0x20u, 0x12345678u);
        tag(ram, kEnd + 0x30u, 7u, 0u, 0u);
    }
    else
    {
        tag(ram, kEnd, 7u, 0u, 0u);
    }

    List list;
    check(flatten(ram.data(), kObjectA, true, list), "the test frame's list is copied");
    std::vector<Tag> recorded;
    Tag t;
    t.kind = TagKind::Matrix; t.block = kObjectA; t.key = kSourceA; t.model = scene.modelA; t.serial = 1u;
    recorded.push_back(t);
    t.kind = TagKind::Matrix; t.block = kObjectB; t.key = kSourceB; t.model = 9u; t.serial = 2u;
    recorded.push_back(t);
    t.kind = TagKind::Camera; t.block = kCameraBlock; t.key = 0u; t.model = 0u; t.serial = 3u;
    recorded.push_back(t);
    resolve(recorded, list);
    list.cameras.push_back(scene.cam);
    if (scene.two)
        list.cameras.push_back(scene.second);
    buffers.clear();
    BoneBuffer buffer;
    buffer.base = scene.boneBase; buffer.slot = 3u; buffer.pair = 0u;
    buffers.push_back(buffer);
    return list;
}

uint32_t offsetIn(const List &list, uint32_t address)
{
    uint32_t at = 0;
    check(offsetOf(list, address, at), "a test address is in the stream");
    return at;
}
} // namespace

int main()
{
    // ---- blend(Matrix)
    const Matrix a = game(at(rotY(0.0), 0.0, 0.0, 0.0)), b = game(at(rotY(90.0), 100.0, 0.0, 0.0));
    check(same(blend(a, b, 0.0f), a) && same(blend(a, b, -1.0f), a), "t = 0 is the first transform, bit for bit");
    check(same(blend(a, b, 1.0f), b) && same(blend(a, b, 2.0f), b), "t = 1 is the second transform, bit for bit");
    const Matrix half = blend(a, b, 0.5f);
    check(apart(half, game(at(rotY(45.0), 50.0, 0.0, 0.0))) < 1e-5, "half way between 0 and 90 degrees is 45 degrees, half the way along");
    check(std::fabs(columnLength(half, 0) - 1.0) < 1e-5 && std::fabs(columnLength(half, 2) - 1.0) < 1e-5,
          "a turning thing keeps its size (number by number it would shrink to 0.71)");
    const Matrix big = blend(game(scaled(rotY(30.0), 2.0, 2.0, 2.0)), game(scaled(rotY(30.0), 4.0, 4.0, 4.0)), 0.5f);
    check(apart(big, game(scaled(rotY(30.0), 3.0, 3.0, 3.0))) < 1e-5, "scale moves straight: half way between 2 and 4 is 3");
    const Matrix around = blend(game(rotY(170.0)), game(rotY(-170.0)), 0.5f);
    check(apart(around, game(rotY(180.0))) < 1e-5, "a rotation turns the short way (170 to -170 degrees passes 180, not 0)");
    const Matrix mirrored = blend(game(scaled(rotY(10.0), -1.0, 1.0, 1.0)), game(scaled(rotY(30.0), -1.0, 1.0, 1.0)), 0.5f);
    check(determinant3(mirrored) < -0.99 && std::fabs(columnLength(mirrored, 0) - 1.0) < 1e-5 &&
              apart(mirrored, game(scaled(rotY(20.0), -1.0, 1.0, 1.0))) < 1e-5,
          "two mirrored transforms blend to a mirrored transform");
    M4 shear = identity();
    shear.m[0][1] = 1.0;
    const Matrix sheared = blend(game(shear), game(at(identity(), 10.0, 0.0, 0.0)), 0.5f);
    M4 expected = identity();
    expected.m[0][1] = 0.5; expected.m[0][3] = 5.0;
    check(apart(sheared, game(expected)) < 1e-6, "a sheared matrix is blended number by number");
    check(jumped(game(at(identity(), 0.0, 0.0, 0.0)), game(at(identity(), 1000.0, 0.0, 0.0))) &&
              !jumped(game(at(identity(), 0.0, 0.0, 0.0)), game(at(identity(), 14.0, 0.0, 0.0))),
          "a thing 1000 units away one frame later jumped; 14 units is motion");

    // ---- blend(Camera)
    const Camera before = camera(28755.0, -4989.0, 20800.0, 40.0, 2.63675), now = camera(28769.0, -4989.5, 20809.5, 60.0, 2.63675);
    const Camera at0 = blend(before, now, 0.0f), at1 = blend(before, now, 1.0f);
    check(same(at0.screenProjView, before.screenProjView) && same(at0.projView, before.projView) && same(at0.clip, before.clip) &&
              same(at0.wideClip, before.wideClip) && same(at0.worldToView, before.worldToView) && same(at0.viewToWorld, before.viewToWorld),
          "the camera at t = 0 is the camera before, bit for bit");
    check(same(at1.screenProjView, now.screenProjView) && same(at1.projView, now.projView) && same(at1.viewToWorld, now.viewToWorld),
          "the camera at t = 1 is the newest camera, bit for bit");
    const Camera middle = blend(before, now, 0.5f), wanted = camera(28762.0, -4989.25, 20804.75, 50.0, 2.63675);
    check(apart(middle.worldToView, wanted.worldToView) < 1e-5 && apart(middle.projView, wanted.projView) < 1e-4 &&
              apart(middle.screenProjView, wanted.screenProjView) < 1e-4 && apart(middle.clip, wanted.clip) < 1e-4 &&
              apart(middle.wideClip, wanted.wideClip) < 1e-4,
          "half way it is the camera at the half-way pose with the same projection");
    check(std::fabs(middle.viewToWorld[12] - 28762.0f) < 0.01f && std::fabs(middle.viewToWorld[14] - 20804.75f) < 0.01f &&
              middle.viewToWorld[7] == now.viewToWorld[7],
          "its position is half way; the unused number of the game's matrix is kept");
    const Camera zoomed = blend(camera(100.0, 0.0, 0.0, 10.0, 2.6), camera(100.0, 0.0, 0.0, 10.0, 3.0), 0.5f);
    check(apart(zoomed.projView, camera(100.0, 0.0, 0.0, 10.0, 2.8).projView) < 1e-4, "a zoom moves the projection straight");

    // ---- extract() and build()
    Scene earlier, later;
    earlier.objectA = game(at(rotY(10.0), 30000.0, -5000.0, 15000.0));
    later.objectA = game(at(rotY(30.0), 30010.0, -5000.0, 15004.0));
    earlier.objectB = game(at(rotY(0.0), 31000.0, -5300.0, 16000.0));
    later.objectB = game(at(rotY(0.0), 31002.0, -5300.0, 16000.0));
    earlier.bone0 = game(at(rotY(80.0), 28700.0, -4900.0, 20790.0));
    later.bone0 = game(at(rotY(95.0), 28712.0, -4900.0, 20795.0));
    earlier.bone1 = game(at(rotY(-20.0), 28701.0, -4850.0, 20790.0));
    later.bone1 = game(at(rotY(-35.0), 28713.0, -4851.0, 20795.0));
    earlier.cam = before;
    later.cam = now;
    earlier.colourA[3] = 0.25f; // object A fades in
    later.colourA[3] = 0.75f;
    earlier.boneBase = 0x017B3480u; // the game writes the two buffers of a pair in turn
    later.boneBase = 0x017B1980u;

    std::vector<BoneBuffer> buffersBefore, buffersNow;
    const List listBefore = makeList(earlier, buffersBefore);
    const List listNow = makeList(later, buffersNow);
    Values valuesBefore, valuesNow;
    extract(listBefore, buffersBefore, valuesBefore);
    extract(listNow, buffersNow, valuesNow);
    check(valuesNow.items.size() == 4u && valuesBefore.items.size() == 4u && valuesNow.cameras.size() == 1u,
          "two object matrices, two bone matrices and the camera are found");
    bool keysEqual = valuesNow.items.size() == valuesBefore.items.size();
    for (size_t i = 0; keysEqual && i < valuesNow.items.size(); ++i)
        keysEqual = valuesNow.items[i].key == valuesBefore.items[i].key;
    check(keysEqual, "a bone has the same key in both frames although its buffer is at another address");

    const uint32_t objectA = offsetIn(listNow, kObjectA + 0x20u), objectB = offsetIn(listNow, kObjectB + 0x20u);
    const uint32_t camA = offsetIn(listNow, kCameraBlock + 0x20u), camClip = offsetIn(listNow, kCameraBlock + 0x60u);
    const uint32_t camPV = offsetIn(listNow, kCameraBlock + 0xA0u), camScreen = offsetIn(listNow, kCameraBlock + 0xE0u);
    const uint32_t freePosition = offsetIn(listNow, kFree + 0x20u), freePV = offsetIn(listNow, kFree + 0x30u);
    const uint32_t bone0 = offsetIn(listNow, later.boneBase + 0x10u), bone1 = offsetIn(listNow, later.boneBase + 0x50u);

    std::vector<uint8_t> stream;
    BuildStats stats;
    build(listNow, valuesNow, valuesBefore, 1.0f, stream, &stats);
    check(stream == listNow.stream, "at t = 1 the stream is the frame's own list, byte for byte");

    build(listNow, valuesNow, valuesBefore, 0.0f, stream, &stats);
    check(same(getMatrix(stream, objectA), earlier.objectA) && same(getMatrix(stream, objectB), earlier.objectB),
          "at t = 0 the object matrices are the frame before's, bit for bit");
    check(same(getMatrix(stream, bone0), earlier.bone0) && same(getMatrix(stream, bone1), earlier.bone1),
          "at t = 0 the bone matrices are the frame before's, bit for bit");
    check(same(getMatrix(stream, camA), before.screenProjView) && same(getMatrix(stream, camClip), before.clip) &&
              same(getMatrix(stream, camPV), before.projView) && same(getMatrix(stream, freePV), before.projView),
          "at t = 0 every copy of a camera matrix is the frame before's, in the camera block and elsewhere");
    check(std::memcmp(stream.data() + freePosition, before.viewToWorld.data() + 12, 16) == 0,
          "at t = 0 the copy of the camera's position is the frame before's");
    check(same(getMatrix(stream, camScreen), game(screen())), "the screen matrix is left alone");
    check(std::memcmp(stream.data() + objectA + 64u, earlier.colourA, 16) == 0,
          "at t = 0 the colour scale behind an object's matrix is the frame before's");
    {
        std::vector<uint8_t> rest = stream, original = listNow.stream;
        for (const uint32_t at : {objectA, objectB, bone0, bone1, camA, camClip, camPV, freePV})
        {
            std::memset(rest.data() + at, 0, 64);
            std::memset(original.data() + at, 0, 64);
        }
        std::memset(rest.data() + freePosition, 0, 16);
        std::memset(original.data() + freePosition, 0, 16);
        std::memset(rest.data() + objectA + 64u, 0, 16);
        std::memset(original.data() + objectA + 64u, 0, 16);
        check(rest == original, "no other byte of the stream changes");
    }

    build(listNow, valuesNow, valuesBefore, 0.5f, stream, &stats);
    check(same(getMatrix(stream, objectA), blend(earlier.objectA, later.objectA, 0.5f)) &&
              same(getMatrix(stream, bone1), blend(earlier.bone1, later.bone1, 0.5f)),
          "half way the matrices are the blended ones");
    check(same(getMatrix(stream, camA), middle.screenProjView) && same(getMatrix(stream, freePV), middle.projView),
          "half way every camera copy is the blended camera's");
    {
        float colour[4];
        std::memcpy(colour, stream.data() + objectA + 64u, 16);
        check(colour[0] == 1.0f && colour[3] == 0.5f, "half way the colour scale is half way (0.25 to 0.75 gives 0.5)");
    }
    check(stats.objects == 2u && stats.bones == 2u && stats.cameraCopies == 5u && stats.unmatched == 0u && stats.jumped == 0u && !stats.cut,
          "the build counts what it blended (2 objects, 2 bones, 5 camera copies)");

    // A thing without a partner, with another model, or that jumped stays at its newest value.
    Scene other = earlier;
    other.modelA = 8u;
    other.objectB = game(at(rotY(0.0), 20000.0, -5300.0, 16000.0));
    std::vector<BoneBuffer> buffersOther;
    const List listOther = makeList(other, buffersOther);
    Values valuesOther;
    extract(listOther, buffersOther, valuesOther);
    build(listNow, valuesNow, valuesOther, 0.5f, stream, &stats);
    check(same(getMatrix(stream, objectA), later.objectA) && stats.unmatched == 1u, "an object whose model changed is left at its newest value");
    check(same(getMatrix(stream, objectB), later.objectB) && stats.jumped == 1u, "an object that jumped is left at its newest value");
    Values none;
    build(listNow, valuesNow, none, 0.5f, stream, &stats);
    check(stream == listNow.stream && stats.unmatched == 4u, "without a frame before nothing is blended");

    // A camera cut: the whole picture stays at the newest frame.
    Scene cutScene = earlier;
    cutScene.cam = camera(20000.0, -4989.0, 20800.0, 40.0, 2.63675);
    std::vector<BoneBuffer> buffersCut;
    const List listCut = makeList(cutScene, buffersCut);
    Values valuesCut;
    extract(listCut, buffersCut, valuesCut);
    build(listNow, valuesNow, valuesCut, 0.5f, stream, &stats);
    check(stats.cut && stream == listNow.stream, "after a camera cut the stream is the frame's own list");
    cutScene.cam = camera(28755.0, -4989.0, 20800.0, -100.0, 2.63675);
    const List listTurn = makeList(cutScene, buffersCut);
    extract(listTurn, buffersCut, valuesCut);
    build(listNow, valuesNow, valuesCut, 0.5f, stream, &stats);
    check(stats.cut && stream == listNow.stream, "a camera that turned 160 degrees in one frame is a cut too");

    // Two cameras of one view in a frame: each copy gets the in-between of its own camera.
    Scene twoBefore = earlier, twoNow = later;
    twoBefore.two = twoNow.two = true;
    twoBefore.second = camera(28755.0, -4989.0, 20800.0, 40.0, 5.0);
    twoNow.second = camera(28769.0, -4989.5, 20809.5, 60.0, 5.0);
    std::vector<BoneBuffer> buffersTwo;
    const List listTwoBefore = makeList(twoBefore, buffersTwo);
    Values valuesTwoBefore, valuesTwoNow;
    extract(listTwoBefore, buffersTwo, valuesTwoBefore);
    std::swap(valuesTwoBefore.cameras[0], valuesTwoBefore.cameras[1]);   // their order must not matter
    const List listTwoNow = makeList(twoNow, buffersTwo);
    extract(listTwoNow, buffersTwo, valuesTwoNow);
    build(listTwoNow, valuesTwoNow, valuesTwoBefore, 0.0f, stream, &stats);
    check(same(getMatrix(stream, camPV), twoBefore.cam.projView) && same(getMatrix(stream, freePV), twoBefore.second.projView),
          "with two cameras of one view each copy gets its own camera's value");

    // ---- sharedOnly(): the "blended content only" test switch.
    {
        // Everything in the test frame is blended: the stream at t = 0 is the list before's stream, byte for byte,
        // and nothing is left out.
        std::vector<uint8_t> atZero, beforeOnly;
        build(listNow, valuesNow, valuesBefore, 0.0f, atZero, nullptr);
        check(atZero == listBefore.stream, "when everything is blended, the stream at t = 0 is the list before's stream");
        SharedStats shared;
        sharedOnly(listBefore, listNow, atZero, beforeOnly, &shared);
        check(shared.partsNow == 5u && shared.partsBefore == 5u && shared.shared == 5u && atZero == listBefore.stream &&
                  beforeOnly == listBefore.stream,
              "all five parts are shared and both streams stay whole");

        // The newer frame has an effect the older one has not, the older one a bone packet of another character,
        // and one object jumped (left at its newest value, so its block is not the list before's).
        Scene withEffect = later, withOther = earlier;
        withEffect.effect = true;
        withOther.objectB = game(at(rotY(0.0), 20000.0, -5300.0, 16000.0));
        withOther.bone1 = game(at(rotY(5.0), 100.0, 200.0, 300.0));
        std::vector<BoneBuffer> buffersEffect, buffersOther2;
        const List listEffect = makeList(withEffect, buffersEffect);
        const List listOther2 = makeList(withOther, buffersOther2);
        buffersOther2.clear(); // the older frame's bone packet belongs to no known character: its bones have no key
        Values valuesEffect, valuesOther2;
        extract(listEffect, buffersEffect, valuesEffect);
        extract(listOther2, buffersOther2, valuesOther2);
        build(listEffect, valuesEffect, valuesOther2, 0.0f, atZero, nullptr);
        sharedOnly(listOther2, listEffect, atZero, beforeOnly, &shared, &valuesEffect, &valuesOther2);
        check(shared.partsNow == 6u && shared.partsBefore == 5u && shared.shared == 3u,
              "object A, the camera block and the free block are shared; object B, the bones and the effect are not");
        check(shared.objectBlocks == 1u && shared.bonePackets == 1u && shared.otherInline == 1u && shared.cameraBlocks == 0u &&
                  shared.otherReferences == 0u && shared.otherInlineBytes == 0x20u,
              "what is left out is counted by kind: one object block, one bone packet, one other inline block");
        check(shared.objects.jumped == 1u && shared.bones.fresh == 1u && shared.objects.different == 0u &&
                  shared.bones.different == 0u && !shared.hasExample,
              "and why: the object jumped, the bones have no partner; nothing the builder blended came out different");

        // The game draws object A with another VU1 program in the newer frame: its block is the frame before's but
        // for that one word. And a block that differs elsewhere (here: a number the builder does not blend) counts
        // as different, with the word reported.
        std::vector<uint8_t> atZero2, beforeOnly2;
        SharedStats shared2;
        Scene otherProgram = later;
        otherProgram.programA = 0x1400006Eu;
        std::vector<BoneBuffer> buffersProgram;
        const List listProgram = makeList(otherProgram, buffersProgram);
        Values valuesProgram;
        extract(listProgram, buffersProgram, valuesProgram);
        build(listProgram, valuesProgram, valuesBefore, 0.0f, atZero2, nullptr);
        sharedOnly(listBefore, listProgram, atZero2, beforeOnly2, &shared2, &valuesProgram, &valuesBefore);
        check(shared2.shared == 4u && shared2.objects.program == 1u && shared2.objects.different == 0u && !shared2.hasExample,
              "an object block that differs only in its draw program is counted as that, not as different");
        build(listNow, valuesNow, valuesBefore, 0.0f, atZero2, nullptr);
        const uint32_t odd = 0x3F000000u;
        std::memcpy(atZero2.data() + objectA - 0x10u, &odd, 4); // the first word of the block's data
        sharedOnly(listBefore, listNow, atZero2, beforeOnly2, &shared2, &valuesNow, &valuesBefore);
        check(shared2.objects.different == 1u && shared2.objects.program == 0u && shared2.hasExample && shared2.exampleKind == 0u &&
                  shared2.exampleWords == 1u && shared2.exampleWord == 0u && shared2.exampleNow == odd,
              "an object block that differs elsewhere is counted as different, with its first differing word");
        const auto zero = [](const std::vector<uint8_t> &bytes, uint32_t from, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
                if (bytes[from + i] != 0u)
                    return false;
            return true;
        };
        const uint32_t blockB = offsetIn(listEffect, kObjectB + 0x10u), effectAt = offsetIn(listEffect, kEnd + 0x10u);
        const uint32_t bonesAt = offsetIn(listEffect, withEffect.boneBase);
        check(zero(atZero, blockB - 8u, 8u + 0x70u) && zero(atZero, effectAt - 8u, 8u + 0x20u) && zero(atZero, bonesAt - 8u, 8u + 0x90u),
              "the newer stream has NOPs where its three unshared parts were, their tag halves included");
        check(zero(beforeOnly, offsetIn(listOther2, kObjectB + 0x10u) - 8u, 8u + 0x70u) &&
                  zero(beforeOnly, offsetIn(listOther2, withOther.boneBase) - 8u, 8u + 0x90u),
              "the older stream has NOPs where its two unshared parts were");
        check(std::memcmp(atZero.data() + objectA, beforeOnly.data() + offsetIn(listOther2, kObjectA + 0x20u), 0x50u) == 0 &&
                  !zero(atZero, objectA, 64u),
              "a shared part holds the same bytes in both streams");
    }

    {
        // A part counts only in its place. Two soldiers use the same model packet (M); the newer frame draws a new
        // soldier (bones Y) before the one both frames have (bones X): the model behind X is the shared one.
        const auto listOf = [](const std::vector<uint32_t> &marks)
        {
            std::vector<uint8_t> ram(kRamBytes, 0u);
            uint32_t at = 0x100000u;
            for (const uint32_t mark : marks)
            {
                tag(ram, at, 1u, 1u, 0u);
                put32(ram, at + 0x10u, mark);
                at += 0x20u;
            }
            tag(ram, at, 7u, 0u, 0u);
            List list;
            flatten(ram.data(), 0x100000u, true, list);
            return list;
        };
        const uint32_t bonesX = 0x11u, bonesY = 0x33u, model = 0x22u;
        const List older = listOf({bonesX, model}), newer = listOf({bonesY, model, bonesX, model});
        std::vector<uint8_t> newerStream = newer.stream, olderStream;
        SharedStats shared;
        sharedOnly(older, newer, newerStream, olderStream, &shared);
        const auto mark = [](const std::vector<uint8_t> &bytes, uint32_t part) { return get32(bytes, 8u + part * 24u); };
        check(shared.shared == 2u && olderStream == older.stream, "the two parts of the older list are both shared");
        check(mark(newerStream, 0u) == 0u && mark(newerStream, 1u) == 0u && mark(newerStream, 2u) == bonesX &&
                  mark(newerStream, 3u) == model,
              "of two equal model packets the one behind the shared bone packet is kept, the other left out");
    }

    std::printf("%s\n", failures == 0 ? "smooth_builder: PASS" : "smooth_builder: FAIL");
    return failures == 0 ? 0 : 1;
}
