// The Bunny Hood: see bunny_hood.h and docs/fork/bunny-hood.md.
#include "bunny_hood.h"

#include "cosmetics.h"
#include "guest_coro.h"
#include "gxruntime/dvd.h"
#include "text_swap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

enum {
    // dItem_data tables in main.dol's data (d_item_data.cpp), one row per item number.
    ITEM_RESOURCE = 0x803842B0u,
    ITEM_RESOURCE_ROW = 0x24u,
    FIELD_ITEM_RES = 0x803866B0u,
    FIELD_ITEM_RES_ROW = 0x1Cu,
    ITEM_INFO = 0x803882B0u,
    ITEM_INFO_ROW = 4u,
    ITEM_CHARM = 0x43u,

    // zel_00.bmg messages: the Hero's Charm's name, description and item-get text, and the Hood's item-get
    // number (0x65 + item).
    MSG_CHARM_NAME = 0x19E,
    MSG_CHARM_INFO = 0x266,
    MSG_CHARM_GET = 0xA8,
    MSG_HOOD_GET = 0xA9,
    TEXT_NAME_AT = 0x000,
    TEXT_INFO_AT = 0x040,
    TEXT_GET_AT = 0x240,
    TEXT_BLOCK = 0x600 + 0xC20,
    ICON_AT = 0x620, // the hood's icon: its own 0xC00 bytes (the screen's charm buffer stays the charm's; a new address is a new texture)
    TEXT_MAX = 0x1C0,

    // dMenu_Collect_c (the Quest Status screen): the global pointer and the fields used.
    DMC_PTR = 0x803F6FF0u,
    DMC_ITEM_TEX = 0x24CCu, // ResTIMG* mItemTexBuffer[5]
    DMC_NOW_ITEM = 0x27EDu,
    DMC_MODE = 0x27EEu,
    NOW_ITEM_CHARM = 0x12,
    ICON_BYTES = 0xC00,

    PAD = 0x803A4DF0u, // g_mDoCPd_cpadInfo[0]
    PAD_HOLD = 0x30u,
    PAD_TRIG = 0x32u,
    PAD_LEFT = 0x0001u,
    PAD_RIGHT = 0x0002u,
    PAD_A = 0x0100u,

    FN_ARCHIVE_HEAP = 0x80011AB4u, // mDoExt_getArchiveHeap()
    FN_HEAP_ALLOC = 0x802B0434u,   // JKRHeap::alloc(size, alignment, heap)
};

#define SPEED_FACTOR 1.5f // MM: speedTarget *= 1.5f in the walk/run action (z_player.c), not while Z-targeting

static bool guest_ptr(u32 p) { return p >= 0x80000000u && p < 0x81800000u; }

