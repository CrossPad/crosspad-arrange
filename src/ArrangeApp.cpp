/**
 * @file ArrangeApp.cpp
 * @brief Arrange -- where a song starts, and the four tracks it is made of.
 *
 * The main place of work on the CrossPad (UX 2000 section 6). A song holds
 * four tracks: four audio, or one seq and three audio. A track is made here
 * and filled by the app that makes that kind of sound -- AUDIO opens the
 * Recorder, SEQ asks whether it plays through the Sampler's kit or Pitch Play
 * and opens the sequencer on a scene of its own -- and any track can be
 * bounced to audio at any time, which is also the only way to free the one
 * seq slot for the next idea.
 *
 * Audio tracks are laid out like a linear DAW: lanes, a bar ruler, every clip
 * a block as long as it plays, in a colour of its own, and a playhead while
 * the song runs -- and stopped, where PLAY starts, which the top bar's
 * position moves a bar a detent. REC and DUB on the top bar record the pads
 * onto the seq track from there (ISongEngine::recordSeq): a REC becomes a
 * block of its own notes as many bars long as it ran -- the playhead then
 * stands where it stopped, so the next REC is the next part -- and a DUB adds
 * to the blocks it plays over; with LOOP over a region the take goes round
 * it. Seq blocks are moved, copied (each copy its own from its first edit),
 * cut and deleted by bars like the audio, and drawn from their notes. A seq
 * track from before blocks had notes of their own plays its scene: one block
 * over the whole song until a cut, a region copy or a take gives it blocks
 * (AudioSong seqBlocks).
 *
 *   top    the project (tap: pick another or start a new one), where the song
 *          is, SYNC (the sequencer plays along, locked to the song's bars and
 *          tempo), LOOP, ZOOM. No back button and no PLAY: back is the power
 *          button's click, the transport is the bar above
 *   lanes  a header per track -- its number and what it holds, AUD / KIT /
 *          PITCH, in the colour of the app that makes it (tap: pick the
 *          track and open its menu -- BOUNCE a seq to audio, DELETE; hold:
 *          MIX) -- the timeline beside them, and under the last of them
 *          "+ ADD TRACK" while there is room
 *   keys   MOVE, DUP and DEL act on the selected block; EDIT is the song's
 *          own editing (below). CLIP EDIT opens the selected track where it
 *          is made -- a seq block in the seq view, the block itself
 *   MIX    the four tracks' levels: a strip each side by side, as on a desk --
 *          a tall fader in dB (-inf, then 0.6 dB a step as in the Mixer app,
 *          unity at 90 %, +6 dB at the top), the dB, M; heard at once, saved
 *          with the song
 *   EDIT   a clip from the project's library onto the selected track, a cut
 *          of the picked track's block at the playhead, the loop region (L
 *          and R, turned like MOVE; with LOOP on the song goes round it, and
 *          PLAY starts at it), copy / paste / duplicate x2 of the region
 *          across every track by bars, the seq's notes included -- inserted,
 *          so nothing after it is written over -- merging audio tracks, or
 *          all of them, into one clip over the region or the whole song, and
 *          the song's length
 *
 * The encoder walks the blocks; a click on one selects it and takes the
 * encoder down to MOVE/DUP/DEL. DUP and DEL act and bring it back to the
 * blocks; MOVE is turned -- each detent a bar -- and the next click brings it
 * back. The power button's click also goes back up. The songs list edits the
 * open song: its tempo (each song has its own; set there or on the top bar,
 * it is the instrument's tempo too) and its name, or deletes it.
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
  #include "crosspad-gui/components/transport_bar.h"
#include "crosspad-gui/components/ui_toast.h"
#include "crosspad-gui/app/App.hpp"
#endif

#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad/platform/PlatformServices.hpp"
/* A seq track names a sequencer scene: this screen reads its bars and notes
 * to draw the lane, and gives a new track a scene of its own. */
#include "crosspad/sequencer/SequencerController.hpp"
#include "crosspad/instrument/IInstrumentHost.hpp"
#include "crosspad/instrument/InstrumentPlayState.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/song/ArrangeFocus.hpp"
#include "crosspad/song/ISongEngine.hpp"
#include "crosspad/song/SeqTrackSound.hpp"
#include "crosspad/transport/SongTransport.hpp"
#include "crosspad-gui/components/app_orchestrator.h"
#include "crosspad-gui/components/mode_overlay.h"
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
/* A screen y as a y inside the root: the root fills the app container, which
 * begins under the transport bar. The whole screen is laid out in the
 * export's screen coordinates, and this is the only place that shift
 * happens. */
#define ARR_Y(v) ((v) - TRANSPORT_BAR_HEIGHT)
/* Zoom: pixels per bar -- a whole song on one screen, phrases, single beats
 * (UX 2000 section 7.3: three levels). */
constexpr int32_t  kZoomPx[] = {3, 12, 48};
constexpr int      kZoomLevels = sizeof(kZoomPx) / sizeof(kZoomPx[0]);
constexpr uint32_t kMaxLength = 999;
/* The export's lane block is LANE_Y0=32, LANE_P=35, LANE_H=31 and a 138 px
 * playhead -- drawn for a screen with no ruler. This one keeps the ruler (it
 * is where the bar numbers and the loop region band live, and LOOP cannot be
 * edited without it), so the ruler's 10 px come off the pitch and the height:
 * 34 and 30 against the export's 35 and 31. Everything else is the export's
 * number. */
constexpr int32_t  kRulerH   = 10;
/* 26, not 30: the status line came back into this screen's own layout when
 * the transport bar took the status bar's row, and the four lanes paid the
 * 16 px for it rather than the tools, which have to stay a finger tall. */
constexpr int32_t  kLaneH    = 30;     // the export's 31, less the ruler's pixel
/* 2, not 4: at 4 the fourth lane ended on 208 and the tool row begins on
 * 206 -- measured on the board, two pixels of overlap. The ruler the export
 * does not have already pushes this block 14 px down; it must not also push
 * it into the keys. */
constexpr int32_t  kLaneGap  = 2;
/* 80: the export's track head, variant A -- the number in its type's colour
 * over the name, S over M at the right edge. The lanes start on its LANE_X 88
 * and end before the meters, on 302. */
constexpr int32_t  kHeadW    = 80;
constexpr int32_t  kMeterW   = 10;     ///< a track's stereo meter, beside the lanes
constexpr int      kMeterSegs = 10;
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
const lv_color_t kRec    = lv_color_hex(0xFF5C5C);     // the playhead while a take runs: REC's red
const lv_color_t kTrack  = lv_color_hex(0x3A3F47);     // a fader's travel
const lv_color_t kWell   = lv_color_hex(0x1B1E22);     // S / M off, a meter's well
const lv_color_t kWave   = lv_color_hex(0x9CD639);     // a meter's green

/* ── the tracks a player makes (UX 2000 section 6) ────────────────────────
 * Arrange holds four tracks: four audio, or one seq and three audio. A track
 * comes from the "+ ADD TRACK" row -- AUDIO opens the Recorder to record into
 * it, SEQ asks what it plays through and opens the sequencer on it -- and any
 * track can be bounced to audio at any time, which is also the only way to
 * free the one seq slot for the next idea. The three colours are the ones
 * those three screens already use, so a badge here names the app it opens. */
const lv_color_t kAudioT = lv_color_hex(0x4EA6D3);    // AUD   -- the Recorder's blue
const lv_color_t kKitT   = lv_color_hex(0xFFC857);    // KIT   -- the Sampler's amber
const lv_color_t kPitchT = lv_color_hex(0xA77BF0);    // PITCH -- Pitch Play's purple

lv_color_t track_color(const crosspad::SongTrack& t)
{
    if (t.kind != crosspad::TrackKind::Seq) return kAudioT;
    return t.source == crosspad::SeqSource::Pitch ? kPitchT : kKitT;
}

const char* track_badge(const crosspad::SongTrack& t)
{
    if (t.kind != crosspad::TrackKind::Seq) return "AUD";
    return t.source == crosspad::SeqSource::Pitch ? "PITCH" : "KIT";
}

/* A block drawn, and its clip's waveform once the engine has it. */
struct Block {
    lv_obj_t*   obj = nullptr;
    int         track = 0;
    int         index = 0;
    std::string file;
    uint32_t    offset = 0;        ///< where in the file it starts (a cut's second half)
    uint32_t    frames = 0;        ///< what it plays from there; 0 = to the end
    uint32_t    bars = 0;          ///< how long the block is
    bool        fitted = false;    ///< its track follows the tempo: the clip fills the bars
    bool        muted = false;
    std::shared_ptr<const std::vector<uint8_t>> peaks;
    uint8_t     peakMax = 0;       ///< the loudest value: the waveform is drawn to it
    bool        missing = false;   ///< its file is not in the project's clips folder
    /* A seq block: its own notes, or -- null -- the track's scene, from its
     * bar @c from of them. */
    bool        seq = false;
    crosspad::SeqPatternPtr notes;
    uint8_t     scene = 0;
    uint16_t    from = 0;
};

lv_obj_t* s_root = nullptr;
lv_obj_t* s_timeline = nullptr;
lv_obj_t* s_playhead = nullptr;
lv_obj_t* s_projLabel = nullptr;
lv_obj_t* s_posLabel = nullptr;
lv_obj_t* s_loopBtn = nullptr;
lv_obj_t* s_syncBtn = nullptr;
lv_obj_t* s_editBtn = nullptr;   ///< the timeline's EDIT, beside SYNC
lv_obj_t* s_heads[kTracks] = {};
lv_obj_t* s_headNum[kTracks] = {};
lv_obj_t* s_headName[kTracks] = {};
lv_obj_t* s_headMute[kTracks] = {};
lv_obj_t* s_headSolo[kTracks] = {};
/// A track's stereo meter beside the lanes: its well, ten segments a side.
lv_obj_t* s_meterWell[kTracks] = {};
lv_obj_t* s_meterSeg[kTracks][2][kMeterSegs] = {};
float     s_meterShown[kTracks][2] = {};
uint8_t   s_meterLit[kTracks][2] = {};
bool      s_trackQuiet[kTracks] = {};  ///< silenced: its meter stays down
lv_obj_t* s_trackNameArea = nullptr;   ///< RENAME's field, while its sheet is up
lv_obj_t* s_procBox = nullptr;         ///< "processing time stretch", over the lanes
lv_obj_t* s_procLabel = nullptr;
/* The row under the last track: the one way a track is made. It is drawn in
 * the slot the first track nobody has made would occupy, so the block of
 * lanes never grows past the four it is sized for. */
lv_obj_t* s_addRow = nullptr;
lv_obj_t* s_addInfo = nullptr;
/* A seq track's notes, drawn where its lane is. One object, no children:
 * see seq_draw_cb(). */
lv_obj_t* s_seqBlock = nullptr;
int       s_seqBlockTrack = -1;
uint8_t   s_seqLaneScene = 0;      ///< the scene that lane plays
/* Ticks in a bar of the open song (seq blocks' notes), and the bars the lanes
 * are drawn over -- a take running past them redraws them longer. */
uint32_t  s_seqTicksBar = crosspad::AudioSong::kSeqPpq * crosspad::AudioSong::kDefaultBeatsPerBar;
uint32_t  s_drawnBars = 0;
/* The track whose bounce is running, so its kind is flipped when the WAV
 * lands and not a moment before: until then the notes are still what plays. */
int       s_bouncing = -1;
/* A Pitch seq track's sound (sound_keeper_cb, by Arrange_create). */
constexpr const char* kSoundLogic = "ArrangePitch";
lv_timer_t* s_soundTimer   = nullptr;
bool        s_visible      = false;   ///< this screen is the one on top
bool        s_soundHeld    = false;   ///< the instrument was borrowed for the song
bool        s_soundAsked   = false;   ///< a load was asked for since the host was last Idle
bool        s_soundPads    = false;   ///< kSoundLogic is pushed
bool        s_soundBounce  = false;   ///< a bounce of the Pitch track is capturing it
/* MOVE, DUP, DEL (the selected block's, reached by clicking it), then EDIT. */
constexpr int kBlockTools = 3;
lv_obj_t* s_tools[4] = {};
lv_obj_t* s_lenLabel = nullptr;     // the value in the LEN window
lv_obj_t* s_zoomBtn = nullptr;   ///< the export puts zoom in the context row
/* What PLAY in the transport bar drives while this screen is up, and where it
 * starts -- the selection, which the bar cannot see -- is the core's one song
 * transport, crosspad::songTransport(): the seq view opened from the MODE map
 * reads the same playhead. */
