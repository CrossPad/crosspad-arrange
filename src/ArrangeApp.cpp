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
 *   tools  MOVE, DUP and DEL act on the selected block; ADD a clip from the
 *          project's library to the selected track, EDIT, MIX, and zoom
 *          out/in on the timeline
 *   MIX    the four tracks' levels: a strip each side by side, as on a desk --
 *          a tall fader in dB (-inf, then 0.6 dB a step as in the Mixer app,
 *          unity at 90 %, +6 dB at the top), the dB, M; heard at once, saved
 *          with the song
 *   EDIT   the loop region (L and R, turned like MOVE; with LOOP on the song
 *          goes round it, and PLAY starts at it), copy / paste / duplicate x2
 *          of the region across every track -- inserted, so nothing after it
 *          is written over -- merging tracks, or all of them, into one clip
 *          over the region or the whole song, and the song's length
 *
 * The encoder walks the blocks; a click on one selects it and takes the
 * encoder down to MOVE/DUP/DEL. DUP and DEL act and bring it back to the
 * blocks; MOVE is turned -- each detent a bar -- and the next click brings it
 * back. The power button's click also goes back up. The songs list edits the
 * open song: its tempo (each song has its own) and its name, or deletes it.
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
#include "crosspad-gui/components/status_bar.h"
#include "crosspad-gui/widgets/gui_helpers.h"

#include "lvgl.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
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

/* ── set_text_fmt() is not used here, and this is why ─────────────
 *
 * It frees the label's old text and allocates the new one, but it does not
 * revert the dots of a truncated label first -- lv_label_set_text() does
 * exactly that, one line in, "in case text == label->text". So when a long
 * line is replaced by a shorter one, LVGL still holds the offset the dots were
 * written at, and on the next refresh it puts the four saved characters back at
 * that offset -- past the end of the new, shorter buffer. The heap block next
 * door loses its tail canary, and the crash lands minutes later on whatever
 * frees that block: an assert deep inside LVGL, with nothing in the backtrace
 * to say who wrote the bytes. It read as "Arrange crashes when I click a
 * block", because this screen is the one that formats a line of changing length
 * on every click.
 *
 * Formatting it ourselves and handing lv_label_set_text() a finished string is
 * both the safe way and what every other screen in this firmware does. */
static void set_text_fmt(lv_obj_t* label, const char* fmt, ...)
{
    if (!label || !fmt) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(label, buf);
}

constexpr int      kTracks   = crosspad::AudioSong::kTracks;
/* Zoom: pixels per bar, from a whole song on one screen to single beats. */
constexpr int32_t  kZoomPx[] = {3, 6, 12, 24, 48};
constexpr int      kZoomLevels = sizeof(kZoomPx) / sizeof(kZoomPx[0]);
constexpr uint32_t kMaxLength = 999;
constexpr int32_t  kRulerH   = 12;
constexpr int32_t  kLaneH    = 30;
constexpr int32_t  kLaneGap  = 2;
constexpr int32_t  kHeadW    = 40;     // a finger's width: tap to mute
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
/* A block whose audio is not on the card: grey, and it says so on itself. Not
 * red -- nothing is broken, the song remembers what it played and the file did
 * not travel with it -- and not the clip's own colour either, because a block
 * that looks like the others is read as one that plays. */
const lv_color_t kMissing = lv_color_hex(0x6E757E);
const lv_color_t kHead   = lv_color_hex(0xFFFFFF);
const lv_color_t kTrack  = lv_color_hex(0x3A3F47);     // a fader's travel

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

/* A block drawn, and its clip's waveform once the engine has it. */
struct Block {
    lv_obj_t*   obj = nullptr;
    int         track = 0;
    int         index = 0;
    std::string file;
    uint32_t    color = 0;
    bool        muted = false;
    std::shared_ptr<const std::vector<uint8_t>> peaks;
    uint8_t     peakMax = 0;       ///< the loudest value: the waveform is drawn to it
    bool        missing = false;   ///< its file is not in the project's clips folder
};

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
/* MOVE, DUP, DEL (the selected block's, reached by clicking it), then ADD,
 * EDIT, MIX, zoom out, zoom in. */
constexpr int kBlockTools = 3;
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
/* Where a waveform's values fall: frames per bar at the song's tempo, frames
 * per value, and the engine's count of waveforms that have come in. */
uint32_t s_framesPerBar = 0;
uint32_t s_peakHop = 0;
uint32_t s_peaksGen = 0;
/// A block's waveform starts under its name.
constexpr int32_t kWaveTop = 13;
/* Drawn as bars, 2 px and a 1 px gap, on the block's own grid so scrolling
 * does not shimmer: every fill is a draw task, and LVGL 9.3 walks its whole
 * list to add one, so a column each made a redraw cost the square of it. */
constexpr int32_t kWaveBarW = 2;
constexpr int32_t kWaveBarPitch = 3;

/* The card is read on the platform's side; an overlay opens at once with a
 * line saying so and is filled when the list comes back. */
enum class Pending : uint8_t { None, Projects, Clips };
Pending   s_pending = Pending::None;
uint32_t  s_pendingGen = 0;
lv_obj_t* s_loading = nullptr;
/* A project asked for and not open yet: the timeline waits for it. */
bool      s_opening = false;
/* A rename, tempo change, delete or export the platform is doing on the card. */
enum class EditKind : uint8_t { Tempo, Rename, Delete, Export };
bool      s_editing = false;
EditKind  s_editKind = EditKind::Tempo;
/* The song window's tempo row, the MIX window's dB and M; they live and die
 * with the overlay. */
lv_obj_t* s_tempoItem = nullptr;
lv_obj_t* s_songList = nullptr;     // a song window's list, under its header
lv_obj_t* s_mixDb[kTracks] = {};
lv_obj_t* s_mixMute[kTracks] = {};

#ifdef ARR_USE_ENCODER
lv_group_t* s_encGroup = nullptr;
lv_group_t* s_prevEncGroup = nullptr;
lv_group_t* s_overlayGroup = nullptr;
lv_group_t* s_toolGroup = nullptr;    // MOVE/DUP/DEL while a block is clicked
lv_group_t* s_regionGroup = nullptr;  // the region handle while L or R is turned
#endif
bool s_toolMode = false;
/* MOVE's axis while it is turned: first click along the bars, second across
 * the tracks, third done. Arrows beside the block show which, and a finger
 * can tap them. */
enum class MoveAxis : uint8_t { None, Bars, Tracks };
MoveAxis  s_moveAxis = MoveAxis::None;
lv_obj_t* s_moveArrows[4] = {};       // left, right, up, down; children of the timeline

/* The loop region moved with the encoder: first its start (L), then its end
 * (R). A hidden editable handle in a group of its own takes the turns. */
enum class RegionEdit : uint8_t { None, Start, End };
RegionEdit s_regionEdit = RegionEdit::None;
lv_obj_t*  s_regionHandle = nullptr;

/* Copy region: the blocks of every track that start inside it, by track and
 * by bar from its start. */
struct Copied { int track; uint32_t rel; crosspad::SongClip clip; };
std::vector<Copied> s_clipboard;
uint32_t  s_clipboardBars = 0;

/* A merge the platform is doing on the card, and the tracks picked for one. */
bool      s_merging = false;
uint8_t   s_mergeMask = 0x0F;
lv_obj_t* s_mergeToggles[kTracks] = {};

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
    mix(s.loopStart);
    mix(s.loopEnd);
    mix(s.loop);
    mix((uint32_t)lroundf(s.bpm * 100.0f));     // where each clip's waveform falls on the bars
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

