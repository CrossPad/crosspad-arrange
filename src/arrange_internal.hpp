// Arrange -- the state and helpers the screen's source files share (ArrangeApp.cpp
// holds the state, the others one area each).
#pragma once

#include "crosspad-arrange/ArrangeApp.hpp"

// `class App` lives in a different header per platform, but both derive from
// crosspad_gui::AppBase<App>, so the lifecycle calls used here are identical.
#if __has_include("pc_stubs/PcApp.hpp")
  #include "pc_stubs/PcApp.hpp"
#else
  #include "crosspad-gui/app/App.hpp"
#endif

#include "crosspad-gui/components/transport_bar.h"
#include "crosspad-gui/components/ui_toast.h"
#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad/audio/IInputMonitor.hpp"
#include "crosspad/platform/PlatformServices.hpp"
/* A seq track names a sequencer scene: this screen reads its bars and notes
 * to draw the lane, and gives a new track a scene of its own. */
#include "crosspad/sequencer/SequencerController.hpp"
#include "crosspad/instrument/IInstrumentHost.hpp"
#include "crosspad/instrument/InstrumentPlayState.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/kit/IKitPicker.hpp"
#include "crosspad/sampler/ISecondKit.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/sampletools/ISampleTools.hpp"
#include "crosspad/song/ArrangeAudioTake.hpp"
#include "crosspad/song/ArrangeFocus.hpp"
#include "crosspad/song/ElementNav.hpp"
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