lv_obj_t* s_projBtn = nullptr;
lv_obj_t* s_overlay = nullptr;
Block     s_blocks[kMaxBlocks];
int       s_blockCount = 0;
lv_timer_t* s_timer = nullptr;

int      s_zoom = 1;                // index into kZoomPx; kept across visits
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
/* The handle has been put in edit mode: from then on edit mode ending (the
 * power button's click, a long press) ends the region edit with it. */
bool       s_regionArmed = false;

/* Copy region: what sounds in it on every track, by bars -- the seq's blocks
 * as much as the audio (AudioSong::copyRange()). */
std::vector<crosspad::RangeClip> s_clipboard;
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
        mix(s.tracks[t].solo);
        mix((uint32_t)s.tracks[t].stretch);       // a fitted block's waveform fills its bars
        for (char c : s.tracks[t].name) mix((uint8_t)c);
        mix(s.tracks[t].inUse);
        mix((uint32_t)s.tracks[t].kind);
        mix((uint32_t)s.tracks[t].source);
        mix(s.tracks[t].scene);
        mix(s.tracks[t].seqBlocks);
        for (const auto& c : s.tracks[t].clips) {
            mix(c.bar); mix(c.bars); mix(c.offset); mix(c.frames); mix(c.from);
            mix((uint32_t)(uintptr_t)c.notes.get());   // a dub, the seq view: new notes, same bars
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
/// shows where its beats really fall -- unless its track follows the tempo
/// (stretch or repitch), when the clip fills the block's bars, as the copy
/// that plays does. Drawn to the clip's loudest value --
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
    /* Dark on the audio blue; on a muted track's faded block, the blue. */
    d.color = b.muted ? kAudioT : kInk;
    d.opa = b.muted ? LV_OPA_70 : LV_OPA_60;
    const std::vector<uint8_t>& v = *b.peaks;
    /* From where the block starts in its file, to a cut or the file's end. */
    const size_t skip = b.offset / s_peakHop;
    const size_t stop = b.frames ? std::min(v.size(), (size_t)((b.offset + b.frames) / s_peakHop)) : v.size();
    const double perPx = b.fitted && b.bars
        ? (double)(stop > skip ? stop - skip : 0) / ((double)b.bars * (double)s_pxPerBar)
        : (double)s_framesPerBar / (double)s_pxPerBar / (double)s_peakHop;
    /* A bar is the loudest value under its whole pitch, so no hit falls in a
     * gap; one with sound in it is never less than the silence line. Past
     * the file's end there is nothing. */
    for (int32_t bar = (x1 - a.x1) / kWaveBarPitch; bar <= (x2 - a.x1) / kWaveBarPitch; ++bar) {
        const int32_t px = bar * kWaveBarPitch;
        const size_t i0 = skip + (size_t)(px * perPx);
        if (i0 >= stop) break;
        const size_t i1 = std::min(stop, std::max(i0 + 1, skip + (size_t)((px + kWaveBarPitch) * perPx)));
        uint8_t m = 0;
        for (size_t i = i0; i < i1; ++i) m = std::max(m, v[i]);
        const int32_t h = m ? std::max<int32_t>(1, (int32_t)m * half / b.peakMax) : 0;
        const lv_area_t r = {a.x1 + px, mid - h, std::min(a.x2, a.x1 + px + kWaveBarW - 1), mid + h};
        lv_draw_fill(layer, &d, &r);
    }
}

/// A seq block's notes, over the bars it plays.
///
/// There is no file: the sequencer plays the block's notes along with the
/// song, round and round from the block's bar @c from of them -- its own
/// (a take in Arrange, the seq view's edits), or, a block from before blocks
/// had notes of their own, the track's scene, read from the sequencer so a
/// note recorded there shows here at once. Drawn in the draw event, like the
/// waveforms, so only the bars on screen cost anything. The user data is the
/// block's index, or -1 for a track playing its scene over the whole song.
void seq_draw_cb(lv_event_t* e)
{
    if (!s_pxPerBar) return;
    const int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k >= s_blockCount) return;
    const crosspad::SeqPatternPtr own = k >= 0 ? s_blocks[k].notes : nullptr;
    const uint8_t scene = k >= 0 ? s_blocks[k].scene : s_seqLaneScene;
    const uint32_t from = k >= 0 ? s_blocks[k].from : 0;
    auto* sq = crosspad::getPlatformServices().sequencer;
    if (!own && !sq) return;
    const uint32_t bars = own ? own->bars : sq->getSceneBars(scene);
    const uint32_t n = own ? (uint32_t)own->notes.size() : sq->getSceneEventCount(scene);
    if (!bars || !n) return;

    lv_obj_t* o = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    const lv_area_t& clipArea = layer->_clip_area;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    /* The notes sit in the lane under the name, one row per pad as in the
     * sequencer's own grid -- low pads at the bottom, as on the hardware. */
    const int32_t top = a.y1 + kWaveTop, bottom = a.y2 - 2;
    if (bottom <= top || bottom < clipArea.y1 || top > clipArea.y2) return;
    const int32_t rows = bottom - top;

    lv_draw_fill_dsc_t d;
    lv_draw_fill_dsc_init(&d);
    d.color = kInk;
    d.opa = LV_OPA_70;
    const int32_t loopPx = (int32_t)bars * s_pxPerBar;
    if (loopPx <= 0) return;
    /* The block's first pixel is this far into a pass of the notes. */
    const int32_t fromPx = (int32_t)(from % bars) * s_pxPerBar;
    /* Only the repeats that touch the area being redrawn. */
    const int32_t first = std::max<int32_t>(0, (clipArea.x1 - a.x1 + fromPx) / loopPx);
    const int32_t last  = std::min<int32_t>((a.x2 - a.x1 + fromPx) / loopPx, (clipArea.x2 - a.x1 + fromPx) / loopPx);
    const float span = (float)(bars * s_seqTicksBar);
    for (uint32_t i = 0; i < n; ++i) {
        float at = 0.0f, held = 0.0f;
        uint8_t pad = 0, vel = 0;
        if (own) {
            const crosspad::SeqNote& note = own->notes[i];
            pad = note.pad;
            at = (float)note.tick / span;
            held = (float)note.len / span;
        } else {
            if (!sq->getSceneEvent(scene, (uint16_t)i, at, pad, vel)) continue;
            held = sq->getSceneEventLength(scene, (uint16_t)i);
        }
        if (pad >= 16) continue;
        const int32_t y = bottom - 1 - (int32_t)pad * rows / 16;
        /* A held note is a line as long as it was held. */
        const int32_t len = std::max<int32_t>(2, (int32_t)(held * (float)loopPx));
        for (int32_t r = first; r <= last; ++r) {
            const int32_t x = a.x1 - fromPx + r * loopPx + (int32_t)(at * (float)loopPx);
            if (x < a.x1) continue;
            if (x > a.x2 || x + len - 1 < clipArea.x1 || x > clipArea.x2) continue;
            const lv_area_t rect = {x, y, std::min(a.x2, x + len - 1), y + 1};
            lv_draw_fill(layer, &d, &rect);
        }
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
        /* A seq block has no file: its notes are drawn from the scene. */
        if (b.peaks || !b.obj || b.file.empty() || !se->clipPeaks(b.file, cp)) continue;
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

/// What this screen has to say that the bar count does not. A toast, because
/// the export leaves it no row -- and only when it changes, or a line would
/// be raised again every refresh.
void note(const char* text)
{
    static char last[160] = {};
    if (!text || !*text) { last[0] = '\0'; return; }
    if (std::strncmp(last, text, sizeof(last)) == 0) return;
    std::snprintf(last, sizeof(last), "%s", text);
    ui_toast_show(text, 2500, UI_TOAST_INFO);
}

void note_fmt(const char* fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    note(buf);
}

void update_info()
{
    auto* se = engine();
    if (!se) return;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const auto& song = se->song();
    /* Selecting a block locates the song to it: PLAY on the bar starts there.
     * Set here rather than at the eight places the selection moves -- this
     * runs after every one of them -- but only when the selection changed,
     * or a repaint after a STOP would undo the bar the stop held. */
    {
        static int locTrack = -1, locIndex = -1;
        const auto& sel = song.tracks[s_selTrack].clips;
        const bool valid = s_selIndex >= 0 && s_selIndex < (int)sel.size();
        if (valid && (s_selTrack != locTrack || s_selIndex != locIndex)) {
            crosspad::songTransport().setStartBar(sel[s_selIndex].bar);
        }
        locTrack = s_selTrack;
        locIndex = valid ? s_selIndex : -1;
    }
    if (s_regionEdit != RegionEdit::None) {
        note_fmt( "Loop region: bars %u - %u   |   turn to move %s, click for %s",
                              (unsigned)(song.loopStart + 1), (unsigned)song.loopEnd,
                              s_regionEdit == RegionEdit::Start ? "L" : "R",
                              s_regionEdit == RegionEdit::Start ? "R" : "done");
        return;
    }
    /* The export gives this corner one dim label and the position writer
     * already fills it with "18 bars" / "bar 2 / 4" -- its own words. So the
     * status line speaks only where it has something that is not in the bar
     * count: a region being edited, a clip whose file is gone, an empty song.
     * Everything else leaves the slot to the position. */
    const auto& cs = song.tracks[s_selTrack].clips;
    if (s_selIndex >= 0 && s_selIndex < (int)cs.size()) {
        const auto& c = cs[s_selIndex];
        /* The name first and the reason plainly: a block whose file is gone is
         * the one thing on this screen that cannot be fixed on this screen. */
        if (block_missing(s_selTrack, s_selIndex)) {
            note_fmt( "%s  -  NOT IN clips/   |   bar %u, %u bars",
                                  short_name(c.file).c_str(), (unsigned)(c.bar + 1), (unsigned)c.bars);
            return;
        }
        return;   // the bar count already says this; see the note above
    } else if (se->playEndBars() == 0) {
        note( "Empty song -- REC records the pads here, or EDIT adds a clip");
    } else {
        return;   // likewise
    }
}

void select(int track, int index)
{
    s_selTrack = track;
    s_selIndex = index;
    for (int i = 0; i < s_blockCount; ++i) paint_block(s_blocks[i]);
    if (s_seqBlock) {
        /* The seq over the whole song is its track's only block: lit when the
         * track is picked, the way a selected block is. */
        const bool sel = s_seqBlockTrack == s_selTrack;
        lv_obj_set_style_outline_width(s_seqBlock, sel ? 2 : 0, 0);
        lv_obj_set_style_outline_color(s_seqBlock, kHead, 0);
        lv_obj_set_style_outline_pad(s_seqBlock, 1, 0);
    }
    for (int t = 0; t < kTracks; ++t) {
        if (s_heads[t]) lv_obj_set_style_border_width(s_heads[t], t == s_selTrack ? 2 : 0, 0);
    }
    update_info();
}

void enter_tools();
void place_move_arrows();
void rebuild_timeline();
int move_target(const crosspad::AudioSong& song, int from, int dir);

/// The seq over the whole song, as a block: picked by focus, and a click
/// makes it one (seqBlocksExplicit, as a cut does) and opens MOVE / DUP / DEL
/// on it -- on the next turn of the loop, since that rebuilds the timeline
/// and the lane being clicked with it.
void seq_lane_cb(lv_event_t* e)
{
    const int track = (int)(intptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        select(track, -1);
        return;
    }
    lv_async_call([](void* p) {
        const int t = (int)(intptr_t)p;
        auto* se = engine();
        if (!se) return;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            auto& song = se->song();
            if (!song.tracks[t].inUse || song.tracks[t].kind != crosspad::TrackKind::Seq) return;
            song.seqBlocksExplicit(se->playEndBars());
        }
        se->commit();
        s_selTrack = t;
        s_selIndex = 0;
        rebuild_timeline();
        enter_tools();
    }, (void*)(intptr_t)track);
}

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
    lv_group_add_obj(s_encGroup, s_projBtn);
    lv_group_add_obj(s_encGroup, s_editBtn);
    lv_group_add_obj(s_encGroup, s_syncBtn);
    lv_group_add_obj(s_encGroup, s_loopBtn);
    lv_group_add_obj(s_encGroup, s_zoomBtn);
    for (int i = 0; i < s_blockCount; ++i) lv_group_add_obj(s_encGroup, s_blocks[i].obj);
    if (s_seqBlock) lv_group_add_obj(s_encGroup, s_seqBlock);
    for (auto* h : s_heads) {
        /* A header that belongs to no track is hidden, and LVGL's group walk
         * skips hidden objects anyway -- but it is not added at all, so the
         * walk does not have to. */
        if (h && !lv_obj_has_flag(h, LV_OBJ_FLAG_HIDDEN)) lv_group_add_obj(s_encGroup, h);
    }
    if (s_addRow && !lv_obj_has_flag(s_addRow, LV_OBJ_FLAG_HIDDEN)) lv_group_add_obj(s_encGroup, s_addRow);
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