/// A block's waveform, under its name: the clip at the speed it plays, so a
/// file shorter than its block ends early, and one made at another tempo
/// shows where its beats really fall. Drawn to the clip's loudest value --
/// the lanes are too short for a quiet clip at full scale -- and only the
/// bars in the area being redrawn.
void block_draw_cb(lv_event_t* e)
{
    const int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= s_blockCount) return;
    const Block& b = s_blocks[k];
    if (!b.peaks || b.peaks->empty() || !b.peakMax || !s_framesPerBar || !s_peakHop || !s_pxPerBar) return;
    lv_layer_t* layer = lv_event_get_layer(e);
    const lv_area_t& clip = layer->_clip_area;
    lv_area_t a;
    lv_obj_get_coords(b.obj, &a);
    const int32_t top = a.y1 + kWaveTop, bottom = a.y2 - 1;
    const int32_t mid = (top + bottom) / 2, half = (bottom - top) / 2;
    if (half <= 0 || bottom < clip.y1 || top > clip.y2) return;
    const int32_t x1 = std::max(a.x1, clip.x1), x2 = std::min(a.x2, clip.x2);
    if (x1 > x2) return;

    lv_draw_fill_dsc_t d;
    lv_draw_fill_dsc_init(&d);
    /* Dark on the clip's colour; on a muted track's faded block, the colour. */
    d.color = b.muted ? lv_color_hex(b.color) : kInk;
    d.opa = b.muted ? LV_OPA_70 : LV_OPA_60;
    const std::vector<uint8_t>& v = *b.peaks;
    const double perPx = (double)s_framesPerBar / (double)s_pxPerBar / (double)s_peakHop;
    /* A bar is the loudest value under its whole pitch, so no hit falls in a
     * gap; one with sound in it is never less than the silence line. Past
     * the file's end there is nothing. */
    for (int32_t bar = (x1 - a.x1) / kWaveBarPitch; bar <= (x2 - a.x1) / kWaveBarPitch; ++bar) {
        const int32_t px = bar * kWaveBarPitch;
        const size_t i0 = (size_t)(px * perPx);
        if (i0 >= v.size()) break;
        const size_t i1 = std::min(v.size(), std::max(i0 + 1, (size_t)((px + kWaveBarPitch) * perPx)));
        uint8_t m = 0;
        for (size_t i = i0; i < i1; ++i) m = std::max(m, v[i]);
        const int32_t h = m ? std::max<int32_t>(1, (int32_t)m * half / b.peakMax) : 0;
        const lv_area_t r = {a.x1 + px, mid - h, std::min(a.x2, a.x1 + px + kWaveBarW - 1), mid + h};
        lv_draw_fill(layer, &d, &r);
    }
}

/// @p b's waveform, and the value it is drawn to.
void set_peaks(Block& b, const crosspad::ClipPeaks& cp)
{
    b.peaks = cp.values;
    b.peakMax = cp.values && !cp.values->empty() ? *std::max_element(cp.values->begin(), cp.values->end()) : 0;
    b.missing = cp.missing;
    s_peakHop = cp.hop;
    /* A block with nothing behind it goes grey and says MISSING on itself,
     * rather than looking like a clip that happens to be silent -- which is how
     * a song whose audio did not travel with it reads as a bug in the
     * sequencer. The name is not lost: selecting the block puts it in the line
     * underneath, where there is room for it. */
    if (b.missing && b.obj) {
        lv_obj_set_style_bg_color(b.obj, kMissing, 0);
        lv_obj_set_style_bg_opa(b.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b.obj, 1, 0);
        lv_obj_set_style_border_color(b.obj, kBg, 0);
        if (lv_obj_t* l = lv_obj_get_child(b.obj, 0)) {
            lv_obj_set_style_text_color(l, kBg, 0);
            lv_label_set_text(l, "MISSING");
        }
    }
}

/// The waveforms that have come in since the blocks were drawn.
void take_peaks(crosspad::ISongEngine* se)
{
    for (int i = 0; i < s_blockCount; ++i) {
        Block& b = s_blocks[i];
        crosspad::ClipPeaks cp;
        if (b.peaks || !b.obj || !se->clipPeaks(b.file, cp)) continue;
        set_peaks(b, cp);
        lv_obj_invalidate(b.obj);
    }
}

void paint_block(const Block& b)
{
    if (!b.obj) return;
    const bool sel = b.track == s_selTrack && b.index == s_selIndex;
    lv_obj_set_style_outline_width(b.obj, sel ? 2 : 0, 0);
    lv_obj_set_style_outline_color(b.obj, kHead, 0);
    lv_obj_set_style_outline_pad(b.obj, 1, 0);
}

/// Whether the block standing for track @p tr's clip @p idx has no file.
bool block_missing(int tr, int idx)
{
    for (int i = 0; i < s_blockCount; ++i) {
        if (s_blocks[i].track == tr && s_blocks[i].index == idx) return s_blocks[i].missing;
    }
    return false;
}

