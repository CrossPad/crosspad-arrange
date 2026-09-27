/**
 * @file ArrangeApp.cpp
 * @brief Arrange -- four audio tracks of clips on a bar timeline.
 *
 * The audio song mode. Scenes made in the Sequencer arrive here as clips
 * (its BOUNCE writes them and places them on a track); here they are laid out
 * like a linear DAW: four lanes, a bar ruler, every clip a block as long as it
 * plays, in a colour of its own, and a playhead while the song runs.
 *
 *   top    back, the project (tap: pick another or start a new one), where the
 *          song is, SYNC (the sequencer plays along, locked to the song's
 *          bars and tempo), LOOP, play/stop -- play starts at the selected
 *          block
 *   lanes  a header per track (tap: mute), the timeline beside them
 *   tools  move the selected block a bar left/right, DUP after it, DEL,
 *          ADD a clip from the project's library to the selected track, LEN
 *          (how many bars the song plays and loops over: set, or up to its
 *          last block) and zoom out/in on the timeline
 *
 * Everything goes through crosspad::ISongEngine: edits are made on its song
 * under songMutex() and committed, which hands playback the new arrangement
 * at once and has it saved. The pads stay the Sampler's.
 */

#include "crosspad-arrange/ArrangeApp.hpp"

// `class App` lives in a different header per platform, but both derive from
// crosspad_gui::AppBase<App>, so the lifecycle calls used here are identical.
#if __has_include("pc_stubs/PcApp.hpp")
  #include "pc_stubs/PcApp.hpp"
#else
  #include "crosspad-gui/app/App.hpp"
#endif

#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/ISongEngine.hpp"
#include "crosspad-gui/widgets/gui_helpers.h"

#include "lvgl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef ESP_PLATFORM
extern "C" {
  extern lv_indev_t* indev_encoderTop;
}
#define ARR_USE_ENCODER 1
#endif

namespace {

constexpr int      kTracks   = crosspad::AudioSong::kTracks;
/* Zoom: pixels per bar, from a whole song on one screen to single beats. */
constexpr int32_t  kZoomPx[] = {3, 6, 12, 24, 48};
constexpr int      kZoomLevels = sizeof(kZoomPx) / sizeof(kZoomPx[0]);
constexpr uint32_t kMaxLength = 999;
constexpr int32_t  kRulerH   = 12;
constexpr int32_t  kLaneH    = 30;
constexpr int32_t  kLaneGap  = 2;
constexpr int32_t  kHeadW    = 24;
constexpr uint32_t kTickMs   = 50;
constexpr int      kMaxBlocks = 64;
/// Drawn blocks per track: a long track must not use up the ones the others need.
constexpr int      kMaxBlocksPerTrack = kMaxBlocks / kTracks;

const lv_color_t kBg     = lv_color_hex(0x16181B);
const lv_color_t kPanel  = lv_color_hex(0x20242A);
const lv_color_t kLane   = lv_color_hex(0x1B1E23);
const lv_color_t kText   = lv_color_hex(0xE8EAED);
const lv_color_t kDim    = lv_color_hex(0x7B828B);
const lv_color_t kInk    = lv_color_hex(0x16181B);
const lv_color_t kAccent = lv_color_hex(0xC6FF47);
const lv_color_t kMute   = lv_color_hex(0xFF5C5C);
const lv_color_t kHead   = lv_color_hex(0xFFFFFF);

/// A clip's colour, from its file: every block of the same clip matches, so a
/// repeat reads as a repeat anywhere on the timeline.
const uint32_t kClipColors[] = {
    0xC6FF47, 0x5AC8FF, 0xFFC857, 0xFF7A9A, 0xB48CFF, 0xFF9552, 0x4FE0B0, 0x7AA2FF,
};

uint32_t clip_color(const std::string& file)
{
    uint32_t h = 2166136261u;
    for (char c : file) h = (h ^ (uint8_t)c) * 16777619u;
    return kClipColors[h % 8];
}

struct Block { lv_obj_t* obj = nullptr; int track = 0; int index = 0; };

lv_obj_t* s_root = nullptr;
lv_obj_t* s_timeline = nullptr;
lv_obj_t* s_playhead = nullptr;
lv_obj_t* s_projLabel = nullptr;
lv_obj_t* s_posLabel = nullptr;
lv_obj_t* s_infoLabel = nullptr;
lv_obj_t* s_playBtn = nullptr;
lv_obj_t* s_loopBtn = nullptr;
lv_obj_t* s_syncBtn = nullptr;
lv_obj_t* s_heads[kTracks] = {};
lv_obj_t* s_tools[8] = {};
lv_obj_t* s_lenLabel = nullptr;     // the value in the LEN window
lv_obj_t* s_backBtn = nullptr;
lv_obj_t* s_projBtn = nullptr;
lv_obj_t* s_overlay = nullptr;
Block     s_blocks[kMaxBlocks];
int       s_blockCount = 0;
lv_timer_t* s_timer = nullptr;

int      s_zoom = 2;                // index into kZoomPx; kept across visits
int32_t  s_pxPerBar = kZoomPx[2];
int      s_selTrack = 0;
int      s_selIndex = -1;
uint32_t s_shape = 0;

/* The card is read on the platform's side; an overlay opens at once with a
 * line saying so and is filled when the list comes back. */
enum class Pending : uint8_t { None, Projects, Clips };
Pending   s_pending = Pending::None;
uint32_t  s_pendingGen = 0;
lv_obj_t* s_loading = nullptr;
/* A project asked for and not open yet: the timeline waits for it. */
bool      s_opening = false;

#ifdef ARR_USE_ENCODER
lv_group_t* s_encGroup = nullptr;
lv_group_t* s_prevEncGroup = nullptr;
lv_group_t* s_overlayGroup = nullptr;
#endif

crosspad::ISongEngine* engine()
{
    return crosspad::getPlatformServices().songEngine;
}

/// Everything the timeline draws, folded into one word; when it changes (an
/// edit, a bounce landing, another project) the timeline is rebuilt.
uint32_t song_shape(const crosspad::AudioSong& s)
{
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
    for (char c : s.dir) mix((uint8_t)c);
    mix(s.length);
    for (int t = 0; t < kTracks; ++t) {
        mix(0xABCDu + t);
        mix(s.tracks[t].muted);
        for (const auto& c : s.tracks[t].clips) {
            mix(c.bar); mix(c.bars);
            for (char ch : c.file) mix((uint8_t)ch);
        }
    }
    return h;
}

std::string short_name(const std::string& file)
{
    std::string n = file;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".wav") == 0) n.resize(n.size() - 4);
    return n;
}