namespace arrange_ui {

inline constexpr int      kTracks   = crosspad::AudioSong::kTracks;
/* A screen y as a y inside the root: the root fills the app container, which
 * begins under the transport bar. The whole screen is laid out in the
 * export's screen coordinates -- drawn under a 30 px bar -- and this is the
 * only place that shift happens. Under the 40 px bar (crosspad-gui 2.4) the
 * root is 200 px, not 210: the export's 35 (the key row) lands 2 px under
 * the bar, the lanes give 2 px each (kLaneH) and the key row moves up 2
 * (ARR_Y(204)); subtracting the bar's own height put the key row 5 px under
 * it. */
inline constexpr int32_t  kExportTop = 33;
#define ARR_Y(v) ((v) - kExportTop)
/* The context row's keys that move when an element is open: the breadcrumbs
 * take the place of the project button, the position, EDIT and SYNC, and the
 * keys left close up behind them; EDIT goes to the key at the row's end. */
struct CtxKeys { int32_t follow, loop, zoom; };
inline constexpr CtxKeys kCtxSong    = {112, 225, 270};
inline constexpr CtxKeys kCtxElement = {152, 180, 225};
inline constexpr int32_t kCtxMoreX   = 270;       ///< where ZOOM stood
inline constexpr int32_t kCrumbsW    = kCtxElement.follow - 4 - 6;   ///< from the row's left edge to FOLLOW

/* Zoom: pixels per bar -- a whole song on one screen, phrases, single beats
 * (UX 2000 section 7.3: three levels). */
inline constexpr int32_t  kZoomPx[] = {3, 12, 48};
inline constexpr int      kZoomLevels = sizeof(kZoomPx) / sizeof(kZoomPx[0]);
inline constexpr uint32_t kMaxLength = 999;
/* The export's lane block is LANE_Y0=32, LANE_P=35, LANE_H=31 and a 138 px
 * playhead -- drawn for a screen with no ruler. This one keeps the ruler (it
 * is where the bar numbers and the loop region band live, and LOOP cannot be
 * edited without it), so the ruler's 10 px come off the pitch and the height:
 * 34 and 30 against the export's 35 and 31. Everything else is the export's
 * number. */
inline constexpr int32_t  kRulerH   = 10;
/* 26, not 30: the status line came back into this screen's own layout when
 * the transport bar took the status bar's row, and the four lanes paid the
 * 16 px for it rather than the tools, which have to stay a finger tall. */
inline constexpr int32_t  kLaneH    = 28;     // the export's 31, less the ruler's pixel and the taller bar's two
/* 2, not 4: at 4 the fourth lane ended on 208 and the tool row begins on
 * 206 -- measured on the board, two pixels of overlap. The ruler the export
 * does not have already pushes this block 14 px down; it must not also push
 * it into the keys. */
inline constexpr int32_t  kLaneGap  = 2;
/* 80: the export's track head, variant A -- the number in its type's colour,
 * S, M and a seq track's ARM on its line, the name under them. The lanes start on its LANE_X 88
 * and end before the meters, on 302. */
inline constexpr int32_t  kHeadW    = 80;
inline constexpr int32_t  kMeterW   = 10;     ///< a track's stereo meter, beside the lanes
inline constexpr int      kMeterSegs = 10;
inline constexpr uint32_t kTickMs   = 50;
inline constexpr int      kMaxBlocks = 64;
/// Drawn blocks per track: a long track must not use up the ones the others need.
inline constexpr int      kMaxBlocksPerTrack = kMaxBlocks / kTracks;

inline const lv_color_t kBg     = lv_color_hex(0x16181B);
inline const lv_color_t kPanel  = lv_color_hex(0x20242A);
inline const lv_color_t kLane   = lv_color_hex(0x1B1E23);
inline const lv_color_t kText   = lv_color_hex(0xE8EAED);
inline const lv_color_t kDim    = lv_color_hex(0x7B828B);
inline const lv_color_t kInk    = lv_color_hex(0x16181B);
inline const lv_color_t kAccent = lv_color_hex(0xC6FF47);
inline const lv_color_t kMute   = lv_color_hex(0xFF5C5C);
/* A block whose audio is not on the card: grey, and it says so on itself. Not
 * red -- nothing is broken, the song remembers what it played and the file did
 * not travel with it -- and not the clip's own colour either, because a block
 * that looks like the others is read as one that plays. */
inline const lv_color_t kMissing = lv_color_hex(0x6E757E);
inline const lv_color_t kHead   = lv_color_hex(0xFFFFFF);
inline const lv_color_t kIce    = lv_color_hex(0x7FD4FF);     // a frozen track's name
inline const lv_color_t kRec    = lv_color_hex(0xFF5C5C);     // the playhead while a take runs: REC's red
inline const lv_color_t kTrack  = lv_color_hex(0x3A3F47);     // a fader's travel
inline const lv_color_t kWell   = lv_color_hex(0x1B1E22);     // S / M off, a meter's well
inline const lv_color_t kWave   = lv_color_hex(0x9CD639);     // a meter's green

/* ── the tracks a player makes (UX 2000 section 6) ────────────────────────
 * Arrange holds four tracks, up to three of them seqs: two kit tracks, a kit
 * each, and a Pitch track (AudioSong::freeSeqRoute()). A track comes from
 * the "+ ADD TRACK" row -- AUDIO is recorded into here, KITS asks for Perf,
 * Drums, Slice or Pitch, also recorded into here -- and any track
 * can be bounced to audio at any time, which is also how a seq slot is freed
 * for the next idea. The three colours are the ones those three screens
 * already use, so a badge here names the app that edits it. */
inline const lv_color_t kAudioT = lv_color_hex(0x4EA6D3);    // AUD   -- the Recorder's blue

inline const lv_color_t kKitT   = lv_color_hex(0xFFC857);    // KIT   -- the Sampler's amber

inline const lv_color_t kPitchT = lv_color_hex(0xA77BF0);    // PITCH -- Pitch Play's purple

/// The inputs an audio track can record, in the order its input sheet lists
/// them, and what the header and the sheet call each.
struct TrackInput { crosspad::RecInput in; const char* name; };

inline constexpr TrackInput kTrackInputs[] = {
    {crosspad::RecInput::Mic, "MIC"},          {crosspad::RecInput::Line, "LINE"},
    {crosspad::RecInput::MicLine, "MIC+LINE"}, {crosspad::RecInput::Usb, "USB"},
    {crosspad::RecInput::Pads, "PADS"},
};

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
    bool        element = false;   ///< plays an element's bake, not a clip: no waveform, no peaks
    /* A seq block: its own notes, or -- null -- the track's scene, from its
     * bar @c from of them. */
    bool        seq = false;
    crosspad::SeqPatternPtr notes;
    uint8_t     scene = 0;
    uint16_t    from = 0;
};

/* A Pitch seq track's sound (sound_keeper_cb, by Arrange_create). */
inline constexpr const char* kSoundLogic = "ArrangePitch";

/* The second kit's pads, while its track is armed (sound_keeper_cb). */
inline constexpr const char* kKit2Logic = "ArrangeKit2";

/* MOVE, DUP, DEL (the selected block's, reached by clicking it), then EDIT. */
inline constexpr int kBlockTools = 3;

/// A block's waveform starts under its name.
inline constexpr int32_t kWaveTop = 13;