void update_info()
{
    auto* se = engine();
    if (!se || !s_infoLabel) return;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const auto& song = se->song();
    if (s_regionEdit != RegionEdit::None) {
        set_text_fmt(s_infoLabel, "Loop region: bars %u - %u   |   turn to move %s, click for %s",
                              (unsigned)(song.loopStart + 1), (unsigned)song.loopEnd,
                              s_regionEdit == RegionEdit::Start ? "L" : "R",
                              s_regionEdit == RegionEdit::Start ? "R" : "done");
        return;
    }
    const auto& cs = song.tracks[s_selTrack].clips;
    if (s_selIndex >= 0 && s_selIndex < (int)cs.size()) {
        const auto& c = cs[s_selIndex];
        /* The name first and the reason plainly: a block whose file is gone is
         * the one thing on this screen that cannot be fixed on this screen. */
        if (block_missing(s_selTrack, s_selIndex)) {
            set_text_fmt(s_infoLabel, "%s  -  NOT IN clips/   |   bar %u, %u bars",
                                  short_name(c.file).c_str(), (unsigned)(c.bar + 1), (unsigned)c.bars);
            return;
        }
        set_text_fmt(s_infoLabel, "T%d  %s  -  bar %u, %u bars   |   %d bpm",
                              s_selTrack + 1, short_name(c.file).c_str(), (unsigned)(c.bar + 1),
                              (unsigned)c.bars, (int)lroundf(song.bpm));
    } else if (song.playEnd() == 0) {
        lv_label_set_text(s_infoLabel, "Empty song -- BOUNCE a scene in the Sequencer, or ADD a clip");
    } else {
        set_text_fmt(s_infoLabel, "Track %d selected   |   %d bpm   |   %u bars%s", s_selTrack + 1,
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

void enter_tools();
void place_move_arrows();

void block_cb(lv_event_t* e)
{
    const int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= s_blockCount) return;
    select(s_blocks[k].track, s_blocks[k].index);
    if (s_blocks[k].obj) lv_obj_scroll_to_view(s_blocks[k].obj, LV_ANIM_ON);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) enter_tools();
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
    for (auto* h : s_heads) if (h) lv_group_add_obj(s_encGroup, h);
    for (int t = kBlockTools; t < (int)(sizeof(s_tools) / sizeof(s_tools[0])); ++t) {
        if (s_tools[t]) lv_group_add_obj(s_encGroup, s_tools[t]);
    }
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
    s_framesPerBar = se->framesPerBar();
    s_peaksGen = se->peaksGeneration();
    s_playhead = nullptr;
    for (auto& a : s_moveArrows) a = nullptr;
    s_blockCount = 0;

    const uint32_t end = song.playEnd();
    /* Past the end as well, so there is room to lengthen the song or move a
     * block out -- and at least a screenful, so a short song zoomed out does
     * not leave the lanes half drawn. */
    const int32_t view = lv_obj_get_content_width(s_timeline);
    uint32_t bars = std::max({song.songEnd(), end, song.loopEnd}) + 8;
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
            set_text_fmt(num, "%u", (unsigned)(bar + 1));
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
            lv_obj_set_style_pad_top(o, 1, 0);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
            lv_obj_add_event_cb(o, block_cb, LV_EVENT_FOCUSED, (void*)(intptr_t)s_blockCount);
            lv_obj_add_event_cb(o, block_cb, LV_EVENT_CLICKED, (void*)(intptr_t)s_blockCount);
            lv_obj_add_event_cb(o, block_draw_cb, LV_EVENT_DRAW_MAIN_END, (void*)(intptr_t)s_blockCount);

            lv_obj_t* lb = lv_label_create(o);
            lv_obj_set_style_text_font(lb, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(lb, kInk, 0);
            lv_label_set_long_mode(lb, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(lb, LV_PCT(100));
            lv_label_set_text(lb, short_name(c.file).c_str());

            Block b;
            b.obj = o;
            b.track = tr;
            b.index = i;
            b.file = c.file;
            b.color = clip_color(c.file);
            b.muted = song.tracks[tr].muted;
            crosspad::ClipPeaks cp;
            if (se->clipPeaks(c.file, cp)) set_peaks(b, cp);
            s_blocks[s_blockCount++] = std::move(b);
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

    /* The loop region: a band on the ruler, bright while the song loops on
     * it, faint over the lanes, and a line at each end -- the one being
     * turned lit. */
    if (song.hasRegion()) {
        const int32_t h = kRulerH + 2 + kTracks * (kLaneH + kLaneGap);
        const int32_t x1 = (int32_t)song.loopStart * s_pxPerBar, x2 = (int32_t)song.loopEnd * s_pxPerBar;
        auto band = [](int32_t x, int32_t y, int32_t w, int32_t hh, lv_color_t c, lv_opa_t opa) {
            lv_obj_t* o = lv_obj_create(s_timeline);
            lv_obj_remove_style_all(o);
            lv_obj_set_pos(o, x, y);
            lv_obj_set_size(o, w, hh);
            lv_obj_set_style_bg_color(o, c, 0);
            lv_obj_set_style_bg_opa(o, opa, 0);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        };
        band(x1, kRulerH + 2, x2 - x1, h - kRulerH - 2, kAccent, LV_OPA_10);
        band(x1, 0, x2 - x1, kRulerH, kAccent, song.loop ? LV_OPA_70 : LV_OPA_30);
        band(x1 - 1, 0, 2, h, s_regionEdit == RegionEdit::Start ? kHead : kAccent, LV_OPA_COVER);
        band(x2 - 1, 0, 2, h, s_regionEdit == RegionEdit::End ? kHead : kAccent, LV_OPA_COVER);
    }

    s_playhead = lv_obj_create(s_timeline);
    lv_obj_remove_style_all(s_playhead);
    /* Arrows (if MOVE is on) go on last, over the blocks; placed below, once
     * the blocks are sized. */
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
    place_move_arrows();
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
void show_tempo();
void rename_failed();
void export_done(bool ok);

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
    if (s_merging) {
        char buf[48];
        if (se->editState() == crosspad::SongOpState::Busy) {
            std::snprintf(buf, sizeof(buf), "Merging ... %u%%", (unsigned)se->editProgress());
            set_text(s_infoLabel, buf);
        } else {
            s_merging = false;
            const bool ok = se->editState() == crosspad::SongOpState::Ready;
            rebuild_timeline();
            update_info();
            if (!ok) set_text(s_infoLabel, "Not merged: no card, nothing audible on those tracks, or the card is full.");
        }
    }
    if (s_editing && se->editState() != crosspad::SongOpState::Busy) {
        s_editing = false;
        const bool failed = se->editState() == crosspad::SongOpState::Failed;
        show_tempo();                 // the tempo row, if its window is still open
        rebuild_timeline();
        update_info();
        if (s_editKind == EditKind::Export) export_done(!failed);
        else if (failed && s_editKind == EditKind::Rename) rename_failed();
        else if (failed && s_editKind == EditKind::Delete && s_infoLabel) {
            lv_label_set_text(s_infoLabel, "Not deleted: no card, or the card kept some of its files.");
        } else if (failed && s_infoLabel) lv_label_set_text(s_infoLabel, "The tempo was not set: no song is open.");
    }
    if (se->peaksGeneration() != s_peaksGen) {
        s_peaksGen = se->peaksGeneration();
        take_peaks(se);
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

void move_arrow_cb(lv_event_t* e);

/// The arrows around the selected block while MOVE is on: the axis being
/// turned lit, the other dim, and none where the block cannot go.
void place_move_arrows()
{
    for (auto& a : s_moveArrows) {
        if (a) lv_obj_delete(a);
        a = nullptr;
    }
    if (s_moveAxis == MoveAxis::None || !s_timeline) return;
    lv_obj_t* blk = nullptr;
    for (int i = 0; i < s_blockCount; ++i) {
        if (s_blocks[i].track == s_selTrack && s_blocks[i].index == s_selIndex) { blk = s_blocks[i].obj; break; }
    }
    if (!blk) return;
    lv_obj_update_layout(blk);
    const int32_t x = lv_obj_get_x(blk), y = lv_obj_get_y(blk);
    const int32_t w = lv_obj_get_width(blk), h = lv_obj_get_height(blk);
    constexpr int32_t kA = 16;
    uint32_t bar = 0;
    if (auto* se = engine()) {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& cs = se->song().tracks[s_selTrack].clips;
        if (s_selIndex >= 0 && s_selIndex < (int)cs.size()) bar = cs[s_selIndex].bar;
    }
    struct Spot { int32_t x, y; const char* sym; bool can; bool bars; };
    const Spot spots[4] = {
        {x - kA - 1,         y + (h - kA) / 2, LV_SYMBOL_LEFT,  bar > 0,                true},
        {x + w + 1,          y + (h - kA) / 2, LV_SYMBOL_RIGHT, true,                   true},
        {x + (w - kA) / 2,   y - kA / 2 - 2,   LV_SYMBOL_UP,    s_selTrack > 0,         false},
        {x + (w - kA) / 2,   y + h - kA / 2 + 2, LV_SYMBOL_DOWN, s_selTrack < kTracks - 1, false},
    };
    for (int d = 0; d < 4; ++d) {
        if (!spots[d].can) continue;
        const bool lit = spots[d].bars == (s_moveAxis == MoveAxis::Bars);
        lv_obj_t* a = lv_button_create(s_timeline);
        lv_obj_remove_style_all(a);
        lv_obj_set_size(a, kA, kA);
        lv_obj_set_pos(a, std::max<int32_t>(0, spots[d].x), std::max<int32_t>(0, spots[d].y));
        lv_obj_set_style_radius(a, kA / 2, 0);
        lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(a, lit ? kAccent : kPanel, 0);
        lv_obj_set_style_border_width(a, 1, 0);
        lv_obj_set_style_border_color(a, lit ? kInk : kDim, 0);
        lv_obj_add_event_cb(a, move_arrow_cb, LV_EVENT_CLICKED, (void*)(intptr_t)d);
        lv_obj_t* l = lv_label_create(a);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, lit ? kInk : kDim, 0);
        lv_label_set_text(l, spots[d].sym);
        lv_obj_center(l);
        s_moveArrows[d] = a;
    }
}

/// MOVE/DUP/DEL lit, the encoder on them: what a click on a block opens.
void paint_tools()
{
    for (int t = 0; t < kBlockTools; ++t) {
        if (!s_tools[t]) continue;
        /* MOVE's label takes its colour from the button, so it turns dark on
         * the accent while MOVE is turned; DUP and DEL carry their own. */
        lv_obj_set_style_text_color(t == 0 ? s_tools[t] : lv_obj_get_child(s_tools[t], 0),
                                    s_toolMode ? kText : kDim, 0);
        lv_obj_set_style_border_width(s_tools[t], s_toolMode ? 1 : 0, 0);
        lv_obj_set_style_border_color(s_tools[t], kAccent, 0);
    }
}

void enter_tools()
{
    auto* se = engine();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& cs = se->song().tracks[s_selTrack].clips;
        if (s_selIndex < 0 || s_selIndex >= (int)cs.size()) return;
    }
    s_toolMode = true;
    paint_tools();
#ifdef ARR_USE_ENCODER
    if (s_toolGroup && indev_encoderTop) {
        lv_group_set_editing(s_toolGroup, false);
        lv_indev_set_group(indev_encoderTop, s_toolGroup);
        lv_group_focus_obj(s_tools[0]);
    }
#endif
}

/// Back up to the blocks, on the one selected.
void set_move_axis(MoveAxis a);

void leave_tools()
{
    if (!s_toolMode) return;
    s_toolMode = false;
    set_move_axis(MoveAxis::None);
    paint_tools();
#ifdef ARR_USE_ENCODER
    if (s_toolGroup) lv_group_set_editing(s_toolGroup, false);
    if (s_encGroup && indev_encoderTop) {
        lv_indev_set_group(indev_encoderTop, s_encGroup);
        for (int i = 0; i < s_blockCount; ++i) {
            if (s_blocks[i].track == s_selTrack && s_blocks[i].index == s_selIndex) {
                lv_group_focus_obj(s_blocks[i].obj);
                break;
            }
        }
    }
#endif
}

void scroll_to_selected()
{
    for (int i = 0; i < s_blockCount; ++i) {
        if (s_blocks[i].track == s_selTrack && s_blocks[i].index == s_selIndex) {
            lv_obj_scroll_to_view(s_blocks[i].obj, LV_ANIM_OFF);
            break;
        }
    }
}

/// The selected block to the track above (-1) or below (+1), on its bar.
void move_track(int dir)
{
    auto* se = engine();
    const int to = s_selTrack + dir;
    if (!se || to < 0 || to >= kTracks) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& from = se->song().tracks[s_selTrack].clips;
        if (s_selIndex < 0 || s_selIndex >= (int)from.size()) return;
        const crosspad::SongClip c = from[s_selIndex];
        from.erase(from.begin() + s_selIndex);
        auto& dst = se->song().tracks[to].clips;
        auto pos = std::upper_bound(dst.begin(), dst.end(), c,
            [](const crosspad::SongClip& a, const crosspad::SongClip& b) { return a.bar < b.bar; });
        s_selIndex = (int)(pos - dst.begin());
        dst.insert(pos, c);
        s_selTrack = to;
    }
    se->commit();
    rebuild_timeline();
    select(s_selTrack, s_selIndex);
}

void move_bars(int step)
{
    edit_selected([step](crosspad::AudioSong&, crosspad::SongClip& c) {
        if (step < 0 && c.bar) --c.bar;
        else if (step > 0)     ++c.bar;
        return c;
    });
}

void set_move_axis(MoveAxis a)
{
    s_moveAxis = a;
    if (s_tools[0]) {
        lv_label_set_text(lv_obj_get_child(s_tools[0], 0),
                          a == MoveAxis::Bars   ? LV_SYMBOL_LEFT "  " LV_SYMBOL_RIGHT :
                          a == MoveAxis::Tracks ? LV_SYMBOL_UP "  " LV_SYMBOL_DOWN : "MOVE");
    }
    place_move_arrows();
}

/// MOVE is turned: the first click along the bars (a bar per detent), the
/// next across the tracks (a track per detent), the third ends it and goes
/// back to the blocks. A tap starts it for the encoder too.
void move_cb(lv_event_t* e)
{
    lv_obj_t* b = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_group_t* g = lv_obj_get_group(b);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        /* The encoder's first click puts MOVE in edit mode; LVGL tells it with
         * FOCUSED, not CLICKED. */
        const bool editing = g && lv_group_get_editing(g);
        if (editing && s_moveAxis == MoveAxis::None) set_move_axis(MoveAxis::Bars);
        else if (!editing && s_moveAxis != MoveAxis::None) set_move_axis(MoveAxis::None);
        return;
    }
    if (code == LV_EVENT_CLICKED) {
        if (g && lv_group_get_editing(g)) {
            if (s_moveAxis == MoveAxis::Bars) set_move_axis(MoveAxis::Tracks);
            else                              leave_tools();
            return;
        }
        enter_tools();
#ifdef ARR_USE_ENCODER
        if (s_toolGroup && s_toolMode) lv_group_set_editing(s_toolGroup, true);
#endif
        if (s_moveAxis == MoveAxis::None) set_move_axis(MoveAxis::Bars);
        return;
    }
    const uint32_t key = lv_event_get_key(e);
    const int step = key == LV_KEY_RIGHT || key == LV_KEY_UP ? 1 : (key == LV_KEY_LEFT || key == LV_KEY_DOWN ? -1 : 0);
    if (!step) return;
    /* Turning right goes down the lanes, the way a list scrolls. */
    if (s_moveAxis == MoveAxis::Tracks) move_track(step);
    else                                move_bars(step);
    scroll_to_selected();
}

/// A tapped arrow: moved on the next turn of the LVGL loop, since the move
/// rebuilds the timeline and with it the arrow being clicked.
void move_arrow_cb(lv_event_t* e)
{
    const int d = (int)(intptr_t)lv_event_get_user_data(e);
    lv_async_call([](void* p) {
        const int dir = (int)(intptr_t)p;
        s_moveAxis = dir < 2 ? MoveAxis::Bars : MoveAxis::Tracks;
        if (dir == 0) move_bars(-1);
        else if (dir == 1) move_bars(+1);
        else move_track(dir == 2 ? -1 : +1);
        set_move_axis(s_moveAxis);
        scroll_to_selected();
    }, (void*)(intptr_t)d);
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
    leave_tools();
    scroll_to_selected();
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
    leave_tools();
}

void leave_region_edit();

void close_overlay();

/// The power button's click goes up one level: a window, then a region or
/// a block being edited, then out of the app.
bool back_handler(lv_obj_t*)
{
    if (s_overlay) {
        for (auto& t : s_mergeToggles) t = nullptr;
        close_overlay();
        return true;
    }
    if (s_regionEdit != RegionEdit::None) { leave_region_edit(); return true; }
    if (!s_toolMode) return false;
    leave_tools();
    return true;
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
        /* Looping a region: from its start, the place being worked on. */
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        const auto& cs = song.tracks[s_selTrack].clips;
        if (song.loop && song.hasRegion())                  from = song.loopStart;
        else if (s_selIndex >= 0 && s_selIndex < (int)cs.size()) from = cs[s_selIndex].bar;
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
    lv_group_delete_deferred(s_overlayGroup);     // the click that closed it is still being read
    s_overlayGroup = nullptr;
#endif
    lv_obj_delete_async(s_overlay);
    s_overlay = nullptr;
    s_songList = nullptr;
    s_tempoItem = nullptr;
    for (auto& d : s_mixDb) d = nullptr;
    for (auto& m : s_mixMute) m = nullptr;
}

/// A window over the timeline: full size, a column, the encoder its own.
lv_obj_t* overlay_open()
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
    return s_overlay;
}