// ── drawing ────────────────────────────────────────────────────────────────

void paint_block(const Block& b)
{
    if (!b.obj) return;
    const bool sel = b.track == s_selTrack && b.index == s_selIndex;
    lv_obj_set_style_outline_width(b.obj, sel ? 2 : 0, 0);
    lv_obj_set_style_outline_color(b.obj, kHead, 0);
    lv_obj_set_style_outline_pad(b.obj, 1, 0);
}

void update_info()
{
    auto* se = engine();
    if (!se || !s_infoLabel) return;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const auto& song = se->song();
    const auto& cs = song.tracks[s_selTrack].clips;
    if (s_selIndex >= 0 && s_selIndex < (int)cs.size()) {
        const auto& c = cs[s_selIndex];
        lv_label_set_text_fmt(s_infoLabel, "T%d  %s  -  bar %u, %u bars   |   %d bpm",
                              s_selTrack + 1, short_name(c.file).c_str(), (unsigned)(c.bar + 1),
                              (unsigned)c.bars, (int)lroundf(song.bpm));
    } else if (song.playEnd() == 0) {
        lv_label_set_text(s_infoLabel, "Empty song -- BOUNCE a scene in the Sequencer, or ADD a clip");
    } else {
        lv_label_set_text_fmt(s_infoLabel, "Track %d selected   |   %d bpm   |   %u bars%s", s_selTrack + 1,
                              (int)lroundf(song.bpm), (unsigned)song.playEnd(), song.length ? "" : " (auto)");
    }
}

void select(int track, int index)
{
    s_selTrack = track;
    s_selIndex = index;
    for (int i = 0; i < s_blockCount; ++i) paint_block(s_blocks[i]);
    for (int t = 0; t < kTracks; ++t) {
        if (s_heads[t]) lv_obj_set_style_border_width(s_heads[t], t == s_selTrack ? 2 : 0, 0);
    }
    update_info();
}

void block_cb(lv_event_t* e)
{
    const int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= s_blockCount) return;
    select(s_blocks[k].track, s_blocks[k].index);
    if (s_blocks[k].obj) lv_obj_scroll_to_view(s_blocks[k].obj, LV_ANIM_ON);
}

void rebuild_group()
{
#ifdef ARR_USE_ENCODER
    if (!s_encGroup) return;
    lv_obj_t* focused = lv_group_get_focused(s_encGroup);
    lv_group_remove_all_objs(s_encGroup);
    lv_group_add_obj(s_encGroup, s_backBtn);
    lv_group_add_obj(s_encGroup, s_projBtn);
    lv_group_add_obj(s_encGroup, s_syncBtn);
    lv_group_add_obj(s_encGroup, s_loopBtn);
    lv_group_add_obj(s_encGroup, s_playBtn);
    for (int i = 0; i < s_blockCount; ++i) lv_group_add_obj(s_encGroup, s_blocks[i].obj);
    for (auto* t : s_tools) if (t) lv_group_add_obj(s_encGroup, t);
    for (auto* h : s_heads) if (h) lv_group_add_obj(s_encGroup, h);
    bool alive = false;
    for (uint32_t k = 0; k < lv_group_get_obj_count(s_encGroup); ++k) {
        if (lv_group_get_obj_by_index(s_encGroup, k) == focused) { alive = true; break; }
    }
    if (alive) {
        lv_group_focus_obj(focused);
    } else {
        for (int i = 0; i < s_blockCount; ++i) {
            if (s_blocks[i].track == s_selTrack && s_blocks[i].index == s_selIndex) {
                lv_group_focus_obj(s_blocks[i].obj);
                break;
            }
        }
    }
#endif
}

