// SPDX-License-Identifier: MIT
// Host test of input_own.h (kiln.ui.input, 0.2.0-dev.31.13): a press an owning subscriber takes is its own until the
// release; focus loss, a world change, unload and a disown end it; the stuck-press guard; and tests/own_parity.json, the
// cases the test kit's fake kiln.ui.input replays too (managed/tests/Kiln.TestKit.Tests/UiInputFakeTests.cs).
//   tests/input_own_test <own_parity.json>   (built with -I native/third_party, for nlohmann/json)
#include "../input_own.h"
#include "../pad_view.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../../../core/abi/services/kiln_ui.h"
#include "../../../core/abi/services/kiln_ui_input.h"
#include "nlohmann/json.hpp"

namespace {
int g_fail = 0, g_pass = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    (ok ? g_pass : g_fail)++;
}

using namespace kui::own;

// ---- the parity cases' model: the router input_own.cpp is, without callbacks or a window ----
struct Owner {
    uint64_t handle;
    uint32_t sources;
    std::string name;
    std::vector<uint32_t> take;  // codes; empty + all: every press
    bool all = false;
    std::vector<std::string> log;
};

std::string code(uint32_t id) {
    char b[32];
    if (controller_of(id)) std::snprintf(b, sizeof b, "%x@%u", vk_of(id), controller_of(id));
    else std::snprintf(b, sizeof b, "%x", vk_of(id));
    return b;
}

uint32_t parse_code(const std::string& s) {
    const auto at = s.find('@');
    const uint32_t vk = static_cast<uint32_t>(std::stoul(s.substr(0, at), nullptr, 16));
    const uint32_t c = at == std::string::npos ? 0 : static_cast<uint32_t>(std::stoul(s.substr(at + 1)));
    return id_of(vk, c);
}

const char* why(uint32_t e) {
    switch (e) {
        case kEndFocus: return "focus";
        case kEndWorld: return "world";
        case kEndLost: return "lost";
        default: return "?";
    }
}

struct Model {
    Table table;
    uint32_t gen = 1;
    std::vector<Owner> owners;  // subscribed, in order
    std::vector<Owner> gone;    // unsubscribed (their logs are still compared)
    std::vector<std::string> game, plain;

    Owner* find(uint64_t h) {
        for (auto& o : owners) if (o.handle == h) return &o;
        for (auto& o : gone) if (o.handle == h) return &o;
        return nullptr;
    }
    void deliver(const std::vector<Note>& notes) {
        for (const auto& n : notes) {
            Owner* o = find(n.sub);
            if (!o) continue;
            if (n.kind == kRepeat) o->log.push_back("repeat " + code(n.id));
            else if (n.kind == kRelease) o->log.push_back("release " + code(n.id));
            else if (n.kind == kEnd) o->log.push_back(std::string("end ") + code(n.id) + " " + why(n.end));
        }
    }
    void down(uint32_t id) {
        std::vector<Note> notes;
        const Decision d = table.transition(id, true, false, gen, &notes);
        deliver(notes);
        if (d.handled) return;  // never for a new press
        const uint64_t who = offer(table, id, gen, owners, [&](Owner& o) {
            o.log.push_back("press " + code(id));
            if (o.all) return true;
            for (const uint32_t t : o.take) if (t == vk_of(id)) return true;
            return false;
        });
        if (who) return;
        plain.push_back("down " + code(id));
        game.push_back("down " + code(id));
    }
    void repeat(uint32_t id) {
        std::vector<Note> notes;
        const Decision d = table.transition(id, true, true, gen, &notes);
        deliver(notes);
        if (d.handled) {
            if (!d.keep) game.push_back("repeat " + code(id));
            return;
        }
        plain.push_back("repeat " + code(id));
        game.push_back("repeat " + code(id));
    }
    void up(uint32_t id) {
        std::vector<Note> notes;
        const Decision d = table.transition(id, false, false, gen, &notes);
        deliver(notes);
        if (d.handled) {
            if (!d.keep) game.push_back("up " + code(id));
            return;
        }
        plain.push_back("up " + code(id));
        game.push_back("up " + code(id));
    }
};

uint32_t sources_of(const nlohmann::json& j) {
    uint32_t s = 0;
    for (const auto& x : j) {
        const std::string n = x.get<std::string>();
        s |= n == "keys" ? kKeys : n == "mouse" ? kMouse : n == "pointer" ? kPointer : n == "pad" ? kPad : 0u;
    }
    return s;
}

std::string join(const std::vector<std::string>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + v[i];
    return s + "]";
}