void set_bg(lv_obj_t* o, lv_color_t c);
void set_ink(lv_obj_t* o, lv_color_t c);
void set_text(lv_obj_t* l, const char* text);

/// S or M on a header: lit in its colour, or dark.
void style_toggle(lv_obj_t* b, bool on, lv_color_t col)
{
    if (!b) return;
    set_bg(b, on ? col : kWell);
    set_ink(lv_obj_get_child(b, 0), on ? kInk : kDim);
}
void head_mute_cb(lv_event_t* e);
void head_solo_cb(lv_event_t* e);
void toggle_mute(int t);
void toggle_solo(int t);

void rebuild_timeline()
{
    auto* se = engine();
    if (!se || !s_timeline) return;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    /* A seq track holds no audio -- the sequencer plays its scene, its
     * blocks have no file. So a seq track that does hold a file is a bounce
     * that landed while this screen was not up to see it (MODE + pad during
     * a bounce), and saying so here costs a scan and leaves no track that is
     * drawn as notes and plays as audio. */
    {
        auto& mut = se->song();
        bool healed = false;
        for (int t = 0; t < kTracks; ++t) {
            const auto& cs = mut.tracks[t].clips;
            if (mut.tracks[t].inUse && mut.tracks[t].kind == crosspad::TrackKind::Seq &&
                std::any_of(cs.begin(), cs.end(), [](const crosspad::SongClip& c) { return !c.file.empty(); })) {
                mut.bounceSeqTrack(t);
                healed = true;
            }
        }
        if (healed) se->commit();
    }
    const auto& song = se->song();

    lv_obj_clean(s_timeline);
    s_framesPerBar = se->framesPerBar();
    s_peaksGen = se->peaksGeneration();
    s_playhead = nullptr;
    for (auto& a : s_moveArrows) a = nullptr;
    s_blockCount = 0;

    const uint32_t end = se->playEndBars();
    /* Past the end as well, so there is room to lengthen the song or move a
     * block out -- and at least a screenful, so a short song zoomed out does
     * not leave the lanes half drawn. */
    const int32_t view = lv_obj_get_content_width(s_timeline);
    uint32_t bars = std::max({song.songEnd(), end, song.loopEnd}) + 8;
    bars = std::max<uint32_t>(bars, (uint32_t)(view / s_pxPerBar) + 1);
    /* A take running on past the last block: lanes for where it is going. */
    if (se->playing()) bars = std::max<uint32_t>(bars, (uint32_t)se->positionBars() + 16);
    s_drawnBars = bars;
    s_seqTicksBar = song.seqTicksPerBar();
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

    /* Only the tracks somebody made. They are always the first of the four:
     * a track is taken from the lowest free one and removing one closes the
     * gap (AudioSong::freeTrack(), removeTrack()), so "in use" and "before
     * usedTracks()" are the same set. */
    const int used = song.usedTracks();
    s_seqBlock = nullptr;
    s_seqBlockTrack = -1;
    for (int tr = 0; tr < used; ++tr) {
        const int32_t y = kRulerH + 2 + tr * (kLaneH + kLaneGap);
        lv_obj_t* lane = lv_obj_create(s_timeline);
        lv_obj_remove_style_all(lane);
        lv_obj_set_pos(lane, 0, y);
        lv_obj_set_size(lane, (int32_t)bars * s_pxPerBar, kLaneH);
        lv_obj_set_style_bg_color(lane, kLane, 0);
        lv_obj_set_style_bg_opa(lane, LV_OPA_COVER, 0);
        lv_obj_remove_flag(lane, LV_OBJ_FLAG_CLICKABLE);

        const bool seq = song.tracks[tr].kind == crosspad::TrackKind::Seq;
        if (seq && !song.tracks[tr].seqBlocks) {
            /* One block over the whole song, because that is how long the
             * pattern plays. It has no clip of its own to move or duplicate
             * until it is given blocks (seqBlocksExplicit) -- by a cut, a
             * region copy, or a click on it, which opens the block's keys
             * like any block's (seq_lane_cb). The encoder stops on it. */
            const uint32_t over = std::max<uint32_t>(end, 1);
            lv_obj_t* o = lv_obj_create(s_timeline);
            lv_obj_remove_style_all(o);
            lv_obj_set_pos(o, 0, y);
            lv_obj_set_size(o, std::max<int32_t>(2, (int32_t)over * s_pxPerBar - 1), kLaneH);
            lv_obj_set_style_radius(o, 3, 0);
            lv_obj_set_style_bg_color(o, track_color(song.tracks[tr]), 0);
            lv_obj_set_style_bg_opa(o, song.silenced(tr) ? LV_OPA_30 : LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(o, 2, 0);
            lv_obj_set_style_pad_top(o, 1, 0);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(o, seq_lane_cb, LV_EVENT_FOCUSED, (void*)(intptr_t)tr);
            lv_obj_add_event_cb(o, seq_lane_cb, LV_EVENT_CLICKED, (void*)(intptr_t)tr);
            s_seqLaneScene = song.tracks[tr].scene;
            lv_obj_add_event_cb(o, seq_draw_cb, LV_EVENT_DRAW_MAIN_END, (void*)(intptr_t)-1);
            lv_obj_t* lb = lv_label_create(o);
            lv_obj_set_style_text_font(lb, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(lb, kInk, 0);
            lv_label_set_long_mode(lb, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(lb, LV_PCT(100));
            set_text_fmt(lb, "Seq %u", (unsigned)(song.tracks[tr].scene + 1));
            s_seqBlock = o;
            s_seqBlockTrack = tr;
            continue;
        }

        const auto& cs = song.tracks[tr].clips;
        for (int i = 0; i < (int)cs.size() && i < kMaxBlocksPerTrack && s_blockCount < kMaxBlocks; ++i) {
            const auto& c = cs[i];
            lv_obj_t* o = lv_obj_create(s_timeline);
            lv_obj_remove_style_all(o);
            lv_obj_set_pos(o, (int32_t)c.bar * s_pxPerBar, y);
            lv_obj_set_size(o, std::max<int32_t>(2, (int32_t)c.bars * s_pxPerBar - 1), kLaneH);
            lv_obj_set_style_radius(o, 3, 0);
            /* Coloured by type, like the track's badge (UX 2000 section 7). */
            lv_obj_set_style_bg_color(o, seq ? track_color(song.tracks[tr]) : kAudioT, 0);
            lv_obj_set_style_bg_opa(o, song.silenced(tr) ? LV_OPA_30 : LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(o, 2, 0);
            lv_obj_set_style_pad_top(o, 1, 0);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
            lv_obj_add_event_cb(o, block_cb, LV_EVENT_FOCUSED, (void*)(intptr_t)s_blockCount);
            lv_obj_add_event_cb(o, block_cb, LV_EVENT_CLICKED, (void*)(intptr_t)s_blockCount);
            lv_obj_add_event_cb(o, seq ? seq_draw_cb : block_draw_cb, LV_EVENT_DRAW_MAIN_END,
                                (void*)(intptr_t)s_blockCount);

            lv_obj_t* lb = lv_label_create(o);
            lv_obj_set_style_text_font(lb, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(lb, kInk, 0);
            lv_label_set_long_mode(lb, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(lb, LV_PCT(100));
            /* A block of its own notes is the seq track's part; one that
             * plays the scene says which. */
            if (seq && c.notes) lv_label_set_text(lb, "SEQ");
            else if (seq)       set_text_fmt(lb, "Seq %u", (unsigned)(song.tracks[tr].scene + 1));
            else                lv_label_set_text(lb, short_name(c.file).c_str());

            Block b;
            b.obj = o;
            b.track = tr;
            b.index = i;
            b.file = c.file;
            b.offset = c.offset;
            b.frames = c.frames;
            b.bars = c.bars;
            b.fitted = song.tracks[tr].stretch != crosspad::StretchMode::Off;
            b.muted = song.silenced(tr);
            b.seq = seq;
            b.notes = c.notes;
            b.scene = song.tracks[tr].scene;
            b.from = c.from;
            crosspad::ClipPeaks cp;
            if (!seq && se->clipPeaks(c.file, cp)) set_peaks(b, cp);
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
    lv_obj_set_size(s_playhead, 2, kRulerH + 2 + std::max(1, used) * (kLaneH + kLaneGap));
    lv_obj_set_style_bg_color(s_playhead, kHead, 0);
    lv_obj_set_style_bg_opa(s_playhead, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_playhead, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_playhead, LV_OBJ_FLAG_HIDDEN);

    for (int t = 0; t < kTracks; ++t) {
        if (!s_heads[t]) continue;
        if (t >= used) {
            lv_obj_add_flag(s_heads[t], LV_OBJ_FLAG_HIDDEN);
            if (s_meterWell[t]) lv_obj_add_flag(s_meterWell[t], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_heads[t], LV_OBJ_FLAG_HIDDEN);
        if (s_meterWell[t]) lv_obj_remove_flag(s_meterWell[t], LV_OBJ_FLAG_HIDDEN);
        /* The number in the colour of the app that makes the track -- the
         * only place the screen says a track is a seq; the name under it, or
         * AUD / KIT / PITCH until it has one, dim while the track is silent. */
        const auto& tr = song.tracks[t];
        s_trackQuiet[t] = song.silenced(t);
        set_ink(s_headNum[t], track_color(tr));
        if (s_headName[t]) {
            set_text(s_headName[t], tr.name.empty() ? track_badge(tr) : tr.name.c_str());
            set_ink(s_headName[t], s_trackQuiet[t] || tr.name.empty() ? kDim : kText);
        }
        style_toggle(s_headSolo[t], tr.solo, kKitT);
        style_toggle(s_headMute[t], tr.muted, kMute);
    }

    /* "+ ADD TRACK", in the slot of the first track nobody has made -- and
     * gone once all four are. The counter beside it is the rule the player
     * needs before choosing: how many tracks are left, and whether the one
     * seq slot is free. */
    if (s_addRow) {
        if (used >= kTracks) {
            lv_obj_add_flag(s_addRow, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_addRow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(s_addRow, ARR_Y(62) + kRulerH + 4 + used * (kLaneH + kLaneGap));
            if (s_addInfo)
                set_text_fmt(s_addInfo, "%d / %d tracks  -  seq %d / %d", used, kTracks,
                             song.seqTracks(), crosspad::AudioSong::kMaxSeqTracks);
        }
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
    /* No PLAY button on this screen any more -- the transport bar has it, and
     * it paints itself from the same state. */
    (void)playing;
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

/// The meters beside the lanes, from what each track is playing: straight
/// up, down at ~28 dB a second. A segment lights at its level -- green
/// below -6 dB, amber to -1, red above -- and only those that change are
/// touched.
void show_meters(crosspad::ISongEngine* se)
{
    static constexpr float kSegDb[kMeterSegs] = {-48, -42, -36, -30, -24, -18, -12, -6, -3, -1};
    constexpr float kFall = 0.85f;             // a tick's fall
    for (int t = 0; t < kTracks; ++t) {
        if (!s_meterWell[t]) continue;
        float lv[2] = {0.0f, 0.0f};
        se->trackLevels(t, lv[0], lv[1]);
        for (int c = 0; c < 2; ++c) {
            s_meterShown[t][c] = s_trackQuiet[t] ? 0.0f : std::max(lv[c], s_meterShown[t][c] * kFall);
            const float v = s_meterShown[t][c];
            const float db = v > 0.0f ? 20.0f * std::log10(v) : -96.0f;
            uint8_t lit = 0;
            while (lit < kMeterSegs && db >= kSegDb[lit]) ++lit;
            if (lit == s_meterLit[t][c]) continue;
            for (int i = 0; i < kMeterSegs; ++i) {
                const bool on = i < lit, was = i < s_meterLit[t][c];
                if (on != was) lv_obj_set_style_bg_opa(s_meterSeg[t][c][i], on ? LV_OPA_COVER : LV_OPA_20, 0);
            }
            s_meterLit[t][c] = lit;
        }
    }
}

/// Stretched or repitched blocks still being fitted to the tempo: a box over
/// the lanes for as long as any is waiting -- they are made only while
/// stopped, and until then those blocks play as recorded.
void show_processing(crosspad::ISongEngine* se)
{
    if (!s_procBox) return;
    const bool up = !lv_obj_has_flag(s_procBox, LV_OBJ_FLAG_HIDDEN);
    const uint32_t waiting = se->stretchWaiting();
    if (!waiting) {
        if (up) {
            lv_obj_add_flag(s_procBox, LV_OBJ_FLAG_HIDDEN);
            note("Time stretch done.");
        }
        return;
    }
    char buf[72];
    if (se->playing()) {
        std::snprintf(buf, sizeof(buf), "TIME STRETCH\n%u block%s wait for STOP",
                      (unsigned)waiting, waiting == 1 ? "" : "s");
    } else {
        std::snprintf(buf, sizeof(buf), "PROCESSING TIME STRETCH\n%u block%s left  -  %u%%",
                      (unsigned)waiting, waiting == 1 ? "" : "s", (unsigned)se->stretchProgress());
    }
    set_text(s_procLabel, buf);
    if (!up) {
        lv_obj_remove_flag(s_procBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_procBox);
    }
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
        if (st == crosspad::SongOpState::Failed) {
            note( "That song could not be read; it is left untouched on the card.");
        }
    }
    show_processing(se);
    if (s_merging) {
        char buf[48];
        if (se->editState() == crosspad::SongOpState::Busy) {
            std::snprintf(buf, sizeof(buf), "Merging ... %u%%", (unsigned)se->editProgress());
            note( buf);
        } else {
            s_merging = false;
            const bool ok = se->editState() == crosspad::SongOpState::Ready;
            rebuild_timeline();
            update_info();
            if (!ok) note( "Not merged: no card, nothing audible on those tracks, or the card is full.");
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
        else if (failed && s_editKind == EditKind::Delete) {
            note( "Not deleted: no card, or the card kept some of its files.");
        } else if (failed) note( "The tempo was not set: no song is open.");
    }
    /* A bounce started from BOUNCE on a seq track. Polled the way the
     * Sequencer's own bounce window polls it: anything but Running is over. */
    if (s_bouncing >= 0) {
        const crosspad::BounceStatus st = se->bounceStatus();
        if (st.state == crosspad::BounceState::Running) {
            const unsigned pct = st.framesTotal
                ? (unsigned)((uint64_t)st.framesDone * 100 / st.framesTotal) : 0u;
            note_fmt("Bouncing track %d to audio ... %u%%", s_bouncing + 1, pct);
        } else {
            const int track = s_bouncing;
            s_bouncing = -1;
            if (st.state == crosspad::BounceState::Done) {
                /* The engine wrote the WAV, put its block on this track and
                 * made the track audio, which frees the seq slot. */
                rebuild_timeline();
                note_fmt("Track %d is audio now -- the seq slot is free.", track + 1);
            } else if (!st.error.empty()) {
                note_fmt("Not bounced: %s.", st.error.c_str());
            } else {
                note("Not bounced: nothing audible in that scene, no card, or it was cancelled.");
            }
        }
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
        total = se->playEndBars();
    }
    if (shape != s_shape) rebuild_timeline();
    show_meters(se);
    sync_transport();

    if (!s_playhead) return;
    char buf[32];
    if (!se->playing()) {
        /* Stopped, it stands where PLAY starts: the top bar's position turns
         * it a bar a detent, and the grid follows it there -- only when it
         * moves, so a finger scrolling the timeline is not pulled back. */
        static uint32_t shownAt = UINT32_MAX;
        const uint32_t at = crosspad::songTransport().startBar();
        const int32_t x = (int32_t)at * s_pxPerBar;
        set_bg(s_playhead, kHead);
        if (lv_obj_has_flag(s_playhead, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(s_playhead, LV_OBJ_FLAG_HIDDEN);
        if (lv_obj_get_x(s_playhead) != x) lv_obj_set_pos(s_playhead, x, 0);
        if (at != shownAt) {
            shownAt = at;
            const int32_t view = lv_obj_get_content_width(s_timeline);
            const int32_t left = lv_obj_get_scroll_x(s_timeline);
            if (x < left || x > left + view - 10) {
                lv_obj_scroll_to_x(s_timeline, x > view / 3 ? x - view / 3 : 0, LV_ANIM_ON);
            }
        }
        std::snprintf(buf, sizeof(buf), "%u bars", (unsigned)total);
        set_text(s_posLabel, buf);
        return;
    }
    const double pos = se->positionBars();
    /* A take past the last block runs off the lanes: they are drawn on. */
    if ((uint32_t)pos + 2 >= s_drawnBars) {
        rebuild_timeline();
        if (!s_playhead) return;
    }
    const int32_t x = (int32_t)(pos * s_pxPerBar);
    /* Red while the pads are being recorded onto the song. */
    set_bg(s_playhead, se->seqTake() != crosspad::SyncTake::None ? kRec : kHead);
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
    bool up = false, down = false;
    if (auto* se = engine()) {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& cs = se->song().tracks[s_selTrack].clips;
        if (s_selIndex >= 0 && s_selIndex < (int)cs.size()) bar = cs[s_selIndex].bar;
        up   = move_target(se->song(), s_selTrack, -1) >= 0;
        down = move_target(se->song(), s_selTrack, +1) >= 0;
    }
    struct Spot { int32_t x, y; const char* sym; bool can; bool bars; };
    const Spot spots[4] = {
        {x - kA - 1,         y + (h - kA) / 2, LV_SYMBOL_LEFT,  bar > 0, true},
        {x + w + 1,          y + (h - kA) / 2, LV_SYMBOL_RIGHT, true,    true},
        {x + (w - kA) / 2,   y - kA / 2 - 2,   LV_SYMBOL_UP,    up,      false},
        {x + (w - kA) / 2,   y + h - kA / 2 + 2, LV_SYMBOL_DOWN, down,   false},
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

/// The track a block moving up (-1) or down (+1) from @p from lands on, or
/// -1. Only tracks in use and only of its own kind: an unused track is not on
/// the screen, an audio block on a seq track would be a file the sequencer
/// cannot play, and a seq block is its track's scene (UX 2000 section 7.1).
int move_target(const crosspad::AudioSong& song, int from, int dir)
{
    /* There is one seq track, so a seq block has nowhere else to go. */
    if (song.tracks[from].kind != crosspad::TrackKind::Audio) return -1;
    for (int t = from + dir; t >= 0 && t < kTracks; t += dir) {
        const auto& tr = song.tracks[t];
        if (tr.inUse && tr.kind == crosspad::TrackKind::Audio) return t;
    }
    return -1;
}

/// The selected block to the track above (-1) or below (+1), on its bar.
void move_track(int dir)
{
    auto* se = engine();
    if (!se) return;
    int to = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        to = move_target(se->song(), s_selTrack, dir);
    }
    if (to < 0) return;
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

void delete_track_sheet();

/// DEL: the selected clip -- or, with only a track picked by its header, the
/// track itself, after asking (UX 2000 section 7.3).
void del_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& cs = se->song().tracks[s_selTrack].clips;
        if (s_selIndex < 0 || s_selIndex >= (int)cs.size()) {
            if (se->song().tracks[s_selTrack].inUse) delete_track_sheet();
            return;
        }
        cs.erase(cs.begin() + s_selIndex);
        if (s_selIndex >= (int)cs.size()) s_selIndex = (int)cs.size() - 1;
    }
    se->commit();
    rebuild_timeline();
    leave_tools();
}

void leave_region_edit();

void close_overlay();

/// The power button's click goes back one step, and only one: a window, the
/// loop region being turned, MOVE being turned, the keys a block opened --
/// and out of the app with none of them left. A block that is only picked is
/// not a step: the encoder picks one by walking over it, the pick outlives
/// the screen, and a click spent unpicking it showed nothing but a 2 px
/// outline going, so getting out took two clicks or more (12.10.2026). (The
/// encoder's edit mode and the top bar's focus are stepped out of before this
/// is asked: powerBack().)
bool back_handler(lv_obj_t*)
{
    if (s_overlay) {
        for (auto& t : s_mergeToggles) t = nullptr;
        close_overlay();
        return true;
    }
    if (s_regionEdit != RegionEdit::None) { leave_region_edit(); return true; }
    if (s_toolMode) {
        /* Turned by a finger on its arrows, MOVE has no edit mode for the
         * step before this to have left: it stops here, the keys stay. */
        if (s_moveAxis != MoveAxis::None) { set_move_axis(MoveAxis::None); place_move_arrows(); return true; }
        leave_tools();
        return true;
    }
    return false;
}

void track_menu();

/// A track header picks the track and opens its menu -- BOUNCE and DELETE,
/// what acts on a track rather than on a block. EDIT acts on the one picked,
/// and a seq track that loops over the whole song has no block to click, so
/// this is the only way to reach it -- which is why the click does not mute.
/// Mute is the M button in MIX (a long press on the same header), where the
/// levels are.
void head_cb(lv_event_t* e)
{
    auto* se = engine();
    if (!se) return;
    const int t = (int)(intptr_t)lv_event_get_user_data(e);
    int index = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& tr = se->song().tracks[t];
        if (!tr.inUse) return;
        index = s_selTrack == t ? s_selIndex : (tr.clips.empty() ? -1 : 0);
    }
    select(t, index);
    /* The release of a hold that opened MIX is a click too; MIX is up then. */
    track_menu();
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

/* One key where the export puts it, instead of the two it has no room for:
 * in, and round to the widest when there is no further in to go. */
void zoom_cycle_cb(lv_event_t*)
{
    if (s_zoom >= kZoomLevels - 1) { s_zoom = 0; s_pxPerBar = kZoomPx[0]; rebuild_timeline(); }
    else                             zoom(+1);
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
    s_trackNameArea = nullptr;
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
            /* One tempo: the sequencer, the click and the bar's BPM go with
             * the song. */
            if (auto* sq = crosspad::getPlatformServices().sequencer) sq->setBpm((float)s_editBpm);
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
    note_fmt("Renaming to %s ...", name.c_str());
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
    note("Deleting ...");
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
        if (!report.empty()) note(report[0].c_str());
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
    note("Writing the Ableton Live Set ...");
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
    if (s_opening) note_fmt("Opening %s ...", name.c_str());
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
        if (song.tracks[s_selTrack].kind == crosspad::TrackKind::Seq) {
            note("That track plays a seq. Bounce it to audio first (its header's menu).");
            return;
        }
        if (!song.tracks[s_selTrack].inUse) song.addAudioTrack(s_selTrack);
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
    if (code == LV_EVENT_FOCUSED) {
        lv_group_t* g = lv_obj_get_group(lv_event_get_target_obj(e));
        if (s_regionArmed && !(g && lv_group_get_editing(g))) leave_region_edit();
        return;
    }
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
        s_regionArmed = true;
    }
#endif
}

void leave_region_edit()
{
    if (s_regionEdit == RegionEdit::None) return;
    s_regionEdit = RegionEdit::None;
    s_regionArmed = false;
#ifdef ARR_USE_ENCODER
    if (s_regionGroup) lv_group_set_editing(s_regionGroup, false);
    if (s_encGroup && indev_encoderTop) lv_indev_set_group(indev_encoderTop, s_encGroup);
#endif
    rebuild_timeline();
    update_info();
}

/// Before an edit by bars: a seq track still looping over the whole song
/// becomes one block of it, so the edit moves its notes with the audio, and
/// the samples a bar that cuts an audio block where a bar says. Caller holds
/// songMutex().
uint32_t prepare_bar_edit(crosspad::AudioSong& song)
{
    auto* se = engine();
    song.seqBlocksExplicit(se->playEndBars());
    return se->framesPerBar();
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
        const uint32_t spb = prepare_bar_edit(song);
        s_clipboard = song.copyRange(song.loopStart, song.loopEnd, spb);
        s_clipboardBars = song.loopEnd - song.loopStart;
    });
}

/// The copied bars in at the region's start; what was there moves on, cut
/// where the region starts if it crossed it.
void region_paste_cb(lv_event_t*)
{
    region_edit([](crosspad::AudioSong& song) {
        if (s_clipboard.empty() || !s_clipboardBars) return;
        const uint32_t spb = prepare_bar_edit(song);
        song.insertBars(song.loopStart, s_clipboardBars, spb);
        song.pasteRange(s_clipboard, song.loopStart);
        song.loopEnd = song.loopStart + s_clipboardBars;
    });
}

/// The region's bars again right after it, and the region twice as long:
/// 4 bars, 8, 16 -- as the Sequencer's x2. The seq goes by bars too: its
/// notes in the region are what repeats, not its whole pattern.
void region_dup_cb(lv_event_t*)
{
    region_edit([](crosspad::AudioSong& song) {
        const uint32_t w = song.loopEnd - song.loopStart;
        const uint32_t spb = prepare_bar_edit(song);
        const auto part = song.copyRange(song.loopStart, song.loopEnd, spb);
        song.insertBars(song.loopEnd, w, spb);
        song.pasteRange(part, song.loopEnd);
        song.loopEnd += w;
    });
}

/// The bar the playhead is on: where the song is while it plays, where PLAY
/// starts while it is stopped (the top bar's position turns it).
uint32_t playhead_bar()
{
    auto* se = engine();
    if (se && se->playing()) {
        const double b = se->positionBars();
        return b > 0.0 ? (uint32_t)b : 0;
    }
    return crosspad::songTransport().startBar();
}

/// The block of the picked track under the playhead, in two there.
void cut_cb(lv_event_t*)
{
    auto* se = engine();
    close_overlay();
    if (!se) return;
    const uint32_t bar = playhead_bar();
    int second = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        const uint32_t spb = prepare_bar_edit(song);
        second = song.cutClip(s_selTrack, bar, spb);
    }
    if (second < 0) {
        note_fmt("Nothing on track %d runs across bar %u to cut.", s_selTrack + 1, (unsigned)bar + 1);
        return;
    }
    se->commit();
    /* On the second half: the playhead stays on the cut. */
    s_selIndex = second;
    rebuild_timeline();
    note_fmt("Cut at bar %u.", (unsigned)bar + 1);
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
        note( "Merging ...");
    } else {
        note( "Nothing to merge there.");
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
                            "muted tracks are left as they are, and a seq track until it is bounced "
                            "(its header's menu).");
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

/// UNDO / REDO: the arrangement a step back or forward (ISongEngine::undo).
/// The timeline repaints from the song's shape on the next tick.
void undo_redo(bool back)
{
    auto* se = engine();
    close_overlay();
    if (!se) return;
    if (back ? se->undo() : se->redo()) {
        select(s_selTrack, -1);
        note(back ? "Undone." : "Redone.");
    } else {
        note("Not now: a merge, a bounce or a new tempo is still being written.");
    }
}

void edit_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se || s_overlay || s_merging) return;
    bool region = false, block = false, tracked = false;
    uint32_t a = 0, b = 0;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        tracked = song.tracks[s_selTrack].inUse;
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
    /* What was just done, back or again, before anything new. */
    if (const uint32_t n = se->undoSteps()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), LV_SYMBOL_LEFT " Undo   (%u)", (unsigned)n);
        overlay_item(buf, [](lv_event_t*) { undo_redo(true); }, nullptr);
    }
    if (const uint32_t n = se->redoSteps()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), LV_SYMBOL_RIGHT " Redo   (%u)", (unsigned)n);
        overlay_item(buf, [](lv_event_t*) { undo_redo(false); }, nullptr);
    }
    /* First, because it is the one row here that puts something on a track:
     * it was the "+ ADD" key before the export's row gave that slot to EDIT
     * and BOUNCE. A track is normally filled by recording into it; this is
     * for a clip the project already has. */
    overlay_item(LV_SYMBOL_PLUS " Add a clip from the library ...",
                 [](lv_event_t* e) { close_overlay(); add_cb(e); }, nullptr);
    if (tracked) {
        char cut[48];
        std::snprintf(cut, sizeof(cut), LV_SYMBOL_CUT " Cut track %d at bar %u", s_selTrack + 1,
                      (unsigned)playhead_bar() + 1);
        overlay_item(cut, cut_cb, nullptr);
    }
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

// ── Arrange is where a song starts ────────────────────────────────────────
/*
 * The one flow in this screen that is not editing: making a track.
 *
 *   + ADD TRACK ──> "New track": AUDIO or SEQ
 *                      |            |
 *                      |            +--> "plays with": KIT or PITCH
 *                      |                      |
 *                      +--> the Recorder       +--> the sequencer on its scene
 *
 * Every dialog says the same two things, because they are the rules a player
 * has to know before choosing and nobody reads them anywhere else: four
 * tracks, four audio or one seq and three audio -- and any track can be
 * bounced to audio at any time, which is also how the seq slot comes free.
 */

/// A sheet over the timeline: the overlay machinery (its own encoder group,
/// the power button's click closing it) with the column layout off, because
/// a sheet places its cards rather than stacking them.
lv_obj_t* sheet_open()
{
    overlay_open();
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_layout(s_overlay, LV_LAYOUT_NONE);
    return s_overlay;
}

lv_obj_t* sheet_text(lv_obj_t* parent, int32_t x, int32_t y, const char* text,
                     const lv_font_t* font, lv_color_t col)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_label_set_text(l, text);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/// One choice: a card with a tag in its colour, a name, and two lines saying
/// what it is for. A card that cannot be taken is dimmed and says why in the
/// same two lines, so the reason is read where the choice is made.
lv_obj_t* sheet_card(int32_t x, const char* tag, const char* name, const char* l1,
                     const char* l2, lv_color_t col, bool enabled, lv_event_cb_t cb, void* ud)
{
    lv_obj_t* c = lv_button_create(s_overlay);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, ARR_Y(74));
    lv_obj_set_size(c, 152, 96);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, kPanel, 0);
    lv_obj_set_style_bg_color(c, lv_color_lighten(kPanel, 40), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(c, col, 0);
    lv_obj_set_style_border_width(c, enabled ? 2 : 1, 0);
    lv_obj_set_style_outline_color(c, kHead, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(c, 2, LV_STATE_FOCUS_KEY);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    sheet_text(c, 10, 8, tag, &lv_font_montserrat_10, col);
    sheet_text(c, 10, 22, name, &lv_font_montserrat_18, kText);
    sheet_text(c, 10, 54, l1, &lv_font_montserrat_10, kDim);
    sheet_text(c, 10, 68, l2, &lv_font_montserrat_10, kDim);
    if (!enabled) {
        lv_obj_set_style_opa(c, LV_OPA_40, 0);
        return c;                       // not clickable, not in the group
    }
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, c);
#endif
    return c;
}

/// The box that carries the rules, under the cards.
void sheet_rules(const char* line, const char* accented)
{
    lv_obj_t* box = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 4, ARR_Y(174));
    lv_obj_set_size(box, 312, 28);
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_bg_color(box, kLane, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    sheet_text(box, 8, 2, line, &lv_font_montserrat_10, kText);
    sheet_text(box, 8, 15, accented, &lv_font_montserrat_10, kAccent);
}

/// A sheet's keys, filling the width on the export's key row.
void sheet_keys(const char* const* labels, const lv_event_cb_t* cbs, int n)
{
    const int32_t gap = 4, avail = 320 - 8 - gap * (n - 1);
    int32_t x = 4;
    for (int i = 0; i < n; ++i) {
        const int32_t w = (i == n - 1) ? (316 - x) : avail / n;
        lv_obj_t* b = lv_button_create(s_overlay);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, x, ARR_Y(206));
        lv_obj_set_size(b, w, 28);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, kPanel, 0);
        lv_obj_set_style_bg_color(b, lv_color_lighten(kPanel, 40), LV_STATE_PRESSED);
        lv_obj_set_style_outline_color(b, kHead, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
        lv_obj_add_event_cb(b, cbs[i], LV_EVENT_CLICKED, nullptr);
        lv_obj_t* l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, kText, 0);
        lv_label_set_text(l, labels[i]);
        lv_obj_center(l);
#ifdef ARR_USE_ENCODER
        if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, b);
#endif
        x += w + gap;
    }
}

void new_track_sheet();
void seq_source_sheet();
void open_seq_track(int track);

/// Is the selected track one that holds a seq? What a bounce needs.
bool selected_is_seq()
{
    auto* se = engine();
    if (!se || s_selTrack < 0 || s_selTrack >= kTracks) return false;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const auto& t = se->song().tracks[s_selTrack];
    return t.inUse && t.kind == crosspad::TrackKind::Seq;
}

void cancel_sheet_cb(lv_event_t*) { close_overlay(); }
void back_to_add_cb(lv_event_t*)  { close_overlay(); new_track_sheet(); }

void choose_audio_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se) return;
    int made = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        made = song.freeTrack();
        if (made < 0 || !song.addAudioTrack(made)) made = -1;
    }
    close_overlay();
    if (made < 0) { note("No room: all four tracks are in use."); return; }
    se->commit();
    select(made, -1);
    rebuild_timeline();
    /* An empty audio track is filled by recording into it. The Recorder is
     * told which track sent it, so it knows where its take belongs and can
     * offer the way back. */
    crosspad::ArrangeFocus& f = crosspad::arrangeFocus();
    f.clear();
    f.track = made;
    f.bar = playhead_bar();      // the take, with the song, starts there
    if (!crosspad_gui::mode_overlay_open_app("Recorder")) {
        f.clear();
        note_fmt("Track %d added. No Recorder in this firmware -- EDIT it to add a clip.", made + 1);
    }
}

