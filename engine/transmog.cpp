// SPDX-License-Identifier: GPL-3.0-or-later
// Transmog (transmog.h). The armor list, its names and the row layout come from the player's own game
// files (the parambnd / paramdefbnd / item.msgbnd the game reads, mods included), so nothing about the
// params is typed in here; the look is written into the game's live EquipParamProtector rows.
#include "transmog.h"

#include "bbport_settings.h"
#include "imgui.h"
#include "../gamedata/container.h"
#include "../gamedata/fmg.h"
#include "../gamedata/param.h"
#include "../party/game_state.h"
#include "../party/seamless_rules.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" int runtime_file_translate(const char*, char*, std::size_t);

namespace BbEngine::Transmog {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr const char* kParam = "EquipParamProtector";
constexpr const char* kType = "EQUIP_PARAM_PROTECTOR_ST";
constexpr std::int32_t kHidden = -1;   // look: no armor model
constexpr int kSlots = 4;              // head, chest, arms, legs = equipment slots 8..11
constexpr int kFirstEquipSlot = 8;
const char* const kSlotNames[kSlots] = {"Head", "Chest", "Arms", "Legs"};

struct Piece {
    std::int32_t id = 0;
    int slot = -1;
    std::string name;
};

// Static data (loaded once on a worker thread; read-only afterwards).
struct Data {
    std::vector<bbdata::ParamField> look;    // fields copied for the look
    std::vector<bbdata::ParamField> sound;   // fields copied when sounds follow the look
    std::vector<Piece> pieces[kSlots];       // named pieces per slot, menu order
    std::unordered_map<std::int32_t, const Piece*> by_id;
    std::int32_t bare[kSlots] = {0, 0, 0, 0};  // the "no armor" row per slot (hidden look), 0: unknown
    std::string error;
};

std::atomic<int> g_load{0};  // 0 not started, 1 loading, 2 ready, 3 failed
Data g_data;

// The player's choices: item id -> look id (kHidden: no model). Shared between the menu and the tick.
std::mutex g_mu;
std::map<std::int32_t, std::int32_t> g_looks;
bool g_sounds = true;
bool g_dirty = true;                       // apply now (a menu change / first start)
std::atomic<std::uint32_t> g_wearing[kSlots]{};  // equipped piece per slot (tick -> menu)
std::atomic<bool> g_have_player{false};

// Original field values of every row we wrote, so "its own look" can be put back.
std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> g_original;  // key: id << 1 | sounds-set

const char* ConfigPath() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

fs::path IniPath() {
    fs::path p(ConfigPath());
    return p.parent_path() / "transmog.ini";
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::printf("Transmog: %s\n", buf);
    std::fflush(stdout);
}

void LoadIni() {
    std::ifstream f(IniPath());
    std::string line;
    std::lock_guard<std::mutex> lk(g_mu);
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "sounds") {
            g_sounds = std::atoi(v.c_str()) != 0;
            continue;
        }
        char* end = nullptr;
        const long id = std::strtol(k.c_str(), &end, 10);
        if (end == k.c_str() || id <= 0) continue;
        g_looks[std::int32_t(id)] = std::int32_t(std::strtol(v.c_str(), nullptr, 10));
    }
    Log("%zu saved look(s) from %s", g_looks.size(), IniPath().string().c_str());
}

void SaveIni() {  // g_mu held
    std::ofstream f(IniPath(), std::ios::trunc);
    f << "; Transmog: <armor id>=<look id> (-1: hidden). Managed by the guide's Transmog tab.\n";
    f << "sounds=" << (g_sounds ? 1 : 0) << "\n";
    for (const auto& [id, look] : g_looks) f << id << "=" << look << "\n";
}

std::string GameFile(const char* rel) {
    char resolved[1024]{};
    const std::string guest = std::string("/app0/dvdroot_ps4/") + rel;
    if (!runtime_file_translate(guest.c_str(), resolved, sizeof resolved) && resolved[0]) return resolved;
    const char* game = std::getenv("BB_GAME_DIR");
    if (!game) return "";
    std::error_code ec;
    const fs::path a = fs::path(game) / "dvdroot_ps4" / rel;
    return fs::exists(a, ec) ? a.string() : (fs::path(game) / rel).string();
}

bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