void parity(const char* path) {
    std::ifstream f(path);
    check(f.good(), std::string("own_parity.json opens (") + path + ")");
    if (!f.good()) return;
    const auto doc = nlohmann::json::parse(f, nullptr, true, true);
    for (const auto& c : doc["cases"]) {
        const std::string name = c["name"].get<std::string>();
        Model m;
        uint64_t next = 100;
        for (const auto& o : c["owners"]) {
            Owner w{next++, sources_of(o["sources"]), o["name"].get<std::string>(), {}, false, {}};
            for (const auto& t : o["take"]) {
                const std::string s = t.get<std::string>();
                if (s == "*") w.all = true;
                else w.take.push_back(static_cast<uint32_t>(std::stoul(s, nullptr, 16)));
            }
            m.owners.push_back(w);
        }
        bool steps_ok = true;
        std::string bad;
        for (const auto& s : c["steps"]) {
            const std::string op = s[0].get<std::string>();
            if (op == "down") m.down(parse_code(s[1].get<std::string>()));
            else if (op == "repeat") m.repeat(parse_code(s[1].get<std::string>()));
            else if (op == "up") m.up(parse_code(s[1].get<std::string>()));
            else if (op == "focus") {
                std::vector<Note> notes;
                m.table.end_all(kEndFocus, &notes);
                m.deliver(notes);
            } else if (op == "world") {
                std::vector<Note> notes;
                m.table.end_world(++m.gen, &notes);
                m.deliver(notes);
            } else if (op == "unsubscribe" || op == "disown") {
                const std::string who = s[1].get<std::string>();
                for (size_t i = 0; i < m.owners.size(); ++i)
                    if (m.owners[i].name == who) {
                        m.table.disown(m.owners[i].handle);
                        if (op == "unsubscribe") {
                            m.gone.push_back(m.owners[i]);
                            m.owners.erase(m.owners.begin() + static_cast<long>(i));
                        }
                        break;
                    }
            } else if (op == "leak") {
                const uint32_t id = parse_code(s[1].get<std::string>());
                m.table.leak(id);
                m.game.push_back("down " + code(id));
            } else if (op == "mk") {
                const uint32_t want = s[1].get<uint32_t>(), got = hidden_mk(m.table);
                if (want != got) {
                    steps_ok = false;
                    bad += " mk " + std::to_string(got) + " (want " + std::to_string(want) + ")";
                }
            } else if (op == "pad") {
                const uint32_t ctl = s[1].get<uint32_t>(), want = s[2].get<uint32_t>(), got = m.table.pad_owned(ctl);
                if (want != got) {
                    steps_ok = false;
                    bad += " pad " + std::to_string(ctl) + " " + std::to_string(got) + " (want " + std::to_string(want) + ")";
                }
            } else {
                steps_ok = false;
                bad += " unknown step " + op;
            }
        }
        check(steps_ok, "parity '" + name + "': the steps' checks" + bad);
        auto list = [&](const char* key) {
            std::vector<std::string> v;
            for (const auto& x : c[key]) v.push_back(x.get<std::string>());
            return v;
        };
        check(m.game == list("game"), "parity '" + name + "': the game gets " + join(m.game));
        check(m.plain == list("plain"), "parity '" + name + "': Kiln's other handlers get " + join(m.plain));
        std::vector<Owner> all = m.owners;
        all.insert(all.end(), m.gone.begin(), m.gone.end());
        for (const auto& o : all)
            check(o.log == list(o.name.c_str()), "parity '" + name + "': " + o.name + " gets " + join(o.log));
    }
}

}  // namespace

