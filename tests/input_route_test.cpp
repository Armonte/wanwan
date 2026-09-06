// Unit test for the input binder's two device decisions: which physical pad
// each player reads, and which pad press becomes a binding.
//
// This exists because NO harness in this repo can reach the configuration
// where both of them go wrong. The two-instance gate drives synthetic inputs
// and never opens a physical device, so "two controllers plugged in" -- the
// exact shape of the field report this was written for -- is unreachable
// there and always will be. The rules are therefore pure (input_route.h) and
// exercised directly, the same arrangement alt_colors_test.cpp uses.
//
// Build + run (host compiler), from repo root:
//   g++ -std=c++17 -Wall -o /tmp/input_route_test tests/input_route_test.cpp
//   /tmp/input_route_test
#include "../FM2KHook/src/ui/input_route.h"
#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr)                                                    \
    do {                                                               \
        if (expr) {                                                    \
            ++g_pass;                                                  \
        } else {                                                       \
            ++g_fail;                                                  \
            std::printf("  FAIL  line %d:  %s\n", __LINE__, #expr);    \
        }                                                              \
    } while (0)

using namespace fm2k::inputroute;

// Two different pads, and a second unit of the SAME model as pad A (same
// GUID, no serial -- what SDL reports for two identical XInput pads).
static const uint32_t kJidA = 11;
static const uint32_t kJidB = 22;
static const uint32_t kJidA2 = 33;
static const char* kIdA = "03000000abcd0000ef00000000000000";
static const char* kIdB = "030000005e040000e002000000000000";

static PlayerProfile Legacy(int index) {
    PlayerProfile p;
    p.has_gamepad_binding = true;
    p.legacy_index = index;
    return p;
}

static PlayerProfile Pinned(const char* identity) {
    PlayerProfile p;
    p.has_gamepad_binding = true;
    p.device_id = identity;
    p.legacy_index = -1;
    return p;
}

static Route RouteOf(const std::vector<Pad>& pads,
                     const PlayerProfile (&players)[kPlayers],
                     uint32_t (&sticky)[kPlayers], int slot) {
    ResolveIdentities(pads, players, sticky);
    return RouteForPlayer(pads, players[slot], sticky[slot]);
}

static void TestOnePadIsUnambiguous() {
    // The baseline the field report calls "labbing offline feels totally
    // fine": one pad, and every routing style lands on it.
    const std::vector<Pad> pads = {{kJidA, kIdA}};

    PlayerProfile legacy[kPlayers] = {Legacy(0), Legacy(-1)};
    uint32_t sticky[kPlayers] = {0, 0};
    CHECK(RouteOf(pads, legacy, sticky, 0).jid == kJidA);
    CHECK(RouteOf(pads, legacy, sticky, 1).jid == kJidA);

    PlayerProfile pinned[kPlayers] = {Pinned(kIdA), PlayerProfile{}};
    uint32_t sticky2[kPlayers] = {0, 0};
    CHECK(RouteOf(pads, pinned, sticky2, 0).jid == kJidA);
    CHECK(RouteOf(pads, pinned, sticky2, 0).via == Via::IDENTITY);
}

static void TestLegacyIndexAimsBothPlayersAtOnePad() {
    // MEASURED DEFECT, shipped behaviour. Both players' gamepad rows carry
    // gamepad_index 0 -- what you get when the profile was bound while only
    // one pad was connected, since bind capture stamps the index of the pad
    // it saw. Plug in a second pad and NOTHING re-routes them: both players
    // read pad A, so one person's presses drive both characters and the
    // other person's pad is dead.
    const std::vector<Pad> pads = {{kJidA, kIdA}, {kJidB, kIdB}};
    PlayerProfile players[kPlayers] = {Legacy(0), Legacy(0)};
    uint32_t sticky[kPlayers] = {0, 0};

    const Route p1 = RouteOf(pads, players, sticky, 0);
    const Route p2 = RouteOf(pads, players, sticky, 1);
    CHECK(p1.jid == kJidA);
    CHECK(p2.jid == kJidA);
    CHECK(RoutesCollide(p1, p2));   // the invariant violation, now nameable

    // With ONE pad connected this config is indistinguishable from a correct
    // one, which is why it only shows up when a second person sits down.
    const std::vector<Pad> one = {{kJidA, kIdA}};
    uint32_t sticky_one[kPlayers] = {0, 0};
    CHECK(RoutesCollide(RouteOf(one, players, sticky_one, 0),
                        RouteOf(one, players, sticky_one, 1)));
}

static void TestLegacyIndexIsNotADevice() {
    // MEASURED DEFECT, shipped behaviour. A list index names a POSITION, not
    // a device: the same profile reads a different physical pad when the
    // list order changes (unplug/replug, a pad that enumerates first, a
    // wireless pad that reconnects). The player's inputs do not stop working
    // in a way they can see -- they start driving the wrong character.
    PlayerProfile players[kPlayers] = {Legacy(0), Legacy(1)};
    uint32_t sticky[kPlayers] = {0, 0};

    const std::vector<Pad> ab = {{kJidA, kIdA}, {kJidB, kIdB}};
    CHECK(RouteOf(ab, players, sticky, 0).jid == kJidA);
    CHECK(RouteOf(ab, players, sticky, 1).jid == kJidB);

    const std::vector<Pad> ba = {{kJidB, kIdB}, {kJidA, kIdA}};
    CHECK(RouteOf(ba, players, sticky, 0).jid == kJidB);   // swapped
    CHECK(RouteOf(ba, players, sticky, 1).jid == kJidA);   // swapped

    // Unplugging the OTHER player's pad shortens the list, and P2's index 1
    // now names nothing -- silent, with no message anywhere.
    const std::vector<Pad> b_only = {{kJidB, kIdB}};
    uint32_t sticky_b[kPlayers] = {0, 0};
    CHECK(RouteOf(b_only, players, sticky_b, 1).jid == 0);
}

static void TestIdentityRoutingIsImmune() {
    // The identity mechanism is what makes the two cases above impossible --
    // it is only inert because a profile written before it existed, or one
    // whose rows were captured without ever touching the device dropdown,
    // carries no device_id at all.
    PlayerProfile players[kPlayers] = {Pinned(kIdA), Pinned(kIdB)};
    uint32_t sticky[kPlayers] = {0, 0};

    const std::vector<Pad> ab = {{kJidA, kIdA}, {kJidB, kIdB}};
    CHECK(RouteOf(ab, players, sticky, 0).jid == kJidA);
    CHECK(RouteOf(ab, players, sticky, 1).jid == kJidB);

    // Reordered list: each player keeps their own physical pad.
    const std::vector<Pad> ba = {{kJidB, kIdB}, {kJidA, kIdA}};
    CHECK(RouteOf(ba, players, sticky, 0).jid == kJidA);
    CHECK(RouteOf(ba, players, sticky, 1).jid == kJidB);
    CHECK(!RoutesCollide(RouteForPlayer(ba, players[0], sticky[0]),
                         RouteForPlayer(ba, players[1], sticky[1])));

    // P1's pad away: P1 goes silent, and NEVER borrows P2's.
    const std::vector<Pad> b_only = {{kJidB, kIdB}};
    uint32_t sticky_b[kPlayers] = {0, 0};
    CHECK(RouteOf(b_only, players, sticky_b, 0).jid == 0);
    CHECK(RouteOf(b_only, players, sticky_b, 1).jid == kJidB);
}

static void TestTwoIdenticalPadsGetOneEach() {
    // Two units of the same model with no serial share an identity string.
    // Claims are what stop both players resolving to the same one.
    const std::vector<Pad> pads = {{kJidA, kIdA}, {kJidA2, kIdA}};
    PlayerProfile players[kPlayers] = {Pinned(kIdA), Pinned(kIdA)};
    uint32_t sticky[kPlayers] = {0, 0};

    ResolveIdentities(pads, players, sticky);
    CHECK(sticky[0] == kJidA);
    CHECK(sticky[1] == kJidA2);
    CHECK(!RoutesCollide(RouteForPlayer(pads, players[0], sticky[0]),
                         RouteForPlayer(pads, players[1], sticky[1])));

    // Sticky across a refresh even when the list reorders around them.
    const std::vector<Pad> reordered = {{kJidA2, kIdA}, {kJidA, kIdA}};
    ResolveIdentities(reordered, players, sticky);
    CHECK(sticky[0] == kJidA);
    CHECK(sticky[1] == kJidA2);
}

// --- bind capture ---------------------------------------------------------

static PadState MakePad(uint32_t jid) {
    PadState s;
    s.jid = jid;
    s.buttons.assign(16, 0);
    s.axes.assign(6, 0);
    return s;
}

static void TestParkedAxisCannotBeCaptured() {
    // THE CAPTURE DEFECT. The arm gate releases at 85% of the axis range and
    // the capture fires at 50%, so a stick parked in between -- deadzone
    // drift, a PS3/Qanba stick that boots with an axis off-centre -- arms
    // fine and is then read as a press instantly. With two pads on the
    // table, the pad it steals from is the OTHER player's, and every control
    // in the bind-all walk lands on it in one pass.
    constexpr int kCapture = 16384;   // the shipped capture threshold
    constexpr int kArm     = 28000;   // the shipped arm threshold

    std::vector<PadState> live = {MakePad(kJidA), MakePad(kJidB)};
    live[0].axes[3] = 20000;          // P1's pad, parked between the two
    CHECK(live[0].axes[3] > kCapture);
    CHECK(live[0].axes[3] < kArm);    // so the bind still armed

    // Snapshot taken at arm time sees the same parked value.
    const std::vector<PadState> armed = live;

    // P2 pushes LEFT on their own pad (index 1).
    live[1].axes[0] = -30000;

    const Capture c = PickPadCapture(live, armed, /*prefer=*/-1, kCapture);
    CHECK(c.kind == Capture::Kind::PAD_AXIS);
    CHECK(c.pad_index == 1);          // P2's pad, not P1's parked axis
    CHECK(c.code == 0);
    CHECK(c.axis_dir == -1);

    // Without the snapshot the parked axis wins, which is the shipped
    // behaviour this pins as wrong.
    const Capture old = PickPadCapture(live, {}, -1, kCapture);
    CHECK(old.pad_index == 0);
    CHECK(old.code == 3);
}

static void TestHeldButtonOnOtherPadCannotBeCaptured() {
    // Two people at one PC: the other player is resting a thumb on their
    // face button while you bind. List order used to decide it for you.
    std::vector<PadState> live = {MakePad(kJidA), MakePad(kJidB)};
    live[0].buttons[2] = 1;                 // already down when we armed
    const std::vector<PadState> armed = live;
    live[1].buttons[5] = 1;                 // our actual press

    const Capture c = PickPadCapture(live, armed, -1, 16384);
    CHECK(c.kind == Capture::Kind::PAD_BUTTON);
    CHECK(c.pad_index == 1);
    CHECK(c.code == 5);

    // A press on the ALREADY-HELD button still counts once it is released
    // and pressed again -- the snapshot is taken at arm time, not live.
    std::vector<PadState> live2 = {MakePad(kJidA), MakePad(kJidB)};
    const std::vector<PadState> armed2 = live2;   // nothing held at arm
    live2[0].buttons[2] = 1;
    const Capture c2 = PickPadCapture(live2, armed2, -1, 16384);
    CHECK(c2.pad_index == 0);
    CHECK(c2.code == 2);
}

static void TestOwnPadWinsTies() {
    // Both pads fire on the same tick (both players press at once). The pad
    // this player is already pinned to takes it, instead of list order.
    std::vector<PadState> live = {MakePad(kJidA), MakePad(kJidB)};
    const std::vector<PadState> armed = live;
    live[0].buttons[1] = 1;
    live[1].buttons[7] = 1;

    CHECK(PickPadCapture(live, armed, /*prefer=*/1, 16384).pad_index == 1);
    CHECK(PickPadCapture(live, armed, /*prefer=*/0, 16384).pad_index == 0);
    // No preference falls back to list order, as before.
    CHECK(PickPadCapture(live, armed, /*prefer=*/-1, 16384).pad_index == 0);

    // Preference is a preference, not a lock: a player rebinding onto a NEW
    // device still reaches it when their old pad is quiet.
    std::vector<PadState> only_other = {MakePad(kJidA), MakePad(kJidB)};
    const std::vector<PadState> armed2 = only_other;
    only_other[1].buttons[3] = 1;
    CHECK(PickPadCapture(only_other, armed2, /*prefer=*/0, 16384).pad_index == 1);
}

static void TestSnapshotIgnoredWhenListChanged() {
    // A pad hot-plugged between arm and press shifts the list. The snapshot
    // is keyed by jid, so a stale entry is discarded rather than matched
    // against the wrong device.
    std::vector<PadState> armed = {MakePad(kJidA)};
    armed[0].buttons[4] = 1;
    std::vector<PadState> live = {MakePad(kJidB), MakePad(kJidA)};
    live[0].buttons[4] = 1;   // different device, same slot index

    const Capture c = PickPadCapture(live, armed, -1, 16384);
    CHECK(c.pad_index == 0);
    CHECK(c.code == 4);
}

int main() {
    TestOnePadIsUnambiguous();
    TestLegacyIndexAimsBothPlayersAtOnePad();
    TestLegacyIndexIsNotADevice();
    TestIdentityRoutingIsImmune();
    TestTwoIdenticalPadsGetOneEach();
    TestParkedAxisCannotBeCaptured();
    TestHeldButtonOnOtherPadCannotBeCaptured();
    TestOwnPadWinsTies();
    TestSnapshotIgnoredWhenListChanged();

    std::printf("input_route_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