void rebuild_timeline()
{
    auto* se = engine();
    if (!se || !s_timeline) return;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const auto& song = se->song();

    lv_obj_clean(s_timeline);
    s_playhead = nullptr;
    s_blockCount = 0;

    const uint32_t end = song.playEnd();
    /* Past the end as well, so there is room to lengthen the song or move a
     * block out -- and at least a screenful, so a short song zoomed out does
     * not leave the lanes half drawn. */
    const int32_t view = lv_obj_get_content_width(s_timeline);
    uint32_t bars = std::max(song.songEnd(), end) + 8;
    bars = std::max<uint32_t>(bars, (uint32_t)(view / s_pxPerBar) + 1);
    /* A tick per bar while they are far enough apart, else per 4; a number
     * wherever there is room for one, on a bar that starts a 4-bar phrase. */
    const uint32_t tickStep = s_pxPerBar >= 6 ? 1 : 4;
    uint32_t numStep = 4;
    while ((int32_t)numStep * s_pxPerBar < 22) numStep *= 2;
    if (s_pxPerBar >= 24) numStep = 1;
    for (uint32_t bar = 0; bar < bars; bar += tickStep) {
        lv_obj_t* t = lv_obj_create(s_timeline);
        lv_obj_remove_style_all(t);
        const bool major = bar % 4 == 0;
        lv_obj_set_size(t, 1, major ? 6 : 3);
        lv_obj_set_style_bg_color(t, kDim, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_pos(t, (int32_t)bar * s_pxPerBar, kRulerH - (major ? 6 : 3));
        if (bar % numStep == 0) {
            lv_obj_t* num = lv_label_create(s_timeline);
            lv_obj_set_style_text_font(num, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(num, kDim, 0);
            lv_label_set_text_fmt(num, "%u", (unsigned)(bar + 1));
            lv_obj_set_pos(num, (int32_t)bar * s_pxPerBar + 2, -2);
        }
    }

    for (int tr = 0; tr < kTracks; ++tr) {
        const int32_t y = kRulerH + 2 + tr * (kLaneH + kLaneGap);
        lv_obj_t* lane = lv_obj_create(s_timeline);
        lv_obj_remove_style_all(lane);
        lv_obj_set_pos(lane, 0, y);
        lv_obj_set_size(lane, (int32_t)bars * s_pxPerBar, kLaneH);
        lv_obj_set_style_bg_color(lane, kLane, 0);
        lv_obj_set_style_bg_opa(lane, LV_OPA_COVER, 0);
        lv_obj_remove_flag(lane, LV_OBJ_FLAG_CLICKABLE);

        const auto& cs = song.tracks[tr].clips;
        for (int i = 0; i < (int)cs.size() && i < kMaxBlocksPerTrack && s_blockCount < kMaxBlocks; ++i) {
            const auto& c = cs[i];
            lv_obj_t* o = lv_obj_create(s_timeline);
            lv_obj_remove_style_all(o);
            lv_obj_set_pos(o, (int32_t)c.bar * s_pxPerBar, y);
            lv_obj_set_size(o, std::max<int32_t>(2, (int32_t)c.bars * s_pxPerBar - 1), kLaneH);
            lv_obj_set_style_radius(o, 3, 0);
            lv_obj_set_style_bg_color(o, lv_color_hex(clip_color(c.file)), 0);
            lv_obj_set_style_bg_opa(o, song.tracks[tr].muted ? LV_OPA_30 : LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(o, 2, 0);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
            lv_obj_add_event_cb(o, block_cb, LV_EVENT_FOCUSED, (void*)(intptr_t)s_blockCount);
            lv_obj_add_event_cb(o, block_cb, LV_EVENT_CLICKED, (void*)(intptr_t)s_blockCount);

            lv_obj_t* lb = lv_label_create(o);
            lv_obj_set_style_text_font(lb, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(lb, kInk, 0);
            lv_label_set_long_mode(lb, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(lb, LV_PCT(100));
            lv_label_set_text(lb, short_name(c.file).c_str());

            s_blocks[s_blockCount++] = Block{o, tr, i};
        }
    }

    /* The end of the song: a line, and what lies past it dimmed -- blocks
     * there are kept but not played. */
    if (end) {
        const int32_t h = kRulerH + 2 + kTracks * (kLaneH + kLaneGap);
        lv_obj_t* past = lv_obj_create(s_timeline);
        lv_obj_remove_style_all(past);
        lv_obj_set_pos(past, (int32_t)end * s_pxPerBar, 0);
        lv_obj_set_size(past, (int32_t)(bars - std::min(bars, end)) * s_pxPerBar, h);
        lv_obj_set_style_bg_color(past, kBg, 0);
        lv_obj_set_style_bg_opa(past, LV_OPA_60, 0);
        lv_obj_remove_flag(past, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* line = lv_obj_create(s_timeline);
        lv_obj_remove_style_all(line);
        lv_obj_set_pos(line, (int32_t)end * s_pxPerBar - 1, 0);
        lv_obj_set_size(line, 2, h);
        lv_obj_set_style_bg_color(line, song.length ? kAccent : kDim, 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }

    s_playhead = lv_obj_create(s_timeline);
    lv_obj_remove_style_all(s_playhead);
    lv_obj_set_size(s_playhead, 2, kRulerH + 2 + kTracks * (kLaneH + kLaneGap));
    lv_obj_set_style_bg_color(s_playhead, kHead, 0);
    lv_obj_set_style_bg_opa(s_playhead, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_playhead, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_playhead, LV_OBJ_FLAG_HIDDEN);

    for (int t = 0; t < kTracks; ++t) {
        if (!s_heads[t]) continue;
        const bool m = song.tracks[t].muted;
        lv_obj_set_style_bg_color(s_heads[t], m ? kMute : kPanel, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_heads[t], 0), m ? kInk : kText, 0);
    }
    lv_label_set_text(s_projLabel, song.name.empty() ? "(no card)" : song.name.c_str());

    /* The selection by what it was, not where: an edit may have re-sorted. */
    const auto& sel = song.tracks[s_selTrack].clips;
    if (s_selIndex >= (int)sel.size()) s_selIndex = (int)sel.size() - 1;
    s_shape = song_shape(song);
    rebuild_group();
    select(s_selTrack, s_selIndex);
}

// ── transport and position ─────────────────────────────────────────────────

/* Only what changed: setting a style or a text, even to what it already is,
 * redraws the object, and these run every tick -- while the song streams from
 * the card on the same core. */
void set_bg(lv_obj_t* o, lv_color_t c)
{
    if (o && !lv_color_eq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), c)) lv_obj_set_style_bg_color(o, c, 0);
}

void set_ink(lv_obj_t* o, lv_color_t c)
{
    if (o && !lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c)) lv_obj_set_style_text_color(o, c, 0);
}

void set_text(lv_obj_t* l, const char* text)
{
    if (l && std::strcmp(lv_label_get_text(l), text) != 0) lv_label_set_text(l, text);
}

void sync_transport()
{
    auto* se = engine();
    if (!se) return;
    const bool playing = se->playing();
    set_text(lv_obj_get_child(s_playBtn, 0), playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    set_bg(s_playBtn, playing ? kAccent : kPanel);
    set_ink(lv_obj_get_child(s_playBtn, 0), playing ? kInk : kText);
    bool loop = true, sync = false;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        loop = se->song().loop;
        sync = se->song().syncSeq;
    }
    set_bg(s_loopBtn, loop ? kAccent : kPanel);
    set_ink(lv_obj_get_child(s_loopBtn, 0), loop ? kInk : kDim);
    set_bg(s_syncBtn, sync ? kAccent : kPanel);
    set_ink(lv_obj_get_child(s_syncBtn, 0), sync ? kInk : kDim);
}

void fill_clips();
void fill_projects();

void tick_cb(lv_timer_t*)
{
    auto* se = engine();
    if (!se) return;
    if (s_pending != Pending::None && se->listGeneration() != s_pendingGen) {
        const Pending what = s_pending;
        s_pending = Pending::None;
        if (s_loading) { lv_obj_delete(s_loading); s_loading = nullptr; }
        if (what == Pending::Projects) fill_projects();
        else                           fill_clips();
    }
    if (s_opening) {
        const auto st = se->openState();
        if (st == crosspad::SongOpState::Busy) return;
        s_opening = false;
        rebuild_timeline();
        if (st == crosspad::SongOpState::Failed && s_infoLabel) {
            lv_label_set_text(s_infoLabel, "That song could not be read; it is left untouched on the card.");
        }
    }
    if (s_overlay) return;
    uint32_t shape = 0, total = 0;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        shape = song_shape(se->song());
        total = se->song().playEnd();
    }
    if (shape != s_shape) rebuild_timeline();
    sync_transport();

    if (!s_playhead) return;
    char buf[32];
    if (!se->playing()) {
        if (!lv_obj_has_flag(s_playhead, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(s_playhead, LV_OBJ_FLAG_HIDDEN);
        std::snprintf(buf, sizeof(buf), "%u bars", (unsigned)total);
        set_text(s_posLabel, buf);
        return;
    }
    const double pos = se->positionBars();
    const int32_t x = (int32_t)(pos * s_pxPerBar);
    if (lv_obj_has_flag(s_playhead, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(s_playhead, LV_OBJ_FLAG_HIDDEN);
    if (lv_obj_get_x(s_playhead) != x) lv_obj_set_pos(s_playhead, x, 0);
    std::snprintf(buf, sizeof(buf), "bar %d / %u", (int)pos + 1, (unsigned)total);
    set_text(s_posLabel, buf);

    const int32_t view = lv_obj_get_content_width(s_timeline);
    const int32_t left = lv_obj_get_scroll_x(s_timeline);
    if (x < left || x > left + view * 2 / 3) {
        lv_obj_scroll_to_x(s_timeline, x > view / 3 ? x - view / 3 : 0, LV_ANIM_ON);
    }
}

// ── editing ────────────────────────────────────────────────────────────────

/// Run @p edit on the selected block under the song lock, commit, and keep
/// the selection on the block the edit returned -- the moved one, or the copy
/// a duplicate appended -- wherever it now sorts. Moved into place by hand
/// rather than found again afterwards: two blocks of one clip on one bar
/// look alike, and a search can land on the other one.
template <typename F>
void edit_selected(F edit)
{
    auto* se = engine();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        auto& cs = song.tracks[s_selTrack].clips;
        if (s_selIndex < 0 || s_selIndex >= (int)cs.size()) return;
        const size_t before = cs.size();
        const crosspad::SongClip keep = edit(song, cs[s_selIndex]);
        const size_t at = cs.size() > before ? cs.size() - 1 : (size_t)s_selIndex;
        cs.erase(cs.begin() + at);
        auto pos = std::upper_bound(cs.begin(), cs.end(), keep,
            [](const crosspad::SongClip& a, const crosspad::SongClip& b) { return a.bar < b.bar; });
        s_selIndex = (int)(pos - cs.begin());
        cs.insert(pos, keep);
    }
    se->commit();
    rebuild_timeline();
}

void left_cb(lv_event_t*)
{
    edit_selected([](crosspad::AudioSong&, crosspad::SongClip& c) {
        if (c.bar) --c.bar;
        return c;
    });
}

void right_cb(lv_event_t*)
{
    edit_selected([](crosspad::AudioSong&, crosspad::SongClip& c) {
        ++c.bar;
        return c;
    });
}

/// A copy straight after the block, which is how a part is repeated.
void dup_cb(lv_event_t*)
{
    edit_selected([](crosspad::AudioSong& song, crosspad::SongClip& c) {
        crosspad::SongClip copy = c;
        copy.bar = c.bar + c.bars;
        song.tracks[s_selTrack].clips.push_back(copy);
        return copy;
    });
}

void del_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& cs = se->song().tracks[s_selTrack].clips;
        if (s_selIndex < 0 || s_selIndex >= (int)cs.size()) return;
        cs.erase(cs.begin() + s_selIndex);
        if (s_selIndex >= (int)cs.size()) s_selIndex = (int)cs.size() - 1;
    }
    se->commit();
    rebuild_timeline();
}

void head_cb(lv_event_t* e)
{
    auto* se = engine();
    if (!se) return;
    const int t = (int)(intptr_t)lv_event_get_user_data(e);
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& tr = se->song().tracks[t];
        tr.muted = !tr.muted;
        const bool keep = s_selTrack == t;
        s_selTrack = t;
        if (!keep) s_selIndex = tr.clips.empty() ? -1 : 0;
    }
    se->commit();
    rebuild_timeline();
}

void loop_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        se->song().loop = !se->song().loop;
    }
    se->commit();
    sync_transport();
}

