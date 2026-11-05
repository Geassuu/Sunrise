/**
 * Hide-player: skips the local player's skinned draws while photo mode is on. The detour wraps
 * the skinned pose producer the Sunrise Studio link also observes, one call per skinned draw.
 * A draw belongs to the player when its owner is the player's object, or any object whose
 * parent chain reaches the player: the first-person weapon and the class-item cloth hang off
 * child objects the game creates under the player's entity. Such draws are answered with zero,
 * which the caller reads as "nothing was produced for this draw" and drops from the frame.
 * If a future build changes that contract, the Photo page's hide-player toggle turns the skip
 * off without a rebuild.
 */

#include "photo.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

#include "../../../core/logging/log.h"
#include "../../content/handles/handle_resolver.h"
#include "../../hooking/detour.h"
#include "../../memory/current_process_memory.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/signature_text.h"
#include "internal.h"

namespace sunrise::client::hooks::photo {

/** Distinct model handles of the player's skinned draws, collected for the read-only probe. */
std::array<std::atomic<std::uint32_t>, 16> g_playerModels{};

namespace {

/** The producer pattern the Studio pose link resolves; it matches one function per build. */
constexpr std::string_view kProducerText =
    "4C 8B DC 55 41 54 41 56 49 8D AB C8 FC FF FF 48 81 EC 20 04 00 00 "
    "48 8B 05 ? ? ? ? 48 33 C4 48 89 85 B0 02 00 00 0F 10 41 18";
/** Compiled pattern bytes of the producer signature. */
constexpr auto kProducer =
    patterns::signature<patterns::signature_length(kProducerText)>(kProducerText);
/** The producer's first handle decode, ending in its load of the handle-table slot. */
constexpr std::string_view kTablesLoadText =
    "8B 54 24 68 33 F6 83 FA FF 0F 84 ? ? ? ? 48 8B 5C 24 70 8B C2 C1 F8 0D 81 E2 FF 1F 00 00 "
    "8B C8 48 81 C9 00 00 FC 0F 0F B7 C0 48 C1 E9 12 48 23 C8 48 8B 05 ? ? ? ?";
/** Compiled pattern bytes of the tables-load signature. */
constexpr auto kTablesLoad =
    patterns::signature<patterns::signature_length(kTablesLoadText)>(kTablesLoadText);
/** The tables load's operand occupies bytes 53..56. */
constexpr std::size_t kTablesOperand = 53, kTablesNext = 57;

/**
 * The static part-record processor's entry: the function reads the record's own draw-enable
 * byte at +0x22 and the entity tables, the same submission path the sword shell and the
 * weapon's static parts take. Read-only probe until the owner field is decoded.
 */
constexpr std::string_view kStaticRecordText =
    "55 56 57 41 54 41 56 48 81 EC E0 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 D0 00 00 00 "
    "48 8B 69 10 33 C0 0F 10 41 22";
constexpr auto kStaticRecord =
    patterns::signature<patterns::signature_length(kStaticRecordText)>(kStaticRecordText);

/**
 * The sibling static processor: the same family, but it reads the record's +0x22 flag first
 * and takes the record link from +0x10. One of the two runs per static part.
 */
constexpr std::string_view kStaticRecord2Text =
    "55 56 57 41 54 41 55 48 81 EC 80 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 70 "
    "0F 10 41 22 48 8B 51 10";
constexpr auto kStaticRecord2 =
    patterns::signature<patterns::signature_length(kStaticRecord2Text)>(kStaticRecord2Text);

/** The draw record's owner handle sits four bytes in, the same read the Studio observer makes. */
constexpr std::size_t kDrawOwnerOffset = sizeof(std::uint32_t);

/** Object memory offsets the Studio datum readers also use. */
constexpr std::size_t kObjectSelfOffset = 0x0C;
constexpr std::size_t kObjectParentOffset = 60;
/** The datum head that carries the parent link; a shorter stride cannot store it. */
constexpr std::uint32_t kDatumLinkBytes = 64;
/** Handle slot bits, the same mask the Studio datum reader applies. */
constexpr std::uint32_t kSlotMask = 0x1FFFU;
/** A parent chain longer than this is treated as unreadable; it guards against a cycle. */
constexpr std::size_t kMaximumAncestors = 8;

/** Runtime table rows link components and store their body displacement. Studio's own layout. */
struct RuntimeTable final {
    std::uintptr_t unused{}, rows{}, pool{};
    std::array<std::byte, 24> reserved{};
    std::uint32_t stride{};
    std::int32_t correctionMask{};
    std::uint64_t tail{};
};
/** One runtime component entry, as the Studio pose capture reads it. */
struct ComponentEntry final {
    std::uint64_t prefix{}, correction{};
    std::uint32_t opaque16{}, next{};
};
/** The head of one component record; its owner links the component to its object. */
struct ComponentPrefix final {
    std::array<std::byte, 36> prefix{};
    std::uint32_t self{}, reserved{}, owner{};
};
static_assert(sizeof(RuntimeTable) == 64);
static_assert(offsetof(RuntimeTable, stride) == 48);
static_assert(sizeof(ComponentEntry) == 24);
static_assert(offsetof(ComponentPrefix, owner) == 44);
/** Handle table selection uses ten bits, extended to fourteen by the sign bit. */
constexpr std::uint32_t kHandleSignBit = 0x80000000U, kRuntimeTableMask = 0x3FFU,
                       kExtendedTableMask = 0x3FFF;
/** The sign bit's selector shift, the same decode the Studio capture uses. */
constexpr std::uint32_t kHandleIndexShift = 13;

using Producer = char(__fastcall*)(std::uintptr_t, void*, std::uintptr_t, void*, void*, int);
/** The static part-record processor: the record, then three link arguments. */
using StaticRecordProcessor =
    void(__fastcall*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

std::atomic_bool g_installed{false};
hooking::detour::Handle g_hideHandle{};
hooking::detour::Handle g_staticRecordHandle{};
hooking::detour::Handle g_staticRecord2Handle{};
/** The handle-table storage slot the tables-load decodes; zero while unresolved. */
std::atomic<std::uintptr_t> g_tablesSlot{0};

/** What a draw owner matched, in increasing distance from the player. */
enum class Match : std::uint32_t {
    none = 0,
    player = 1,
    parent = 2,
    component = 3,
    carrier = 4,
};

/**
 * Reads one scalar from this process under a fault guard.
 * @param address Source address.
 * @param output Receives the value.
 * @return True when every byte arrived.
 */
template <typename Value> [[nodiscard]] bool read(std::uintptr_t address, Value& output) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    return memory::copy_current_process(
        nullptr, address, std::as_writable_bytes(std::span(&output, 1)));
}

/** @return The object pool base and stride values, or false while they cannot be read. */
[[nodiscard]] bool pool(std::uintptr_t& base, std::uint32_t& stride) noexcept {
    const std::uintptr_t baseSlot = g_poolBaseSlot.load(std::memory_order_relaxed);
    const std::uintptr_t strideSlot = g_poolStrideSlot.load(std::memory_order_relaxed);
    if (baseSlot == 0 || strideSlot == 0) {
        return false;
    }
    // The slots hold the game's current pool values; reading them per call keeps a teardown from
    // publishing a stale base through the cached slot addresses.
    return read(baseSlot, base) && base != 0 && read(strideSlot, stride)
           && stride >= kDatumLinkBytes;
}

/** @return The parent handle of one object, or kInvalidHandle when it cannot be read. */
[[nodiscard]] std::uint32_t parent_of(std::uint32_t handle, std::uintptr_t base,
                                      std::uint32_t stride) noexcept {
    const std::uint64_t offset = static_cast<std::uint64_t>(stride) * (handle & kSlotMask);
    const std::uintptr_t object = base + static_cast<std::uintptr_t>(offset);
    std::uint32_t self = kInvalidHandle;
    std::uint32_t parent = kInvalidHandle;
    if (!read(object + kObjectSelfOffset, self) || self != handle
        || !read(object + kObjectParentOffset, parent)) {
        return kInvalidHandle;
    }
    return parent;
}

/**
 * Resolves the object a component belongs to through the runtime component tables.
 * The class-item cloth can keep its component on the player's entity while its own datum hangs
 * off elsewhere, so the component owner is the one link the parent chain misses.
 * @param component Component handle from a draw record.
 * @return The owning object handle, or kInvalidHandle when the tables cannot be read.
 */
[[nodiscard]] std::uint32_t component_owner(std::uint32_t component) noexcept {
    if (component == kInvalidHandle) {
        return kInvalidHandle;
    }
    const std::uintptr_t slot = g_tablesSlot.load(std::memory_order_relaxed);
    if (slot == 0) {
        return kInvalidHandle;
    }
    std::uintptr_t object = 0;
    std::uintptr_t tables = 0;
    if (!read(slot, object) || !read(object, tables) || tables == 0) {
        return kInvalidHandle;
    }
    const auto index =
        (component >> kHandleIndexShift)
        & ((component & kHandleSignBit) != 0 ? kExtendedTableMask : kRuntimeTableMask);
    if (tables > (std::numeric_limits<std::uintptr_t>::max)() - sizeof(RuntimeTable) * index) {
        return kInvalidHandle;
    }
    RuntimeTable table{};
    if (!read(tables + sizeof(RuntimeTable) * index, table) || table.rows == 0
        || table.stride < sizeof(ComponentEntry)) {
        return kInvalidHandle;
    }
    const auto offset = std::uint64_t(table.stride) * (component & kSlotMask);
    if (offset > (std::numeric_limits<std::uint32_t>::max)()
        || table.rows > (std::numeric_limits<std::uintptr_t>::max)() - offset) {
        return kInvalidHandle;
    }
    const std::uintptr_t entry = table.rows + static_cast<std::uintptr_t>(offset);
    ComponentEntry node{};
    if (!read(entry, node)) {
        return kInvalidHandle;
    }
    const auto mask =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(table.correctionMask));
    const std::uintptr_t base = entry - static_cast<std::uintptr_t>(node.correction & mask);
    ComponentPrefix prefix{};
    if (!read(base, prefix) || prefix.self != component) {
        return kInvalidHandle;
    }
    return prefix.owner;
}

/**
 * Tests one draw owner against the local player and the camera carrier. The match set is the
 * player object and its parent: the first-person weapon hangs off the player's chain, while
 * entity-level attachments like the class-item cloth can hang off the player's parent as a
 * sibling object. The carrier matches on its own object and its children.
 * @param owner Draw owner handle to test.
 * @return What the owner matched, or Match::none.
 */
[[nodiscard]] Match match_kind(std::uint32_t owner) noexcept {
    if (owns_player_object(owner)) {
        return Match::player;
    }
    const std::uint32_t carrier = g_carrierHandle.load(std::memory_order_relaxed);
    if (carrier != 0 && owner == carrier) {
        return Match::carrier;
    }
    const std::uint32_t player = g_playerHandle.load(std::memory_order_acquire);
    if (player == kInvalidHandle) {
        return Match::none;
    }
    const std::uint32_t playerParent = g_playerParent.load(std::memory_order_relaxed);
    std::uintptr_t base = 0;
    std::uint32_t stride = 0;
    if (!pool(base, stride)) {
        return Match::none;
    }
    std::uint32_t cursor = owner;
    for (std::size_t depth = 0; depth < kMaximumAncestors; ++depth) {
        const std::uint32_t parent = parent_of(cursor, base, stride);
        if (parent == kInvalidHandle) {
            return Match::none;
        }
        if (parent == player) {
            return Match::player;
        }
        if (parent == playerParent) {
            return Match::parent;
        }
        if (carrier != 0 && parent == carrier) {
            return Match::carrier;
        }
        cursor = parent;
    }
    return Match::none;
}

/**
 * Tests one draw record against the local player. The chain test covers the player object, its
 * children (the weapon) and its siblings (entity-level attachments); the component test covers
 * objects that keep their component on the player's entity while their datum hangs elsewhere.
 * @param draw Draw record address; its first field is the component handle.
 * @param owner Draw owner handle to test.
 * @return What the draw matched, or Match::none.
 */
[[nodiscard]] Match match_for(std::uintptr_t draw, std::uint32_t owner) noexcept {
    const Match chain = match_kind(owner);
    if (chain != Match::none) {
        return chain;
    }
    const std::uint32_t player = g_playerHandle.load(std::memory_order_acquire);
    if (player == kInvalidHandle) {
        return Match::none;
    }
    const std::uint32_t playerParent = g_playerParent.load(std::memory_order_relaxed);
    std::uint32_t component = kInvalidHandle;
    if (!read(draw, component)) {
        return Match::none;
    }
    const std::uint32_t ownerOf = component_owner(component);
    if (ownerOf == kInvalidHandle) {
        return Match::none;
    }
    if (ownerOf == player || (playerParent != kInvalidHandle && ownerOf == playerParent)) {
        return Match::component;
    }
    return Match::none;
}

/**
 * One skinned draw, called by the game while the render frame is built.
 * A hidden player draw never reaches the original producer: with its palettes never produced,
 * the player's meshes collapse and drop out of sight, which is what hides them.
 */
char __fastcall hide_body(std::uintptr_t draw,
                          void* flags,
                          std::uintptr_t pose,
                          void* allocator,
                          void* corrections,
                          int pass) noexcept {
    if (hide_player_active() && draw != 0) {
        std::uint32_t owner = kInvalidHandle;
        if (read(draw + kDrawOwnerOffset, owner) && match_for(draw, owner) != Match::none) {
            // Collect the model handle for the read-only part-table probe before skipping.
            std::uint32_t model = 0;
            if (read(draw + sizeof(std::uint64_t), model) && model != 0) {
                for (std::atomic<std::uint32_t>& slot : g_playerModels) {
                    std::uint32_t expected = 0;
                    if (slot.compare_exchange_strong(expected, model, std::memory_order_relaxed)
                        || expected == model) {
                        break;
                    }
                }
            }
            return 0;
        }
    }
    const Producer original = reinterpret_cast<Producer>(g_hideHandle.original);
    return original != nullptr ? original(draw, flags, pose, allocator, corrections, pass) : 0;
}

/**
 * Read-only probe on the static part-record processor: the record's own words and its links,
 * logged once, so the owner field can be decoded for the static-path skip.
 */
void __fastcall static_record_probe(std::uintptr_t record,
                                    std::uintptr_t second,
                                    std::uintptr_t third,
                                    std::uintptr_t fourth) noexcept {
    static bool reported{false};
    if (!reported) {
        reported = true;
        trace("static", "probe=fired1");
        std::array<char, 400> line{};
        std::size_t used = 0;
        used += static_cast<std::size_t>(std::snprintf(line.data(),
                                                       line.size(),
                                                       "rec=0x%llX args=%llX,%llX,%llX",
                                                       static_cast<unsigned long long>(record),
                                                       static_cast<unsigned long long>(second),
                                                       static_cast<unsigned long long>(third),
                                                       static_cast<unsigned long long>(fourth)));
        for (std::size_t i = 0; i < 12 && used + 12 < line.size(); ++i) {
            std::uint32_t word = 0;
            if (read(record + i * 4, word)) {
                used += static_cast<std::size_t>(std::snprintf(line.data() + used,
                                                               line.size() - used,
                                                               " %08X",
                                                               static_cast<unsigned>(word)));
            }
        }
        trace("static", line.data());
        std::uintptr_t link = 0;
        if (read(record + 0x18, link) && link != 0) {
            std::array<char, 400> linkLine{};
            std::size_t linkUsed = 0;
            linkUsed += static_cast<std::size_t>(std::snprintf(linkLine.data(),
                                                               linkLine.size(),
                                                               "link=0x%llX",
                                                               static_cast<unsigned long long>(link)));
            for (std::size_t i = 0; i < 12 && linkUsed + 12 < linkLine.size(); ++i) {
                std::uint32_t word = 0;
                if (read(link + i * 4, word)) {
                    linkUsed += static_cast<std::size_t>(std::snprintf(linkLine.data() + linkUsed,
                                                                       linkLine.size() - linkUsed,
                                                                       " %08X",
                                                                       static_cast<unsigned>(word)));
                }
            }
            trace("static", linkLine.data());
        }
    }
    const StaticRecordProcessor original =
        reinterpret_cast<StaticRecordProcessor>(g_staticRecordHandle.original);
    if (original != nullptr) {
        original(record, second, third, fourth);
    }
}

/** The sibling processor's probe; logs once and names itself. */
void __fastcall static_record_probe2(std::uintptr_t record,
                                     std::uintptr_t second,
                                     std::uintptr_t third,
                                     std::uintptr_t fourth) noexcept {
    static bool reported{false};
    if (!reported) {
        reported = true;
        trace("static", "probe=fired2");
        std::array<char, 400> line{};
        std::size_t used = 0;
        used += static_cast<std::size_t>(std::snprintf(line.data(),
                                                       line.size(),
                                                       "rec=0x%llX args=%llX,%llX,%llX",
                                                       static_cast<unsigned long long>(record),
                                                       static_cast<unsigned long long>(second),
                                                       static_cast<unsigned long long>(third),
                                                       static_cast<unsigned long long>(fourth)));
        for (std::size_t i = 0; i < 12 && used + 12 < line.size(); ++i) {
            std::uint32_t word = 0;
            if (read(record + i * 4, word)) {
                used += static_cast<std::size_t>(std::snprintf(line.data() + used,
                                                               line.size() - used,
                                                               " %08X",
                                                               static_cast<unsigned>(word)));
            }
        }
        trace("static", line.data());
    }
    const StaticRecordProcessor original =
        reinterpret_cast<StaticRecordProcessor>(g_staticRecord2Handle.original);
    if (original != nullptr) {
        original(record, second, third, fourth);
    }
}

/** @param reason Key naming the step that failed. */
void report_install_failure(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=photo stage=install result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
        trace("install", line.data() + 22);
    }
}

} // namespace