static float read_f32(CPUState* cpu, u32 address) {
    const u32 bits = mem_read32(cpu, address);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static void write_f32(CPUState* cpu, u32 address, float f) {
    u32 bits;
    memcpy(&bits, &f, sizeof bits);
    mem_write32(cpu, address, bits);
}

// ----------------------------------------------------------------- state ----

// The hood is worn on the local player (Anchor's pose flag, the draw and the speed read this).
static bool g_worn;
static volatile bool g_enabled;
void bluewake_bunny_hood_enable(bool on) { g_enabled = on; }
bool bluewake_bunny_hood_enabled(void) { return g_enabled; }
bool bluewake_bunny_hood_worn(void) { return g_enabled && g_worn; }

static u8 collect_byte(CPUState* cpu) { return (u8)mem_read8(cpu, BLUEWAKE_BUNNY_COLLECT); }

// -------------------------------------------------------------- item rows ----

static bool g_tables_done;
static bool g_item_model_done;
static u8* g_item_arc; // the hood's VamuY.arc as served over the disc's (kept to serve it again after an in-place randomizer switch)
static u32 g_item_arc_len;

static bool extras_file(const char* name, char* out, size_t cap);
static u8* read_whole(const char* path, u32* length);
static u32 be32(const u8* p);
static u32 be16(const u8* p);

// The hood's own model for the item-get and the chests: assets/bunnyhood/VamuY.arc is the disc's Hero's Charm archive
// with the hood's item model added as one more file (index in hood.json); it is served over res/Object/VamuY.arc, and
// item 0x44's rows point at that file instead of the charm's. Without the file the charm's model stands in.
static void use_item_model(CPUState* cpu) {
    if (g_item_model_done)
        return;
    g_item_model_done = true;
    char path[1024];
    u32 arc_len = 0, json_len = 0;
    u8* arc = NULL;
    u8* json = NULL;
    if (extras_file("VamuY.arc", path, sizeof path))
        arc = read_whole(path, &arc_len);
    if (extras_file("hood.json", path, sizeof path))
        json = read_whole(path, &json_len);
    int index = -1;
    if (json != NULL) {
        char* text = (char*)realloc(json, json_len + 1u);
        if (text != NULL) {
            json = (u8*)text;
            text[json_len] = '\0';
            const char* at = strstr(text, "\"vamuy_item_index\"");
            if (at != NULL && (at = strchr(at, ':')) != NULL)
                index = atoi(at + 1);
        }
    }
    if (getenv("BLUEWAKE_BUNNY_ITEM_INDEX") != NULL)
        index = atoi(getenv("BLUEWAKE_BUNNY_ITEM_INDEX")); // a test
    // The game loads an archive's resources by type through its directory nodes: the file must be in a 'BDL ' node's range,
    // or the item's model would be NULL and the game would hang on it.
    bool listed = false;
    if (arc != NULL && arc_len > 0x40u && index > 0) {
        // getFirstResource(type) walks only the FIRST node of a type: the file must be in the first 'BDLM' node's range.
        const u32 nodes = be32(arc + 0x20u + 0u), nodes_at = 0x20u + be32(arc + 0x24u);
        for (u32 n = 0; n < nodes && nodes_at + 16u * (n + 1u) <= arc_len; ++n) {
            const u8* e = arc + nodes_at + 16u * n;
            const u32 first = be32(e + 12u), count = be16(e + 10u);
            if (memcmp(e, "BDLM", 4) == 0) {
                listed = (u32)index >= first && (u32)index < first + count;
                break;
            }
        }
    }
    if (arc != NULL && index > 0 && !listed) {
        fprintf(stderr, "[bunny] VamuY.arc file %d is not in the first 'BDLM' directory node's range: the Hero's Charm's model is used instead\n", index);
        index = -1;
    }
    if (arc != NULL && index > 0 && index < 64 && dvd_overlay_set("res/Object/VamuY.arc", arc, arc_len)) {
        const u32 row = ITEM_RESOURCE + BLUEWAKE_BUNNY_HOOD_ITEM * ITEM_RESOURCE_ROW;
        const u32 field = FIELD_ITEM_RES + BLUEWAKE_BUNNY_HOOD_ITEM * FIELD_ITEM_RES_ROW;
        mem_write16(cpu, row + 0x08u, (u16)index); // mBmdIdx
        mem_write16(cpu, row + 0x0Au, 0xFFFFu);    // mSrtIdx: no texture animation
        mem_write32(cpu, field + 0x00u, mem_read32(cpu, row)); // the Hero's Charm has no field model: its field row has no archive
        mem_write16(cpu, field + 0x04u, (u16)index);
        mem_write16(cpu, field + 0x06u, 0xFFFFu);
        // The charm's heap for the model's instance is 0x1000; the hood's model (a few hundred display-list kilobytes) needs more or
        // its demo item never gets made.
        mem_write16(cpu, row + 0x20u, 0xFFF0u);   // item_resource mHeapSize
        mem_write16(cpu, field + 0x18u, 0xFFF0u); // field_item_res mHeapSize
        fprintf(stderr, "[bunny] item model: file %d of VamuY.arc (%u bytes) served over the disc's\n", index, arc_len);
    } else
        fprintf(stderr, "[bunny] no VamuY.arc or index: the item uses the Hero's Charm's model\n");
    free(arc);
    free(json);
}

// Item 0x44 gets Hero's Charm's rows (model, field model, pick-up hitbox): the charm's model stands in until the
// hood has one of its own. Only when the rows are still the game's empty ones.
static void patch_item_tables(CPUState* cpu) {
    const u32 charm_row = ITEM_RESOURCE + ITEM_CHARM * ITEM_RESOURCE_ROW;
    const u32 hood_row = ITEM_RESOURCE + BLUEWAKE_BUNNY_HOOD_ITEM * ITEM_RESOURCE_ROW;
    if (g_tables_done) {
        // The randomizer's in-place switch reloads the game's memory and reopens the disc image: the rows are the game's empty
        // ones again and the served archive is gone (the hood then showed the Hero's Charm's model, or none). Make both again.
        static u32 tick;
        if (guest_ptr(mem_read32(cpu, charm_row)) && mem_read32(cpu, hood_row) == 0u) {
            g_tables_done = false;
            g_item_model_done = false;
            fprintf(stderr, "[bunny] item rows are the game's again (a switch of the game's data): made again\n");
        } else if (g_item_arc != NULL && (++tick % 90u) == 0u && !dvd_overlay_has("res/Object/VamuY.arc")) {
            dvd_overlay_set("res/Object/VamuY.arc", g_item_arc, g_item_arc_len);
            fprintf(stderr, "[bunny] the hood's VamuY.arc is served again (the disc image was reopened)\n");
        }
    }
    if (g_tables_done)
        return;
    if (!guest_ptr(mem_read32(cpu, charm_row)))
        return; // the data is not there yet
    if (mem_read32(cpu, hood_row) != 0u) {
        g_tables_done = true; // another change took it, or already ours
        return;
    }
    for (u32 o = 0; o < ITEM_RESOURCE_ROW; o += 4u)
        mem_write32(cpu, hood_row + o, mem_read32(cpu, charm_row + o));
    for (u32 o = 0; o < FIELD_ITEM_RES_ROW; o += 4u)
        mem_write32(cpu, FIELD_ITEM_RES + BLUEWAKE_BUNNY_HOOD_ITEM * FIELD_ITEM_RES_ROW + o,
                    mem_read32(cpu, FIELD_ITEM_RES + ITEM_CHARM * FIELD_ITEM_RES_ROW + o));
    // The Hero's Charm has no field model (its field row has no archive): the hood's field item uses the item's own archive.
    mem_write32(cpu, FIELD_ITEM_RES + BLUEWAKE_BUNNY_HOOD_ITEM * FIELD_ITEM_RES_ROW, mem_read32(cpu, hood_row));
    mem_write16(cpu, FIELD_ITEM_RES + BLUEWAKE_BUNNY_HOOD_ITEM * FIELD_ITEM_RES_ROW + 4u, mem_read16(cpu, hood_row + 8u));  // mBmdIdx
    mem_write16(cpu, FIELD_ITEM_RES + BLUEWAKE_BUNNY_HOOD_ITEM * FIELD_ITEM_RES_ROW + 6u, mem_read16(cpu, hood_row + 0xAu)); // mSrtIdx
    mem_write16(cpu, FIELD_ITEM_RES + BLUEWAKE_BUNNY_HOOD_ITEM * FIELD_ITEM_RES_ROW + 0x18u, mem_read16(cpu, hood_row + 0x20u)); // heap
    mem_write32(cpu, ITEM_INFO + BLUEWAKE_BUNNY_HOOD_ITEM * ITEM_INFO_ROW,
                mem_read32(cpu, ITEM_INFO + ITEM_CHARM * ITEM_INFO_ROW));
    g_tables_done = true;
    use_item_model(cpu);
    fprintf(stderr, "[bunny] item rows 0x%02X made from the Hero's Charm's\n", BLUEWAKE_BUNNY_HOOD_ITEM);
}

// ------------------------------------------------------------------ text ----

static u32 g_text_buf; // guest memory of TEXT_BLOCK bytes (archive heap), 0 until allocated
static u8 g_name_text[TEXT_MAX], g_info_text[TEXT_MAX], g_get_text[TEXT_MAX];
static u32 g_name_len, g_info_len, g_get_len;
static bool g_text_ready;

typedef struct TextCopy {
    u32 bmg;            // the message file's address, 0 = unused slot
    u32 data;           // DAT1 + 8: what an entry's offset is added to
    u32 name_entry, info_entry, get_entry; // INF1 entries (guest addresses)
    u32 orig_name, orig_info;              // the charm's own text offsets
    bool redirected;
} TextCopy;
enum { TEXT_COPIES = 4 };
static TextCopy g_copies[TEXT_COPIES];

static u32 find_bytes(const u8* hay, u32 n, const u8* needle, u32 m) {
    for (u32 i = 0; i + m <= n; ++i)
        if (memcmp(hay + i, needle, m) == 0)
            return i;
    return 0xFFFFFFFFu;
}

// A message's bytes up to its end, as a lone 0 byte. A control code is 0x1A, its own length (counting the 0x1A and the
// length byte) and the rest, and may hold 0 bytes; it is copied whole. The result is NUL terminated, its length
// excludes the NUL.
static u32 read_text(CPUState* cpu, u32 address, u8* out, u32 max) {
    u32 n = 0;
    while (n + 1u < max) {
        const u8 c = (u8)mem_read8(cpu, address + n);
        if (c == 0u)
            break;
        if (c == 0x1Au) {
            const u32 len = (u8)mem_read8(cpu, address + n + 1u);
            if (len < 2u || n + len + 1u >= max)
                break;
            for (u32 i = 0; i < len; ++i, ++n)
                out[n] = (u8)mem_read8(cpu, address + n);
            continue;
        }
        out[n++] = c;
    }
    out[n] = 0u;
    return n;
}

// The three texts, from the charm's own in the loaded file (so the box codes, colours and prompts are the
// game's): the name replaces the name; the get text keeps its opening and swaps the body; the description keeps
// its Equip / Don't Equip prompt and swaps the lines above it.
static bool build_texts(CPUState* cpu, const TextCopy* c) {
    u8 charm_get[TEXT_MAX], charm_info[TEXT_MAX];
    const u32 get_n = read_text(cpu, c->data + mem_read32(cpu, c->get_entry), charm_get, TEXT_MAX);
    // (the get entry here is the charm's own, MSG_CHARM_GET, see install_copy)
    const u32 info_n = read_text(cpu, c->data + c->orig_info, charm_info, TEXT_MAX);

    static const u8 name_old[] = "Hero's Charm";
    static const u8 marker_get[] = {0x1A, 0x07, 0x00, 0x00, 0x07, 0x00, 0x0A};
    static const u8 marker_info[] = {0x1A, 0x06, 0xFF, 0x00, 0x00, 0x02};
    static const char hood_name[] = "Bunny Hood";
    static const char get_body[] = "My, what long ears it has!\nWill the power of the wild spring\nforth? Run faster while you wear it!\n"
                                   "Equip it on the Quest Status screen!";
    static const char info_body[] = "This hood is filled with the speed\nand hearing of the wild. Wear it and\nyour legs will carry you faster. \n";

    // Name.
    g_name_len = (u32)strlen(hood_name) + 1u;
    memcpy(g_name_text, hood_name, g_name_len);

    // Item-get: [opening with the name swapped] + marker + body.
    {
        u32 m = find_bytes(charm_get, get_n, marker_get, sizeof marker_get);
        const u32 nm = find_bytes(charm_get, get_n, name_old, sizeof name_old - 1u);
        if (nm == 0xFFFFFFFFu)
            return false; // not the English file
        u32 end; // the opening ends here: the body follows
        if (m != 0xFFFFFFFFu && nm < m) {
            end = m + (u32)sizeof marker_get;
        } else {
            // The randomizer's "instant text boxes" removes the wait code (1A 07 00 00 07 ..): the opening is "You got the
            // <name>!" with its codes, up to and including the line break after it.
            u32 bang = nm + (u32)sizeof name_old - 1u;
            while (bang < get_n && charm_get[bang] != '!')
                ++bang;
            if (bang >= get_n)
                return false;
            end = bang + 1u;
            while (end < get_n && charm_get[end] == 0x1A && charm_get[end + 1u] >= 2u && end + charm_get[end + 1u] <= get_n)
                end += charm_get[end + 1u];
            if (end < get_n && charm_get[end] == 0x0A)
                ++end;
        }
        u32 n = 0;
        memcpy(g_get_text, charm_get, nm);
        n = nm;
        memcpy(g_get_text + n, hood_name, strlen(hood_name));
        n += (u32)strlen(hood_name);
        const u32 after = nm + (u32)sizeof name_old - 1u;
        memcpy(g_get_text + n, charm_get + after, end - after);
        n += end - after;
        memcpy(g_get_text + n, get_body, sizeof get_body); // with its NUL
        n += (u32)sizeof get_body;
        if (n > TEXT_MAX)
            return false;
        g_get_len = n;
    }
    // Description: [lines] + the charm's prompt from its marker on.
    {
        const u32 m = find_bytes(charm_info, info_n, marker_info, sizeof marker_info);
        if (m == 0xFFFFFFFFu)
            return false;
        u32 n = (u32)strlen(info_body);
        memcpy(g_info_text, info_body, n);
        memcpy(g_info_text + n, charm_info + m, info_n - m + 1u); // with its NUL
        n += info_n - m + 1u;
        if (n > TEXT_MAX)
            return false;
        g_info_len = n;
    }
    return true;
}

static void write_text_block(CPUState* cpu) {
    for (u32 i = 0; i < g_name_len; ++i)
        mem_write8(cpu, g_text_buf + TEXT_NAME_AT + i, g_name_text[i]);
    for (u32 i = 0; i < g_info_len; ++i)
        mem_write8(cpu, g_text_buf + TEXT_INFO_AT + i, g_info_text[i]);
    for (u32 i = 0; i < g_get_len; ++i)
        mem_write8(cpu, g_text_buf + TEXT_GET_AT + i, g_get_text[i]);
}

static u32 entry_by_message(CPUState* cpu, u32 inf, u32 entries, u32 size, u16 message) {
    for (u32 i = 0; i < entries; ++i) {
        const u32 e = inf + 16u + i * size;
        if (mem_read16(cpu, e + 4u) == message && mem_read32(cpu, e) != 0u)
            return e;
    }
    return 0u;
}

// A message file in memory: finds INF1 and DAT1, the charm's entries and a free one, and gives the Hood its
// item-get message (an entry whose data offset is 0 is skipped by the game's search, so it is free).
#define INSTALL_FAIL(why)                                                                   \
    do {                                                                                    \
        static int reported_;                                                               \
        if (reported_++ < 3)                                                                \
            fprintf(stderr, "[bunny] message file at 0x%08X not used: %s\n", bmg, (why));  \
        return false;                                                                       \
    } while (0)

static bool install_copy(CPUState* cpu, u32 bmg, TextCopy* out) {
    if (!guest_ptr(bmg) || mem_read32(cpu, bmg) != 0x4D455347u || mem_read32(cpu, bmg + 4u) != 0x626D6731u)
        INSTALL_FAIL("no header");
    const u32 blocks = mem_read32(cpu, bmg + 8u), sections = mem_read32(cpu, bmg + 12u);
    if (blocks < 2u || blocks > 0x00400000u / 32u || sections == 0u || sections > 16u)
        INSTALL_FAIL("odd header");
    u32 inf = 0u, dat = 0u, at = bmg + 32u;
    for (u32 i = 0; i < sections; ++i) {
        const u32 magic = mem_read32(cpu, at), size = mem_read32(cpu, at + 4u);
        if (size < 8u || at + size > bmg + blocks * 32u)
            INSTALL_FAIL("odd section");
        if (magic == 0x494E4631u)
            inf = at;
        else if (magic == 0x44415431u)
            dat = at;
        at += size;
    }
    if (inf == 0u || dat == 0u)
        INSTALL_FAIL("no INF1 or DAT1");
    const u32 entries = mem_read16(cpu, inf + 8u), size = mem_read16(cpu, inf + 10u);
    if (size < 14u || size > 64u || entries == 0u)
        INSTALL_FAIL("odd entry size");
    TextCopy c = {.bmg = bmg, .data = dat + 8u};
    c.name_entry = entry_by_message(cpu, inf, entries, size, MSG_CHARM_NAME);
    c.info_entry = entry_by_message(cpu, inf, entries, size, MSG_CHARM_INFO);
    const u32 charm_get = entry_by_message(cpu, inf, entries, size, MSG_CHARM_GET);
    if (c.name_entry == 0u || c.info_entry == 0u || charm_get == 0u)
        INSTALL_FAIL("no charm messages");
    c.orig_name = mem_read32(cpu, c.name_entry);
    c.orig_info = mem_read32(cpu, c.info_entry);
    // Is it English? The name reads "Hero's Charm".
    u8 probe[16];
    read_text(cpu, c.data + c.orig_name, probe, sizeof probe);
    if (strcmp((char*)probe, "Hero's Charm") != 0)
        INSTALL_FAIL("not the English item names");
    // The Hood's entry: already there (this copy was installed before), or a free one made from the charm's.
    u32 hood = entry_by_message(cpu, inf, entries, size, MSG_HOOD_GET);
    if (hood == 0u) {
        // A free entry has message number 0: the game never asks for that, whatever its offset is (the randomizer's file has
        // no zero-offset ones, but 85 numbered 0 with an offset). Last ones first, away from the file's real texts.
        for (u32 i = entries; i > 0u && hood == 0u; --i) {
            const u32 e = inf + 16u + (i - 1u) * size;
            if (mem_read16(cpu, e + 4u) == 0u)
                hood = e;
        }
        if (hood == 0u)
            INSTALL_FAIL("no free entry");
        for (u32 o = 0; o < size; o += 2u)
            mem_write16(cpu, hood + o, (u16)mem_read16(cpu, charm_get + o));
        mem_write16(cpu, hood + 4u, MSG_HOOD_GET);
    }
    c.get_entry = charm_get; // build_texts reads the charm's get text through this
    if (!g_text_ready) {
        if (!build_texts(cpu, &c))
            INSTALL_FAIL("the charm's texts have another shape");
        g_text_ready = true;
    }
    write_text_block(cpu);
    c.get_entry = hood;
    mem_write32(cpu, hood, (u32)(g_text_buf + TEXT_GET_AT - c.data)); // wraps: the buffer is anywhere in memory
    *out = c;
    fprintf(stderr, "[bunny] messages in the file at 0x%08X: item-get %X at entry 0x%08X, text at 0x%08X\n", bmg,
            MSG_HOOD_GET, hood, g_text_buf);
    return true;
}

static bool copy_valid(CPUState* cpu, const TextCopy* c) {
    return c->bmg != 0u && mem_read32(cpu, c->bmg) == 0x4D455347u && mem_read16(cpu, c->get_entry + 4u) == MSG_HOOD_GET &&
           mem_read32(cpu, c->get_entry) == (u32)(g_text_buf + TEXT_GET_AT - c->data);
}

// Points the charm's name and description at the hood's text (or back).
static void redirect_charm_text(CPUState* cpu, bool hood) {
    for (int i = 0; i < TEXT_COPIES; ++i) {
        TextCopy* c = &g_copies[i];
        if (c->bmg == 0u || !copy_valid(cpu, c) || c->redirected == hood)
            continue;
        mem_write32(cpu, c->name_entry, hood ? (u32)(g_text_buf + TEXT_NAME_AT - c->data) : c->orig_name);
        mem_write32(cpu, c->info_entry, hood ? (u32)(g_text_buf + TEXT_INFO_AT - c->data) : c->orig_info);
        c->redirected = hood;
    }
}

static void text_retrace(CPUState* cpu) {
    if (g_text_buf == 0u || cpu->ram == NULL)
        return;
    static u32 tick;
    ++tick;
    bool any = false;
    for (int i = 0; i < TEXT_COPIES; ++i) {
        TextCopy* c = &g_copies[i];
        if (c->bmg != 0u && !copy_valid(cpu, c)) {
            fprintf(stderr, "[bunny] message file at 0x%08X is gone\n", c->bmg);
            *c = (TextCopy){0};
        }
        any |= c->bmg != 0u;
    }
    if (any && (tick % 300u) != 0u)
        return;
    if (!any && (tick % 60u) != 1u)
        return;
    const u8* ram = cpu->ram;
    for (u32 offset = 0; offset + 8u <= cpu->ram_size;) {
        const u8* hit = memmem(ram + offset, cpu->ram_size - offset, "MESGbmg1", 8);
        if (hit == NULL)
            break;
        const u32 address = 0x80000000u + (u32)(hit - ram);
        offset = (u32)(hit - ram) + 8u;
        bool known = false;
        for (int i = 0; i < TEXT_COPIES; ++i)
            known |= g_copies[i].bmg == address;
        if (known)
            continue;
        for (int i = 0; i < TEXT_COPIES; ++i)
            if (g_copies[i].bmg == 0u) {
                install_copy(cpu, address, &g_copies[i]);
                break;
            }
    }
}

// ------------------------------------------------------- icon of the slot ----

static u8 g_hood_icon[ICON_BYTES + 1];
static u32 g_hood_icon_len; // 0 = the file is not there (the slot then keeps the charm's picture)
static bool g_icon_tried;
static bool g_icon_dirty; // the picture's bytes changed: the panes read their texture again (menu_pass)
static u8 g_charm_icon[ICON_BYTES];
static bool g_charm_icon_saved;
static bool g_icon_is_hood;

#ifdef _WIN32
static bool extras_file(const char* name, char* out, size_t cap) {
    char path[MAX_PATH];
    const char* made = getenv("BLUEWAKE_EXTRAS_DIR");
    if (made != NULL && *made != '\0') {
        snprintf(path, sizeof path, "%s\\assets\\bunnyhood\\%s", made, name);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, cap, "%s", path);
            return true;
        }
    }
    const DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0u || n >= MAX_PATH)
        return false;
    char* slash = strrchr(path, '\\');
    if (slash == NULL)
        return false;
    *slash = '\0';
    snprintf(out, cap, "%s\\assets\\bunnyhood\\%s", path, name);
    return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}
#elif defined(__linux__)
static bool extras_file(const char* name, char* out, size_t cap) {
    char path[PATH_MAX];
    const char* made = getenv("BLUEWAKE_EXTRAS_DIR");
    if (made != NULL && *made != '\0') {
        snprintf(path, sizeof path, "%s/assets/bunnyhood/%s", made, name);
        if (access(path, R_OK) == 0) {
            snprintf(out, cap, "%s", path);
            return true;
        }
    }
    const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0)
        return false;
    path[n] = '\0';
    char* slash = strrchr(path, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    snprintf(out, cap, "%s/assets/bunnyhood/%s", path, name);
    return access(out, R_OK) == 0;
}
#else
static bool extras_file(const char* name, char* out, size_t cap) {
    (void)name;
    (void)out;
    (void)cap;
    return false;
}
#endif

static void load_hood_icon(void) {
    if (g_icon_tried)
        return;
    g_icon_tried = true;
    char path[1024];
    if (!extras_file("bunnyhood_icon.bti", path, sizeof path)) {
        fprintf(stderr, "[bunny] no bunnyhood_icon.bti: the slot keeps the charm's picture\n");
        return;
    }
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return;
    const size_t n = fread(g_hood_icon, 1, sizeof g_hood_icon, f);
    fclose(f);
    // 48x48, C8 (9), the game's item icon layout: image at 0x20, palette after it.
    if (n < 0x40u || n > ICON_BYTES || g_hood_icon[0] != 9u || g_hood_icon[2] != 0u || g_hood_icon[3] != 48u ||
        g_hood_icon[4] != 0u || g_hood_icon[5] != 48u) {
        fprintf(stderr, "[bunny] %s is not a 48x48 C8 icon: the slot keeps the charm's picture\n", path);
        return;
    }
    g_hood_icon_len = (u32)n;
    fprintf(stderr, "[bunny] hood icon %u bytes from %s\n", g_hood_icon_len, path);
}