/* Drawn as bars, 2 px and a 1 px gap, on the block's own grid so scrolling
 * does not shimmer: every fill is a draw task, and LVGL 9.3 walks its whole
 * list to add one, so a column each made a redraw cost the square of it. */
inline constexpr int32_t kWaveBarW = 2;

inline constexpr int32_t kWaveBarPitch = 3;

/* The card is read on the platform's side; an overlay opens at once with a
 * line saying so and is filled when the list comes back. */
enum class Pending : uint8_t { None, Projects, Clips, Elements };

/* A rename, tempo change, delete or export the platform is doing on the card. */
enum class EditKind : uint8_t { Tempo, Rename, Delete, Export };

/* MOVE's axis while it is turned: first click along the bars, second across
 * the tracks, third done. Arrows beside the block show which, and a finger
 * can tap them. */
enum class MoveAxis : uint8_t { None, Bars, Tracks };

/* The loop region moved with the encoder: first its start (L), then its end
 * (R). A hidden editable handle in a group of its own takes the turns. */
enum class RegionEdit : uint8_t { None, Start, End };

// State, defined in ArrangeApp.cpp.
extern lv_obj_t* s_root;
extern lv_obj_t* s_timeline;
extern lv_obj_t* s_playhead;
extern lv_obj_t* s_projLabel;
extern lv_obj_t* s_posLabel;
extern lv_obj_t* s_loopBtn;
extern lv_obj_t* s_syncBtn;
extern lv_obj_t* s_editBtn;
extern lv_obj_t* s_followBtn;
extern bool      s_follow;
extern lv_obj_t* s_heads[kTracks];
extern lv_obj_t* s_headNum[kTracks];
extern lv_obj_t* s_headName[kTracks];
extern lv_obj_t* s_headMute[kTracks];
extern lv_obj_t* s_headSolo[kTracks];
extern lv_obj_t* s_headArm[kTracks];
extern lv_obj_t* s_meterWell[kTracks];
extern lv_obj_t* s_meterSeg[kTracks][2][kMeterSegs];
extern float     s_meterShown[kTracks][2];
extern uint8_t   s_meterLit[kTracks][2];
extern bool      s_trackQuiet[kTracks];
extern lv_obj_t* s_trackNameArea;
extern lv_obj_t* s_procBox;
extern lv_obj_t* s_procLabel;
extern lv_obj_t* s_addRow;
extern lv_obj_t* s_addInfo;
extern lv_obj_t* s_seqBlock;
extern int       s_seqBlockTrack;
extern uint8_t   s_seqLaneScene;
extern uint32_t  s_seqTicksBar;
extern uint32_t  s_drawnBars;
extern int       s_bouncing;
extern bool        s_kit2Pads;
extern lv_timer_t* s_soundTimer;
extern bool        s_visible;
extern bool        s_soundHeld;
extern bool        s_soundAsked;
extern bool        s_soundPads;
extern bool        s_soundBounce;
extern lv_obj_t* s_tools[4];
extern lv_obj_t* s_lenLabel;
extern lv_obj_t* s_zoomBtn;
extern lv_obj_t* s_projBtn;
extern lv_obj_t* s_overlay;
extern Block     s_blocks[kMaxBlocks];
extern int       s_blockCount;
extern int      s_zoom;
extern int32_t  s_pxPerBar;
extern int      s_selTrack;
extern int      s_selIndex;
extern uint32_t s_shape;
extern uint32_t s_framesPerBar;
extern uint32_t s_peakHop;
extern uint32_t s_peaksGen;
extern Pending   s_pending;
extern uint32_t  s_pendingGen;
extern lv_obj_t* s_loading;
extern bool      s_opening;
extern bool      s_editing;
extern EditKind  s_editKind;
extern lv_obj_t* s_tempoItem;
extern lv_obj_t* s_songList;
extern lv_obj_t* s_mixDb[kTracks];
extern lv_obj_t* s_mixMute[kTracks];
extern lv_obj_t* s_mixSolo[kTracks];
extern lv_obj_t* s_mixPan[kTracks];
extern lv_obj_t* s_mixPanSlider[kTracks];
extern lv_obj_t* s_mixMeter[kTracks][2];
extern float     s_mixShown[kTracks][2];
#ifdef ARR_USE_ENCODER
extern lv_group_t* s_encGroup;
extern lv_group_t* s_overlayGroup;
extern lv_group_t* s_toolGroup;
extern lv_group_t* s_regionGroup;
#endif
extern bool s_toolMode;
extern MoveAxis  s_moveAxis;
extern lv_obj_t* s_moveArrows[4];
extern RegionEdit s_regionEdit;
extern lv_obj_t*  s_regionHandle;
extern bool       s_regionArmed;
extern std::vector<crosspad::RangeClip> s_clipboard;
extern uint32_t  s_clipboardBars;
extern bool      s_merging;
extern uint8_t   s_mergeMask;
extern lv_obj_t* s_mergeToggles[kTracks];
extern lv_obj_t* s_nameArea;
extern int s_afterPick;