/** Reads one object's parent handle from the datum pool. */
std::uint32_t datum_parent(std::uint32_t handle) noexcept {
    if (handle == kInvalidHandle) {
        return kInvalidHandle;
    }
    std::uintptr_t base = 0;
    std::uint32_t stride = 0;
    if (!pool(base, stride)) {
        return kInvalidHandle;
    }
    return parent_of(handle, base, stride);
}

namespace {

/** One backed-up part-flag word, restored when the hide turns off. */
struct FlagPatch final {
    std::uintptr_t address{};
    std::uint16_t original{};
};

/** One authored part row, read from the model record. */
struct PartRow final {
    std::uint32_t material{};
    std::uint32_t first{};
    std::uint32_t count{};
    std::int16_t logical{-1};
    std::uint16_t flags{};
    std::uint8_t lod{};
};

/** Camera-thread state; refresh_player_handle is the only writer. */
std::array<FlagPatch, 512> g_flagPatches{};
std::size_t g_flagPatchCount{};
bool g_flagsPatched{false};
std::array<std::uint32_t, 16> g_patchedModels{};
std::size_t g_patchedModelCount{};

/** @param p Part row address. @return True when every field was read. */
[[nodiscard]] bool read_part_row(std::uintptr_t p, PartRow& row) noexcept {
    return read(p, row.material) && read(p + 8, row.first) && read(p + 0xC, row.count)
           && read(p + 0x14, row.logical) && read(p + 0x18, row.flags) && read(p + 0x1B, row.lod);
}

/** @param row Read part row. @return True when its fields fit the authored layout. */
[[nodiscard]] bool plausible_part_row(const PartRow& row) noexcept {
    // The material may be 0 or 0xFFFFFFFF: parts with no direct material draw from their
    // external group, and a selected material of -1 is itself a draw stop. The index and flag
    // fields carry the signal instead.
    return row.logical >= 0 && row.logical < 64 && (row.flags & ~0xFFU) == 0 && row.lod <= 7
           && row.first <= 0x1000000 && row.count > 0 && row.count <= 0x400000;
}

/**
 * Finds the part table in one model record by its LOD chain: two consecutive 0x20 rows with the
 * same material, a rising LOD byte and a continuing index range. The runtime record is flat, so
 * the table sits at a per-model offset after the mesh data.
 * @param model Model record address.
 * @param table Receives the first part row address.
 * @return The number of plausible rows, or zero when no chain matched.
 */
[[nodiscard]] std::size_t find_part_table(std::uintptr_t model, std::uintptr_t& table) noexcept {
    for (std::uint32_t offset = 0x100; offset < 0x500; offset += 4) {
        const std::uintptr_t first = model + offset;
        PartRow a{};
        PartRow b{};
        if (!read_part_row(first, a) || !plausible_part_row(a)) {
            continue;
        }
        if (!read_part_row(first + 0x20, b) || !plausible_part_row(b)) {
            continue;
        }
        if (b.material != a.material || b.lod <= a.lod) {
            continue;
        }
        const std::uint32_t chainEnd = a.first + a.count;
        if (b.first < chainEnd || b.first > chainEnd + 4) {
            continue;
        }
        std::size_t rows = 2;
        for (std::size_t extra = 2; extra < 128; ++extra) {
            PartRow next{};
            if (!read_part_row(first + extra * 0x20, next) || !plausible_part_row(next)) {
                break;
            }
            ++rows;
        }
        table = first;
        return rows;
    }
    return 0;
}

/** Sets the 0x40 hidden-part flag on every row of one model's part table. */
void patch_model_parts(std::size_t rows, std::uintptr_t table) noexcept {
    const std::size_t span = rows * 0x20;
    DWORD oldProtect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(table),
                       static_cast<SIZE_T>(span),
                       PAGE_READWRITE,
                       &oldProtect)
        == FALSE) {
        return;
    }
    for (std::size_t row = 0; row < rows; ++row) {
        if (g_flagPatchCount >= g_flagPatches.size()) {
            break;
        }
        const std::uintptr_t flags = table + row * 0x20 + 0x18;
        std::uint16_t current = 0;
        if (!read(flags, current)) {
            continue;
        }
        g_flagPatches[g_flagPatchCount++] = FlagPatch{flags, current};
        *reinterpret_cast<volatile std::uint16_t*>(flags) =
            static_cast<std::uint16_t>(current | 0x40U);
    }
    (void)VirtualProtect(
        reinterpret_cast<void*>(table), static_cast<SIZE_T>(span), oldProtect, &oldProtect);
}