void choose_seq_cb(lv_event_t*)
{
    close_overlay();
    seq_source_sheet();
}

void choose_source_cb(lv_event_t* e)
{
    const auto src = (crosspad::SeqSource)(intptr_t)lv_event_get_user_data(e);
    auto* se = engine();
    auto* sq = crosspad::getPlatformServices().sequencer;
    if (!se) return;
    /* A scene of its own, so the track keeps playing what was put on it when
     * the player switches scenes somewhere else. A kit with no room left for
     * another scene lends this track the one that is current -- better than
     * refusing the track. */
    uint8_t scene = sq ? sq->getActiveScene() : 0;
    if (sq && sq->addScene()) scene = sq->getActiveScene();
    int made = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        made = song.freeTrack();
        if (made < 0 || !song.addSeqTrack(made, src, scene)) made = -1;
        /* A song that starts as a seq track has no clip to take its tempo
         * from: its grid is the sequencer's, or the two drift apart. */
        if (made >= 0 && song.bpm <= 0.0f && sq) song.bpm = sq->getBpm();
    }
    close_overlay();
    if (made < 0) { note("No room: all four tracks are in use, or the seq slot is taken."); return; }
    se->commit();
    select(made, -1);
    rebuild_timeline();
    open_seq_track(made);
}

/// A seq track's screen, on its scene and knowing it came from here: a Pitch
/// track opens in Pitch Play -- the very screen the MODE map's Pitch opens,
/// which takes the transport for this scene so REC and DUB record what is
/// played on its keys -- and a kit track in the sequencer's seq view.
void open_seq_track(int track)
{
    auto* se = engine();
    auto* sq = crosspad::getPlatformServices().sequencer;
    if (!se || track < 0 || track >= kTracks) return;
    crosspad::ArrangeFocus& f = crosspad::arrangeFocus();
    bool moved = false;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& t = se->song().tracks[track];
        if (!t.inUse || t.kind != crosspad::TrackKind::Seq) return;
        /* A scene the sequencer does not have -- made while its file was not
         * read, or removed since: a scene of its own again, as when the track
         * was made. Left as it was, the selection below is refused, the take
         * goes into whatever scene is up, and this track never plays it. */
        if (sq && t.scene >= sq->getSceneCount()) {
            sq->addScene();
            t.scene = sq->getActiveScene();
            moved = true;
        }
        f.clear();
        f.track  = track;
        f.seq    = true;
        f.source = t.source;
        f.scene  = t.scene;
        /* The block the seq view edits: the one picked, or the one under the
         * playhead -- or, on an empty bar, the block its first notes make. */
        const bool picked = s_selTrack == track && s_selIndex >= 0 && s_selIndex < (int)t.clips.size();
        f.bar = picked ? t.clips[(size_t)s_selIndex].bar : playhead_bar();
    }
    if (moved) se->commit();
    if (sq) sq->selectScene(f.scene);
    const bool pitch = f.source == crosspad::SeqSource::Pitch;
    if (!crosspad_gui::mode_overlay_open_app(pitch ? "PitchPlay" : "Sequencer")) {
        f.clear();
        note(pitch ? "No Pitch Play in this firmware." : "No sequencer in this firmware.");
    }
}