lv_obj_t* overlay_list(const char* title)
{
    overlay_open();
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

// ── the song windows: a header with the way back, a list under it ────────

constexpr int32_t kHeadH = 28;
constexpr int32_t kRowH = 30;

/// A header button: "< SONGS" on the left, an action like "+ NEW" on the right.
lv_obj_t* head_button(lv_obj_t* head, const char* text, bool accent, lv_event_cb_t cb)
{
    lv_obj_t* b = lv_button_create(head);
    lv_obj_set_size(b, LV_SIZE_CONTENT, kHeadH - 6);
    lv_obj_set_style_pad_hor(b, 8, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, accent ? kAccent : kBg, 0);
    lv_obj_set_style_text_color(b, accent ? kInk : kText, 0);
    lv_obj_set_style_outline_color(b, kHead, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    /* One line of fixed height: "..." only cuts a label that cannot grow. */
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(l, 200, 0);
    lv_obj_set_height(l, lv_font_get_line_height(&lv_font_montserrat_14));
    lv_label_set_text(l, text);
    lv_obj_center(l);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, b);
#endif
    return b;
}

/// A song window: a header whose left end is the way back (@p back names
/// where it goes) and whose right end is one action, and a list under it
/// that scrolls. Rows go into s_songList.
void song_window(const char* back, lv_event_cb_t backCb, const char* action = nullptr,
                 lv_event_cb_t actionCb = nullptr)
{
    overlay_open();
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_style_pad_row(s_overlay, 0, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* head = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), kHeadH);
    lv_obj_set_style_pad_hor(head, 4, 0);
    lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(head, kPanel, 0);
    lv_obj_set_style_border_width(head, 1, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(head, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    std::string label = std::string(LV_SYMBOL_LEFT "  ") + back;
    for (char& c : label) c = (char)std::toupper((unsigned char)c);
    head_button(head, label.c_str(), false, backCb);
    if (action) head_button(head, action, true, actionCb);

    s_songList = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_songList);
    lv_obj_set_width(s_songList, LV_PCT(100));
    lv_obj_set_flex_grow(s_songList, 1);
    lv_obj_set_style_pad_all(s_songList, 6, 0);
    lv_obj_set_style_pad_row(s_songList, 4, 0);
    lv_obj_set_flex_flow(s_songList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_songList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_songList, LV_SCROLLBAR_MODE_ACTIVE);
}

/// A row's look, on @p r: dark panel, the text colour on the row so both
/// labels take it -- dark on the accent while the row is being turned.
void style_row(lv_obj_t* r)
{
    lv_obj_set_size(r, LV_PCT(100), kRowH);
    lv_obj_set_style_radius(r, 6, 0);
    lv_obj_set_style_shadow_width(r, 0, 0);
    lv_obj_set_style_pad_hor(r, 10, 0);
    lv_obj_set_style_pad_ver(r, 0, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, kPanel, 0);
    lv_obj_set_style_bg_color(r, kTrack, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(r, kAccent, LV_STATE_EDITED);
    lv_obj_set_style_text_color(r, kText, 0);
    lv_obj_set_style_text_color(r, kInk, LV_STATE_EDITED);
    lv_obj_set_style_outline_color(r, kHead, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(r, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
}

/// A row of the song window's list: @p name on the left, @p value dimmer on
/// the right. Rows are 30 px and one line each: a long name ends in "...".
/// @p row is one already made in the list (an editable button), else a new one.
lv_obj_t* list_row(const char* name, const char* value, lv_event_cb_t cb, void* ud = nullptr,
                   lv_obj_t* row = nullptr)
{
    lv_obj_t* r = row ? row : lv_obj_create(s_songList);
    lv_obj_remove_style_all(r);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    style_row(r);
    if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    /* One line of fixed height each: "..." only cuts a label that cannot grow. */
    const int32_t line = lv_font_get_line_height(&lv_font_montserrat_14);
    lv_obj_t* n = lv_label_create(r);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(n, 1);
    lv_obj_set_height(n, line);
    lv_label_set_text(n, name);
    lv_obj_t* v = lv_label_create(r);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_opa(v, LV_OPA_60, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(v, LV_PCT(60), 0);
    lv_obj_set_height(v, line);
    lv_label_set_text(v, value ? value : "");
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, r);
#endif
    return r;
}

/// Small dim text in the song window's list: a hint, a report line.
lv_obj_t* list_note(const char* text, const lv_font_t* font = &lv_font_montserrat_10, lv_color_t c = kDim)
{
    lv_obj_t* l = lv_label_create(s_songList);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_text(l, text);
    return l;
}

std::vector<std::string> s_projects;
std::vector<crosspad::ClipInfo> s_library;

void cancel_cb(lv_event_t*) { close_overlay(); }

// ── the open song's settings: tempo and name ───────────────────────────────

int32_t   s_editBpm = 120;          // the tempo being dialled, applied on the click that ends it

void show_tempo()
{
    if (!s_tempoItem) return;
    set_text_fmt(lv_obj_get_child(s_tempoItem, 1), "%03d BPM", (int)s_editBpm);
}

void song_edit_cb(lv_event_t*);
void project_cb(lv_event_t*);

/// Turned to dial, clicked to set: a tempo change resizes every block (see
/// ISongEngine::requestTempo), so it is made once, not per detent.
void tempo_cb(lv_event_t* e)
{
    lv_obj_t* b = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_group_t* g = lv_obj_get_group(b);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (g && lv_group_get_editing(g)) {
            lv_group_set_editing(g, false);
            auto* se = engine();
            if (se && se->requestTempo((float)s_editBpm)) {
                s_editing = true;
                s_editKind = EditKind::Tempo;
                lv_label_set_text(lv_obj_get_child(b, 1), "setting ...");
            }
        } else if (g) {
            lv_group_focus_obj(b);
            lv_group_set_editing(g, true);
        }
        return;
    }
    const uint32_t key = lv_event_get_key(e);
    const int step = key == LV_KEY_RIGHT || key == LV_KEY_UP ? 1 : (key == LV_KEY_LEFT || key == LV_KEY_DOWN ? -1 : 0);
    if (!step) return;
    s_editBpm = std::max<int32_t>(40, std::min<int32_t>(300, s_editBpm + step));
    show_tempo();
}

/// A row that says what the window is for: the accent, or red for going.
void emphasise_row(lv_obj_t* r, lv_color_t bg)
{
    lv_obj_set_style_bg_color(r, bg, 0);
    lv_obj_set_style_text_color(r, kInk, 0);
}

std::string open_song_name()
{
    auto* se = engine();
    if (!se) return {};
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    return se->song().name;
}

lv_obj_t* s_nameArea = nullptr;

void rename_failed()
{
    if (s_overlay) return;
    song_window("Rename", song_edit_cb);
    list_note("That name is taken, or not one a folder can have.", &lv_font_montserrat_14, kText);
    emphasise_row(list_row(LV_SYMBOL_OK "  OK", nullptr, song_edit_cb), kAccent);
}

void rename_done_cb(lv_event_t* e)
{
    const bool ok = lv_event_get_code(e) == LV_EVENT_READY;
    std::string name = s_nameArea ? lv_textarea_get_text(s_nameArea) : "";
    close_overlay();
    s_nameArea = nullptr;
    auto* se = engine();
    if (!ok || !se || name.empty()) return;
    if (!se->requestRename(name)) { rename_failed(); return; }
    s_editing = true;
    s_editKind = EditKind::Rename;
    if (s_infoLabel) set_text_fmt(s_infoLabel, "Renaming to %s ...", name.c_str());
}

void rename_cb(lv_event_t*)
{
    const std::string name = open_song_name();
    close_overlay();
    song_window("Rename", song_edit_cb);
    lv_obj_remove_flag(s_songList, LV_OBJ_FLAG_SCROLLABLE);
    s_nameArea = lv_textarea_create(s_songList);
    lv_textarea_set_one_line(s_nameArea, true);
    lv_textarea_set_max_length(s_nameArea, 40);
    lv_textarea_set_text(s_nameArea, name.c_str());
    lv_obj_set_width(s_nameArea, LV_PCT(100));
    lv_obj_set_style_bg_color(s_nameArea, kPanel, 0);
    lv_obj_set_style_border_width(s_nameArea, 0, 0);
    lv_obj_set_style_radius(s_nameArea, 6, 0);
    lv_obj_set_style_text_color(s_nameArea, kText, 0);
    lv_obj_set_style_text_font(s_nameArea, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(s_nameArea, kAccent, LV_PART_CURSOR);
    lv_obj_t* kb = lv_keyboard_create(s_songList);
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_flex_grow(kb, 1);
    lv_keyboard_set_textarea(kb, s_nameArea);
    lv_obj_add_event_cb(kb, rename_done_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, rename_done_cb, LV_EVENT_CANCEL, nullptr);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) {
        lv_group_add_obj(s_overlayGroup, kb);
        lv_group_focus_obj(kb);
        lv_group_set_editing(s_overlayGroup, true);
    }
#endif
}

void delete_confirmed_cb(lv_event_t*)
{
    close_overlay();
    auto* se = engine();
    if (!se || !se->requestDelete()) return;
    s_selTrack = 0;
    s_selIndex = -1;
    s_editing = true;
    s_editKind = EditKind::Delete;
    if (s_infoLabel) lv_label_set_text(s_infoLabel, "Deleting ...");
}

/// Asked once more, "Keep it" first: the song and its clips leave the card.
void delete_cb(lv_event_t*)
{
    const std::string title = "Delete \"" + open_song_name() + "\"?";
    close_overlay();
    song_window("Delete song", song_edit_cb);
    list_note(title.c_str(), &lv_font_montserrat_14, kText);
    list_note("The song and every clip in it go from the card; this cannot be undone. "
              "The first song left opens instead, or a new one.");
    lv_obj_t* keep = list_row("Keep it", nullptr, song_edit_cb);
    emphasise_row(list_row(LV_SYMBOL_TRASH "  Delete", nullptr, delete_confirmed_cb), kMute);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_focus_obj(keep);
#endif
}

/// What the export wrote, where, and what it left out -- or why it did not.
void export_done(bool ok)
{
    auto* se = engine();
    if (!se) return;
    const std::vector<std::string> report = se->editReport();
    if (s_overlay) {
        if (s_infoLabel && !report.empty()) set_text(s_infoLabel, report[0].c_str());
        return;
    }
    song_window("Ableton Live", song_edit_cb);
    list_note(ok ? LV_SYMBOL_OK "  Live Set written" : LV_SYMBOL_WARNING "  No Live Set written",
              &lv_font_montserrat_14, ok ? kAccent : kMute);
    for (size_t i = 0; i < report.size() && i < 6; ++i) {
        list_note(report[i].c_str(), i ? &lv_font_montserrat_10 : &lv_font_montserrat_14, i ? kDim : kText);
    }
    if (report.size() > 6) {
        list_note(("... and " + std::to_string(report.size() - 6) + " more").c_str());
    }
    if (ok) list_note("Open the .als on the card in Live 12, or copy the song's folder off it first.");
    lv_obj_t* done = list_row(LV_SYMBOL_OK "  OK", nullptr, [](lv_event_t*) { close_overlay(); });
    emphasise_row(done, kAccent);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_focus_obj(done);
#endif
}

void export_cb(lv_event_t*)
{
    close_overlay();
    auto* se = engine();
    if (!se || !se->requestExportLiveSet()) return;
    s_editing = true;
    s_editKind = EditKind::Export;
    if (s_infoLabel) lv_label_set_text(s_infoLabel, "Writing the Ableton Live Set ...");
}

/// The open song's settings, a row each: tempo (click, turn, click), name,
/// the export and the delete. Back is the songs list.
void song_edit_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se) return;
    std::string name, summary;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        s_editBpm = song.bpm > 0.0f ? (int32_t)lroundf(song.bpm) : 120;
        name = song.name;
        size_t clips = 0;
        for (const auto& t : song.tracks) clips += t.clips.size();
        summary = std::to_string(song.playEnd()) + " bars, " + std::to_string(clips) +
                  (clips == 1 ? " clip" : " clips") + ", " + std::to_string((int)song.beatsPerBar) + "/4";
    }
    close_overlay();
    song_window(name.c_str(), [](lv_event_t*) { close_overlay(); project_cb(nullptr); });
    list_note(summary.c_str());
    /* Turned like MOVE: click, turn, click. */
    s_tempoItem = list_row("Tempo", "", nullptr, nullptr, lv_CreateEditableButton(s_songList));
    lv_obj_add_event_cb(s_tempoItem, tempo_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_tempoItem, tempo_cb, LV_EVENT_CLICKED, nullptr);
    show_tempo();
    list_row("Name", (name + "  " LV_SYMBOL_RIGHT).c_str(), rename_cb);
    list_row("Export to Ableton Live", ".als  " LV_SYMBOL_RIGHT, export_cb);
    lv_obj_t* del = list_row("Delete song", LV_SYMBOL_RIGHT, delete_cb);
    lv_obj_set_style_text_color(del, kMute, 0);
    list_note("Tempo: click, turn, click. Clips keep their speed -- each block is resized to what its "
              "file lasts at the new tempo. SYNC plays the sequencer at it.");
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_focus_obj(s_tempoItem);
#endif
}