/** Writes back every backed-up flag word. */
void restore_part_flags() noexcept {
    for (std::size_t i = 0; i < g_flagPatchCount; ++i) {
        const std::uintptr_t flags = g_flagPatches[i].address;
        DWORD oldProtect = 0;
        if (VirtualProtect(reinterpret_cast<void*>(flags),
                           sizeof(std::uint16_t),
                           PAGE_READWRITE,
                           &oldProtect)
            != FALSE) {
            *reinterpret_cast<volatile std::uint16_t*>(flags) = g_flagPatches[i].original;
            (void)VirtualProtect(
                reinterpret_cast<void*>(flags), sizeof(std::uint16_t), oldProtect, &oldProtect);
        }
    }
    g_flagPatchCount = 0;
    g_patchedModelCount = 0;
    g_patchedModels = {};
}

} // namespace

/**
 * Applies or restores the part flags while the hide is on. When it turns on, every collected
 * player model gets the 0x40 hidden-part flag on its part rows, which the game's own draw
 * decision honors, cloth parts included. When it turns off, every original flag is restored.
 */
void update_part_flags() noexcept {
    const bool want = hide_player_active();
    if (want == g_flagsPatched) {
        return;
    }
    if (!want) {
        restore_part_flags();
        g_flagsPatched = false;
        return;
    }
    const content::handles::Source source{
        g_tablesSlot.load(std::memory_order_relaxed), nullptr, &memory::copy_current_process};
    for (std::atomic<std::uint32_t>& slot : g_playerModels) {
        const std::uint32_t handle = slot.load(std::memory_order_relaxed);
        if (handle == 0 || handle == kInvalidHandle) {
            continue;
        }
        bool known = false;
        for (std::size_t i = 0; i < g_patchedModelCount; ++i) {
            known = known || g_patchedModels[i] == handle;
        }
        if (known) {
            continue;
        }
        if (g_patchedModelCount < g_patchedModels.size()) {
            g_patchedModels[g_patchedModelCount++] = handle;
        }
        std::uintptr_t model = 0;
        if (!content::handles::resolve(source, handle, model) || model == 0) {
            continue;
        }
        std::uintptr_t table = 0;
        const std::size_t rows = find_part_table(model, table);
        if (rows == 0) {
            continue;
        }
        patch_model_parts(rows, table);
    }
    g_flagsPatched = true;
}