void new_track_sheet()
{
    auto* se = engine();
    if (!se || s_overlay || s_merging) return;
    int used = 0;
    bool seqFree = true;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        used = se->song().usedTracks();
        seqFree = se->song().seqSlotFree();
    }
    if (used >= kTracks) { note("All four tracks are in use."); return; }

    sheet_open();
    char title[32];
    std::snprintf(title, sizeof(title), "New track  %d / %d", used + 1, kTracks);
    sheet_text(s_overlay, 8, ARR_Y(36), title, &lv_font_montserrat_16, kText);
    lv_obj_t* hint = sheet_text(s_overlay, 0, ARR_Y(40), "what will it hold?",
                                &lv_font_montserrat_10, kDim);
    lv_obj_update_layout(hint);
    lv_obj_set_x(hint, 312 - lv_obj_get_width(hint));

    sheet_card(4, "AUDIO", "Audio", "a recorded or loaded loop,", "plays as clips",
               kAudioT, true, choose_audio_cb, nullptr);
    sheet_card(164, "SEQ", "Seq",
               seqFree ? "pads recorded as notes," : "the seq slot is taken:",
               seqFree ? "editable, any sound"     : "bounce it to free it",
               kAccent, seqFree, choose_seq_cb, nullptr);

    sheet_rules("Arrange holds 4 tracks: 4 audio, or 1 seq + 3 audio.",
                "Any track can be bounced to audio at any time.");
    static const char* kLabels[] = {"CANCEL"};
    static const lv_event_cb_t kCbs[] = {cancel_sheet_cb};
    sheet_keys(kLabels, kCbs, 1);
}