static u32 icon_buffer(CPUState* cpu, u32 dmc) {
    const u32 buf = mem_read32(cpu, dmc + DMC_ITEM_TEX + 4u * 4u);
    return guest_ptr(buf) ? buf : 0u;
}

static void show_icon(CPUState* cpu, u32 dmc, bool hood) {
    const u32 buf = icon_buffer(cpu, dmc);
    if (buf == 0u)
        return;
    load_hood_icon();
    if (!g_charm_icon_saved) {
        for (u32 i = 0; i < ICON_BYTES; ++i)
            g_charm_icon[i] = (u8)mem_read8(cpu, buf + i);
        g_charm_icon_saved = true;
        g_icon_is_hood = false;
    }
    const bool want = hood && g_hood_icon_len != 0u;
    if (want == g_icon_is_hood)
        return;
    if (want && g_text_buf == 0u)
        return; // the block is not there yet: next time
    if (want)
        for (u32 i = 0; i < g_hood_icon_len; ++i)
            mem_write8(cpu, g_text_buf + ICON_AT + i, g_hood_icon[i]);
    const u32 n = g_hood_icon_len;
    g_icon_is_hood = want;
    g_icon_dirty = true;
    fprintf(stderr, "[bunny] icon: %s shown from %08X (%u bytes)\n", want ? "hood" : "charm", want ? g_text_buf + ICON_AT : buf, n);
}

// ------------------------------------------------------------- the slot ----

// While the Quest Status screen exists, the game's own bits for the charm slot (owned, worn) are those of the item
// the slot shows, so its pane, tint and Equip prompt work unchanged. What the player does with the worn bit is
// moved to the real item; the real bits come back when the screen goes.
static bool g_translated;
static u8 g_real;
static u8 g_last_v1;

static bool sel_is_hood(u8 real) {
    const bool c = (real & BLUEWAKE_BUNNY_CHARM_OWNED) != 0u, h = (real & BLUEWAKE_BUNNY_HOOD_OWNED) != 0u;
    if (h && !c)
        return true;
    if (c && !h)
        return false;
    return (real & BLUEWAKE_BUNNY_SLOT_HOOD) != 0u;
}

static u8 view_of(u8 real) {
    const bool hood = sel_is_hood(real);
    const u8 owned = hood ? (real & BLUEWAKE_BUNNY_HOOD_OWNED) != 0u : (real & BLUEWAKE_BUNNY_CHARM_OWNED) != 0u;
    const u8 worn = hood ? (real & BLUEWAKE_BUNNY_HOOD_WORN) != 0u : (real & BLUEWAKE_BUNNY_CHARM_WORN) != 0u;
    return (u8)((real & ~(BLUEWAKE_BUNNY_CHARM_OWNED | BLUEWAKE_BUNNY_CHARM_WORN)) | owned | (worn << 1));
}

// Only one of the two is ever worn: the one just put on wins.
static u8 normalize(u8 real, u8 just_worn) {
    if ((real & BLUEWAKE_BUNNY_CHARM_WORN) && (real & BLUEWAKE_BUNNY_HOOD_WORN)) {
        if (just_worn == BLUEWAKE_BUNNY_HOOD_WORN)
            real &= (u8)~BLUEWAKE_BUNNY_CHARM_WORN;
        else
            real &= (u8)~BLUEWAKE_BUNNY_HOOD_WORN;
    }
    if (!(real & BLUEWAKE_BUNNY_HOOD_OWNED))
        real &= (u8)~BLUEWAKE_BUNNY_HOOD_WORN;
    return real;
}

static void menu_sync(CPUState* cpu) {
    const u32 dmc = mem_read32(cpu, DMC_PTR);
    const bool menu = guest_ptr(dmc) && mem_read32(cpu, DMC_PTR) != 0u;
    u8 now = collect_byte(cpu);
    if (!menu) {
        if (g_translated) {
            // What the player did last frame, then the real byte.
            if (((now >> 1) & 1u) != g_last_v1) {
                const bool hood = sel_is_hood(g_real);
                const u8 bit = hood ? BLUEWAKE_BUNNY_HOOD_WORN : BLUEWAKE_BUNNY_CHARM_WORN;
                g_real = (u8)(((now >> 1) & 1u) ? (g_real | bit) : (g_real & ~bit));
                g_real = normalize(g_real, ((now >> 1) & 1u) ? bit : 0u);
            }
            mem_write8(cpu, BLUEWAKE_BUNNY_COLLECT, g_real);
            g_translated = false;
            g_charm_icon_saved = false; // the screen's buffers go with it
            g_icon_is_hood = false;
            redirect_charm_text(cpu, false);
            fprintf(stderr, "[bunny] slot byte %02X -> %02X on leaving the screen\n", now, g_real);
        }
        return;
    }
    if (!g_translated) {
        g_real = now;
        g_real = normalize(g_real, 0u);
        g_translated = true;
        g_last_v1 = (u8)(view_of(g_real) >> 1) & 1u;
        fprintf(stderr, "[bunny] slot byte %02X: the screen shows the %s\n", now,
                sel_is_hood(g_real) ? "Bunny Hood" : "Hero's Charm");
    } else if (((now >> 1) & 1u) != g_last_v1) {
        // The player put the shown item on or took it off.
        const bool hood = sel_is_hood(g_real);
        const u8 bit = hood ? BLUEWAKE_BUNNY_HOOD_WORN : BLUEWAKE_BUNNY_CHARM_WORN;
        const bool on = ((now >> 1) & 1u) != 0u;
        g_real = (u8)(on ? (g_real | bit) : (g_real & ~bit));
        g_real = normalize(g_real, on ? bit : 0u);
        fprintf(stderr, "[bunny] %s %s -> real byte %02X\n", hood ? "Bunny Hood" : "Hero's Charm", on ? "worn" : "taken off",
                g_real);
    }
    const u8 view = view_of(g_real);
    if (view != now)
        mem_write8(cpu, BLUEWAKE_BUNNY_COLLECT, view);
    g_last_v1 = (u8)(view >> 1) & 1u;
    const bool hood = sel_is_hood(g_real);
    show_icon(cpu, dmc, hood);
    redirect_charm_text(cpu, hood);
}

u8 bluewake_bunny_hood_real_collect(CPUState* cpu) { return g_translated ? g_real : collect_byte(cpu); }

void bluewake_bunny_hood_add_owned(CPUState* cpu, u8 owned_bits) {
    owned_bits &= (u8)(BLUEWAKE_BUNNY_CHARM_OWNED | BLUEWAKE_BUNNY_HOOD_OWNED);
    if (g_translated) {
        g_real |= owned_bits; // the screen's view follows on its next frame
    } else {
        mem_write8(cpu, BLUEWAKE_BUNNY_COLLECT, (u8)(collect_byte(cpu) | owned_bits));
    }
}

static void set_slot(CPUState* cpu, bool hood) {
    if (!g_translated)
        return;
    g_real = (u8)(hood ? (g_real | BLUEWAKE_BUNNY_SLOT_HOOD) : (g_real & ~BLUEWAKE_BUNNY_SLOT_HOOD));
    menu_sync(cpu);
}

// ------------------------------------------- hold A and press left or right ----

enum { SWAP_IDLE, SWAP_HELD };
static int g_swap_state = SWAP_IDLE;
static bool g_swap_armed, g_swap_moved;

// The swap animation: the shown icon shrinks away (frames 1..ANIM_MID), the slot changes to the other item at the
// middle, and the other icon grows in (ANIM_MID..ANIM_FRAMES). It runs from the dScnPly_Draw hook (menu_pass): that is
// after the pause screen's own update of the frame (which sets the icons' scale again each frame) and before the
// screen's panes draw, which happens when the draw lists run.
enum { ANIM_FRAMES = 12, ANIM_MID = 6 };
static int g_anim;         // 0 idle, else the frame
static bool g_anim_target; // the slot shows the hood when the middle comes
static bool g_sound_pending;

static void request_swap(void) {
    if (g_anim != 0)
        return;
    g_anim = 1;
    g_anim_target = !sel_is_hood(g_real);
    g_sound_pending = true;
    fprintf(stderr, "[bunny] swap: the slot will show the %s\n", g_anim_target ? "Bunny Hood" : "Hero's Charm");
}

static void hook_menu(CPUState* cpu) {
    const u32 dmc = cpu->gpr[3];
    if (!guest_ptr(dmc) || dmc != mem_read32(cpu, DMC_PTR))
        return;
    menu_sync(cpu);
    const bool both = (g_real & BLUEWAKE_BUNNY_CHARM_OWNED) && (g_real & BLUEWAKE_BUNNY_HOOD_OWNED);
    // noteCheck(): the description note (m7E8.mUserArea, pane_class +0x36) is not up.
    const bool note_up = mem_read16(cpu, dmc + 0x7E8u + 0x36u) == 1u;
    const bool on_slot = mem_read8(cpu, dmc + DMC_NOW_ITEM) == NOW_ITEM_CHARM && mem_read8(cpu, dmc + DMC_MODE) == 0u && !note_up;
    if (!both || !on_slot) {
        g_swap_state = SWAP_IDLE;
        return;
    }
    u16 hold = (u16)mem_read16(cpu, PAD + PAD_HOLD), trig = (u16)mem_read16(cpu, PAD + PAD_TRIG);
    const float stick = read_f32(cpu, PAD);
    const bool left = (hold & PAD_LEFT) != 0u || stick < -0.5f, right = (hold & PAD_RIGHT) != 0u || stick > 0.5f;
    if (g_swap_state == SWAP_IDLE) {
        if ((trig & PAD_A) == 0u)
            return;
        // A went down: the screen does not see it until it is known whether this is a swap.
        g_swap_state = SWAP_HELD;
        g_swap_armed = !(left || right);
        g_swap_moved = false;
        mem_write16(cpu, PAD + PAD_TRIG, (u16)(trig & ~PAD_A));
        return;
    }
    if ((hold & PAD_A) == 0u) {
        // Let go. Without a swap this was a plain A: the screen gets it now.
        g_swap_state = SWAP_IDLE;
        if (!g_swap_moved)
            mem_write16(cpu, PAD + PAD_TRIG, (u16)(trig | PAD_A));
        return;
    }
    // Held: the stick and the pad's left and right belong to the swap, not to the cursor.
    if (!(left || right))
        g_swap_armed = true;
    else if (g_swap_armed) {
        g_swap_armed = false;
        g_swap_moved = true;
        request_swap();
    }
    write_f32(cpu, PAD, 0.0f);
    write_f32(cpu, PAD + 4u, 0.0f);
    write_f32(cpu, PAD + 8u, 0.0f);
    mem_write16(cpu, PAD + PAD_HOLD, (u16)(hold & ~(PAD_LEFT | PAD_RIGHT)));
    mem_write16(cpu, PAD + PAD_TRIG, (u16)(trig & ~(PAD_LEFT | PAD_RIGHT | PAD_A)));
}

// ---------------------------------------------------------- the effects ----

static u32 hook_item_get(CPUState* cpu) {
    if ((cpu->gpr[3] & 0xFFu) != BLUEWAKE_BUNNY_HOOD_ITEM)
        return 0u;
    u8 byte = collect_byte(cpu);
    if (g_translated) // an item cannot arrive with the pause screen up, but if it does the real byte gets it
        g_real |= BLUEWAKE_BUNNY_HOOD_OWNED;
    else {
        byte |= BLUEWAKE_BUNNY_HOOD_OWNED;
        mem_write8(cpu, BLUEWAKE_BUNNY_COLLECT, byte);
    }
    fprintf(stderr, "[bunny] item 0x%02X got: collect byte %02X\n", BLUEWAKE_BUNNY_HOOD_ITEM, byte);
    return 0u;
}

// checkNextMode, after mMaxNormalSpeed (0x2A8(r31)) is chosen. r24's low byte is nonzero in the lock-on case.
static u32 hook_speed(CPUState* cpu) {
    if (!g_worn || (cpu->gpr[24] & 0xFFu) != 0u)
        return 0u;
    const u32 player = cpu->gpr[31];
    if (!guest_ptr(player))
        return 0u;
    write_f32(cpu, player + 0x2A8u, read_f32(cpu, player + 0x2A8u) * SPEED_FACTOR);
    return 0u;
}

// ---- the pause screen, once a frame (called from the dScnPly_Draw hook's fiber) ----

enum {
    FN_PANE_SCALE_XY = 0x8003BD50u,  // fopMsgM_paneScaleXY(fopMsgM_pane_class*, float)
    FN_CHANGE_TEXTURE = 0x802D3B78u, // J2DPicture::changeTexture(const ResTIMG*, u8)
    FN_ITEMNAME_SET = 0x801A2CA4u,   // dMenu_Collect_c::itemnameSet()
    FN_SE_START = 0x800F0D10u,       // mDoAud_seStart(id, pos, param, reverb)
    SE_SWAP = 0x080Eu,               // JA_SE_ITM_MENU_CURSOR
    DMC_ICON_A = 0x2030u,            // m2030[5]: the item pictures
    DMC_ICON_B = 0x2148u,            // m2148[5]: the same pictures, the cursor's
    PANE_CLASS = 0x38u,
    PANE_SIZE_ORIG = 0x24u,
    PANE_SIZE = 0x2Cu,
};

static float smooth(float x) { return x * x * (3.0f - 2.0f * x); }

static float anim_scale(int t) {
    if (t >= ANIM_MID)
        return smooth((float)(t - ANIM_MID) / (float)(ANIM_FRAMES - ANIM_MID));
    return 1.0f - smooth((float)t / (float)ANIM_MID);
}

// BLUEWAKE_BUNNY_AUTOSWAP=n: a test, the slot swaps by itself every n frames while the screen is up (and both are owned).
static int auto_swap_frames(void) {
    static int n = -1;
    if (n < 0) {
        const char* v = getenv("BLUEWAKE_BUNNY_AUTOSWAP");
        n = v != NULL ? atoi(v) : 0;
    }
    return n;
}