/// Open project @p i of the list, or a new "Song N" with -1.
void pick_project(int i)
{
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
    if (s_opening && s_infoLabel) set_text_fmt(s_infoLabel, "Opening %s ...", name.c_str());
}

void project_pick_cb(lv_event_t* e) { pick_project((int)(intptr_t)lv_event_get_user_data(e)); }

/// "+ NEW" is in the header from the start, but a new name is only known
/// once the list is: until then it does nothing.
void new_song_cb(lv_event_t*)
{
    if (s_pending != Pending::Projects) pick_project(-1);
}

void request_list(Pending what, const char* title)
{
    auto* se = engine();
    if (!se || s_overlay) return;
    s_pendingGen = se->listGeneration();
    const bool asked = what == Pending::Projects ? se->requestProjects() : se->requestClips();
    const char* wait = asked ? "Reading the card ..." : "The card is busy, try again.";
    if (what == Pending::Projects) {
        song_window(title, cancel_cb, LV_SYMBOL_PLUS " NEW", new_song_cb);
        s_loading = list_note(wait, &lv_font_montserrat_14);
    } else {
        overlay_list(title);
        s_loading = lv_label_create(s_overlay);
        lv_obj_set_style_text_color(s_loading, kDim, 0);
        lv_label_set_text(s_loading, wait);
        if (!asked) overlay_item(LV_SYMBOL_CLOSE " Cancel", cancel_cb, nullptr);
    }
    s_pending = asked ? what : Pending::None;
}