void seq_source_sheet()
{
    auto* se = engine();
    if (!se || s_overlay) return;
    int used = 0;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        used = se->song().usedTracks();
    }
    sheet_open();
    char title[48];
    std::snprintf(title, sizeof(title), "Seq track %d  -  plays with", used + 1);
    sheet_text(s_overlay, 8, ARR_Y(36), title, &lv_font_montserrat_16, kText);

    sheet_card(4, "SAMPLER", "Kit", "16 pads, one sample each:", "drums, chops",
               kKitT, true, choose_source_cb, (void*)(intptr_t)crosspad::SeqSource::Kit);
    sheet_card(164, "PITCH PLAY", "Pitch", "one sample in pitch:", "bass, keys, melody",
               kPitchT, true, choose_source_cb, (void*)(intptr_t)crosspad::SeqSource::Pitch);

    sheet_rules("Pitch opens Pitch Play; Kit opens the seq view.",
                "Bounce turns the seq into audio and frees the slot.");
    static const char* kLabels[] = {LV_SYMBOL_LEFT " BACK", "CANCEL"};
    static const lv_event_cb_t kCbs[] = {back_to_add_cb, cancel_sheet_cb};
    sheet_keys(kLabels, kCbs, 2);
}

void bounce_go_cb(lv_event_t*)
{
    auto* se = engine();
    const int track = s_selTrack;
    close_overlay();
    if (!se) return;
    /* A Pitch track is bounced through its instrument, so it has to be in:
     * notes before the bank is built would be silence in the clip. */
    bool pitch = false;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        pitch = se->song().tracks[track].source == crosspad::SeqSource::Pitch;
    }
    if (pitch && !crosspad::seqTrackSound().pitch.ready()) {
        note("The track's instrument is still loading -- BOUNCE again in a moment.");
        return;
    }
    /* The scene looped over the whole song -- what the track plays -- into
     * one clip on this track; the engine turns it into audio when it lands. */
    if (pitch) {
        s_soundBounce = true;
        crosspad::seqTrackSound().routePitch.store(true, std::memory_order_release);
    }
    if (!se->bounceSeqTrack((uint8_t)track)) {
        s_soundBounce = false;
        note("Not bounced: no song is open, or a bounce is already running.");
        return;
    }
    s_bouncing = track;
    note_fmt("Bouncing track %d to audio ...", track + 1);
}

/// A question over a dimmed Arrange: 272 x 120, a 1 px border in @p accent,
/// a title, two lines, and CANCEL beside the key that does it.
void confirm_sheet(const char* title, const char* line, const char* line2, lv_color_t line2Col,
                   lv_color_t accent, lv_color_t okFg, const char* okText, lv_event_cb_t okCb)
{
    sheet_open();
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, 0);

    lv_obj_t* box = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 24, ARR_Y(60));
    lv_obj_set_size(box, 272, 120);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_bg_color(box, kPanel, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, accent, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    sheet_text(box, 12, 10, title, &lv_font_montserrat_16, kText);
    sheet_text(box, 12, 38, line, &lv_font_montserrat_10, kDim);
    sheet_text(box, 12, 52, line2, &lv_font_montserrat_10, line2Col);

    struct { const char* text; lv_event_cb_t cb; lv_color_t bg; lv_color_t fg; } keys[2] = {
        { "CANCEL", cancel_sheet_cb, kPanel, kText },
        { okText,   okCb,            accent, okFg  },
    };
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* b = lv_button_create(box);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, 12 + i * 130, 78);
        lv_obj_set_size(b, 118, 30);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, keys[i].bg, 0);
        lv_obj_set_style_bg_color(b, lv_color_lighten(keys[i].bg, 40), LV_STATE_PRESSED);
        lv_obj_set_style_outline_color(b, kHead, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
        lv_obj_add_event_cb(b, keys[i].cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, keys[i].fg, 0);
        lv_label_set_text(l, keys[i].text);
        lv_obj_center(l);
#ifdef ARR_USE_ENCODER
        if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, b);
#endif
    }
}

/// "Bounce T3 to audio?" -- the dialog, over a dimmed Arrange. An audio
/// track already is one: nothing to ask.
void bounce_sheet()
{
    if (!selected_is_seq()) return;
    if (s_overlay || s_merging || s_bouncing >= 0) return;
    char title[32];
    std::snprintf(title, sizeof(title), "Bounce T%d to audio?", s_selTrack + 1);
    confirm_sheet(title, "The notes become one audio clip on this track.",
                  "The seq slot is free again for a new idea.", kAccent,
                  kKitT, kInk, "BOUNCE", bounce_go_cb);
}

void delete_track_go_cb(lv_event_t*)
{
    auto* se = engine();
    const int track = s_selTrack;
    close_overlay();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        if (!se->song().tracks[track].inUse) return;
        se->song().removeTrack(track);
    }
    se->commit();
    select(std::max(0, track - 1), -1);
    rebuild_timeline();
    note_fmt("Track %d deleted.", track + 1);
}

/// "Delete T3?" -- DEL with a track and no clip picked.
void delete_track_sheet()
{
    if (s_overlay || s_merging || s_bouncing >= 0) return;
    char title[32];
    std::snprintf(title, sizeof(title), "Delete T%d?", s_selTrack + 1);
    confirm_sheet(title, "Its clips leave the song; the files stay in clips/.",
                  "The tracks under it move up.", kDim,
                  kMute, kInk, "DELETE", delete_track_go_cb);
}

void add_track_cb(lv_event_t*) { new_track_sheet(); }

void track_name_done_cb(lv_event_t* e)
{
    const bool ok = lv_event_get_code(e) == LV_EVENT_READY;
    auto* se = engine();
    std::string name = s_trackNameArea ? lv_textarea_get_text(s_trackNameArea) : "";
    close_overlay();
    if (!ok || !se) return;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& t = se->song().tracks[s_selTrack];
        if (!t.inUse) return;
        t.name = name;
    }
    se->commit();
}

/// RENAME on a track's menu: the name its header shows, kept with the song.
void rename_track_sheet()
{
    auto* se = engine();
    if (!se || s_overlay) return;
    std::string name;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& t = se->song().tracks[s_selTrack];
        if (!t.inUse) return;
        name = t.name;
    }
    char title[32];
    std::snprintf(title, sizeof(title), "Track %d  -  name", s_selTrack + 1);
    overlay_list(title);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    s_trackNameArea = lv_textarea_create(s_overlay);
    lv_textarea_set_one_line(s_trackNameArea, true);
    lv_textarea_set_max_length(s_trackNameArea, 24);
    lv_textarea_set_text(s_trackNameArea, name.c_str());
    lv_obj_set_width(s_trackNameArea, LV_PCT(100));
    lv_obj_set_style_bg_color(s_trackNameArea, kPanel, 0);
    lv_obj_set_style_border_width(s_trackNameArea, 0, 0);
    lv_obj_set_style_radius(s_trackNameArea, 6, 0);
    lv_obj_set_style_text_color(s_trackNameArea, kText, 0);
    lv_obj_set_style_text_font(s_trackNameArea, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(s_trackNameArea, kAccent, LV_PART_CURSOR);
    lv_obj_t* kb = lv_keyboard_create(s_overlay);
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_flex_grow(kb, 1);
    lv_keyboard_set_textarea(kb, s_trackNameArea);
    lv_obj_add_event_cb(kb, track_name_done_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, track_name_done_cb, LV_EVENT_CANCEL, nullptr);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) {
        lv_group_add_obj(s_overlayGroup, kb);
        lv_group_focus_obj(kb);
        lv_group_set_editing(s_overlayGroup, true);
    }
#endif
}

/// TIME STRETCH on an audio track: its blocks keep their bars through a
/// change of tempo and play their audio fitted to them -- stretched, the
/// pitch kept, or repitched, the pitch moving with the tempo as on a
/// turntable. Rendered by the song engine while stopped, as recorded until
/// then (SongTrack::stretch).
void stretch_mode_cb(lv_event_t* e)
{
    const auto mode = (crosspad::StretchMode)(intptr_t)lv_event_get_user_data(e);
    auto* se = engine();
    close_overlay();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& t = se->song().tracks[s_selTrack];
        if (!t.inUse || t.kind != crosspad::TrackKind::Audio || t.stretch == mode) return;
        t.stretch = mode;
    }
    se->commit();
    rebuild_timeline();   // the waveforms: fitted to the bars, or at their own speed
    switch (mode) {
    case crosspad::StretchMode::Off:
        note_fmt("Track %d plays as recorded again.", s_selTrack + 1);
        break;
    case crosspad::StretchMode::KeepPitch:
        note_fmt("Track %d follows the tempo, its pitch kept -- fitted while stopped.", s_selTrack + 1);
        break;
    case crosspad::StretchMode::Repitch:
        note_fmt("Track %d follows the tempo, its pitch with it -- fitted while stopped.", s_selTrack + 1);
        break;
    }
}