static bool menu_pass_needed(CPUState* cpu) {
    return g_translated && guest_ptr(mem_read32(cpu, DMC_PTR));
}

static void menu_pass(GuestCoro* co, CPUState* cpu) {
    // The screen made this frame (the game's execute runs before its draw, the host's retrace after it) is translated now, so its
    // first drawn frame already has the slot's item and the hood's picture: otherwise the Hero's Charm showed for one frame (BR-0006).
    menu_sync(cpu);
    const u32 dmc = mem_read32(cpu, DMC_PTR);
    if (!guest_ptr(dmc) || !g_translated) {
        g_anim = 0;
        return;
    }
    static int idle_frames;
    const int every = auto_swap_frames();
    if (every > 0 && g_anim == 0 && (g_real & BLUEWAKE_BUNNY_CHARM_OWNED) && (g_real & BLUEWAKE_BUNNY_HOOD_OWNED) &&
        ++idle_frames >= every) {
        idle_frames = 0;
        request_swap();
    }
    if (g_anim > 0) {
        ++g_anim;
        if (g_anim == ANIM_MID) {
            set_slot(cpu, g_anim_target);
            // The item-name pane was filled for the old item: build it again (BR-0032).
            bluewake_guest_call(co, FN_ITEMNAME_SET, dmc, 0, 0, 0, 0, 0);
        }
        if (g_anim > ANIM_FRAMES)
            g_anim = 0;
    }
    if (g_sound_pending) {
        g_sound_pending = false;
        bluewake_guest_call(co, FN_SE_START, SE_SWAP, 0, 0, 0, 0, 0);
    }
    if (g_icon_dirty) {
        // The panes made their texture objects when the screen was made: tell them the picture's bytes changed.
        const u32 buf = icon_buffer(cpu, dmc);
        if (buf != 0u) {
            for (int k = 0; k < 2; ++k) {
                const u32 pane = mem_read32(cpu, dmc + (k == 0 ? DMC_ICON_A : DMC_ICON_B) + 4u * PANE_CLASS);
                if (guest_ptr(pane)) {
                    bluewake_guest_call(co, FN_CHANGE_TEXTURE, pane, g_icon_is_hood ? g_text_buf + ICON_AT : buf, 0, 0, 0, 0);
                    fprintf(stderr, "[bunny] icon: pane %08X reads its texture again\n", pane);
                }
            }
        }
        g_icon_dirty = false;
    }
    if (g_anim == 0)
        return;
    const float scale = anim_scale(g_anim);
    for (int k = 0; k < 2; ++k) {
        const u32 pane = dmc + (k == 0 ? DMC_ICON_A : DMC_ICON_B) + 4u * PANE_CLASS;
        const float orig = read_f32(cpu, pane + PANE_SIZE_ORIG), now = read_f32(cpu, pane + PANE_SIZE);
        if (orig < 1.0f)
            continue;
        cpu->fpr[1] = now / orig * scale;
        cpu->ps1[1] = now / orig * scale;
        bluewake_guest_call(co, FN_PANE_SCALE_XY, pane, 0, 0, 0, 0, 0);
    }
}

u32 bluewake_bunny_hood_hook(CPUState* cpu, u32 id) {
    if (!g_enabled)
        return 0u;
    switch (id) {
    case BLUEWAKE_BUNNY_HOOK_ITEM_GET:
        return hook_item_get(cpu);
    case BLUEWAKE_BUNNY_HOOK_SPEED:
        return hook_speed(cpu);
    case BLUEWAKE_BUNNY_HOOK_MENU:
        hook_menu(cpu);
        return 0u;
    case BLUEWAKE_BUNNY_HOOK_MENU_END:
        menu_sync(cpu);
        g_anim = 0;
        return 0u;
    default:
        return 0u;
    }
}

static void colors_poll(void);

void bluewake_bunny_hood_retrace(CPUState* cpu) {
    if (!g_enabled) {
        g_worn = false;
        return;
    }
    if (cpu == NULL || cpu->ram == NULL)
        return;
    patch_item_tables(cpu);
    colors_poll();
    menu_sync(cpu);
    if (!g_translated) {
        g_anim = 0; // the screen went away mid-swap
        g_sound_pending = false;
    }
    // Worn now: the real bits outside the pause screen, the hood's own bit inside it.
    const u8 byte = g_translated ? g_real : collect_byte(cpu);
    g_worn = (byte & BLUEWAKE_BUNNY_HOOD_WORN) != 0u && (byte & BLUEWAKE_BUNNY_HOOD_OWNED) != 0u &&
             (byte & BLUEWAKE_BUNNY_CHARM_WORN) == 0u;
    if (!g_translated) {
        const u8 fixed = normalize(byte, 0u);
        if (fixed != byte)
            mem_write8(cpu, BLUEWAKE_BUNNY_COLLECT, fixed);
    }
    text_retrace(cpu);
}

// ------------------------------------------------------------ the model ----

enum {
    PLAYER = 0x803CA754u, // play.mpPlayerPtr[0]
    PLAYER_CL_MODEL = 0x32Cu,
    PLAYER_POS = 0x1F8u,
    ACTOR_STATUS = 0x1C4u,            // fopAc_ac_c::actor_status; fopAcStts_NODRAW_e
    ACTOR_STATUS_NODRAW = 0x01000000u,
    ACTOR_CONDITION = 0x1C8u,         // fopAc_ac_c::actor_condition; fopAcCnd_NODRAW_e
    ACTOR_CONDITION_NODRAW = 0x04u,
    PLAYER_NO_RESET_FLG0 = 0x29Cu,    // daPy_py_c::mNoResetFlg0; daPyFlg0_NO_DRAW
    PLAYER_FLG0_NO_DRAW = 0x08000000u,
    PLAYER_CAMERA_INFO_IDX = 0x356Cu, // daPy_lk_c::mCameraInfoIdx
    CAMERA_ATTENTION_STATUS = 0x803C4C08u + 0x12A0u + 0x4870u + 8u, // g_dComIfG_gameInfo.play.mCameraInfo[0].mCameraAttentionStatus
    CAM_ATTN_SUBJECT = 0x02u,        // dCamAttnStts_SUBJECT_e
    CAM_ATTN_FIRST_PERSON = 0x20u,   // dCamAttnStts_00000020_e
    STAY_ROOM = 0x803F6A78u,
    ENV_LIGHT = 0x803E4AB4u,
    J3D_SYS = 0x803EDA58u,
    DRAW_LIST = 0x803C4C08u + 0x5D1Cu,
    DRAW_OPA_P1 = 0x0Cu,
    MODEL_DATA = 0x04u,
    MODEL_BASE_MTX = 0x24u,
    MODEL_NODE_MTX = 0x8Cu,
    CL_JNT_HEAD = 0x0F,

    FN_CREATE_HEAP_TO_CURRENT = 0x80011CC0u,
    FN_ADJUST_HEAP = 0x80011D0Cu,
    FN_DESTROY_HEAP = 0x80011D48u,
    FN_RESTORE_HEAP = 0x80011DDCu,
    FN_MODEL_CREATE = 0x80016BB8u,
    FN_NEW = 0x802B0C38u,
    FN_NEW_ALIGNED = 0x802B0C60u,
    FN_TEVSTR_INIT = 0x80196EB4u,
    FN_SETTING_TEVSTRUCT = 0x80193028u,
    FN_SET_LIGHT_TEV = 0x80193A34u,
    FN_MODEL_ENTRY_DL = 0x8000F974u,
    FN_LOAD_BDL = 0x802FB94Cu,
    FN_MODEL_CALC = 0x802EE8C0u,
    BDL_LOAD_FLAGS = 0x00002020u,

    MAX_WEARERS = 17, // the local player and Anchor's puppets
    SLOT_HEAP = 0x20000,
};

typedef struct Mat34 {
    float m[3][4];
} Mat34;

static void read_mat(CPUState* cpu, u32 address, Mat34* m) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            m->m[i][j] = read_f32(cpu, address + 16u * (u32)i + 4u * (u32)j);
}

static void write_mat(CPUState* cpu, u32 address, const Mat34* m) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            write_f32(cpu, address + 16u * (u32)i + 4u * (u32)j, m->m[i][j]);
}

// The generated model, read once: assets/bunnyhood/bunnyhood.arc (RARC, uncompressed) holding bdl/bunnyhood.bdl, or a
// bare bunnyhood.bdl beside it.
static u8* g_bdl;
static u32 g_bdl_len;
static bool g_bdl_tried;

static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u32 be16(const u8* p) { return (u32)p[0] << 8 | p[1]; }