/** Resolves the producer and attaches the hide-player detour. */
bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    std::byte* const producer = patterns::scan_main_image_unique(kProducer, "photo_hide_player");
    if (producer == nullptr) {
        report_install_failure("producer");
        return false;
    }
    if (!hooking::detour::install(
            hooking::detour::Spec{producer, reinterpret_cast<void*>(&hide_body)}, g_hideHandle)) {
        report_install_failure("attach");
        return false;
    }
    // The static part-record processors: read-only probes until the owner field is decoded.
    std::byte* const staticRecord =
        patterns::scan_main_image_unique(kStaticRecord, "photo_hide_static");
    if (staticRecord != nullptr) {
        (void)hooking::detour::install(
            hooking::detour::Spec{staticRecord, reinterpret_cast<void*>(&static_record_probe)},
            g_staticRecordHandle);
        trace("install", "static=ok");
    } else {
        report_install_failure("static");
    }
    std::byte* const staticRecord2 =
        patterns::scan_main_image_unique(kStaticRecord2, "photo_hide_static2");
    if (staticRecord2 != nullptr) {
        (void)hooking::detour::install(
            hooking::detour::Spec{staticRecord2, reinterpret_cast<void*>(&static_record_probe2)},
            g_staticRecord2Handle);
        trace("install", "static2=ok");
    } else {
        report_install_failure("static2");
    }
    // The component tables extend the hide to draws whose owner chain is disconnected from the
    // player. A miss only narrows the hide to the chain rules, so it is logged, not fatal.
    std::byte* const tables =
        patterns::scan_main_image_unique(kTablesLoad, "photo_hide_tables");
    if (tables != nullptr) {
        g_tablesSlot.store(
            reinterpret_cast<std::uintptr_t>(patterns::resolve_relative(tables + kTablesOperand,
                                                                        tables + kTablesNext)),
            std::memory_order_release);
    } else {
        report_install_failure("tables");
    }
    g_installed.store(true, std::memory_order_release);
    g_hideInstalled.store(true, std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=photo stage=install result=ok");
    return true;
}

/** Disarms the skip first, then detaches the producer detour. */
void uninstall() noexcept {
    g_hideInstalled.store(false, std::memory_order_release);
    if (!g_installed.load(std::memory_order_acquire)) {
        return;
    }
    // A failed detach keeps the handle, so the detour still reaches the original. The skip is
    // already off, which makes the replacement inert.
    if (!hooking::detour::uninstall(g_hideHandle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=photo stage=uninstall result=fail");
        return;
    }
    (void)hooking::detour::uninstall(g_staticRecordHandle);
    (void)hooking::detour::uninstall(g_staticRecord2Handle);
    g_installed.store(false, std::memory_order_release);
}

} // namespace sunrise::client::hooks::photo