/// The sequencer along with the song, from the next Play on: started with
/// it on the same sample, at its tempo, the scene laid on its bars.
void sync_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        se->song().syncSeq = !se->song().syncSeq;
    }
    se->commit();
    sync_transport();
}

/// From the selected block, or from the top: the place you are looking at is
/// the place you want to hear.
void play_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se) return;
    if (se->playing()) { se->stop(); sync_transport(); return; }
    uint32_t from = 0;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& cs = se->song().tracks[s_selTrack].clips;
        if (s_selIndex >= 0 && s_selIndex < (int)cs.size()) from = cs[s_selIndex].bar;
    }
    se->play(from);
    sync_transport();
}

/// Zoom about the middle of what is on screen: the bar there stays there.
void zoom(int dir)
{
    const int z = std::max(0, std::min(kZoomLevels - 1, s_zoom + dir));
    if (z == s_zoom || !s_timeline) return;
    const int32_t view = lv_obj_get_content_width(s_timeline);
    const int32_t mid = lv_obj_get_scroll_x(s_timeline) + view / 2;
    const int32_t oldPx = s_pxPerBar;
    s_zoom = z;
    s_pxPerBar = kZoomPx[z];
    rebuild_timeline();
    lv_obj_update_layout(s_timeline);
    const int32_t x = mid * s_pxPerBar / oldPx - view / 2;
    lv_obj_scroll_to_x(s_timeline, x > 0 ? x : 0, LV_ANIM_OFF);
}