static u8* read_whole(const char* path, u32* length) {
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 8 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    u8* data = (u8*)malloc((size_t)n);
    if (data != NULL && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *length = (u32)n;
    return data;
}

// A file of an uncompressed RARC by its leaf name: the bytes in place, or NULL.
static const u8* rarc_find(const u8* arc, u32 length, const char* leaf, u32* size) {
    if (length < 0x40 || memcmp(arc, "RARC", 4) != 0)
        return NULL;
    const u32 data_at = 0x20u + be32(arc + 0x0C);
    const u8* info = arc + 0x20;
    const u32 files = be32(info + 8), file_at = 0x20u + be32(info + 12), names_at = 0x20u + be32(info + 20);
    for (u32 i = 0; i < files; ++i) {
        const u8* e = arc + file_at + 0x14u * i;
        if (file_at + 0x14u * (i + 1u) > length)
            break;
        if ((be16(e + 4) & 0x0100u) == 0u) // a directory entry
            continue;
        if (names_at + be16(e + 6) >= length)
            continue;
        const char* name = (const char*)arc + names_at + be16(e + 6);
        if (strcmp(name, leaf) != 0)
            continue;
        const u32 off = data_at + be32(e + 8), n = be32(e + 12);
        if (off + n > length)
            return NULL;
        *size = n;
        return arc + off;
    }
    return NULL;
}

static void load_model_file(void) {
    if (g_bdl_tried)
        return;
    g_bdl_tried = true;
    char path[1024];
    u32 n = 0;
    const char* test_model = getenv("BLUEWAKE_BUNNY_MODEL"); // a test: another file of assets/bunnyhood as the head model
    if (extras_file(test_model != NULL ? test_model : "bunnyhood.bdl", path, sizeof path) && (g_bdl = read_whole(path, &n)) != NULL) {
        g_bdl_len = n;
        fprintf(stderr, "[bunny] model: %u bytes from %s\n", n, path);
        return;
    }
    if (extras_file("bunnyhood.arc", path, sizeof path)) {
        u8* arc = read_whole(path, &n);
        u32 size = 0;
        const u8* bdl = arc != NULL ? rarc_find(arc, n, "bunnyhood.bdl", &size) : NULL;
        if (bdl != NULL && (g_bdl = (u8*)malloc(size)) != NULL) {
            memcpy(g_bdl, bdl, size);
            g_bdl_len = size;
            fprintf(stderr, "[bunny] model: %u bytes from %s\n", size, path);
        } else
            fprintf(stderr, "[bunny] %s holds no bunnyhood.bdl\n", path);
        free(arc);
        return;
    }
    fprintf(stderr, "[bunny] no model in assets/bunnyhood: the hood is not drawn\n");
}

// JNT1's joint names: the indices of the ear joints. Names of the generated model (docs/fork/bunny-hood.md Part B).
enum { EARS = 4, JOINT_NONE = 0xFFFF };
static const char* const kEarNames[EARS] = {"ear_L0", "ear_L1", "ear_R0", "ear_R1"};
static u32 g_ear[EARS] = {JOINT_NONE, JOINT_NONE, JOINT_NONE, JOINT_NONE};
// Band joints named follow_<N>: they follow Link's cl.bdl joint N (the hat's joints the band is weighted to).
enum { MAX_FOLLOW = 6 };
static u32 g_follow_joint[MAX_FOLLOW];
static int g_follow_link[MAX_FOLLOW];
static int g_follow_count;
static u32 g_joints;

static bool parse_joints(void) {
    if (g_bdl_len < 0x40 || memcmp(g_bdl, "J3D2bdl4", 8) != 0)
        return false;
    g_follow_count = 0;
    const u32 chunks = be32(g_bdl + 12);
    u32 at = 0x20u;
    for (u32 c = 0; c < chunks && at + 8u <= g_bdl_len; ++c) {
        const u32 size = be32(g_bdl + at + 4);
        if (memcmp(g_bdl + at, "JNT1", 4) == 0) {
            g_joints = be16(g_bdl + at + 8);
            const u32 names = at + be32(g_bdl + at + 20);
            const u32 remap = at + be32(g_bdl + at + 16);
            if (names + 4u > g_bdl_len)
                return false;
            const u32 count = be16(g_bdl + names);
            for (u32 k = 0; k < count && k < g_joints; ++k) {
                const u32 name_at = names + be16(g_bdl + names + 4u + 4u * k + 2u);
                if (name_at >= g_bdl_len)
                    return false;
                const u32 joint = be16(g_bdl + remap + 2u * k);
                for (int e = 0; e < EARS; ++e)
                    if (strncmp((const char*)g_bdl + name_at, kEarNames[e], 16) == 0)
                        g_ear[e] = joint;
                if (strncmp((const char*)g_bdl + name_at, "follow_", 7) == 0 && g_follow_count < MAX_FOLLOW) {
                    g_follow_joint[g_follow_count] = joint;
                    g_follow_link[g_follow_count++] = atoi((const char*)g_bdl + name_at + 7);
                }
            }
            if (g_follow_count != 0)
                fprintf(stderr, "[bunny] %d band joints follow Link's hat joints\n", g_follow_count);
            for (int e = 0; e < EARS; ++e)
                if (g_ear[e] == JOINT_NONE)
                    fprintf(stderr, "[bunny] the model has no joint %s: that ear does not move\n", kEarNames[e]);
            return true;
        }
        at += size;
    }
    return false;
}

// Per wearer: the J3DModel, its light struct and position, and the ears' springs.
typedef struct Wearer {
    u32 heap, model, tev, pos;
    bool failed;
    float angle[EARS][2], speed[EARS][2];
    float last_head[3], last_velocity[3], last_accel[3];
    bool have_last;
} Wearer;

static Wearer g_wearer[MAX_WEARERS];
static u32 g_data; // the J3DModelData
static u32 g_data_heap;
static bool g_data_failed;

#define CALL(fn, a, b, c, d) bluewake_guest_call(co, (fn), (a), (b), (c), (d), 0, 0)

// ----- the colours (Cosmetics Editor, "Bunny Hood") -----
// The editor keeps three colours by id in cosmetics.c ("BunnyHood.Body", ".InnerEars", ".Band"). They are written into a
// copy of the model's bytes before the game's loader reads it: each region is a material of the model (names in
// kRegions), whose colour sits in one of the material's four TEV colour registers (MAT3's tevColor table, s10 RGBA);
// the model then multiplies or replaces its texture with it. The model is loaded again when a colour changes.
typedef struct Region {
    const char* id;       // cosmetics id
} Region;
enum { REGIONS = 3, TARGETS = 4 };
static const Region kRegions[REGIONS] = {{"BunnyHood.Body"}, {"BunnyHood.InnerEars"}, {"BunnyHood.Band"}};
// What each colour is written to (docs/fork/bunny-hood.md Part B): the model's materials hold the colours in TEV colour
// registers, the Body in both the fur and the inner ear (whose texture mask blends C0, the body, with C1, the inner ear).
typedef struct Target {
    int region;           // index into kRegions
    const char* material; // MAT3 name
    int reg;              // 0 or 1: the logical C0 / C1 = the material's tevColor index 1 / 2 (index 0 is rewritten by the game's lighting)
    int material_no;      // 0 fur, 1 inner_ear, 2 band: the row of hood.json's mdl3_patch
} Target;
static const Target kTargets[TARGETS] = {
    {0, "fur", 0, 0}, {0, "inner_ear", 0, 1}, {1, "inner_ear", 1, 1}, {2, "band", 0, 2}};
// hood.json's mdl3_patch.head: file offsets of the BP colour writes in the display lists, per material (fur, inner_ear,
// band) and register (C0, C1): one offset for the R/A write and three for the B/G write, 4 numbers each.
static u32 g_mdl3[3][2][4];
static bool g_mdl3_ok, g_mdl3_tried;
// The loaded model's display lists are the file's bytes in the guest buffer g_dl_buf: the colours as loaded (cosmetics
// applied) are kept here, and the lighting pass (BR-0012) rewrites the BP colour writes with them scaled by the scene's light.
static u32 g_dl_buf;
static u8 g_base[3][2][3];
static bool g_base_ok;
// The same colours in the model's material blocks (the draw patches the display lists from them): where each is, found once.
typedef struct BlockColor { u32 at; u8 base[3]; } BlockColor;
static BlockColor g_blk[32];
static int g_blk_n;
static bool g_blk_scanned;
static u8 g_applied_rgb[REGIONS][4]; // what the loaded data has; [3] = 1 when a colour was applied
static bool g_recolor;               // a colour changed since the data was made: make it again

static void load_mdl3_offsets(void) {
    char path[1024];
    u32 n = 0;
    if (!extras_file("hood.json", path, sizeof path))
        return;
    u8* json = read_whole(path, &n);
    if (json == NULL)
        return;
    char* text = (char*)realloc(json, n + 1u);
    if (text == NULL)
        return;
    text[n] = '\0';
    const char* at = strstr(text, "\"mdl3_patch\"");
    if (at != NULL)
        at = strstr(at, "\"head\"");
    u32 values[24];
    int count = 0;
    while (at != NULL && *at != '\0' && count < 24) {
        if (*at >= '0' && *at <= '9') {
            char* end;
            const unsigned long v = strtoul(at, &end, 10);
            if (v >= 1000ul) // an offset (the register names C0 and C1 are quoted and small)
                values[count++] = (u32)v;
            at = end;
        } else
            ++at;
    }
    if (count == 24) {
        memcpy(g_mdl3, values, sizeof g_mdl3);
        g_mdl3_ok = true;
    } else
        fprintf(stderr, "[bunny] hood.json has no mdl3_patch: recolouring changes only the material table\n");
    free(text);
}

static void colors_poll(void) {
    for (int i = 0; i < REGIONS; ++i) {
        u8 rgba[4] = {0, 0, 0, 0};
        const bool on = bluewake_cosmetics_get(kRegions[i].id, rgba);
        const u8 now[4] = {rgba[0], rgba[1], rgba[2], (u8)(on ? 1 : 0)};
        if (memcmp(now, g_applied_rgb[i], 4) != 0)
            g_recolor = true;
    }
}

static void put24(u8* bdl, u32 len, u32 off, u32 value) {
    if (off + 3u > len)
        return;
    bdl[off] = (u8)(value >> 16);
    bdl[off + 1] = (u8)(value >> 8);
    bdl[off + 2] = (u8)value;
}

// Writes the chosen colours into a model copy. Returns how many targets were set.
static int colors_apply(u8* bdl, u32 len) {
    int done = 0;
    u32 at = 0x20u;
    const u32 chunks = be32(bdl + 12);
    if (!g_mdl3_tried) {
        g_mdl3_tried = true;
        load_mdl3_offsets();
    }
    for (u32 c = 0; c < chunks && at + 8u <= len; ++c) {
        const u32 size = be32(bdl + at + 4);
        if (memcmp(bdl + at, "MAT3", 4) != 0) {
            at += size;
            continue;
        }
        const u32 count = be16(bdl + at + 8);
        const u32 init = at + be32(bdl + at + 12), remap = at + be32(bdl + at + 16), names = at + be32(bdl + at + 20);
        const u32 table = at + be32(bdl + at + 12 + 4 * 17); // tevColor (s16 x 4 each)
        const u32 nnames = be16(bdl + names);
        for (int t = 0; t < TARGETS; ++t) {
            const Target* tg = &kTargets[t];
            u8 rgba[4];
            if (!bluewake_cosmetics_get(kRegions[tg->region].id, rgba))
                continue;
            for (u32 k = 0; k < nnames && k < count; ++k) {
                const u32 name_at = names + be16(bdl + names + 4u + 4u * k + 2u);
                if (name_at >= len || strcmp((const char*)bdl + name_at, tg->material) != 0)
                    continue;
                const u32 entry = init + 0x14Cu * be16(bdl + remap + 2u * k);
                const u32 idx = be16(bdl + entry + 0xDCu + 2u * ((u32)tg->reg + 1u)); // tev colour 1 and 2 (0 is the game's light); 0xDC: the material's tevColor index list
                if (idx == 0xFFFFu || table + 8u * (idx + 1u) > len)
                    break;
                u8* tc = bdl + table + 8u * idx; // s16 r, g, b, a
                for (int ch = 0; ch < 3; ++ch) {
                    tc[2 * ch] = 0;
                    tc[2 * ch + 1] = rgba[ch];
                }
                ++done;
                break;
            }
            if (g_mdl3_ok) {
                const u32* o = g_mdl3[tg->material_no][tg->reg];
                put24(bdl, len, o[0], (255u << 12) | rgba[0]); // R, A
                for (int j = 1; j < 4; ++j)
                    put24(bdl, len, o[j], ((u32)rgba[1] << 12) | rgba[2]); // G, B
            }
        }
        break;
    }
    return done;
}

static bool make_data(GuestCoro* co, CPUState* cpu) {
    load_model_file();
    if (g_bdl == NULL || !parse_joints()) {
        g_data_failed = true;
        return false;
    }
    u32 heap = 0u;
    const u32 want[3] = {g_bdl_len * 3u + 0x30000u, g_bdl_len * 2u + 0x20000u, g_bdl_len + 0x18000u};
    for (int k = 0; k < 3 && heap == 0u; ++k)
        heap = CALL(FN_CREATE_HEAP_TO_CURRENT, want[k], 0x20u, 0, 0);
    if (heap == 0u) {
        g_data_failed = true;
        fprintf(stderr, "[bunny] no heap for the model\n");
        return false;
    }
    u32 data = 0u;
    const u32 buf = CALL(FN_NEW_ALIGNED, g_bdl_len, 0x20u, 0, 0);
    if (buf != 0u) {
        u8* copy = (u8*)malloc(g_bdl_len);
        if (copy != NULL) {
            memcpy(copy, g_bdl, g_bdl_len);
            const int set = colors_apply(copy, g_bdl_len);
            if (g_mdl3_ok) {
                for (int m = 0; m < 3; ++m)
                    for (int r = 0; r < 2; ++r) {
                        const u32* o = g_mdl3[m][r];
                        if (o[0] + 3u > g_bdl_len || o[1] + 3u > g_bdl_len)
                            continue;
                        const u32 ra = ((u32)copy[o[0]] << 16 | (u32)copy[o[0] + 1] << 8 | copy[o[0] + 2]) & 0x7FFu;
                        const u32 gb = (u32)copy[o[1]] << 16 | (u32)copy[o[1] + 1] << 8 | copy[o[1] + 2];
                        g_base[m][r][0] = (u8)ra;
                        g_base[m][r][1] = (u8)((gb >> 12) & 0x7FFu);
                        g_base[m][r][2] = (u8)(gb & 0x7FFu);
                    }
                g_dl_buf = buf;
                g_base_ok = true;
            }
            if (set != 0)
                fprintf(stderr, "[bunny] %d colour regions recoloured\n", set);
        }
        for (int i = 0; i < REGIONS; ++i) {
            u8 rgba[4] = {0, 0, 0, 0};
            const bool on = bluewake_cosmetics_get(kRegions[i].id, rgba);
            g_applied_rgb[i][0] = rgba[0];
            g_applied_rgb[i][1] = rgba[1];
            g_applied_rgb[i][2] = rgba[2];
            g_applied_rgb[i][3] = (u8)(on ? 1 : 0);
        }
        g_recolor = false;
        for (u32 i = 0; i < g_bdl_len; ++i)
            mem_write8(cpu, buf + i, copy != NULL ? copy[i] : g_bdl[i]);
        free(copy);
        data = CALL(FN_LOAD_BDL, buf, getenv("BLUEWAKE_BUNNY_LOADFLAGS") != NULL ? (u32)strtoul(getenv("BLUEWAKE_BUNNY_LOADFLAGS"), NULL, 0) : BDL_LOAD_FLAGS, 0, 0);
    }
    CALL(FN_ADJUST_HEAP, heap, 0, 0, 0);
    CALL(FN_RESTORE_HEAP, 0, 0, 0, 0);
    if (!guest_ptr(data)) {
        CALL(FN_DESTROY_HEAP, heap, 0, 0, 0);
        g_data_failed = true;
        fprintf(stderr, "[bunny] the loader refused the model (buffer %08X)\n", buf);
        return false;
    }
    g_data = data;
    g_data_heap = heap;
    fprintf(stderr, "[bunny] model data %08X in heap %08X, %u joints\n", data, heap, mem_read16(cpu, data + 0x28u));
    return true;
}

// Heap attempts that failed since the last success or scene change (make_wearer, teardown).
static int g_heap_attempts;

static bool make_wearer(GuestCoro* co, CPUState* cpu, Wearer* w, int room) {
    // The game's heap can be tight (a vanilla Outset session after the item-get): a smaller slot is tried, and a failure is
    // retried on later frames, every frame for 120 frames and then twice a second, never given up for good. The count used
    // to last the whole session, so once 120 failures had added up the hood stopped drawing for the rest of it while its
    // speed kept working.
    static const u32 sizes[3] = {SLOT_HEAP, 0x14000u, 0xC000u};
    if (g_heap_attempts > 120 && (g_heap_attempts++ % 15) != 0)
        return false;
    u32 heap = 0u;
    for (int k = 0; k < 3 && heap == 0u; ++k)
        heap = CALL(FN_CREATE_HEAP_TO_CURRENT, sizes[k], 0x20u, 0, 0);
    if (heap == 0u) {
        if (g_heap_attempts++ == 0)
            fprintf(stderr, "[bunny] no heap for the wearer's model: trying again on later frames\n");
        return false;
    }
    if (g_heap_attempts != 0)
        fprintf(stderr, "[bunny] the wearer's heap was made after %d tries\n", g_heap_attempts);
    g_heap_attempts = 0;
    const u32 model = CALL(FN_MODEL_CREATE, g_data, 0x80000u, getenv("BLUEWAKE_BUNNY_DIFFER") != NULL ? (u32)strtoul(getenv("BLUEWAKE_BUNNY_DIFFER"), NULL, 0) : 0x11000022u, 0);
    const u32 tev = CALL(FN_NEW, 0xB0u, 0, 0, 0);
    const u32 pos = CALL(FN_NEW, 0x20u, 0, 0, 0);
    CALL(FN_ADJUST_HEAP, heap, 0, 0, 0);
    CALL(FN_RESTORE_HEAP, 0, 0, 0, 0);
    if (model == 0u || tev == 0u || pos == 0u) {
        CALL(FN_DESTROY_HEAP, heap, 0, 0, 0);
        w->failed = true;
        fprintf(stderr, "[bunny] the wearer's model could not be made (model %08X, tev %08X, pos %08X, heap %u)\n", model, tev, pos, SLOT_HEAP);
        return false;
    }
    for (u32 i = 0; i < 0xB0u; i += 4u)
        mem_write32(cpu, tev + i, 0u);
    CALL(FN_TEVSTR_INIT, tev, (u32)room, 0xFFu, 0);
    w->heap = heap;
    w->model = model;
    w->tev = tev;
    w->pos = pos;
    fprintf(stderr, "[bunny] wearer model %08X (heap %08X)\n", model, heap);
    return true;
}

// ----- the ears -----
// Head motion drives each ear's two angles (bend forward/back and lean sideways) through a damped spring, the first
// joint of an ear stiffer than its tip. All in radians and frames (the game draws 30 a second).
// The head joint's frame is +X up, +Y front, Z sideways (cl.bdl joint 15).
static const float kStiff[2] = {0.10f, 0.045f};  // spring per frame^2
static const float kDamp[2] = {0.20f, 0.12f};    // per frame
static const float kDrive[2] = {0.0075f, 0.0100f}; // acceleration (units/frame^2) to angular acceleration: 1 unit gives ~0.1 rad
static const float kLimit[2] = {0.35f, 0.45f};     // 20 and 26 degrees: an ear's two joints stack to under 46
static const float kAccelClamp = 2.0f;             // units/frame^2 of head acceleration taken in
static const float kJump = 30.0f;                  // a head that moved more than this in a frame was moved, not walked
// Which of a joint's rotation angles (x, y, z) each spring writes: the bend and the lean. From Part B of
// docs/fork/bunny-hood.md: each ear joint's +X runs along the ear; bend about local Z (+ tips the ear forward), lean
// about local Y (+ tips the left ear inward, the right ear outward).
static int g_axis_bend = 2, g_axis_lean = 1;
static float g_sign_bend = 1.0f, g_sign_lean = 1.0f;

static void step_ears(Wearer* w, const Mat34* head) {
    float velocity[3] = {0, 0, 0}, accel[3] = {0, 0, 0};
    if (w->have_last) {
        float moved = 0.0f;
        for (int k = 0; k < 3; ++k) {
            velocity[k] = head->m[k][3] - w->last_head[k];
            moved += velocity[k] * velocity[k];
        }
        if (moved > kJump * kJump) { // a spawn or warp: the springs rest
            for (int k = 0; k < 3; ++k)
                velocity[k] = 0.0f;
        } else
            for (int k = 0; k < 3; ++k) {
                accel[k] = velocity[k] - w->last_velocity[k];
                accel[k] = accel[k] > kAccelClamp ? kAccelClamp : accel[k] < -kAccelClamp ? -kAccelClamp : accel[k];
                accel[k] = 0.7f * w->last_accel[k] + 0.3f * accel[k]; // a low pass: walking bobs the head
            }
    }
    for (int k = 0; k < 3; ++k)
        w->last_accel[k] = accel[k];
    for (int k = 0; k < 3; ++k) {
        w->last_head[k] = head->m[k][3];
        w->last_velocity[k] = velocity[k];
    }
    w->have_last = true;
    // The acceleration in the head's own frame: the rotation's transpose times it.
    float local[3];
    for (int r = 0; r < 3; ++r)
        local[r] = head->m[0][r] * accel[0] + head->m[1][r] * accel[1] + head->m[2][r] * accel[2];
    for (int e = 0; e < EARS; ++e) {
        const int seg = e & 1;
        const float drive_bend = -local[1] * kDrive[seg] - local[0] * kDrive[seg] * 0.5f; // forward and up push the ear back / down
        const float drive_lean = local[2] * kDrive[seg] * (e < 2 ? 1.0f : -1.0f); // "inward" is positive
        const float drives[2] = {drive_bend, drive_lean};
        for (int a = 0; a < 2; ++a) {
            float* angle = &w->angle[e][a];
            float* speed = &w->speed[e][a];
            *speed += drives[a] - kStiff[seg] * *angle - kDamp[seg] * *speed;
            *angle += *speed;
            if (*angle > kLimit[seg]) {
                *angle = kLimit[seg];
                *speed = 0.0f;
            } else if (*angle < -kLimit[seg]) {
                *angle = -kLimit[seg];
                *speed = 0.0f;
            }
        }
    }
}

static s16 to_s16(float radians) { return (s16)lrintf(radians * (32768.0f / 3.14159265f)); }

// ----- the band follows Link's hat -----
// Link's hat swings on its own joints while the band is fitted to the bind pose: a follower joint takes the movement of
// Link's joint N relative to his head, from its bind pose, so the band sits on the hat as it is drawn.
enum { LINK_JOINTS = 42, FOLLOW_HEAD = 15 };

static void m_mul(const Mat34* a, const Mat34* b, Mat34* out) {
    Mat34 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
        r.m[i][3] = a->m[i][0] * b->m[0][3] + a->m[i][1] * b->m[1][3] + a->m[i][2] * b->m[2][3] + a->m[i][3];
    }
    *out = r;
}