void stretch_sheet()
{
    auto* se = engine();
    if (!se || s_overlay || s_merging || s_bouncing >= 0) return;
    crosspad::StretchMode cur = crosspad::StretchMode::Off;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& t = se->song().tracks[s_selTrack];
        if (!t.inUse || t.kind != crosspad::TrackKind::Audio) return;
        cur = t.stretch;
    }
    char title[40];
    std::snprintf(title, sizeof(title), "Track %d  -  time stretch", s_selTrack + 1);
    overlay_list(title);
    static const struct { crosspad::StretchMode mode; const char* text; } kRows[] = {
        {crosspad::StretchMode::Off,       "Off  -  plays as recorded"},
        {crosspad::StretchMode::KeepPitch, "Stretch  -  keeps its pitch"},
        {crosspad::StretchMode::Repitch,   "Repitch  -  like a turntable"},
    };
    for (const auto& r : kRows) {
        char text[64];
        std::snprintf(text, sizeof(text), "%s %s", r.mode == cur ? LV_SYMBOL_OK : "    ", r.text);
        overlay_item(text, stretch_mode_cb, (void*)(intptr_t)r.mode);
    }
    overlay_item(LV_SYMBOL_CLOSE " Cancel", cancel_cb, nullptr);
}

/// What a tap on a track's header opens: what acts on the track as a whole.
/// Room for more; BOUNCE left the key row for it.
void track_menu()
{
    auto* se = engine();
    if (!se || s_overlay || s_merging || s_bouncing >= 0) return;
    bool seq = false, muted = false, solo = false;
    crosspad::StretchMode stretch = crosspad::StretchMode::Off;
    char title[32];
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& t = se->song().tracks[s_selTrack];
        if (!t.inUse) return;
        seq = t.kind == crosspad::TrackKind::Seq;
        stretch = t.stretch;
        muted = t.muted;
        solo = t.solo;
        std::snprintf(title, sizeof(title), "Track %d  -  %s", s_selTrack + 1, track_badge(t));
    }
    overlay_list(title);
    if (seq) overlay_item(LV_SYMBOL_AUDIO " Bounce to audio ...", [](lv_event_t*) { close_overlay(); bounce_sheet(); }, nullptr);
    else overlay_item(stretch == crosspad::StretchMode::KeepPitch ? LV_SYMBOL_SHUFFLE " Time stretch: STRETCH ..."
                      : stretch == crosspad::StretchMode::Repitch ? LV_SYMBOL_SHUFFLE " Time stretch: REPITCH ..."
                                                                  : LV_SYMBOL_SHUFFLE " Time stretch: OFF ...",
                      [](lv_event_t*) { close_overlay(); stretch_sheet(); }, nullptr);
    overlay_item(muted ? LV_SYMBOL_MUTE " Mute: ON" : LV_SYMBOL_VOLUME_MAX " Mute: OFF",
                 [](lv_event_t*) { close_overlay(); toggle_mute(s_selTrack); }, nullptr);
    overlay_item(solo ? LV_SYMBOL_OK " Solo: ON" : LV_SYMBOL_AUDIO " Solo: OFF",
                 [](lv_event_t*) { close_overlay(); toggle_solo(s_selTrack); }, nullptr);
    overlay_item(LV_SYMBOL_EDIT " Rename track ...", [](lv_event_t*) { close_overlay(); rename_track_sheet(); }, nullptr);
    overlay_item(LV_SYMBOL_TRASH " Delete track ...", [](lv_event_t*) { close_overlay(); delete_track_sheet(); }, nullptr);
    overlay_item(LV_SYMBOL_CLOSE " Cancel", cancel_cb, nullptr);
}

/// CLIP EDIT: the selected track, in the app that makes it.
///
/// A seq track opens the sequencer on its scene (a Pitch track Pitch Play,
/// whose SEQ key leads on to the notes); an audio track opens the Recorder on
/// the block picked -- or the one under the playhead -- to trim it and put it
/// back where it was, or, on an empty bar, to record into the track. The
/// timeline's editing is the EDIT beside SYNC (edit_cb).
void clip_edit_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se || s_overlay || s_merging) return;
    bool inUse = false, seq = false;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& t = se->song().tracks[s_selTrack];
        inUse = t.inUse;
        seq = t.kind == crosspad::TrackKind::Seq;
    }
    if (!inUse) { note("Pick a track or a block first -- EDIT at the top edits the timeline."); return; }
    if (seq) { open_seq_track(s_selTrack); return; }
    int pickedBar = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& cs = se->song().tracks[s_selTrack].clips;
        if (s_selIndex >= 0 && s_selIndex < (int)cs.size()) pickedBar = (int)cs[(size_t)s_selIndex].bar;
    }
    crosspad::ArrangeFocus& f = crosspad::arrangeFocus();
    f.clear();
    f.track = s_selTrack;
    f.bar = pickedBar >= 0 ? (uint32_t)pickedBar : playhead_bar();
    if (!crosspad_gui::mode_overlay_open_app("Recorder")) {
        f.clear();
        note("No Recorder in this firmware. EDIT adds a clip from the library.");
    }
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

/// M and S on a header, and in its menu. The timeline repaints from the
/// song's shape on the next tick.
void toggle_mute(int t)
{
    auto* se = engine();
    if (!se || t < 0 || t >= kTracks) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& tr = se->song().tracks[t];
        if (!tr.inUse) return;
        tr.muted = !tr.muted;
    }
    se->commit();
}

void toggle_solo(int t)
{
    auto* se = engine();
    if (!se || t < 0 || t >= kTracks) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& tr = se->song().tracks[t];
        if (!tr.inUse) return;
        tr.solo = !tr.solo;
    }
    se->commit();
}

void head_mute_cb(lv_event_t* e) { toggle_mute((int)(intptr_t)lv_event_get_user_data(e)); }
void head_solo_cb(lv_event_t* e) { toggle_solo((int)(intptr_t)lv_event_get_user_data(e)); }

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

// ── A Pitch seq track's sound ──────────────────────────────────────────────
//
// A seq track that plays through Pitch Play needs the pitched engine, which
// the kits' engine gives up to it (one arena between them). The seq view used
// to be the only place that borrowed it, so a Pitch track was silent in
// Arrange and played only in EDIT. This keeper holds the instrument while the
// arrangement can play it -- this screen up, the song playing, or a bounce of
// the track running -- and tells the sequencer's sink to route the song's
// notes to it (SeqTrackSound). With this screen up the pads play it too: the
// kit has given up the arena, so the kit's pads would be silent here anyway.
//
// It outlives the screen (the song does), so it is a timer of its own and
// never deleted. LVGL task only.

/// The song has a Pitch seq track that plays (in use, not silenced).
bool song_has_pitch_track()
{
    auto* se = engine();
    if (!se) return false;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const auto& song = se->song();
    for (int i = 0; i < kTracks; ++i) {
        const auto& t = song.tracks[i];
        if (t.inUse && t.kind == crosspad::TrackKind::Seq &&
            t.source == crosspad::SeqSource::Pitch && !song.silenced(i)) return true;
    }
    return false;
}

void sound_keeper_cb(lv_timer_t*)
{
    auto& ts = crosspad::seqTrackSound();
    auto* se = engine();
    auto* host = crosspad::getPlatformServices().instrumentHost;
    auto& pm = crosspad::getPadManager();

    if (s_soundBounce && (!se || se->bounceStatus().state != crosspad::BounceState::Running)) {
        s_soundBounce = false;
    }
    const bool playing = se && se->playing();
    const std::string path = crosspad::instrumentPlayState().instrumentPath;
    const bool want = host && !path.empty() && song_has_pitch_track() &&
                      (s_visible || playing || s_soundBounce);

    if (want) {
        /* Asked once each time the kits have the engine back: Pitch Play and
         * the seq view give it back when they close, whatever else played it. */
        const auto st = host->state();
        if (st == crosspad::IInstrumentHost::State::Idle && !s_soundAsked) {
            s_soundAsked = host->requestOpen(path);
        } else if (st == crosspad::IInstrumentHost::State::Ready) {
            s_soundAsked = false;
        }
        s_soundHeld = true;
    } else {
        s_soundAsked = false;
    }
    const bool ready = want && host->state() == crosspad::IInstrumentHost::State::Ready;
    ts.pitch.setReady(ready);
    ts.routePitch.store(want && (playing || s_soundBounce), std::memory_order_release);

    /* The fingers, here: pushed only with the MODE map down -- the map keeps
     * its own entry on the stack while it is up -- and popped only while it
     * is ours on top. */
    const bool padsWanted = want && s_visible && !crosspad_gui::mode_overlay_is_open();
    if (padsWanted && !s_soundPads) {
        pm.pushPadLogic(kSoundLogic);
        s_soundPads = true;
    } else if (!padsWanted && s_soundPads && pm.getActivePadLogic() == kSoundLogic) {
        pm.popPadLogic();
        s_soundPads = false;
    }

    if (!want && s_soundHeld) {
        s_soundHeld = false;
        /* Not out from under the screen that plays it now: Pitch Play and the
         * seq view borrow the same engine and give it back themselves. */
        crosspad_gui::ILvglApp* fg = crosspad_gui::AppOrchestrator::getInstance().getForegroundApp();
        const char* name = fg ? fg->getName() : nullptr;
        const bool theirs = name && (std::strcmp(name, "PitchPlay") == 0 || std::strcmp(name, "Sequencer") == 0);
        if (!theirs && host && host->state() != crosspad::IInstrumentHost::State::Idle) host->release();
    }
}