void LoadData() {
    Data d;
    try {
        const std::string pb_path = GameFile("param/gameparam/gameparam.parambnd.dcx");
        const std::string db_path = GameFile("paramdef/paramdef.paramdefbnd.dcx");
        if (pb_path.empty() || db_path.empty()) throw bbdata::FormatError("game folder unknown (BB_GAME_DIR)");
        const bbdata::Bytes pb = bbdata::ReadMaybeDcx(pb_path), db = bbdata::ReadMaybeDcx(db_path);
        const auto pe = bbdata::ParseBnd4(pb), de = bbdata::ParseBnd4(db);
        const bbdata::BndEntry* p = bbdata::FindBndEntry(pe, std::string(kParam) + ".param");
        const bbdata::BndEntry* def = bbdata::FindBndEntry(de, std::string(kParam) + ".paramdef");
        if (!def) def = bbdata::FindBndEntry(de, std::string(kType) + ".paramdef");
        if (!p || !def) throw bbdata::FormatError("EquipParamProtector or its paramdef not found");
        const bbdata::Span ps = bbdata::Span(pb).Sub(std::int64_t(p->offset), p->size);
        const bbdata::Span ds = bbdata::Span(db).Sub(std::int64_t(def->offset), def->size);
        const bbdata::ParamTable t = bbdata::ParseParam(ps, ds);
        const auto layout = bbdata::ParamLayout(ds);
        std::string look_names, sound_names;
        for (const auto& f : layout) {
            if (f.type == "raw" || f.type == "f32") continue;
            const std::string& n = f.name;
            if (StartsWith(n, "equipModel") || StartsWith(n, "invisibleFlag") || n == "useFaceScale" ||
                StartsWith(n, "faceScale")) {
                d.look.push_back(f);
                if (!StartsWith(n, "invisibleFlag")) look_names += n + " ";
            } else if (StartsWith(n, "defenseMaterial") || StartsWith(n, "partsDmgType")) {
                d.sound.push_back(f);
                sound_names += n + " ";
            }
        }
        if (d.look.empty()) throw bbdata::FormatError("no equipModel* fields in the paramdef");
        Log("look fields: %s+ %zu invisibleFlag(s); sound fields: %s", look_names.c_str(),
            std::count_if(d.look.begin(), d.look.end(), [](const auto& f) { return StartsWith(f.name, "invisibleFlag"); }),
            sound_names.c_str());

        auto names = bbdata::ReadItemFmg(fs::path(GameFile("msg")).parent_path().string(), "防具名");
        if (names.empty()) {
            const char* game = std::getenv("BB_GAME_DIR");
            if (game) names = bbdata::ReadItemFmg((fs::path(game) / "dvdroot_ps4").string(), "防具名");
        }
        Log("%zu armor names", names.size());
        const int c_head = t.Col("headEquip"), c_body = t.Col("bodyEquip"), c_arm = t.Col("armEquip"), c_leg = t.Col("legEquip");
        const int c_sort = t.Col("sortId");
        for (std::size_t r = 0; r < t.Rows(); ++r) {
            const std::int32_t id = t.ids[r];
            int slot = -1;
            if (c_head >= 0 && t.At(r, c_head)) slot = 0;
            else if (c_body >= 0 && t.At(r, c_body)) slot = 1;
            else if (c_arm >= 0 && t.At(r, c_arm)) slot = 2;
            else if (c_leg >= 0 && t.At(r, c_leg)) slot = 3;
            else if (c_head < 0) slot = (id / 1000) % 10 <= 3 ? (id / 1000) % 10 : -1;
            if (slot < 0) continue;
            if (id >= 900000 && id <= 903000 && id % 1000 == 0 && d.bare[slot] == 0) d.bare[slot] = id;
            const auto it = names.find(id);
            if (it == names.end() || it->second.empty() || it->second == "?" || it->second.find("ERROR") != std::string::npos)
                continue;
            (void)c_sort;
            d.pieces[slot].push_back({id, slot, it->second});
        }
        for (int s = 0; s < kSlots; ++s) {
            std::stable_sort(d.pieces[s].begin(), d.pieces[s].end(), [](const Piece& a, const Piece& b) { return a.name < b.name; });
            Log("%s: %zu pieces, hidden look from row %d%s", kSlotNames[s], d.pieces[s].size(), d.bare[s],
                d.bare[s] ? "" : " (none found: hidden clears the model id)");
        }
    } catch (const std::exception& e) {
        d.error = e.what();
    }
    g_data = std::move(d);
    for (int s = 0; s < kSlots; ++s)
        for (const Piece& p : g_data.pieces[s]) g_data.by_id[p.id] = &p;
    if (!g_data.error.empty()) Log("not available: %s", g_data.error.c_str());
    g_load = g_data.error.empty() ? 2 : 3;
}