void zoom_out_cb(lv_event_t*) { zoom(-1); }
void zoom_in_cb(lv_event_t*) { zoom(+1); }

void back_cb(lv_event_t* e)
{
    auto* app = static_cast<App*>(lv_event_get_user_data(e));
    if (app) app->destroyAppAsync();
}

// ── overlays: project picker, clip library ─────────────────────────────────

void close_overlay()
{
    s_pending = Pending::None;
    s_loading = nullptr;
    if (!s_overlay) return;
#ifdef ARR_USE_ENCODER
    if (indev_encoderTop && s_encGroup) lv_indev_set_group(indev_encoderTop, s_encGroup);
    if (s_overlayGroup) { lv_group_delete(s_overlayGroup); s_overlayGroup = nullptr; }
#endif
    lv_obj_delete_async(s_overlay);
    s_overlay = nullptr;
}

lv_obj_t* overlay_list(const char* title)
{
    s_overlay = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(s_overlay, kBg, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_overlay, 6, 0);
    lv_obj_set_style_pad_row(s_overlay, 3, 0);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
#ifdef ARR_USE_ENCODER
    s_overlayGroup = lv_group_create();
    if (indev_encoderTop) lv_indev_set_group(indev_encoderTop, s_overlayGroup);
#endif
    lv_obj_t* t = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(t, kText, 0);
    lv_label_set_text(t, title);
    return s_overlay;
}