/// The songs on the card, the open one marked and first to the encoder: a
/// click on it edits it, on any other opens that one.
void fill_projects()
{
    s_projects = engine()->projects();
    const std::string current = open_song_name();
    lv_obj_t* focus = nullptr;
    for (size_t i = 0; i < s_projects.size() && i < 24; ++i) {
        const bool open = s_projects[i] == current;
        lv_obj_t* r = list_row(s_projects[i].c_str(), open ? "EDIT  " LV_SYMBOL_RIGHT : nullptr,
                               open ? song_edit_cb : project_pick_cb, (void*)(intptr_t)i);
        if (!open) continue;
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(r, kAccent, 0);
        lv_obj_set_style_border_width(r, 3, 0);
        lv_obj_t* v = lv_obj_get_child(r, 1);
        lv_obj_set_style_text_color(v, kAccent, 0);
        lv_obj_set_style_text_opa(v, LV_OPA_COVER, 0);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_10, 0);
        focus = r;
    }
    if (s_projects.empty()) list_note("No songs on the card yet: + NEW starts one.", &lv_font_montserrat_14);
    if (!focus) return;
    /* Laid out first: the rows were made this tick and all still sit at the
     * top, so scrolling to one would not move the list. */
    lv_obj_update_layout(s_overlay);
    lv_obj_scroll_to_view(focus, LV_ANIM_OFF);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_focus_obj(focus);
#endif
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
        set_text_fmt(s_lenLabel, "%u bars", (unsigned)song.length);
    } else {
        set_text_fmt(s_lenLabel, "%u bars  (auto: to the last block)", (unsigned)song.songEnd());
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

// ── the loop region ────────────────────────────────────────────────────────

void region_changed()
{
    if (auto* se = engine()) se->commit();
    rebuild_timeline();
    update_info();
}

/// The handle's turns move L, then R, a bar per detent; its click goes from
/// L to R and from R back to the blocks.
void region_cb(lv_event_t* e)
{
    auto* se = engine();
    if (!se) return;
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        if (s_regionEdit == RegionEdit::Start) { s_regionEdit = RegionEdit::End; rebuild_timeline(); update_info(); }
        else leave_region_edit();
        return;
    }
    if (code != LV_EVENT_KEY) return;
    const uint32_t key = lv_event_get_key(e);
    const int d = key == LV_KEY_RIGHT || key == LV_KEY_UP ? 1 : (key == LV_KEY_LEFT || key == LV_KEY_DOWN ? -1 : 0);
    if (!d) return;
    uint32_t at = 0;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        if (s_regionEdit == RegionEdit::Start) {
            const int64_t v = std::max<int64_t>(0, std::min<int64_t>((int64_t)song.loopEnd - 1, (int64_t)song.loopStart + d));
            song.loopStart = (uint32_t)v;
            at = song.loopStart;
        } else {
            const int64_t v = std::max<int64_t>((int64_t)song.loopStart + 1, std::min<int64_t>(kMaxLength, (int64_t)song.loopEnd + d));
            song.loopEnd = (uint32_t)v;
            at = song.loopEnd;
        }
    }
    region_changed();
    /* The end being moved stays on screen. */
    const int32_t x = (int32_t)at * s_pxPerBar;
    const int32_t view = lv_obj_get_content_width(s_timeline);
    const int32_t left = lv_obj_get_scroll_x(s_timeline);
    if (x < left + 10 || x > left + view - 10) lv_obj_scroll_to_x(s_timeline, x > view / 2 ? x - view / 2 : 0, LV_ANIM_OFF);
}

/// Into turning the region, from where it is -- or, with none yet, four
/// bars from the selected block (or the top).
void enter_region_edit()
{
    auto* se = engine();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        if (!song.hasRegion()) {
            const auto& cs = song.tracks[s_selTrack].clips;
            song.loopStart = s_selIndex >= 0 && s_selIndex < (int)cs.size() ? cs[s_selIndex].bar : 0;
            song.loopEnd = song.loopStart + 4;
        }
    }
    s_regionEdit = RegionEdit::Start;
    region_changed();
#ifdef ARR_USE_ENCODER
    if (s_regionGroup && indev_encoderTop) {
        lv_indev_set_group(indev_encoderTop, s_regionGroup);
        lv_group_focus_obj(s_regionHandle);
        lv_group_set_editing(s_regionGroup, true);
    }
#endif
}

void leave_region_edit()
{
    if (s_regionEdit == RegionEdit::None) return;
    s_regionEdit = RegionEdit::None;
#ifdef ARR_USE_ENCODER
    if (s_regionGroup) lv_group_set_editing(s_regionGroup, false);
    if (s_encGroup && indev_encoderTop) lv_indev_set_group(indev_encoderTop, s_encGroup);
#endif
    rebuild_timeline();
    update_info();
}

/// Every block of every track that starts in [from, to), moved @p by bars
/// on -- time inserted, so nothing after the point is written over.
void shift_from(crosspad::AudioSong& song, uint32_t at, uint32_t by)
{
    for (auto& tr : song.tracks) for (auto& c : tr.clips) if (c.bar >= at) c.bar += by;
    if (song.length && song.length > at) song.length += by;
}

void copy_region(crosspad::AudioSong& song, std::vector<Copied>& out)
{
    out.clear();
    for (int t = 0; t < kTracks; ++t) {
        for (const auto& c : song.tracks[t].clips) {
            if (c.bar >= song.loopStart && c.bar < song.loopEnd) out.push_back(Copied{t, c.bar - song.loopStart, c});
        }
    }
}

void paste_at(crosspad::AudioSong& song, const std::vector<Copied>& in, uint32_t at)
{
    for (const auto& k : in) {
        crosspad::SongClip c = k.clip;
        c.bar = at + k.rel;
        song.tracks[k.track].clips.push_back(c);
    }
    for (int t = 0; t < kTracks; ++t) song.sortTrack(t);
}

template <typename F>
void region_edit(F fn)
{
    auto* se = engine();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        fn(se->song());
    }
    s_selIndex = -1;
    close_overlay();
    region_changed();
}

void region_copy_cb(lv_event_t*)
{
    region_edit([](crosspad::AudioSong& song) {
        copy_region(song, s_clipboard);
        s_clipboardBars = song.loopEnd - song.loopStart;
    });
}

/// The copied bars in at the region's start; what was there moves on.
void region_paste_cb(lv_event_t*)
{
    region_edit([](crosspad::AudioSong& song) {
        if (s_clipboard.empty() || !s_clipboardBars) return;
        shift_from(song, song.loopStart, s_clipboardBars);
        paste_at(song, s_clipboard, song.loopStart);
        song.loopEnd = song.loopStart + s_clipboardBars;
    });
}

/// The region's bars again right after it, and the region twice as long:
/// 4 bars, 8, 16 -- as the Sequencer's x2.
void region_dup_cb(lv_event_t*)
{
    region_edit([](crosspad::AudioSong& song) {
        const uint32_t w = song.loopEnd - song.loopStart;
        std::vector<Copied> part;
        copy_region(song, part);
        shift_from(song, song.loopEnd, w);
        paste_at(song, part, song.loopEnd);
        song.loopEnd += w;
    });
}