// ---- field access on a live row ----
std::uint64_t ReadField(std::uint64_t row, const bbdata::ParamField& f, bool* ok) {
    const int size = f.type == "u8" || f.type == "s8" ? 1 : f.type == "u16" || f.type == "s16" ? 2 : 4;
    std::uint64_t raw = 0;
    *ok = coop::SafeRead(row + std::uint64_t(f.off), &raw, std::size_t(size));
    if (f.bit < 0) return raw;
    return (raw >> f.bit) & ((std::uint64_t{1} << f.width) - 1);
}

bool WriteField(std::uint64_t row, const bbdata::ParamField& f, std::uint64_t v) {
    const int size = f.type == "u8" || f.type == "s8" ? 1 : f.type == "u16" || f.type == "s16" ? 2 : 4;
    std::uint64_t raw = 0;
    if (!coop::SafeRead(row + std::uint64_t(f.off), &raw, std::size_t(size))) return false;
    std::uint64_t nv = v;
    if (f.bit >= 0) {
        const std::uint64_t mask = ((std::uint64_t{1} << f.width) - 1) << f.bit;
        nv = (raw & ~mask) | ((v << f.bit) & mask);
    }
    if (nv == raw) return true;
    return coop::SafeWrite(row + std::uint64_t(f.off), &nv, std::size_t(size));
}

std::vector<const bbdata::ParamField*> Fields(bool sounds) {
    std::vector<const bbdata::ParamField*> v;
    for (const auto& f : g_data.look) v.push_back(&f);
    if (sounds)
        for (const auto& f : g_data.sound) v.push_back(&f);
    return v;
}

/// The values the row of `id` has before any transmog (read from the live row the first time).
const std::vector<std::uint64_t>* Original(std::int32_t id) {
    const std::uint64_t key = std::uint64_t(std::uint32_t(id));
    const auto it = g_original.find(key);
    if (it != g_original.end()) return &it->second;
    const std::uint64_t row = coop::GameParamRow(kParam, kType, std::uint32_t(id));
    if (!row) return nullptr;
    std::vector<std::uint64_t> vals;
    for (const auto* f : Fields(true)) {
        bool ok = false;
        vals.push_back(ReadField(row, *f, &ok));
        if (!ok) return nullptr;
    }
    return &(g_original[key] = std::move(vals));
}

/// Writes the wanted look of item `id` into its live row; true when a byte changed.
bool ApplyOne(std::int32_t id, std::int32_t look, bool sounds) {
    const std::uint64_t row = coop::GameParamRow(kParam, kType, std::uint32_t(id));
    if (!row) return false;
    const auto all = Fields(true);
    const std::vector<std::uint64_t>* own = Original(id);
    if (!own) return false;
    // Source values: the look piece's original values, the bare row's for hidden, the own ones for "its own look".
    std::vector<std::uint64_t> want = *own;
    if (look != 0 && look != id) {
        const std::int32_t src_id = look == kHidden ? 0 : look;
        int slot = -1;
        if (const auto it = g_data.by_id.find(id); it != g_data.by_id.end()) slot = it->second->slot;
        const std::int32_t from = src_id ? src_id : (slot >= 0 ? g_data.bare[slot] : 0);
        const std::vector<std::uint64_t>* src = from ? Original(from) : nullptr;
        for (std::size_t i = 0; i < all.size(); ++i) {
            const bool is_sound = i >= g_data.look.size();
            if (is_sound && !sounds) continue;
            if (src) {
                want[i] = (*src)[i];
            } else if (StartsWith(all[i]->name, "equipModelId")) {
                want[i] = 0;  // hidden without a bare row: no model
            }
        }
    }
    bool changed = false;
    for (std::size_t i = 0; i < all.size(); ++i) {
        bool ok = false;
        const std::uint64_t cur = ReadField(row, *all[i], &ok);
        if (ok && cur != want[i]) {
            changed |= WriteField(row, *all[i], want[i]);
        }
    }
    return changed;
}

Clock::time_point g_last_apply{};
struct Refresh {
    std::uint32_t target = 0, other = 0;
    int phase = 0;
    Clock::time_point at{};
    int tries = 0;
    bool gave = false;  // the bare piece was given for the swap: take it away afterwards
    Refresh() = default;
    Refresh(std::uint32_t t, std::uint32_t o, int p, Clock::time_point a) : target(t), other(o), phase(p), at(a) {}
};
Refresh g_refresh[kSlots];
std::map<std::int32_t, std::int32_t> g_applied;  // what the rows hold now (to put back removed looks)

}  // namespace

void RefreshTick();