static bool m_inv(const Mat34* in, Mat34* out) {
    const float(*a)[4] = in->m;
    const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                      a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabsf(det) < 1e-12f)
        return false;
    const float d = 1.0f / det;
    Mat34 r;
    r.m[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * d;
    r.m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * d;
    r.m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * d;
    r.m[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * d;
    r.m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * d;
    r.m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * d;
    r.m[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * d;
    r.m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * d;
    r.m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * d;
    for (int i = 0; i < 3; ++i)
        r.m[i][3] = -(r.m[i][0] * a[0][3] + r.m[i][1] * a[1][3] + r.m[i][2] * a[2][3]);
    *out = r;
    return true;
}

// J3DGetTranslateRotateMtx: the game's own composition of a joint's rotation (s16 x, y, z) and translation.
static void srt_to_mat(const s16 rot[3], const float t[3], Mat34* m) {
    const float k = 3.14159265f / 32768.0f;
    const float sx = sinf(rot[0] * k), cx = cosf(rot[0] * k), sy = sinf(rot[1] * k), cy = cosf(rot[1] * k),
                sz = sinf(rot[2] * k), cz = cosf(rot[2] * k);
    m->m[2][0] = -sy;
    m->m[0][0] = cz * cy;
    m->m[1][0] = sz * cy;
    m->m[2][1] = cy * sx;
    m->m[2][2] = cy * cx;
    m->m[0][1] = sx * cz * sy - cx * sz;
    m->m[1][2] = cx * sz * sy - sx * cz;
    m->m[0][2] = cx * cz * sy + sx * sz;
    m->m[1][1] = sx * sz * sy + cx * cz;
    for (int i = 0; i < 3; ++i)
        m->m[i][3] = t[i];
}

static void mat_to_srt(const Mat34* m, s16 rot[3], float t[3]) {
    const float k = 32768.0f / 3.14159265f;
    float sy = -m->m[2][0];
    sy = sy > 1.0f ? 1.0f : sy < -1.0f ? -1.0f : sy;
    rot[1] = (s16)lrintf(asinf(sy) * k);
    rot[0] = (s16)lrintf(atan2f(m->m[2][1], m->m[2][2]) * k);
    rot[2] = (s16)lrintf(atan2f(m->m[1][0], m->m[0][0]) * k);
    for (int i = 0; i < 3; ++i)
        t[i] = m->m[i][3];
}

// Link's joints' bind poses relative to the head, in cl.bdl's tree, once: Rbind_inv[f] for each follower.
static Mat34 g_follow_bind_inv[MAX_FOLLOW];
static bool g_follow_bind_ok;

static bool link_bind_matrix(CPUState* cpu, u32 joints, const int parent[LINK_JOINTS], int n, Mat34* out) {
    // Joint n's bind matrix from the root: the chain of its parents' transforms composed.
    int chain[LINK_JOINTS], depth = 0;
    for (int j = n; j >= 0 && depth < LINK_JOINTS; j = parent[j])
        chain[depth++] = j;
    Mat34 acc;
    memset(&acc, 0, sizeof acc);
    acc.m[0][0] = acc.m[1][1] = acc.m[2][2] = 1.0f;
    for (int d = depth - 1; d >= 0; --d) {
        const u32 joint = mem_read32(cpu, joints + 4u * (u32)chain[d]);
        if (!guest_ptr(joint))
            return false;
        s16 rot[3];
        float t[3];
        for (int k = 0; k < 3; ++k) {
            rot[k] = (s16)mem_read16(cpu, joint + 0x28u + 2u * (u32)k);
            t[k] = read_f32(cpu, joint + 0x30u + 4u * (u32)k);
        }
        Mat34 local;
        srt_to_mat(rot, t, &local);
        m_mul(&acc, &local, &acc);
    }
    *out = acc;
    return true;
}

static bool compute_follow_bind(CPUState* cpu) {
    const u32 player = mem_read32(cpu, PLAYER);
    const u32 cl = guest_ptr(player) ? mem_read32(cpu, player + PLAYER_CL_MODEL) : 0u;
    const u32 data = guest_ptr(cl) ? mem_read32(cpu, cl + MODEL_DATA) : 0u;
    const u32 joints = guest_ptr(data) ? mem_read32(cpu, data + 0x2Cu) : 0u;
    if (!guest_ptr(joints) || mem_read16(cpu, data + 0x28u) != LINK_JOINTS)
        return false;
    u32 ptr[LINK_JOINTS];
    int parent[LINK_JOINTS];
    for (int j = 0; j < LINK_JOINTS; ++j) {
        ptr[j] = mem_read32(cpu, joints + 4u * (u32)j);
        if (!guest_ptr(ptr[j]))
            return false;
        parent[j] = -1;
    }
    for (int j = 0; j < LINK_JOINTS; ++j) {
        u32 c = mem_read32(cpu, ptr[j] + 0x10u);
        for (int guard = 0; guard < LINK_JOINTS && guest_ptr(c); ++guard, c = mem_read32(cpu, c + 0x14u)) {
            const u32 idx = mem_read16(cpu, c + 0x18u);
            if (idx < LINK_JOINTS)
                parent[idx] = j;
        }
    }
    Mat34 head_bind, head_inv;
    if (!link_bind_matrix(cpu, joints, parent, FOLLOW_HEAD, &head_bind) || !m_inv(&head_bind, &head_inv))
        return false;
    for (int f = 0; f < g_follow_count; ++f) {
        Mat34 nb, rel;
        if (g_follow_link[f] < 0 || g_follow_link[f] >= LINK_JOINTS ||
            !link_bind_matrix(cpu, joints, parent, g_follow_link[f], &nb))
            return false;
        m_mul(&head_inv, &nb, &rel); // joint N in the head's frame at bind
        if (!m_inv(&rel, &g_follow_bind_inv[f]))
            return false;
    }
    return true;
}

// Sets the followers' local transforms from the Link joint matrices (world, 42 of them) of this frame.
static void drive_followers(CPUState* cpu, const Mat34* link_nodes) {
    if (g_follow_count == 0 || link_nodes == NULL)
        return;
    Mat34 head_inv;
    if (!m_inv(&link_nodes[FOLLOW_HEAD], &head_inv))
        return;
    const u32 joints = mem_read32(cpu, g_data + 0x2Cu);
    for (int f = 0; f < g_follow_count; ++f) {
        const u32 joint = guest_ptr(joints) ? mem_read32(cpu, joints + 4u * g_follow_joint[f]) : 0u;
        if (!guest_ptr(joint) || g_follow_link[f] < 0 || g_follow_link[f] >= LINK_JOINTS)
            continue;
        // The model is fitted to Link as he stands in the game (its rest is his joint N relative to the head at the game's
        // own rest pose), so the follower's local transform is just that relative matrix now.
        Mat34 local;
        m_mul(&head_inv, &link_nodes[g_follow_link[f]], &local);
        s16 rot[3];
        float t[3];
        mat_to_srt(&local, rot, t);
        for (int k = 0; k < 3; ++k) {
            mem_write16(cpu, joint + 0x28u + 2u * (u32)k, (u16)rot[k]);
            write_f32(cpu, joint + 0x30u + 4u * (u32)k, t[k]);
        }
    }
}

// Draws the hood on the head matrix of one wearer.
// The hood's materials are unlit (texture * a TEV colour * baked vertex shade), so the scene's light does not reach it. Link's
// own light for this place (the tevStr just set: C0 the ambient, K0 the sun or lamp) scales the colours in the display lists
// instead, per channel, so the hood darkens in shade, indoors and at night as Link does.
static void scan_blocks(CPUState* cpu) {
    g_blk_scanned = true;
    g_blk_n = 0;
    if (!guest_ptr(g_data))
        return;
    const u32 num = mem_read16(cpu, g_data + 0x58u + 4u);
    const u32 nodes = mem_read32(cpu, g_data + 0x58u + 8u);
    if (num == 0u || num > 16u || !guest_ptr(nodes))
        return;
    for (u32 i = 0; i < num; ++i) {
        const u32 mat = mem_read32(cpu, nodes + 4u * i);
        const u32 blk = guest_ptr(mat) ? mem_read32(cpu, mat + 0x2Cu) : 0u;
        if (!guest_ptr(blk))
            continue;
        for (int m = 0; m < 3; ++m)
            for (int r = 0; r < 2; ++r) {
                const u8* base = g_base[m][r];
                for (u32 off = 8u; off < 0x200u && g_blk_n < 32; off += 2u) {
                    if (mem_read16(cpu, blk + off) == base[0] && mem_read16(cpu, blk + off + 2u) == base[1] &&
                        mem_read16(cpu, blk + off + 4u) == base[2] && mem_read16(cpu, blk + off + 6u) <= 255u) {
                        bool dup = false;
                        for (int k = 0; k < g_blk_n; ++k)
                            dup = dup || g_blk[k].at == blk + off;
                        if (dup)
                            continue;
                        g_blk[g_blk_n].at = blk + off;
                        memcpy(g_blk[g_blk_n].base, base, 3);
                        ++g_blk_n;
                        fprintf(stderr, "[bunny] material %u block %08X: colour %u/%u at +0x%X\n", i, blk, m, r, off);
                    }
                }
            }
    }
}

static void light_pass(CPUState* cpu, const Wearer* w) {
    if (!g_base_ok || !g_mdl3_ok || g_dl_buf == 0u)
        return;
    if (!g_blk_scanned)
        scan_blocks(cpu);
    static float ref = -1.0f, floor_ = 0.0f;
    if (ref < 0.0f) {
        ref = getenv("BLUEWAKE_BUNNY_LIGHTREF") != NULL ? (float)atof(getenv("BLUEWAKE_BUNNY_LIGHTREF")) : 255.0f;
        floor_ = getenv("BLUEWAKE_BUNNY_LIGHTMIN") != NULL ? (float)atof(getenv("BLUEWAKE_BUNNY_LIGHTMIN")) : 0.12f;
    }
    float f[3];
    int c0[3], k0[3];
    for (int ch = 0; ch < 3; ++ch) {
        c0[ch] = (s16)mem_read16(cpu, w->tev + 0x80u + 2u * (u32)ch);
        k0[ch] = (int)mem_read8(cpu, w->tev + 0x88u + (u32)ch);
        f[ch] = ((float)c0[ch] + (float)k0[ch]) / ref;
        f[ch] = f[ch] < floor_ ? floor_ : (f[ch] > 1.0f ? 1.0f : f[ch]);
    }
    static int n;
    if (getenv("BLUEWAKE_BUNNY_LIGHTLOG") != NULL && (n++ % 60) == 0)
        fprintf(stderr, "[bunny] light C0 %d %d %d K0 %d %d %d -> scale %.2f %.2f %.2f\n", c0[0], c0[1], c0[2], k0[0], k0[1], k0[2], f[0], f[1], f[2]);
    for (int k = 0; k < g_blk_n; ++k)
        for (int ch = 0; ch < 3; ++ch)
            mem_write16(cpu, g_blk[k].at + 2u * (u32)ch, (u16)((float)g_blk[k].base[ch] * f[ch] + 0.5f));
    for (int m = 0; m < 3; ++m)
        for (int r = 0; r < 2; ++r) {
            const u32* o = g_mdl3[m][r];
            u32 v[3];
            for (int ch = 0; ch < 3; ++ch)
                v[ch] = (u32)((float)g_base[m][r][ch] * f[ch] + 0.5f);
            const u32 ra = (255u << 12) | v[0], gb = (v[1] << 12) | v[2];
            for (int j = 0; j < 4; ++j) {
                const u32 val = j == 0 ? ra : gb;
                const u32 at = g_dl_buf + o[j];
                mem_write8(cpu, at, (u8)(val >> 16));
                mem_write8(cpu, at + 1u, (u8)(val >> 8));
                mem_write8(cpu, at + 2u, (u8)val);
            }
        }
}

static void draw_hood(GuestCoro* co, CPUState* cpu, Wearer* w, const Mat34* head, const Mat34* link_nodes,
                      const float place[3], bool step) {
    if (step)
        step_ears(w, head);
    {
        // The checks the artist asked for: the rest rotations as read, and the spring angles (they must be ~0 standing).
        static int logged;
        if (logged < 4 && (w == &g_wearer[0])) {
            static int frames;
            if (++frames % 90 == 0) {
                ++logged;
                fprintf(stderr, "[bunny] ears: angles L0 %.3f %.3f L1 %.3f %.3f R0 %.3f %.3f R1 %.3f %.3f\n", w->angle[0][0],
                        w->angle[0][1], w->angle[1][0], w->angle[1][1], w->angle[2][0], w->angle[2][1], w->angle[3][0],
                        w->angle[3][1]);
            }
        }
    }
    // The ear joints' rotations: the model's rest plus the springs.
    const u32 joints = mem_read32(cpu, g_data + 0x2Cu);
    static s16 rest[EARS][3];
    static bool rested[EARS];
    for (int e = 0; e < EARS; ++e) {
        if (g_ear[e] == JOINT_NONE || !guest_ptr(joints))
            continue;
        const u32 joint = mem_read32(cpu, joints + 4u * g_ear[e]);
        if (!guest_ptr(joint))
            continue;
        if (!rested[e]) {
            for (int k = 0; k < 3; ++k)
                rest[e][k] = (s16)mem_read16(cpu, joint + 0x28u + 2u * (u32)k);
            rested[e] = true;
            fprintf(stderr, "[bunny] ear %d rest rotation %d %d %d\n", e, rest[e][0], rest[e][1], rest[e][2]);
        }
        s16 rot[3] = {rest[e][0], rest[e][1], rest[e][2]};
        rot[g_axis_bend] = (s16)(rot[g_axis_bend] + to_s16(g_sign_bend * w->angle[e][0]));
        rot[g_axis_lean] = (s16)(rot[g_axis_lean] + to_s16(g_sign_lean * (e < 2 ? 1.0f : -1.0f) * w->angle[e][1]));
        for (int k = 0; k < 3; ++k)
            mem_write16(cpu, joint + 0x28u + 2u * (u32)k, (u16)rot[k]);
    }
    drive_followers(cpu, link_nodes);
    // The hood's place on the head: the generated model is fitted to the head joint's frame (+X up, +Y front, Z sideways); the
    // user found it sat too far back (the band behind and above the ear, the ears tipped back). It is turned forward about the
    // dome's centre (14 degrees) and moved forward 6 and down 2.5 units, in that frame, without changing the generated files.
    static Mat34 fit;
    static bool fit_ready;
    if (!fit_ready) {
        float pitch_deg = 14.0f, shift[3] = {-2.5f, 6.0f, 0.0f};
        if (getenv("BLUEWAKE_BUNNY_FIT") != NULL)
            sscanf(getenv("BLUEWAKE_BUNNY_FIT"), "%f,%f,%f,%f", &pitch_deg, &shift[0], &shift[1], &shift[2]); // a test
        const float th = pitch_deg * 3.14159265f / 180.0f, c = cosf(th), sn = sinf(th);
        const float centre[3] = {22.0f, 0.0f, 0.0f};
        memset(&fit, 0, sizeof fit);
        fit.m[0][0] = c;
        fit.m[0][1] = -sn;
        fit.m[1][0] = sn;
        fit.m[1][1] = c;
        fit.m[2][2] = 1.0f;
        for (int i = 0; i < 3; ++i) {
            float rc = 0.0f;
            for (int j = 0; j < 3; ++j)
                rc += fit.m[i][j] * centre[j];
            fit.m[i][3] = centre[i] - rc + shift[i];
        }
        fit_ready = true;
    }
    Mat34 base;
    m_mul(head, &fit, &base);
    write_mat(cpu, w->model + MODEL_BASE_MTX, &base);
    CALL(FN_MODEL_CALC, w->model, 0, 0, 0);
    for (int i = 0; i < 3; ++i)
        write_f32(cpu, w->pos + 4u * (u32)i, place[i]);
    {
        static int type = -1, nolight = -1;
        if (type < 0) {
            type = getenv("BLUEWAKE_BUNNY_TEVTYPE") != NULL ? atoi(getenv("BLUEWAKE_BUNNY_TEVTYPE")) : 9; // a test
            nolight = getenv("BLUEWAKE_BUNNY_NOLIGHT") != NULL;
        }
        CALL(FN_SETTING_TEVSTRUCT, ENV_LIGHT, (u32)type, w->pos, w->tev);
        if (!nolight)
            CALL(FN_SET_LIGHT_TEV, ENV_LIGHT, w->model, w->tev, 0);
    }
    light_pass(cpu, w);
    mem_write32(cpu, J3D_SYS + 0x48u, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1));
    mem_write32(cpu, J3D_SYS + 0x4Cu, mem_read32(cpu, DRAW_LIST + DRAW_OPA_P1 + 4u));
    CALL(FN_MODEL_ENTRY_DL, w->model, 0, 0, 0);
}

// ------------------------------------------------- the draw hook's fiber ----

static GuestCoro* g_coro_draw;
static GuestCoro* g_coro_delete;

static void teardown(GuestCoro* co);

// ---- giving the item the way the game does ----
// fopAcM_createItem makes a field item (daItem_c, process 0x101: its item at +0x63A, its state at +0x66B); state 7 is
// "init get demo", which orders the item event, makes the held demo item (the model of item_resource) and has Link show
// it with the item-get message; the demo item's delete runs execItemGet.
enum {
    FN_CREATE_ITEM = 0x80026ADCu,     // fopAcM_createItem(pos, item, bit, room, type, angle, action, scale)
    FN_SEARCH_BY_ID = 0x800241C0u,    // fopAcM_SearchByID(u32 process id, fopAc_ac_c** out)
    ITEM_NO_AT = 0x63Au,
    ITEM_STATUS_AT = 0x66Bu,
    ITEM_STATUS_INIT_GET_DEMO = 7,
    SCRATCH_POS = 0x5C0,   // in the text block: a cXyz, a csXyz, a pointer
    SCRATCH_ANGLE = 0x5D0,
    SCRATCH_OUT = 0x5D8,
};
static volatile int g_give_request;
static u32 g_give_item = BLUEWAKE_BUNNY_HOOD_ITEM; // a test can give another item (BLUEWAKE_BUNNY_GIVE_ITEM)
static int g_give_wait; // frames since the item was made (0: none waiting)
static u32 g_give_pid;  // the field item's process id
static int g_give_watch; // frames still to log the item's state (a diagnostic)

bool bluewake_bunny_hood_give(void) {
    if (!g_enabled)
        return false;
    g_give_request = 1;
    return true;
}

// Testing (a scenario's `giveitem 0x07`): any field item given the same way, as a chest or an NPC gives one.
bool bluewake_bunny_hood_give_item(unsigned item) {
    if (!g_enabled)
        return false;
    g_give_item = item;
    g_give_request = 1;
    return true;
}

// Testing (Dev Tools console `spawn NAME [params] [distance]`): an actor made by its profile name a few units in
// front of Link, facing him, so a model or a recolour is checked by looking at it (C-up), wherever the game
// would normally show it. fopAcM_create(name, params, pos, room, angle, scale, createFunc).
enum {
    FN_ACTOR_CREATE = 0x80024598u, // fopAcM_create__FPcUlP4cXyziP5csXyzP4cXyzPFPv_i
    SCRATCH_NAME = 0x5E0,          // in the text block, after the give's scratch: up to 15 characters
};
static char g_spawn_name[16];
static u32 g_spawn_params;
static float g_spawn_dist;
static volatile int g_spawn_request;

bool bluewake_dev_spawn(const char* name, unsigned params, float distance) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= sizeof g_spawn_name)
        return false;
    memcpy(g_spawn_name, name, strlen(name) + 1u);
    g_spawn_params = params;
    g_spawn_dist = distance;
    g_spawn_request = 1;
    return true;
}