void sound_keeper_start()
{
    if (s_soundTimer) return;
    /* Non-owning: the logic is SeqTrackSound's, which outlives every screen. */
    crosspad::getPadManager().registerPadLogic(
        kSoundLogic, std::shared_ptr<crosspad::IPadLogicHandler>(&crosspad::seqTrackSound().pitch,
                                                                 [](crosspad::IPadLogicHandler*) {}));
    s_soundTimer = lv_timer_create(sound_keeper_cb, 100, nullptr);
}

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
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Absolute positions, as the export draws them; ARR_Y shifts a screen y
     * into the root. */
    auto place = [](lv_obj_t* o, int32_t x, int32_t y) { if (o) lv_obj_set_pos(o, x, y); return o; };

    /* ── context row ──────────────────────────────────────────────────────
     * No back button and no PLAY: back is the power button's click, and the
     * transport is the bar above. ZOOM takes the place the two zoom keys had
     * on the tool row, which is where the export puts it. */
    s_projBtn = make_button(s_root, 72, "", project_cb);
    place(s_projBtn, 6, ARR_Y(35));
    s_projLabel = lv_obj_get_child(s_projBtn, 0);
    lv_label_set_long_mode(s_projLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_projLabel, 66);
    lv_obj_set_style_text_align(s_projLabel, LV_TEXT_ALIGN_CENTER, 0);

    /* The export's dim "18 bars". It carries this screen's status line too:
     * what the encoder does in the loop region, that a clip is missing from
     * clips/, that the song is empty and where to start. Scrolled, because
     * those sentences are longer than the slot and the export leaves no row
     * for them. */
    s_posLabel = lv_label_create(s_root);
    lv_obj_set_width(s_posLabel, 46);
    place(s_posLabel, 82, ARR_Y(40));
    lv_obj_set_style_text_font(s_posLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_posLabel, kDim, 0);
    lv_obj_set_style_text_align(s_posLabel, LV_TEXT_ALIGN_RIGHT, 0);
    /* DOT, not SCROLL: a scrolling label is only clipped to a width it
     * actually has, and this one spilled its sentence across the whole top of
     * the screen. The slot carries the export's own words -- "18 bars",
     * "bar 2 / 4" -- which fit; anything longer is said in a toast. */
    lv_label_set_long_mode(s_posLabel, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_posLabel, "");

    /* EDIT here is the timeline's: the loop region, copy / paste, merging,
     * the song's length, a clip from the library (edit_cb). The key at the
     * foot is the clip's (clip_edit_cb). */
    s_editBtn = make_button(s_root, 40, "EDIT", edit_cb);
    place(s_editBtn, 132, ARR_Y(35));
    s_syncBtn = make_button(s_root, 44, "SYNC", sync_cb);
    place(s_syncBtn, 176, ARR_Y(35));
    s_loopBtn = make_button(s_root, 44, "LOOP", loop_cb);
    place(s_loopBtn, 224, ARR_Y(35));
    s_zoomBtn = make_button(s_root, 42, "ZOOM", zoom_cycle_cb);
    place(s_zoomBtn, 272, ARR_Y(35));

    /* ── lanes: the track buttons fixed, the timeline scrolling beside them ──
     * The export's LANE_X=48 and a lane block from screen y=62 to 200. */
    constexpr int32_t kBodyH = kRulerH + 4 + kTracks * (kLaneH + kLaneGap);
    lv_obj_t* body = lv_obj_create(s_root);
    lv_obj_remove_style_all(body);
    place(body, 4, ARR_Y(62));
    lv_obj_set_size(body, 312, kBodyH);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(body, 4, 0);   // 4 + 80 + 4: the lanes from x 88 to 302, the meters on 306

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
        /* The tracks' levels, from the tracks. The export's tool row has five
         * keys and MIX is not one of them, so it moved to the one gesture
         * these buttons were not using -- and the association is exact. */
        lv_obj_add_event_cb(h, mix_cb, LV_EVENT_LONG_PRESSED, nullptr);
        /* "01" in the type's colour over the name; S over M on the right
         * edge, touched there -- the encoder reaches both in the header's
         * menu, so the group keeps one stop a track. */
        lv_obj_t* l = lv_label_create(h);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, kAudioT, 0);
        set_text_fmt(l, "%02d", t + 1);
        lv_obj_set_pos(l, 5, 1);
        lv_obj_t* name = lv_label_create(h);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(name, kText, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(name, 54);
        lv_label_set_text(name, "");
        lv_obj_set_pos(name, 5, 13);
        auto toggle = [&](int32_t y, const char* text, lv_event_cb_t cb) {
            lv_obj_t* b = lv_obj_create(h);
            lv_obj_remove_style_all(b);
            lv_group_remove_obj(b);
            lv_obj_set_size(b, 14, 12);
            lv_obj_set_pos(b, 62, y);
            lv_obj_set_style_radius(b, 3, 0);
            lv_obj_set_style_bg_color(b, kWell, 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(b, 4);
            /* A child's click does not bubble: it never opens the menu. */
            lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void*)(intptr_t)t);
            lv_obj_t* bl = lv_label_create(b);
            lv_obj_set_style_text_font(bl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(bl, kDim, 0);
            lv_label_set_text(bl, text);
            lv_obj_center(bl);
            return b;
        };
        s_headSolo[t] = toggle(3, "S", head_solo_cb);
        s_headMute[t] = toggle(17, "M", head_mute_cb);
        s_heads[t] = h;
        s_headNum[t] = l;
        s_headName[t] = name;
    }

    /* Over the lanes, through which a touch goes: it says, it does not ask. */
    s_procBox = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_procBox);
    lv_obj_set_size(s_procBox, 220, 40);
    place(s_procBox, 50, ARR_Y(110));
    lv_obj_add_flag(s_procBox, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_procBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_procBox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(s_procBox, 6, 0);
    lv_obj_set_style_bg_color(s_procBox, kBg, 0);
    lv_obj_set_style_bg_opa(s_procBox, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_procBox, kAccent, 0);
    lv_obj_set_style_border_width(s_procBox, 2, 0);
    s_procLabel = lv_label_create(s_procBox);
    lv_obj_set_style_text_font(s_procLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_procLabel, kAccent, 0);
    lv_obj_set_style_text_align(s_procLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_procLabel, "");
    lv_obj_center(s_procLabel);

    /* The row that makes a track: full width, under the last one in use,
     * outlined rather than filled -- it is not a track, it is the offer of
     * one. Placed on the root, not inside the lanes, so it does not scroll
     * with the timeline and the lane block stays sized for four. */
    s_addRow = lv_button_create(s_root);
    lv_obj_remove_style_all(s_addRow);
    lv_obj_set_size(s_addRow, 312, kLaneH);
    place(s_addRow, 4, ARR_Y(62) + kRulerH + 4);
    lv_obj_set_style_radius(s_addRow, 4, 0);
    lv_obj_set_style_bg_color(s_addRow, kLane, 0);
    lv_obj_set_style_bg_opa(s_addRow, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_addRow, lv_color_lighten(kLane, 40), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_addRow, kAccent, 0);
    lv_obj_set_style_border_width(s_addRow, 1, 0);
    lv_obj_set_style_border_opa(s_addRow, LV_OPA_50, 0);
    lv_obj_remove_flag(s_addRow, LV_OBJ_FLAG_SCROLLABLE);
    focus_frame(s_addRow);
    lv_obj_add_event_cb(s_addRow, add_track_cb, LV_EVENT_CLICKED, nullptr);
    {
        lv_obj_t* l = lv_label_create(s_addRow);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, kAccent, 0);
        lv_label_set_text(l, LV_SYMBOL_PLUS "  ADD TRACK");
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        s_addInfo = lv_label_create(s_addRow);
        lv_obj_set_style_text_font(s_addInfo, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(s_addInfo, kDim, 0);
        lv_label_set_text(s_addInfo, "");
        lv_obj_align(s_addInfo, LV_ALIGN_RIGHT_MID, -10, 0);
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

    /* The tracks' stereo meters, beside the lanes: a well a track, two
     * columns of ten segments bottom up -- green, two amber, one red -- at a
     * fifth until lit (show_meters). */
    lv_obj_t* meters = lv_obj_create(body);
    lv_obj_remove_style_all(meters);
    lv_obj_set_size(meters, kMeterW, LV_PCT(100));
    lv_obj_remove_flag(meters, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(meters, LV_OBJ_FLAG_CLICKABLE);
    for (int t = 0; t < kTracks; ++t) {
        lv_obj_t* well = lv_obj_create(meters);
        lv_obj_remove_style_all(well);
        lv_obj_set_pos(well, 0, kRulerH + 4 + t * (kLaneH + kLaneGap));
        lv_obj_set_size(well, kMeterW, kLaneH);
        lv_obj_set_style_radius(well, 2, 0);
        lv_obj_set_style_bg_color(well, kWell, 0);
        lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);
        lv_obj_remove_flag(well, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(well, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(well, LV_OBJ_FLAG_HIDDEN);     // shown for the tracks in use
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < kMeterSegs; ++i) {
                lv_obj_t* seg = lv_obj_create(well);
                lv_obj_remove_style_all(seg);
                lv_obj_set_pos(seg, 2 + c * 4, kLaneH - 2 - i * 3);
                lv_obj_set_size(seg, 2, 2);
                lv_obj_set_style_bg_color(seg, i >= 9 ? kMute : i >= 7 ? kKitT : kWave, 0);
                lv_obj_set_style_bg_opa(seg, LV_OPA_20, 0);
                lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);
                s_meterSeg[t][c][i] = seg;
            }
            s_meterShown[t][c] = 0.0f;
            s_meterLit[t][c] = 0;
        }
        s_meterWell[t] = well;
    }

    /* ── tools on the selected block ──────────────────────────────────────
     * The export's five keys at 58 px on a 62 px pitch, filling the width.
     * Z- and Z+ became ZOOM in the context row and MIX a long press on a
     * track button; everything else is where it was. MOVE stays an editable
     * button -- the encoder nudges the block with it, which no plain key
     * could do. */
    constexpr int32_t kToolW = 74, kToolPitch = 78, kToolH = 28;
    s_tools[0] = lv_CreateEditableButton(s_root);
    lv_obj_set_size(s_tools[0], kToolW, kToolH);
    place(s_tools[0], 6, ARR_Y(206));
    lv_obj_set_style_pad_all(s_tools[0], 0, 0);
    lv_obj_set_style_radius(s_tools[0], 4, 0);
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
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_label_set_text(l, "MOVE");
        lv_obj_center(l);
    }
    /* BOUNCE is the track's, in its header's menu (track_menu). */
    struct { const char* text; lv_event_cb_t cb; } kTools[3] = {
        { "DUP",    dup_cb  },
        { "DEL",    del_cb  },
        { "CLIP EDIT", clip_edit_cb },
    };
    for (int i = 0; i < 3; ++i) {
        s_tools[i + 1] = tool_button(s_root, kToolW, kTools[i].text, kTools[i].cb);
        lv_obj_set_height(s_tools[i + 1], kToolH);
        place(s_tools[i + 1], 6 + (i + 1) * kToolPitch, ARR_Y(206));
    }
    /* Two words: the size MOVE is set in. */
    lv_obj_set_style_text_font(lv_obj_get_child(s_tools[3], 0), &lv_font_montserrat_10, 0);
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
    lv_obj_add_event_cb(s_regionHandle, region_cb, LV_EVENT_FOCUSED, nullptr);
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
    if (!engine()) note("No song engine on this platform.");

#ifdef ARR_USE_ENCODER
    if (s_encGroup && s_projBtn) lv_group_focus_obj(s_projBtn);
#endif

    s_timer = lv_timer_create(tick_cb, kTickMs, nullptr);
    sound_keeper_start();

    if (app) {
        app->setOnBack(back_handler);
        app->setOnShow([](lv_obj_t*) {
            /* PLAY in the top bar means this song while this screen is up.
             * Without the claim it still meant the pattern sequencer -- the
             * board claims that at boot -- so opening an arrangement and
             * pressing PLAY started an empty scene and the Arranger made no
             * sound at all. The claim outlives the screen, as every claim
             * does: the last thing opened is what PLAY means. */
            /* A song already playing -- from a track's seq view -- is this
             * song: handed over, not stopped. */
            {
                auto* se = engine();
                crosspad::transportClaim(&crosspad::songTransport(), !(se && se->playing()));
            }
            s_visible = true;
            sound_keeper_cb(nullptr);   // the instrument starts loading now, not in 100 ms
            update_info();
            if (s_timer) lv_timer_resume(s_timer);
#ifdef ARR_USE_ENCODER
            if (indev_encoderTop && s_encGroup) lv_indev_set_group(indev_encoderTop, s_encGroup);
#endif
            /* Sent here by the seq view's BOUNCE: the question, on its track. */
            crosspad::ArrangeFocus& f = crosspad::arrangeFocus();
            if (f.held() && f.bounce) {
                const int track = f.track;
                f.clear();
                select(track, -1);
                bounce_sheet();
            }
        });
        app->setOnHide([](lv_obj_t*) {
            /* The bar goes back to the logo for whatever comes next. */
            s_visible = false;
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
    /* The pads go back before the next screen takes them; the instrument stays
     * while the song plays it (sound_keeper_cb). */
    s_visible = false;
    if (s_soundPads && crosspad::getPadManager().getActivePadLogic() == kSoundLogic) {
        crosspad::getPadManager().popPadLogic();
    }
    s_soundPads = false;
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
    s_addRow = s_addInfo = nullptr;
    s_seqBlock = nullptr;
    s_seqBlockTrack = -1;
    /* A bounce started here keeps running -- it is the engine's -- but
     * nothing is left to flip its track's kind, so it is not left pending
     * either: the next visit reads the song as the engine left it. */
    s_bouncing = -1;
    for (auto& b : s_blocks) b = Block{};
    for (auto& h : s_heads) h = nullptr;
    for (auto& l : s_headNum) l = nullptr;
    for (auto& l : s_headName) l = nullptr;
    for (auto& b : s_headMute) b = nullptr;
    for (auto& b : s_headSolo) b = nullptr;
    for (auto& w : s_meterWell) w = nullptr;
    for (auto& tr : s_meterSeg) for (auto& side : tr) for (auto& s : side) s = nullptr;
    s_procBox = s_procLabel = nullptr;
    for (auto& t : s_tools) t = nullptr;
    s_blockCount = 0;
    /* the status line went with the screen */
    s_root = s_timeline = s_playhead = s_projLabel = s_posLabel = nullptr;
    s_zoomBtn = s_loopBtn = s_syncBtn = s_editBtn = s_projBtn = nullptr;

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

// No pad logic of its own: the pads play the kit over the song, or a Pitch
// track's instrument while one is in it (sound_keeper_cb).
REGISTER_APP(Arrange, nullptr, "arrange.png",
             Arrange_create, Arrange_destroy,
             nullptr, nullptr, nullptr, 0)