lv_obj_t* overlay_item(const char* text, lv_event_cb_t cb, void* ud, bool accent = false)
{
    lv_obj_t* b = lv_button_create(s_overlay);
    lv_obj_set_size(b, LV_PCT(100), 24);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, accent ? kAccent : kPanel, 0);
    lv_obj_set_style_outline_color(b, kHead, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, accent ? kInk : kText, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_text(l, text);
    lv_obj_center(l);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, b);
#endif
    return b;
}

std::vector<std::string> s_projects;
std::vector<crosspad::ClipInfo> s_library;

void project_pick_cb(lv_event_t* e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    auto* se = engine();
    std::string name;
    if (i < 0) {
        /* "Song N", the first N not taken. */
        for (int n = 1; n < 1000; ++n) {
            name = "Song " + std::to_string(n);
            bool taken = false;
            for (const auto& p : s_projects) if (p == name) { taken = true; break; }
            if (!taken) break;
        }
    } else if (i < (int)s_projects.size()) {
        name = s_projects[i];
    }
    close_overlay();
    if (!se || name.empty()) return;
    s_selTrack = 0;
    s_selIndex = -1;
    s_opening = se->requestOpen(name);
    if (s_opening && s_infoLabel) lv_label_set_text_fmt(s_infoLabel, "Opening %s ...", name.c_str());
}

void cancel_cb(lv_event_t*) { close_overlay(); }

void request_list(Pending what, const char* title)
{
    auto* se = engine();
    if (!se || s_overlay) return;
    s_pendingGen = se->listGeneration();
    const bool asked = what == Pending::Projects ? se->requestProjects() : se->requestClips();
    overlay_list(title);
    s_loading = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(s_loading, kDim, 0);
    lv_label_set_text(s_loading, asked ? "Reading the card ..." : "The card is busy, try again.");
    s_pending = asked ? what : Pending::None;
    if (!asked) overlay_item(LV_SYMBOL_CLOSE " Cancel", cancel_cb, nullptr);
}

void fill_projects()
{
    auto* se = engine();
    s_projects = se->projects();
    overlay_item(LV_SYMBOL_PLUS " New song", project_pick_cb, (void*)(intptr_t)-1, true);
    for (size_t i = 0; i < s_projects.size() && i < 24; ++i) {
        overlay_item(s_projects[i].c_str(), project_pick_cb, (void*)(intptr_t)i);
    }
    overlay_item(LV_SYMBOL_CLOSE " Cancel", cancel_cb, nullptr);
}

void project_cb(lv_event_t*)
{
    request_list(Pending::Projects, "Songs");
}

/// A library clip onto the selected track, after its last block.
void library_pick_cb(lv_event_t* e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    auto* se = engine();
    close_overlay();
    if (!se || i < 0 || i >= (int)s_library.size()) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        crosspad::SongClip c;
        c.file = s_library[i].file;
        c.bars = s_library[i].bars;
        c.bar  = song.trackEnd(s_selTrack);
        s_selIndex = song.addClip(s_selTrack, c);
    }
    se->commit();
    rebuild_timeline();
}

void add_cb(lv_event_t*)
{
    char title[48];
    lv_snprintf(title, sizeof(title), "Add to track %d", s_selTrack + 1);
    request_list(Pending::Clips, title);
}

void fill_clips()
{
    auto* se = engine();
    s_library = se->clips();
    for (size_t i = 0; i < s_library.size() && i < 32; ++i) {
        char row[64];
        lv_snprintf(row, sizeof(row), "%s  (%u bars)", short_name(s_library[i].file).c_str(),
                    (unsigned)s_library[i].bars);
        overlay_item(row, library_pick_cb, (void*)(intptr_t)i);
    }
    if (s_library.empty()) {
        lv_obj_t* l = lv_label_create(s_overlay);
        lv_obj_set_style_text_color(l, kDim, 0);
        lv_label_set_text(l, "No clips yet -- BOUNCE a scene in the Sequencer.");
    }
    overlay_item(LV_SYMBOL_CLOSE " Cancel", cancel_cb, nullptr);
}