static void spawn_pass(GuestCoro* co, CPUState* cpu) {
    if (!g_spawn_request || g_text_buf == 0u)
        return;
    g_spawn_request = 0;
    const u32 player = mem_read32(cpu, PLAYER);
    if (!guest_ptr(player))
        return;
    const s16 facing = (s16)mem_read16(cpu, player + 0x206u); // current.angle.y (0 faces +z)
    const float a = (float)facing * 3.14159265f / 32768.f;
    const float pos[3] = {read_f32(cpu, player + PLAYER_POS) + g_spawn_dist * sinf(a), read_f32(cpu, player + PLAYER_POS + 4u),
                          read_f32(cpu, player + PLAYER_POS + 8u) + g_spawn_dist * cosf(a)};
    for (int i = 0; i < 3; ++i) {
        u32 bits;
        memcpy(&bits, &pos[i], 4);
        mem_write32(cpu, g_text_buf + SCRATCH_POS + 4u * (u32)i, bits);
    }
    mem_write16(cpu, g_text_buf + SCRATCH_ANGLE, 0u);
    mem_write16(cpu, g_text_buf + SCRATCH_ANGLE + 2u, (u16)(facing + 0x8000)); // facing Link
    mem_write16(cpu, g_text_buf + SCRATCH_ANGLE + 4u, 0u);
    for (u32 i = 0; i < 16u; ++i)
        mem_write8(cpu, g_text_buf + SCRATCH_NAME + i, (u8)g_spawn_name[i]);
    cpu->gpr[9] = 0u; // no create callback
    const u32 pid = bluewake_guest_call(co, FN_ACTOR_CREATE, g_text_buf + SCRATCH_NAME, g_spawn_params, g_text_buf + SCRATCH_POS,
                                        (u32)(s8)mem_read8(cpu, STAY_ROOM), g_text_buf + SCRATCH_ANGLE, 0u);
    fprintf(stderr, "[test] spawn %s params %08X at %.0f %.0f %.0f: process id %08X\n", g_spawn_name, g_spawn_params,
            pos[0], pos[1], pos[2], pid);
}

static void give_pass(GuestCoro* co, CPUState* cpu) {
    if (g_text_buf == 0u)
        return;
    const u32 player = mem_read32(cpu, PLAYER);
    if (!guest_ptr(player))
        return;
    if (g_give_request != 0 && g_give_wait == 0) {
        g_give_request = 0;
        for (int i = 0; i < 3; ++i)
            mem_write32(cpu, g_text_buf + SCRATCH_POS + 4u * (u32)i, mem_read32(cpu, player + 0x1F8u + 4u * (u32)i));
        for (u32 i = 0; i < 8u; i += 4u)
            mem_write32(cpu, g_text_buf + SCRATCH_ANGLE + i, 0u);
        cpu->gpr[9] = 0u;  // action
        cpu->gpr[10] = 0u; // scale
        const u32 pid = bluewake_guest_call(co, FN_CREATE_ITEM, g_text_buf + SCRATCH_POS, g_give_item, 0xFFFFFFFFu,
                            (u32)(s8)mem_read8(cpu, 0x803F6A78u), 1u, g_text_buf + SCRATCH_ANGLE);
        g_give_pid = pid;
        g_give_wait = 1;
        fprintf(stderr, "[bunny] give: a field item for 0x%02X made at Link's feet (process id %08X)\n", BLUEWAKE_BUNNY_HOOD_ITEM, pid);
        return;
    }
    if (g_give_watch > 0) {
        --g_give_watch;
        if (g_give_watch % 15 == 0 || g_give_watch > 140) {
            mem_write32(cpu, g_text_buf + SCRATCH_OUT, 0u);
            const u32 f = bluewake_guest_call(co, FN_SEARCH_BY_ID, g_give_pid, g_text_buf + SCRATCH_OUT, 0, 0, 0, 0);
            const u32 a = mem_read32(cpu, g_text_buf + SCRATCH_OUT);
            fprintf(stderr, "[bunny] give: watch %d: found %u actor %08X status %u collect %02X\n", g_give_watch, f, a,
                    guest_ptr(a) ? mem_read8(cpu, a + ITEM_STATUS_AT) : 0u, collect_byte(cpu));
        }
    }
    if (g_give_wait == 0)
        return;
    if (++g_give_wait > 240) {
        g_give_wait = 0;
        fprintf(stderr, "[bunny] give: the field item never appeared\n");
        return;
    }
    mem_write32(cpu, g_text_buf + SCRATCH_OUT, 0u);
    const u32 found = bluewake_guest_call(co, FN_SEARCH_BY_ID, g_give_pid, g_text_buf + SCRATCH_OUT, 0, 0, 0, 0);
    const u32 actor = mem_read32(cpu, g_text_buf + SCRATCH_OUT);
    if (g_give_wait < 6 || g_give_wait % 60 == 0)
        fprintf(stderr, "[bunny] give: wait %d, search %u actor %08X item %u\n", g_give_wait, found, actor,
                guest_ptr(actor) ? mem_read8(cpu, actor + ITEM_NO_AT) : 0u);
    if ((found & 0xFFu) != 0u && guest_ptr(actor) && mem_read8(cpu, actor + ITEM_NO_AT) == g_give_item) {
        mem_write8(cpu, actor + ITEM_STATUS_AT, ITEM_STATUS_INIT_GET_DEMO);
        g_give_wait = 0;
        g_give_watch = 150;
        fprintf(stderr, "[bunny] give: the item %08X is in its get-item state\n", actor);
    }
}