// Functions called from more than one file.
void set_text_fmt(lv_obj_t* label, const char* fmt, ...);
lv_color_t track_color(const crosspad::SongTrack& t);
const char* track_badge(const crosspad::SongTrack& t);
const char* input_name(crosspad::RecInput in);
bool track_armed(const crosspad::AudioSong& song, int t);
std::string kit_name(const std::string& kitPath);
std::string track_kit(const crosspad::AudioSong& song, int t);
crosspad::ISongEngine* engine();
uint32_t song_shape(const crosspad::AudioSong& s);
std::string short_name(const std::string& file);
void take_peaks(crosspad::ISongEngine* se);
void note(const char* text);
void note_fmt(const char* fmt, ...);
void update_info();
void select(int track, int index);
void rebuild_timeline();
void set_bg(lv_obj_t* o, lv_color_t c);
void set_ink(lv_obj_t* o, lv_color_t c);
void set_text(lv_obj_t* l, const char* text);
void sync_transport();
void tick_cb(lv_timer_t*);
void place_move_arrows();
void paint_tools();
void enter_tools();
int move_target(const crosspad::AudioSong& song, int from, int dir);
void move_cb(lv_event_t* e);
void dup_cb(lv_event_t*);
void del_cb(lv_event_t*);
bool back_handler(lv_obj_t*);
void head_hold_cb(lv_event_t* e);
void head_cb(lv_event_t* e);
void loop_cb(lv_event_t*);
void follow_cb(lv_event_t*);
void sync_cb(lv_event_t*);
void zoom_cycle_cb(lv_event_t*);
void close_overlay();
lv_obj_t* overlay_open();
lv_obj_t* overlay_list(const char* title);
lv_obj_t* overlay_item(const char* text, lv_event_cb_t cb, void* ud, bool accent = false);
lv_obj_t* overlay_keys(const char* title);
lv_obj_t* overlay_key(const char* text, lv_event_cb_t cb, void* ud = nullptr);
void cancel_cb(lv_event_t*);
void show_tempo();
void rename_failed();
void export_done(bool ok);
void fill_projects();
void project_cb(lv_event_t*);
void add_cb(lv_event_t*);
void fill_clips();
void length_cb(lv_event_t*);
void region_cb(lv_event_t* e);
uint32_t playhead_bar();
void edit_cb(lv_event_t*);
void seq_track_made(int made);
void bounce_sheet();
void delete_track_sheet();
void add_track_cb(lv_event_t*);
void input_sheet(int track);
void track_menu();
void clip_edit_cb(lv_event_t*);
lv_obj_t* make_button(lv_obj_t* parent, int32_t w, const char* text, lv_event_cb_t cb, void* ud = nullptr);
lv_obj_t* make_row(lv_obj_t* parent, int32_t h);
void toggle_mute(int t);
void toggle_solo(int t);
void arm_track(int t);
void mix_forget();
void mix_meters(crosspad::ISongEngine* se);
void style_toggle(lv_obj_t* b, bool on, lv_color_t col);
void mix_cb(lv_event_t*);
bool freeze_progress(crosspad::ISongEngine* se, char* buf, size_t cap);
void sound_keeper_cb(lv_timer_t*);
void sound_keeper_start();
void request_list(Pending what, const char* title);

// Elements (arrange_elements.cpp): arrangements inside the song, one level at a time.
void elements_build_crumbs(lv_obj_t* parent);
void elements_refresh_crumbs();
#ifdef ARR_USE_ENCODER
void elements_group_add(lv_group_t* g);
#endif
void elements_tick(crosspad::ISongEngine* se);
bool elements_back();
bool elements_clip_edit();
void elements_style_block(lv_obj_t* block, lv_obj_t* label, const crosspad::SongClip& c);
void elements_destroy();
void element_new_cb(lv_event_t*);
void element_library_cb(lv_event_t*);
void fill_elements();
bool element_progress(char* buf, size_t cap);
const char* element_title(const std::string& ref);

} // namespace arrange_ui