/// The LEN window: the song's length in bars, by one or by a phrase of four,
/// or back to following the last block. The timeline behind it is rebuilt
/// when the window closes.
void show_length()
{
    auto* se = engine();
    if (!se || !s_lenLabel) return;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const auto& song = se->song();
    if (song.length) {
        lv_label_set_text_fmt(s_lenLabel, "%u bars", (unsigned)song.length);
    } else {
        lv_label_set_text_fmt(s_lenLabel, "%u bars  (auto: to the last block)", (unsigned)song.songEnd());
    }
}

void length_step_cb(lv_event_t* e)
{
    const int step = (int)(intptr_t)lv_event_get_user_data(e);
    auto* se = engine();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        if (step == 0) {
            song.length = 0;
        } else {
            const int64_t from = song.playEnd() ? song.playEnd() : 0;
            const int64_t to = std::max<int64_t>(1, std::min<int64_t>(kMaxLength, from + step));
            song.length = (uint32_t)to;
        }
    }
    se->commit();
    show_length();
}

void length_done_cb(lv_event_t*)
{
    close_overlay();
    s_lenLabel = nullptr;
    rebuild_timeline();
}

void length_cb(lv_event_t*)
{
    if (!engine() || s_overlay) return;
    overlay_list("Song length");
    s_lenLabel = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_lenLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lenLabel, kAccent, 0);
    show_length();
    overlay_item(LV_SYMBOL_PLUS " 1 bar", length_step_cb, (void*)(intptr_t)1);
    overlay_item(LV_SYMBOL_MINUS " 1 bar", length_step_cb, (void*)(intptr_t)-1);
    overlay_item(LV_SYMBOL_PLUS " 4 bars", length_step_cb, (void*)(intptr_t)4);
    overlay_item(LV_SYMBOL_MINUS " 4 bars", length_step_cb, (void*)(intptr_t)-4);
    overlay_item("Auto: to the last block", length_step_cb, (void*)(intptr_t)0);
    overlay_item(LV_SYMBOL_OK " Done", length_done_cb, nullptr, true);
}

// ── building ───────────────────────────────────────────────────────────────

lv_obj_t* make_button(lv_obj_t* parent, int32_t w, const char* text, lv_event_cb_t cb, void* ud = nullptr)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, w, 22);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, kPanel, 0);
    lv_obj_set_style_outline_color(b, kHead, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(l, kText, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

lv_obj_t* make_row(lv_obj_t* parent, int32_t h)
{
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), h);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 4, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

} // namespace