void region_clear_cb(lv_event_t*)
{
    region_edit([](crosspad::AudioSong& song) { song.loopStart = song.loopEnd = 0; });
}

void region_block_cb(lv_event_t*)
{
    region_edit([](crosspad::AudioSong& song) {
        const auto& cs = song.tracks[s_selTrack].clips;
        if (s_selIndex < 0 || s_selIndex >= (int)cs.size()) return;
        song.loopStart = cs[s_selIndex].bar;
        song.loopEnd = cs[s_selIndex].bar + cs[s_selIndex].bars;
    });
}

void region_set_cb(lv_event_t*)
{
    close_overlay();
    enter_region_edit();
}

// ── merging tracks ─────────────────────────────────────────────────────────

/// The tracks in @p mask, over the region or the whole song, into one clip.
void run_merge(uint8_t mask)
{
    auto* se = engine();
    close_overlay();
    if (!se) return;
    uint32_t from = 0, to = 0;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        if (song.hasRegion()) { from = song.loopStart; to = song.loopEnd; }
        else                  { from = 0; to = song.songEnd(); }
    }
    if (se->playing()) se->stop();
    if (to > from && se->requestMerge(mask, from, to)) {
        s_merging = true;
        s_selIndex = -1;
        set_text(s_infoLabel, "Merging ...");
    } else {
        set_text(s_infoLabel, "Nothing to merge there.");
    }
}

void paint_merge_toggles()
{
    for (int t = 0; t < kTracks; ++t) {
        lv_obj_t* b = s_mergeToggles[t];
        if (!b) continue;
        const bool on = s_mergeMask & (1u << t);
        lv_obj_set_style_bg_color(b, on ? kAccent : kPanel, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(b, 0), on ? kInk : kText, 0);
    }
}

void merge_toggle_cb(lv_event_t* e)
{
    s_mergeMask ^= (uint8_t)(1u << (int)(intptr_t)lv_event_get_user_data(e));
    paint_merge_toggles();
}

void merge_tracks_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se) return;
    std::string where;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        char buf[64];
        if (song.hasRegion()) std::snprintf(buf, sizeof(buf), "Merge tracks, bars %u - %u", (unsigned)(song.loopStart + 1), (unsigned)song.loopEnd);
        else                  std::snprintf(buf, sizeof(buf), "Merge tracks, the whole song");
        where = buf;
    }
    close_overlay();
    overlay_list(where.c_str());
    lv_obj_t* row = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 34);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    for (int t = 0; t < kTracks; ++t) {
        lv_obj_t* b = lv_button_create(row);
        lv_obj_set_height(b, 32);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_outline_color(b, kHead, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
        lv_obj_add_event_cb(b, merge_toggle_cb, LV_EVENT_CLICKED, (void*)(intptr_t)t);
        lv_obj_t* l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
        set_text_fmt(l, "%d", t + 1);
        lv_obj_center(l);
#ifdef ARR_USE_ENCODER
        if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, b);
#endif
        s_mergeToggles[t] = b;
    }
    paint_merge_toggles();
    lv_obj_t* hint = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(hint, kDim, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_text(hint, "The lit tracks become one clip on the lowest of them, at their volumes; "
                            "muted tracks are left as they are.");
    overlay_item(LV_SYMBOL_SHUFFLE " Merge", [](lv_event_t*) {
        for (auto& b : s_mergeToggles) b = nullptr;
        run_merge(s_mergeMask);
    }, nullptr, true);
    overlay_item(LV_SYMBOL_CLOSE " Cancel", [](lv_event_t*) {
        for (auto& b : s_mergeToggles) b = nullptr;
        close_overlay();
    }, nullptr);
}

void merge_all_cb(lv_event_t*) { run_merge(0x0F); }

// ── the EDIT menu ──────────────────────────────────────────────────────────

void edit_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se || s_overlay || s_merging) return;
    bool region = false, block = false;
    uint32_t a = 0, b = 0;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        region = song.hasRegion();
        a = song.loopStart;
        b = song.loopEnd;
        const auto& cs = song.tracks[s_selTrack].clips;
        block = s_selIndex >= 0 && s_selIndex < (int)cs.size();
    }
    char title[64];
    if (region) std::snprintf(title, sizeof(title), "Edit   (loop region: bars %u - %u)", (unsigned)(a + 1), (unsigned)b);
    else        std::snprintf(title, sizeof(title), "Edit   (no loop region)");
    overlay_list(title);
    overlay_item(region ? "Move the loop region  (L, click, R)" : "Set a loop region  (L, click, R)", region_set_cb, nullptr);
    if (block) overlay_item("Loop region = the selected block", region_block_cb, nullptr);
    if (region) {
        overlay_item(LV_SYMBOL_COPY " Copy the region", region_copy_cb, nullptr);
        char buf[64];
        if (!s_clipboard.empty()) {
            std::snprintf(buf, sizeof(buf), LV_SYMBOL_PASTE " Paste %u bars at the region", (unsigned)s_clipboardBars);
            overlay_item(buf, region_paste_cb, nullptr);
        }
        overlay_item(LV_SYMBOL_PLUS " Duplicate the region  x2", region_dup_cb, nullptr);
    }
    overlay_item(LV_SYMBOL_SHUFFLE " Merge tracks ...", merge_tracks_cb, nullptr);
    overlay_item(LV_SYMBOL_SHUFFLE " Merge all tracks into one", merge_all_cb, nullptr);
    if (region) overlay_item(LV_SYMBOL_TRASH " Clear the loop region", region_clear_cb, nullptr);
    overlay_item("Song length ...", [](lv_event_t* e) { close_overlay(); length_cb(e); }, nullptr);
    overlay_item(LV_SYMBOL_CLOSE " Cancel", cancel_cb, nullptr);
}

// ── building ───────────────────────────────────────────────────────────────

/// Focus as a frame inside the button: the theme's ring stands 4 px out of it,
/// and a row the button fills cuts that off at the top and bottom.
void focus_frame(lv_obj_t* b)
{
    lv_obj_set_style_outline_width(b, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(b, kHead, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(b, 2, LV_STATE_FOCUS_KEY);
}

lv_obj_t* make_button(lv_obj_t* parent, int32_t w, const char* text, lv_event_cb_t cb, void* ud = nullptr)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, w, 22);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, kPanel, 0);
    focus_frame(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(l, kText, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

/// A tool at the foot of the screen: the row's height, the larger font.
lv_obj_t* tool_button(lv_obj_t* parent, int32_t w, const char* text, lv_event_cb_t cb)
{
    lv_obj_t* b = make_button(parent, w, text, cb);
    lv_obj_set_height(b, LV_PCT(100));
    lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_14, 0);
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

// ── MIX: the four tracks' levels ───────────────────────────────────────────

/* Fader travel as the Mixer app's: 0 is -inf, then 0.6 dB a step up to +6 dB
 * at the top, which puts unity at 90. */
constexpr int32_t kFaderMax   = 100;
constexpr int32_t kFaderUnity = 90;
constexpr float   kDbPerStep  = 0.6f;

float fader_to_gain(int32_t v)
{
    if (v <= 0) return 0.0f;
    return powf(10.0f, ((float)(v - kFaderUnity) * kDbPerStep) / 20.0f);
}

int32_t gain_to_fader(float g)
{
    if (g <= 0.0f) return 0;
    const int32_t v = (int32_t)lroundf(20.0f * log10f(g) / kDbPerStep) + kFaderUnity;
    return v < 1 ? 1 : (v > kFaderMax ? kFaderMax : v);
}

/// Each track's dB and M as the song has them.
void mix_show()
{
    auto* se = engine();
    if (!se) return;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    for (int t = 0; t < kTracks; ++t) {
        const auto& tr = se->song().tracks[t];
        if (s_mixDb[t]) {
            char buf[12];
            if (tr.volume <= 0.0f) std::snprintf(buf, sizeof(buf), "-inf");
            else                   std::snprintf(buf, sizeof(buf), "%+.1f dB", (double)(20.0f * log10f(tr.volume)));
            set_text(s_mixDb[t], buf);
        }
        if (s_mixMute[t]) {
            set_bg(s_mixMute[t], tr.muted ? kMute : kTrack);          // a button on the strip's panel
            set_ink(lv_obj_get_child(s_mixMute[t], 0), tr.muted ? kInk : kText);
        }
    }
}

/// A fader moved: heard at once; commit() saves the song once it rests.
void mix_fader_cb(lv_event_t* e)
{
    auto* se = engine();
    if (!se) return;
    const int t = (int)(intptr_t)lv_event_get_user_data(e);
    const float g = fader_to_gain(lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e))));
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        se->song().tracks[t].volume = g;
    }
    se->commit();
    mix_show();
}