// BLUEWAKE_BUNNY_DUMP=file: once, about three seconds into a scene, Link's 42 joint world matrices as he is drawn
// (the real, hat-physics pose), for the model artist to fit the band to. A diagnostic; off unless set.
static void dump_link_nodes(CPUState* cpu) {
    static int frames;
    static bool done;
    const char* path = getenv("BLUEWAKE_BUNNY_DUMP");
    if (path == NULL || done || ++frames < 180)
        return;
    const u32 player = mem_read32(cpu, PLAYER);
    const u32 cl = guest_ptr(player) ? mem_read32(cpu, player + PLAYER_CL_MODEL) : 0u;
    const u32 nodes = guest_ptr(cl) ? mem_read32(cpu, cl + MODEL_NODE_MTX) : 0u;
    if (!guest_ptr(nodes))
        return;
    FILE* f = fopen(path, "wb");
    if (f == NULL)
        return;
    done = true;
    fprintf(f, "{\"joints\": [\n");
    for (int j = 0; j < 42; ++j) {
        Mat34 m;
        read_mat(cpu, nodes + 48u * (u32)j, &m);
        fprintf(f, " [%.5f,%.5f,%.5f,%.5f, %.5f,%.5f,%.5f,%.5f, %.5f,%.5f,%.5f,%.5f]%s\n", m.m[0][0], m.m[0][1], m.m[0][2],
                m.m[0][3], m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3], m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3],
                j < 41 ? "," : "");
    }
    fprintf(f, "]}\n");
    fclose(f);
    fprintf(stderr, "[bunny] Link's joint matrices written to %s\n", path);
}

// ---- the item-get message box's picture ----
// dMsg_screenDataSetItem reads the item's icon (the hood's table entry is the Charm's) into the box's buffer_p
// (sub_msg_class +0x10CC) and gives it to the 'item' picture (m0624[8]). While the box for the hood's message is up,
// the picture is pointed at the hood's own icon bytes (the block's ICON_AT), as the pause screen's slot is.
enum {
    FN_SEARCH_MSG_BY_NAME = 0x8002AEF4u, // fopMsgM_SearchByName(s16 process name)
    PROFILE_MSG = 0x80393798u,           // g_profile_MSG (mProcName at +8)
    MSG_NO_AT = 0xECu,
    MSG_ITEM_PANE_AT = 0x624u + 8u * 0x38u,
};
static u32 g_msg_done; // the box object already given the hood's picture

static void msg_pass(GuestCoro* co, CPUState* cpu) {
    static unsigned frame;
    if (g_text_buf == 0u || (++frame & 1u) != 0u)
        return;
    load_hood_icon();
    if (g_hood_icon_len == 0u)
        return;
    const u32 name = (u32)(s16)mem_read16(cpu, PROFILE_MSG + 8u);
    const u32 msg = bluewake_guest_call(co, FN_SEARCH_MSG_BY_NAME, name, 0, 0, 0, 0, 0);
    if (!guest_ptr(msg) || mem_read32(cpu, msg + MSG_NO_AT) != 0x65u + BLUEWAKE_BUNNY_HOOD_ITEM) {
        g_msg_done = 0u;
        return;
    }
    const u32 pane = mem_read32(cpu, msg + MSG_ITEM_PANE_AT);
    if (g_msg_done == msg || !guest_ptr(pane) || !guest_ptr(mem_read32(cpu, msg + 0x10CCu)))
        return;
    for (u32 i = 0; i < g_hood_icon_len; ++i)
        mem_write8(cpu, g_text_buf + ICON_AT + i, g_hood_icon[i]);
    bluewake_guest_call(co, FN_CHANGE_TEXTURE, pane, g_text_buf + ICON_AT, 0, 0, 0, 0);
    g_msg_done = msg;
    fprintf(stderr, "[bunny] item-get box %08X: the picture is the hood's\n", msg);
}

static void draw_body(GuestCoro* co, void* arg) {
    (void)arg;
    CPUState* cpu = bluewake_coro_cpu(co);
    if (bluewake_text_swap_wants_fiber())
        bluewake_text_swap_fiber(co, cpu); // Crenando's buffer, a test message (text_swap.c)
    if (!g_enabled)
        return; // the hood is off (this fiber also runs for text_swap)
    if (g_text_buf == 0u) {
        const u32 heap = CALL(FN_ARCHIVE_HEAP, 0, 0, 0, 0);
        const u32 buf = CALL(FN_HEAP_ALLOC, TEXT_BLOCK, 32u, heap, 0);
        fprintf(stderr, "[bunny] text block: archive heap %08X, buffer %08X\n", heap, buf);
        if (guest_ptr(buf)) {
            g_text_buf = buf;
            for (u32 i = 0; i < TEXT_BLOCK; i += 4u)
                mem_write32(cpu, buf + i, 0u);
        }
    }
    give_pass(co, cpu);
    spawn_pass(co, cpu);
    menu_pass(co, cpu);
    msg_pass(co, cpu);
    if (g_recolor && g_data != 0u) {
        teardown(co); // the colours changed: the model is made again with them
        g_data_failed = false;
    }
    dump_link_nodes(cpu);
    if (!g_worn || g_data_failed)
        return;
    const u32 player = mem_read32(cpu, PLAYER);
    const u32 cl = guest_ptr(player) ? mem_read32(cpu, player + PLAYER_CL_MODEL) : 0u;
    const u32 nodes = guest_ptr(cl) ? mem_read32(cpu, cl + MODEL_NODE_MTX) : 0u;
    if (!guest_ptr(nodes))
        return;
    // Not drawn while the game does not draw Link (daPy_lk_c::draw's checkPlayerNoDraw, the actor's own no-draw):
    // events hide him that way, as Beedle's shop does (otherwise the hood stayed in the air).
    if ((mem_read32(cpu, player + PLAYER_NO_RESET_FLG0) & PLAYER_FLG0_NO_DRAW) != 0u ||
        (mem_read32(cpu, player + ACTOR_STATUS) & ACTOR_STATUS_NODRAW) != 0u ||
        (mem_read32(cpu, player + ACTOR_CONDITION) & ACTOR_CONDITION_NODRAW) != 0u)
        return;
    // First person (the camera's subject view: Z/C-up look, aiming, telescope): checkPlayerNoDraw hides Link for it, and
    // the hair is left out while attention status 0x20 is set (BR-0050).
    {
        const u32 idx = mem_read32(cpu, player + PLAYER_CAMERA_INFO_IDX);
        if (idx == 0u && (mem_read32(cpu, CAMERA_ATTENTION_STATUS) & (CAM_ATTN_SUBJECT | CAM_ATTN_FIRST_PERSON)) != 0u)
            return;
    }
    if (g_data == 0u && !make_data(co, cpu))
        return;
    Wearer* w = &g_wearer[0];
    if (w->failed)
        return;
    if (w->model == 0u && !make_wearer(co, cpu, w, (s8)mem_read8(cpu, STAY_ROOM)))
        return;
    const u32 saved_opa = mem_read32(cpu, J3D_SYS + 0x48u), saved_xlu = mem_read32(cpu, J3D_SYS + 0x4Cu);
    const u32 saved_model = mem_read32(cpu, J3D_SYS + 0x38u), saved_tex = mem_read32(cpu, J3D_SYS + 0x58u);
    Mat34 head;
    read_mat(cpu, nodes + 48u * CL_JNT_HEAD, &head);
    float place[3];
    for (int i = 0; i < 3; ++i)
        place[i] = read_f32(cpu, player + PLAYER_POS + 4u * (u32)i);
    Mat34 link_nodes[LINK_JOINTS];
    for (int j = 0; j < LINK_JOINTS; ++j)
        read_mat(cpu, nodes + 48u * (u32)j, &link_nodes[j]);
    draw_hood(co, cpu, w, &head, link_nodes, place, true);
    mem_write32(cpu, J3D_SYS + 0x48u, saved_opa);
    mem_write32(cpu, J3D_SYS + 0x4Cu, saved_xlu);
    mem_write32(cpu, J3D_SYS + 0x38u, saved_model);
    mem_write32(cpu, J3D_SYS + 0x58u, saved_tex);
}

static void teardown(GuestCoro* co);

static void delete_body(GuestCoro* co, void* arg) {
    (void)arg;
    teardown(co);
}

static void teardown(GuestCoro* co) {
    for (int i = 0; i < MAX_WEARERS; ++i) {
        if (g_wearer[i].heap != 0u)
            CALL(FN_DESTROY_HEAP, g_wearer[i].heap, 0, 0, 0);
        memset(&g_wearer[i], 0, sizeof g_wearer[i]);
    }
    if (g_data_heap != 0u)
        CALL(FN_DESTROY_HEAP, g_data_heap, 0, 0, 0);
    g_data = g_data_heap = 0u; g_dl_buf = 0u; g_base_ok = false; g_blk_scanned = false; g_blk_n = 0;
    g_follow_bind_ok = false;
    g_data_failed = false;
    g_heap_attempts = 0;
}

void bluewake_bunny_hood_draw_for(GuestCoro* co, int slot, const float* link_nodes, const float place[3]) {
    if (!g_enabled)
        return;
    CPUState* cpu = bluewake_coro_cpu(co);
    if (slot < 1 || slot >= MAX_WEARERS || g_data_failed)
        return;
    if (g_data == 0u && !make_data(co, cpu))
        return;
    Wearer* w = &g_wearer[slot];
    if (w->failed)
        return;
    if (w->model == 0u && !make_wearer(co, cpu, w, (s8)mem_read8(cpu, STAY_ROOM)))
        return;
    const u32 saved_opa = mem_read32(cpu, J3D_SYS + 0x48u), saved_xlu = mem_read32(cpu, J3D_SYS + 0x4Cu);
    const u32 saved_model = mem_read32(cpu, J3D_SYS + 0x38u), saved_tex = mem_read32(cpu, J3D_SYS + 0x58u);
    const Mat34* nodes = (const Mat34*)link_nodes;
    draw_hood(co, cpu, w, &nodes[FOLLOW_HEAD], nodes, place, true);
    mem_write32(cpu, J3D_SYS + 0x48u, saved_opa);
    mem_write32(cpu, J3D_SYS + 0x4Cu, saved_xlu);
    mem_write32(cpu, J3D_SYS + 0x38u, saved_model);
    mem_write32(cpu, J3D_SYS + 0x58u, saved_tex);
}

bool bluewake_bunny_hood_draw_active(void) { return bluewake_coro_active(g_coro_draw); }

u32 bluewake_bunny_hood_draw_hook(CPUState* cpu, u32 site) {
    if (!g_enabled && !bluewake_coro_active(g_coro_draw) && !bluewake_text_swap_wants_fiber())
        return 0u;
    {
        // A test: BLUEWAKE_BUNNY_GIVE_AT=n gives the hood once, n frames after the scene is drawn.
        static int at = -1, frames;
        if (at < 0) {
            const char* v = getenv("BLUEWAKE_BUNNY_GIVE_AT");
            at = v != NULL ? atoi(v) : 0;
        }
        if (at > 0 && ++frames == at) {
            const char* it = getenv("BLUEWAKE_BUNNY_GIVE_ITEM");
            if (it != NULL)
                g_give_item = (u32)strtoul(it, NULL, 0);
            g_give_request = 1;
        }
    }
    if (g_coro_draw == NULL)
        g_coro_draw = bluewake_coro_create(draw_body);
    if (!bluewake_coro_active(g_coro_draw) && g_text_buf != 0u && !(g_worn && !g_data_failed) && !menu_pass_needed(cpu) && !(g_enabled && (load_hood_icon(), g_hood_icon_len != 0u)) && g_give_request == 0 && g_give_wait == 0 && g_give_watch == 0 && !g_spawn_request && !bluewake_text_swap_wants_fiber())
        return 0u; // nothing to do this frame
    return bluewake_coro_step(g_coro_draw, cpu, site, NULL);
}

bool bluewake_bunny_hood_delete_active(void) { return bluewake_coro_active(g_coro_delete); }

// dScnPly_Delete's first instruction: the scene's heaps go.
u32 bluewake_bunny_hood_delete_hook(CPUState* cpu, u32 site) {
    if (g_coro_delete == NULL)
        g_coro_delete = bluewake_coro_create(delete_body);
    if (!bluewake_coro_active(g_coro_delete) && g_data == 0u && g_wearer[0].heap == 0u)
        return 0u;
    return bluewake_coro_step(g_coro_delete, cpu, site, NULL);
}