lv_obj_t* Arrange_create(lv_obj_t* parent, App* app)
{
#ifdef ARR_USE_ENCODER
    if (indev_encoderTop) s_prevEncGroup = lv_indev_get_group(indev_encoderTop);
    if (!s_encGroup) s_encGroup = lv_group_create();
    if (indev_encoderTop) lv_indev_set_group(indev_encoderTop, s_encGroup);
#endif

    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, kBg, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 5, 0);
    lv_obj_set_style_pad_row(s_root, 4, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    // -- top bar --
    lv_obj_t* bar = make_row(s_root, 22);
    s_backBtn = make_button(bar, 30, LV_SYMBOL_LEFT, back_cb, app);
    s_projBtn = make_button(bar, 90, "", project_cb);
    s_projLabel = lv_obj_get_child(s_projBtn, 0);
    lv_label_set_long_mode(s_projLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_projLabel, 84);
    lv_obj_set_style_text_align(s_projLabel, LV_TEXT_ALIGN_CENTER, 0);
    s_posLabel = lv_label_create(bar);
    lv_obj_set_flex_grow(s_posLabel, 1);
    lv_obj_set_style_text_font(s_posLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_posLabel, kDim, 0);
    lv_obj_set_style_text_align(s_posLabel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_posLabel, "");
    s_syncBtn = make_button(bar, 40, "SYNC", sync_cb);
    s_loopBtn = make_button(bar, 40, "LOOP", loop_cb);
    s_playBtn = make_button(bar, 40, LV_SYMBOL_PLAY, play_cb);

    // -- lanes: fixed headers, the timeline scrolling beside them --
    lv_obj_t* body = make_row(s_root, kRulerH + 4 + kTracks * (kLaneH + kLaneGap));
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(body, 3, 0);

    lv_obj_t* heads = lv_obj_create(body);
    lv_obj_remove_style_all(heads);
    lv_obj_set_size(heads, kHeadW, LV_PCT(100));
    lv_obj_remove_flag(heads, LV_OBJ_FLAG_SCROLLABLE);
    for (int t = 0; t < kTracks; ++t) {
        lv_obj_t* h = lv_button_create(heads);
        lv_obj_set_pos(h, 0, kRulerH + 4 + t * (kLaneH + kLaneGap));
        lv_obj_set_size(h, kHeadW, kLaneH);
        lv_obj_set_style_radius(h, 4, 0);
        lv_obj_set_style_shadow_width(h, 0, 0);
        lv_obj_set_style_pad_all(h, 0, 0);
        lv_obj_set_style_bg_color(h, kPanel, 0);
        lv_obj_set_style_border_color(h, kAccent, 0);
        lv_obj_set_style_outline_color(h, kHead, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_width(h, 2, LV_STATE_FOCUS_KEY);
        lv_obj_add_event_cb(h, head_cb, LV_EVENT_CLICKED, (void*)(intptr_t)t);
        lv_obj_t* l = lv_label_create(h);
        lv_obj_set_style_text_color(l, kText, 0);
        lv_label_set_text_fmt(l, "%d", t + 1);
        lv_obj_center(l);
        s_heads[t] = h;
    }

    s_timeline = lv_obj_create(body);
    lv_obj_remove_style_all(s_timeline);
    lv_obj_set_height(s_timeline, LV_PCT(100));
    lv_obj_set_flex_grow(s_timeline, 1);
    lv_obj_set_style_bg_color(s_timeline, kPanel, 0);
    lv_obj_set_style_bg_opa(s_timeline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_timeline, 4, 0);
    lv_obj_set_style_pad_top(s_timeline, 2, 0);
    lv_obj_set_style_pad_hor(s_timeline, 2, 0);
    lv_obj_set_scroll_dir(s_timeline, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_timeline, LV_SCROLLBAR_MODE_OFF);

    s_infoLabel = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_infoLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_infoLabel, kDim, 0);
    lv_label_set_long_mode(s_infoLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_infoLabel, LV_PCT(100));

    // -- tools on the selected block --
    lv_obj_t* tools = make_row(s_root, 24);
    lv_obj_set_style_pad_column(tools, 3, 0);
    s_tools[0] = make_button(tools, 30, LV_SYMBOL_LEFT, left_cb);
    s_tools[1] = make_button(tools, 30, LV_SYMBOL_RIGHT, right_cb);
    s_tools[2] = make_button(tools, 38, "DUP", dup_cb);
    s_tools[3] = make_button(tools, 38, "DEL", del_cb);
    s_tools[4] = make_button(tools, 46, LV_SYMBOL_PLUS " ADD", add_cb);
    s_tools[5] = make_button(tools, 38, "LEN", length_cb);
    s_tools[6] = make_button(tools, 34, "Z-", zoom_out_cb);
    s_tools[7] = make_button(tools, 34, "Z+", zoom_in_cb);

    lv_obj_update_layout(s_root);
    s_shape = 0;
    rebuild_timeline();
    sync_transport();
    if (!engine()) lv_label_set_text(s_infoLabel, "No song engine on this platform.");

#ifdef ARR_USE_ENCODER
    if (s_encGroup) lv_group_focus_obj(s_playBtn);
#endif

    s_timer = lv_timer_create(tick_cb, kTickMs, nullptr);

    if (app) {
        app->setOnShow([](lv_obj_t*) {
            if (s_timer) lv_timer_resume(s_timer);
#ifdef ARR_USE_ENCODER
            if (indev_encoderTop && s_encGroup) lv_indev_set_group(indev_encoderTop, s_encGroup);
#endif
        });
        app->setOnHide([](lv_obj_t*) {
            if (s_timer) lv_timer_pause(s_timer);
#ifdef ARR_USE_ENCODER
            if (indev_encoderTop && s_prevEncGroup) lv_indev_restore_group(indev_encoderTop, s_prevEncGroup);
#endif
        });
    }
    return s_root;
}

void Arrange_destroy(lv_obj_t* root)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    /* The song keeps playing when the screen closes -- it is the song
     * engine's, not this screen's. Only the widgets go. */
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) { lv_group_delete(s_overlayGroup); s_overlayGroup = nullptr; }
#endif
    s_overlay = nullptr;
    s_loading = nullptr;
    s_pending = Pending::None;
    s_opening = false;
    s_lenLabel = nullptr;
    for (auto& b : s_blocks) b = Block{};
    for (auto& h : s_heads) h = nullptr;
    for (auto& t : s_tools) t = nullptr;
    s_blockCount = 0;
    s_root = s_timeline = s_playhead = s_projLabel = s_posLabel = s_infoLabel = nullptr;
    s_playBtn = s_loopBtn = s_syncBtn = s_backBtn = s_projBtn = nullptr;

#ifdef ARR_USE_ENCODER
    if (indev_encoderTop && s_prevEncGroup) lv_indev_restore_group(indev_encoderTop, s_prevEncGroup);
    if (s_encGroup) {
        lv_group_delete(s_encGroup);
        s_encGroup = nullptr;
    }
    s_prevEncGroup = nullptr;
#endif

    lv_obj_delete_async(root);
}

// No pad logic: the pads keep playing the kit over the song.
REGISTER_APP(Arrange, nullptr, "arrange.png",
             Arrange_create, Arrange_destroy,
             nullptr, nullptr, nullptr, 0)