void Tick() {
    if (g_load == 0) {
        g_load = 1;
        LoadIni();
        std::thread(LoadData).detach();
        return;
    }
    if (g_load != 2) return;
    const coop::GameSnapshot s = coop::ReadGameState(false);
    g_have_player = s.world_up;
    if (!s.world_up || s.loading) return;
    RefreshTick();
    for (int k = 0; k < kSlots; ++k) {
        // While a swap is under way the slot holds the other piece: show (and save to) the real one.
        g_wearing[k] = g_refresh[k].target ? g_refresh[k].target : coop::EquippedSlotId(kFirstEquipSlot + k);
    }

    const auto now = Clock::now();
    bool dirty;
    std::map<std::int32_t, std::int32_t> looks;
    bool sounds;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        dirty = g_dirty;
        g_dirty = false;
        looks = g_looks;
        sounds = g_sounds;
    }
    // A reload puts the params back: check every second, at once after a menu change.
    if (!dirty && now - g_last_apply < std::chrono::seconds(1)) return;
    g_last_apply = now;

    // Every row's own values are read before anything is written (a piece can be another's look).
    for (const auto& [id, look] : looks) {
        Original(id);
        if (look > 0) Original(look);
    }
    for (int k = 0; k < kSlots; ++k)
        if (g_data.bare[k]) Original(g_data.bare[k]);
    std::vector<std::int32_t> changed;
    // Looks removed from the list: put the row back once.
    for (auto it = g_applied.begin(); it != g_applied.end();) {
        if (!looks.count(it->first)) {
            if (ApplyOne(it->first, 0, true)) changed.push_back(it->first);
            it = g_applied.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [id, look] : looks) {
        if (ApplyOne(id, look, sounds)) changed.push_back(id);
        g_applied[id] = look;
    }
    if (changed.empty()) return;
    // An equipped piece changed: the game rebuilds the model only when the slot's item changes, so
    // another owned piece of the slot goes on for a moment, then the piece again (RefreshTick).
    for (int k = 0; k < kSlots; ++k) {
        const std::uint32_t w = g_wearing[k];
        if (!w || std::find(changed.begin(), changed.end(), std::int32_t(w)) == changed.end()) continue;
        g_refresh[k] = {w, 0, 0, now};
    }
}

/// Takes away the bare armor piece given for a swap (an unequipped inventory entry of it).
void RemoveGiven(std::uint32_t id) {
    for (const coop::InventoryEntry& e : coop::InventoryEntries()) {
        if (e.item != (coop::kItemProtector | id) || coop::HandleEquipped(e.handle)) continue;
        const bool ok = coop::RemoveInventoryIndex(e.index);
        Log("naked piece %u taken back out of the inventory: %s", id, ok ? "ok" : "FAILED");
        return;
    }
}

void RefreshTick() {
    const auto now = Clock::now();
    for (int k = 0; k < kSlots; ++k) {
        Refresh& r = g_refresh[k];
        if (!r.target) continue;
        const int slot = kFirstEquipSlot + k;
        if (r.phase == 0) {
            // Naked first: the slot's bare row (900000 + slot * 1000) goes on for a moment. When the
            // player holds none, EquipSlotItem gives one; it is taken away again once the piece is back.
            const std::uint32_t bare = std::uint32_t(g_data.bare[k]);
            std::uint32_t via = 0;
            if (bare) {
                const bool held = coop::ItemCount(coop::kItemProtector, bare) > 0;
                if (coop::EquipSlotItem(slot, coop::kItemProtector, bare) && coop::EquippedSlotId(slot) == bare) {
                    via = bare;
                    r.gave = !held;
                } else {
                    Log("%s %u: could not switch to naked (row %u); trying another piece", kSlotNames[k], r.target, bare);
                    if (!held) RemoveGiven(bare);
                }
            }
            if (!via) {
                // Fallback: another piece of this slot in the inventory.
                for (const coop::InventoryEntry& e : coop::InventoryEntries()) {
                    if ((e.item & 0xf0000000u) != coop::kItemProtector) continue;
                    const std::int32_t id = std::int32_t(e.item & 0x0fffffffu);
                    if (std::uint32_t(id) == r.target) continue;
                    const auto it = g_data.by_id.find(id);
                    if (it != g_data.by_id.end() && it->second->slot == k) { via = std::uint32_t(id); break; }
                }
                if (!via || !coop::EquipSlotItem(slot, coop::kItemProtector, via)) {
                    const bool ok = coop::EquipSlotItem(slot, coop::kItemProtector, r.target);
                    Log("%s %u: look applied; no way to swap the model (refresh %s; take it off and on)", kSlotNames[k],
                        r.target, ok ? "tried" : "FAILED");
                    r = {};
                    continue;
                }
            }
            r.other = via;
            r.phase = 1;
            r.at = now;
        } else if (r.phase == 1 && now - r.at >= std::chrono::milliseconds(120)) {
            const bool ok = coop::EquipSlotItem(slot, coop::kItemProtector, r.target) &&
                            coop::EquippedSlotId(slot) == r.target;
            Log("%s %u: look applied, model refreshed (via %u%s) %s", kSlotNames[k], r.target, r.other,
                r.other == std::uint32_t(g_data.bare[k]) ? ", naked" : "", ok ? "ok" : "FAILED");
            if (!ok) {
                // Try once more next tick rather than leaving the swap on.
                if (++r.tries < 5) { r.at = now; continue; }
            }
            if (r.gave) RemoveGiven(r.other);
            r = {};
        }
    }
}