int main(int argc, char** argv) {
    // ---- the header's numbers are kiln_ui_input.h's ----
    check(kKeys == KILN_UI_OWN_KEYS && kMouse == KILN_UI_OWN_MOUSE && kPointer == KILN_UI_OWN_POINTER && kPad == KILN_UI_OWN_PAD &&
              kPress == KILN_UI_OWNED_PRESS && kRepeat == KILN_UI_OWNED_REPEAT && kRelease == KILN_UI_OWNED_RELEASE &&
              kEnd == KILN_UI_OWNED_END && kEndFocus == KILN_UI_OWNED_END_FOCUS && kEndWorld == KILN_UI_OWNED_END_WORLD &&
              kEndLost == KILN_UI_OWNED_END_LOST && kPadFirst == KILN_UI_VK_PAD_UP && kPadLast == KILN_UI_VK_PAD_RT,
          "input_own.h's sources, kinds, end reasons and pad codes are kiln_ui_input.h's");
    check(sizeof(KilnUiOwnedInput) == 64 && offsetof(KilnUiOwnedInput, controller) == 24 && offsetof(KilnUiOwnedInput, x) == 40 &&
              offsetof(KilnUiOwnedInput, text_input) == 56 && sizeof(KilnUiPadState) == 88 &&
              offsetof(KilnUiPadState, left_x) == 24 && offsetof(KilnUiPadState, polls) == 72,
          "KilnUiOwnedInput is 64 bytes, KilnUiPadState 88 (UiNative.OwnedInput / PadState mirror them)");
    check(source_of(0x01) == kPointer && source_of(0x02) == kPointer && source_of(0x04) == kMouse && source_of(0x06) == kMouse &&
              source_of(0x41) == kKeys && source_of(0x1B) == kKeys && source_of(0x100) == 0 && source_of(0x103) == 0 &&
              source_of(0x20D) == kPad && source_of(0x211) == kPad && source_of(0) == 0 && source_of(0x212) == 0,
          "source_of: left/right are the pointer, middle/side the mouse, a notch of the wheel nobody's");

    // ---- one claim, step by step ----
    {
        Table t;
        std::vector<Note> notes;
        const uint32_t rb = id_of(0x02, 0);
        check(!t.transition(rb, false, false, 1, &notes).handled && notes.empty(), "nothing claimed: a release is not ownership's");
        t.claim(rb, 7, 1);
        check(t.owner(rb) == 7 && t.owns(7, rb) && !t.owns(8, rb) && t.hidden(rb) && hidden_mk(t) == 2,
              "a claim: its owner owns it, the game is not told it is held (MK_RBUTTON hidden)");
        const auto r = t.transition(rb, false, false, 1, &notes);
        check(r.handled && r.keep && notes.size() == 1 && notes[0].sub == 7 && notes[0].kind == kRelease && t.empty(),
              "the release: to the owner alone, kept from the game, and the claim is gone");
    }
    // ---- the stuck-press guard ----
    {
        Table t;
        std::vector<Note> notes;
        const uint32_t a = id_of(0x41, 0);
        t.claim(a, 7, 1);
        t.leak(a);
        check(!t.hidden(a), "a press whose down reached the game is not hidden as held");
        const auto rep = t.transition(a, true, true, 1, &notes);
        check(rep.handled && !rep.keep, "its repeat reaches the game too (the game holds it already)");
        const auto up = t.transition(a, false, false, 1, &notes);
        check(up.handled && !up.keep && notes.size() == 2 && notes[1].kind == kRelease,
              "its release reaches the game (never left held) and the owner still hears it");
    }
    // ---- a focus loss, a world change, unload ----
    {
        Table t;
        std::vector<Note> notes;
        t.claim(id_of(0x02, 0), 7, 1);
        t.claim(id_of(0x41, 0), 8, 1);
        t.claim(id_of(0x20D, 2), 7, 1);
        t.end_all(kEndFocus, &notes);
        check(notes.size() == 3 && notes[0].id == id_of(0x02, 0) && notes[0].kind == kEnd && notes[0].end == kEndFocus &&
                  notes[1].sub == 8 && notes[2].id == id_of(0x20D, 2) && t.empty() && hidden_mk(t) == 0,
              "focus loss: every owner hears End Focus (in id order), every claim goes");
        notes.clear();
        t.claim(id_of(0x02, 0), 7, 1);
        t.end_world(1, &notes);
        check(notes.empty() && t.owner(id_of(0x02, 0)) == 7, "the same world: nothing ends");
        t.end_world(2, &notes);
        check(notes.size() == 1 && notes[0].end == kEndWorld && t.owner(id_of(0x02, 0)) == 0 && t.hidden(id_of(0x02, 0)),
              "a world change: End World, the claim an orphan, still hidden from the game");
        t.end_world(3, &notes);
        check(notes.size() == 1, "an orphan hears no second End");
        notes.clear();
        const auto up = t.transition(id_of(0x02, 0), false, false, 3, &notes);
        check(up.handled && up.keep && notes.empty() && t.empty(), "an orphan's release: kept from the game, to nobody");
        // a claim from an older world, routed before the world poll saw the change
        t.claim(id_of(0x41, 0), 9, 3);
        const auto late = t.transition(id_of(0x41, 0), true, true, 4, &notes);
        check(late.handled && late.keep && notes.size() == 1 && notes[0].end == kEndWorld,
              "a repeat in a newer world than its claim: End World first, then the repeat goes to nobody");
        notes.clear();
        t.claim(id_of(0x05, 0), 9, 4);
        t.claim(id_of(0x06, 0), 9, 4);
        t.claim(id_of(0x04, 0), 10, 4);
        check(t.disown(9) == 2 && t.owner(id_of(0x05, 0)) == 0 && t.owner(id_of(0x04, 0)) == 10 && hidden_mk(t) == 0x70,
              "unsubscribe: its claims are orphans, without a callback, still hidden (MK_XBUTTON1/2, MK_MBUTTON)");
        check(t.disown(10, id_of(0x04, 0)) == 1 && t.owner(id_of(0x04, 0)) == 0, "disown of one id");
    }
    // ---- threads: the window thread and two polling threads share the table ----
    {
        Table t;
        std::atomic<int> handled{0};
        auto worker = [&](uint32_t controller) {
            for (int i = 0; i < 20000; ++i) {
                const uint32_t id = id_of(0x20C + (i & 1), controller);
                t.claim(id, controller + 1, 1);
                std::vector<Note> notes;
                if (t.transition(id, false, false, 1, &notes).handled) handled.fetch_add(1, std::memory_order_relaxed);
                (void)t.pad_owned(controller);
                (void)hidden_mk(t);
            }
        };
        std::thread a(worker, 0), b(worker, 1), c(worker, 2);
        a.join();
        b.join();
        c.join();
        check(handled.load() == 60000 && t.empty(), "three threads claim and release their own controllers' buttons: all handled, none left");
    }

    // ---- pad_state: the game's last poll, normalized (pad_view.h) ----
    {
        kui::pad::View v;
        KilnUiPadState out{};
        v.fill(2, 0, &out);
        check(out.size == sizeof out && out.controller == 2 && !out.connected && out.buttons == 0 && out.polls == 0 && out.poll_ms == 0,
              "pad_state before any poll: not connected, nothing held");
        kiln::ui::PadState s{};
        s.buttons = 0x2000 | 0x0100;  // B and LB
        s.lt = 29;                    // under the threshold: not pulled
        s.rt = 255;
        s.lx = 7000;                  // inside the left dead zone
        s.ly = -7000;                 // radially outside it (9899 > 7849)
        s.rx = 32767;
        s.ry = 0;
        v.poll(s, 77, 0x2000, 1234);
        v.fill(2, 0x0100, &out);
        const float r = std::sqrt(out.left_x * out.left_x + out.left_y * out.left_y);
        check(out.connected && out.buttons == (0x2000u | 0x0100u | (1u << 17)) && out.owned == 0x0100 && out.hidden == 0x2100 &&
                  out.packet == 77 && out.poll_ms == 1234 && out.polls == 1,
              "pad_state: buttons as KILN_UI_VK_PAD_* bits (RT from the pull), owned and hidden, the poll's packet and time");
        check(out.left_x > 0 && out.left_y < 0 && std::fabs(out.left_x + out.left_y) < 1e-5f && r > 0.05f && r < 0.15f,
              "pad_state: a stick past the radial dead zone keeps its direction, scaled from the zone's edge");
        check(out.right_x == 1.0f && out.right_y == 0.0f && out.left_trigger == 0.0f && out.right_trigger == 1.0f &&
                  out.raw_left_x == 7000 && out.raw_right_trigger == 255,
              "pad_state: full tilt is 1, a trigger under XInput's threshold is 0 and full is 1; the raw values are XInput's");
        check(kui::pad::trigger(30) > 0.0f && kui::pad::trigger(29) == 0.0f && kui::pad::held(0, 30, 0) == (1u << 16),
              "pad_state: a trigger counts as pulled (above 0) from 30, as pad_keys.h presses it");
        kiln::ui::PadState in_zone{};
        in_zone.lx = 7000;
        in_zone.ly = 3000;
        in_zone.rx = -8000;
        v.poll(in_zone, 78, 0, 1300);
        v.fill(2, 0, &out);
        check(out.left_x == 0 && out.left_y == 0 && out.right_x == 0 && out.polls == 2, "pad_state: inside the dead zones: 0");
        v.lost(1400);
        v.fill(2, 0, &out);
        check(!out.connected && out.buttons == 0 && out.hidden == 0 && out.polls == 3 && out.poll_ms == 1400,
              "pad_state: a failed poll (unplugged): not connected, nothing held");
    }

    parity(argc > 1 ? argv[1] : "own_parity.json");
    std::printf("input_own: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