void mix_mute_cb(lv_event_t* e)
{
    auto* se = engine();
    if (!se) return;
    const int t = (int)(intptr_t)lv_event_get_user_data(e);
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& tr = se->song().tracks[t];
        tr.muted = !tr.muted;
    }
    se->commit();
    mix_show();
}

/// A track's strip, as on a desk: its number, a tall fader, the dB, M.
void mix_strip(lv_obj_t* strips, int t, float volume)
{
    lv_obj_t* strip = lv_obj_create(strips);
    lv_obj_remove_style_all(strip);
    lv_obj_set_height(strip, LV_PCT(100));
    lv_obj_set_flex_grow(strip, 1);
    lv_obj_set_style_bg_color(strip, kPanel, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(strip, 6, 0);
    lv_obj_set_style_pad_ver(strip, 4, 0);
    lv_obj_set_style_pad_row(strip, 4, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* num = lv_label_create(strip);
    lv_obj_set_style_text_font(num, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(num, kText, 0);
    set_text_fmt(num, "%d", t + 1);

    /* Taller than wide: LVGL makes it a vertical slider, the bottom its 0. */
    lv_obj_t* s = lv_slider_create(strip);
    lv_obj_set_width(s, 8);
    lv_obj_set_flex_grow(s, 1);
    lv_slider_set_range(s, 0, kFaderMax);
    lv_slider_set_value(s, gain_to_fader(volume), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, kTrack, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, kHead, LV_PART_KNOB);
    lv_obj_set_style_radius(s, 4, LV_PART_KNOB);
    lv_obj_set_style_pad_hor(s, 10, LV_PART_KNOB);     // a fader cap, wider than its slot
    lv_obj_set_style_pad_ver(s, 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s, kAccent, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(s, kAccent, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_EDITED);
    lv_obj_set_style_outline_width(s, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_ext_click_area(s, 16);
    lv_obj_add_event_cb(s, mix_fader_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)t);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, s);
#endif

    s_mixDb[t] = lv_label_create(strip);
    lv_obj_set_style_text_font(s_mixDb[t], &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_mixDb[t], kText, 0);
    lv_label_set_text(s_mixDb[t], "");

    s_mixMute[t] = make_button(strip, 40, "M", mix_mute_cb, (void*)(intptr_t)t);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, s_mixMute[t]);
#endif
}

void mix_done_cb(lv_event_t*)
{
    close_overlay();
    rebuild_timeline();                          // a muted track dims on the timeline
}

/// MIX: a strip per track side by side, Done beside the title.
void mix_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se || s_overlay) return;
    float vol[kTracks];
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        for (int t = 0; t < kTracks; ++t) vol[t] = se->song().tracks[t].volume;
    }
    overlay_list("Mix");
    lv_obj_set_style_pad_row(s_overlay, 4, 0);
    lv_obj_t* head = make_row(s_overlay, 24);
    lv_obj_t* title = lv_obj_get_child(s_overlay, 0);
    lv_obj_set_parent(title, head);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_t* done = make_button(head, 64, LV_SYMBOL_OK " Done", mix_done_cb);
    lv_obj_set_style_bg_color(done, kAccent, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(done, 0), kInk, 0);

    lv_obj_t* strips = make_row(s_overlay, 10);
    lv_obj_set_flex_grow(strips, 1);
    lv_obj_set_style_pad_column(strips, 6, 0);
    for (int t = 0; t < kTracks; ++t) mix_strip(strips, t, vol[t]);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, done);   // after the strips: the first fader has the focus
#endif
    mix_show();
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
        lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(l, kText, 0);
        set_text_fmt(l, "%d", t + 1);
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

    /* The status line is the status bar's centre, in place of the logo: the
     * row it took here went to the tools. */
    s_infoLabel = crosspad_gui::statusbar_show_title();

    /* -- tools on the selected block --: as tall as the screen leaves them,
     * a finger's worth, each as wide as its word in the larger font. */
    lv_obj_t* tools = make_row(s_root, 24);
    lv_obj_set_flex_grow(tools, 1);
    lv_obj_set_flex_align(tools, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tools, 0, 0);
    s_tools[0] = lv_CreateEditableButton(tools);
    lv_obj_set_size(s_tools[0], 48, LV_PCT(100));
    lv_obj_set_style_pad_all(s_tools[0], 0, 0);
    lv_obj_set_style_radius(s_tools[0], 6, 0);
    lv_obj_set_style_shadow_width(s_tools[0], 0, 0);
    lv_obj_set_style_bg_color(s_tools[0], kPanel, 0);
    lv_obj_set_style_bg_color(s_tools[0], kAccent, LV_STATE_EDITED);
    lv_obj_set_style_text_color(s_tools[0], kInk, LV_STATE_EDITED);   // the label takes it from here
    focus_frame(s_tools[0]);
    lv_obj_add_event_cb(s_tools[0], move_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_tools[0], move_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_tools[0], move_cb, LV_EVENT_FOCUSED, nullptr);
    {
        lv_obj_t* l = lv_label_create(s_tools[0]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_label_set_text(l, "MOVE");
        lv_obj_center(l);
    }
    s_tools[1] = tool_button(tools, 36, "DUP", dup_cb);
    s_tools[2] = tool_button(tools, 34, "DEL", del_cb);
    s_tools[3] = tool_button(tools, 38, "ADD", add_cb);
    s_tools[4] = tool_button(tools, 38, "EDIT", edit_cb);
    s_tools[5] = tool_button(tools, 34, "MIX", mix_cb);
    s_tools[6] = tool_button(tools, 30, "Z-", zoom_out_cb);
    s_tools[7] = tool_button(tools, 30, "Z+", zoom_in_cb);
#ifdef ARR_USE_ENCODER
    s_toolGroup = lv_group_create();
    for (int t = 0; t < kBlockTools; ++t) lv_group_add_obj(s_toolGroup, s_tools[t]);
    /* The region's handle: nothing to see (the band on the ruler is what
     * shows), only something editable for the encoder to turn. */
    s_regionHandle = lv_CreateEditableButton(s_root);
    lv_obj_add_flag(s_regionHandle, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_regionHandle, 1, 1);
    lv_obj_set_style_bg_opa(s_regionHandle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(s_regionHandle, 0, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(s_regionHandle, region_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_regionHandle, region_cb, LV_EVENT_CLICKED, nullptr);
    s_regionGroup = lv_group_create();
    lv_group_add_obj(s_regionGroup, s_regionHandle);
#endif
    s_regionEdit = RegionEdit::None;
    s_toolMode = false;
    paint_tools();

    lv_obj_update_layout(s_root);
    s_shape = 0;
    rebuild_timeline();
    sync_transport();
    if (!engine() && s_infoLabel) lv_label_set_text(s_infoLabel, "No song engine on this platform.");

#ifdef ARR_USE_ENCODER
    if (s_encGroup) lv_group_focus_obj(s_playBtn);
#endif

    s_timer = lv_timer_create(tick_cb, kTickMs, nullptr);

    if (app) {
        app->setOnBack(back_handler);
        app->setOnShow([](lv_obj_t*) {
            s_infoLabel = crosspad_gui::statusbar_show_title();
            update_info();
            if (s_timer) lv_timer_resume(s_timer);
#ifdef ARR_USE_ENCODER
            if (indev_encoderTop && s_encGroup) lv_indev_set_group(indev_encoderTop, s_encGroup);
#endif
        });
        app->setOnHide([](lv_obj_t*) {
            /* The bar goes back to the logo for whatever comes next. */
            crosspad_gui::statusbar_hide_title();
            s_infoLabel = nullptr;
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
    if (s_toolGroup) { lv_group_delete(s_toolGroup); s_toolGroup = nullptr; }
    if (s_regionGroup) { lv_group_delete(s_regionGroup); s_regionGroup = nullptr; }
#endif
    s_toolMode = false;
    s_regionEdit = RegionEdit::None;
    s_regionHandle = nullptr;
    s_merging = false;
    for (auto& b : s_mergeToggles) b = nullptr;
    s_moveAxis = MoveAxis::None;
    for (auto& a : s_moveArrows) a = nullptr;
    s_overlay = nullptr;
    s_loading = nullptr;
    s_pending = Pending::None;
    s_opening = false;
    s_lenLabel = nullptr;
    s_tempoItem = nullptr;
    for (auto& d : s_mixDb) d = nullptr;
    for (auto& m : s_mixMute) m = nullptr;
    s_nameArea = nullptr;
    for (auto& b : s_blocks) b = Block{};
    for (auto& h : s_heads) h = nullptr;
    for (auto& t : s_tools) t = nullptr;
    s_blockCount = 0;
    crosspad_gui::statusbar_hide_title();
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