// ---- the menu page ----
namespace {
const char* PieceName(std::int32_t id) {
    if (id == kHidden) return "(hidden)";
    if (id == 0) return "(its own look)";
    const auto it = g_data.by_id.find(id);
    static thread_local char buf[32];
    if (it == g_data.by_id.end()) {
        std::snprintf(buf, sizeof buf, "#%d", id);
        return buf;
    }
    return it->second->name.c_str();
}

bool Matches(const std::string& name, const char* filter) {
    if (!filter[0]) return true;
    std::string a = name, b = filter;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    return a.find(b) != std::string::npos;
}

void SetLook(std::int32_t item, std::int32_t look) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (look == 0 || look == item) g_looks.erase(item);
    else g_looks[item] = look;
    g_dirty = true;
    SaveIni();
}
}  // namespace

void DrawPage() {
    const int state = g_load.load();
    if (state < 2) {
        ImGui::TextDisabled("Reading the armor list from the game files...");
        return;
    }
    if (state == 3) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Transmog is not available: %s", g_data.error.c_str());
        return;
    }
    if (!g_have_player) {
        ImGui::TextDisabled("Load a character to change what your equipped armor looks like.");
    }
    ImGui::TextWrapped("Pick a look for each equipped piece. It keeps its own stats. "
                       "\"(its own look)\" removes the transmog, \"(hidden)\" shows no armor.");
    {
        std::lock_guard<std::mutex> lk(g_mu);
        bool snd = g_sounds;
        if (ImGui::Checkbox("Sounds follow the new look", &snd)) {
            g_sounds = snd;
            g_dirty = true;
            SaveIni();
        }
    }
    ImGui::Separator();
    static char filter[kSlots][64] = {};
    for (int k = 0; k < kSlots; ++k) {
        ImGui::PushID(k);
        const std::int32_t wearing = std::int32_t(g_wearing[k].load());
        const bool real = wearing && g_data.by_id.count(wearing);
        std::int32_t look = 0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (const auto it = g_looks.find(wearing); it != g_looks.end()) look = it->second;
        }
        ImGui::SeparatorText(kSlotNames[k]);
        if (!real) {
            ImGui::TextDisabled("Nothing equipped");
            ImGui::PopID();
            continue;
        }
        ImGui::Text("Wearing: %s", PieceName(wearing));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##look", PieceName(look), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::IsWindowAppearing()) {
                ImGui::SetKeyboardFocusHere();
                filter[k][0] = 0;
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##filter", "Type to search", filter[k], sizeof filter[k]);
            if (ImGui::Selectable("(its own look)", look == 0)) SetLook(wearing, 0);
            if (ImGui::Selectable("(hidden)", look == kHidden)) SetLook(wearing, kHidden);
            for (const Piece& p : g_data.pieces[k]) {
                if (!Matches(p.name, filter[k][0] ? filter[k] : "")) continue;
                ImGui::PushID(p.id);
                if (ImGui::Selectable(p.name.c_str(), look == p.id)) SetLook(wearing, p.id);
                if (look == p.id) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    std::map<std::int32_t, std::int32_t> looks;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        looks = g_looks;
    }
    if (ImGui::TreeNode("Saved transmogs")) {
        if (looks.empty()) ImGui::TextDisabled("No saved transmogs.");
        for (const auto& [id, look] : looks) {
            ImGui::PushID(id);
            if (ImGui::SmallButton("Remove")) SetLook(id, 0);
            ImGui::SameLine();
            ImGui::Text("%s  ->  %s", PieceName(id), PieceName(look));
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::TextDisabled("A transmog applies to the item: every copy of it looks the same.");
}

}  // namespace BbEngine::Transmog
