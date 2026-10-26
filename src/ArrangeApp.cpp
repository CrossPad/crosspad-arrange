/**
 * @file ArrangeApp.cpp
 * @brief Arrange -- where a song starts, and the tracks it is made of.
 *
 * The main place of work on the CrossPad (UX 2000 section 6). A song holds
 * up to kTracks tracks, up to two of them seqs: two kit tracks, each playing a kit of
 * its own, or one Pitch track. A track is made here and filled here: AUDIO
 * leaves an empty audio track armed, so REC records its input into it over
 * the song (ArrangeAudioTake; the input -- the mics, the line-in, both, USB
 * or the pads -- is a long press on its header), KITS asks whether it plays
 * through a kit (picked in the kit loader) or Pitch Play and leaves it empty
 * and armed, on a scene of its own: REC records the pads into it, CLIP EDIT
 * opens its seq view (a Pitch track with no instrument picked yet opens
 * Pitch Play, where one is) -- and any track can be bounced to
 * audio at any time, which is also how a seq slot is freed for the next idea.
 *
 * Audio tracks are laid out like a linear DAW: lanes, a bar ruler, every clip
 * a block as long as it plays, in a colour of its own, and a playhead while
 * the song runs -- and stopped, where PLAY starts, which the top bar's
 * position moves a bar a detent. REC and DUB on the top bar record the pads
 * onto the armed seq track from there -- ARM, the dot on a track's header,
 * which on a seq track is also whose kit the pads play here; on an audio
 * track REC records its input instead, and there is no DUB (ISongEngine::recordSeq,
 * AudioSong::armed; SecondKitPadLogic for the second kit): a REC becomes a
 * block of its own notes as many bars long as it ran -- a STOP puts the
 * playhead back on bar one, the next PLAY's downbeat -- and a DUB adds
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
 *   lanes  a header per track -- its number and what it holds, AUD / the
 *          kit's name / PITCH (an audio track without a name says its
 *          input, AUD MIC), in the colour of the app that makes it, ARM (tap:
 *          pick the track and open its menu -- ARM, an audio track's INPUT,
 *          BOUNCE a seq to audio, its KIT, DELETE; hold: an audio track's
 *          input and MIX, MIX on the others) -- the timeline beside
 *          them, and under the last of them
 *          "+ ADD TRACK" while there is room
 *   keys   MOVE, DUP and DEL act on the selected block; EDIT is the song's
 *          own editing (below). CLIP EDIT opens the selected track where it
 *          is made -- a seq block in the seq view, the block itself
 *   MIX    the tracks' levels: a strip each side by side, as on a desk --
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

#include "arrange_internal.hpp"

namespace arrange_ui {

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
void set_text_fmt(lv_obj_t* label, const char* fmt, ...)
{
    if (!label || !fmt) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(label, buf);
}

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

const char* input_name(crosspad::RecInput in)
{
    for (const auto& i : kTrackInputs)
        if (i.in == in) return i.name;
    return "MIC";
}

/// The track REC takes onto: an armed audio track, or else the seq track the
/// seq takes would go to.
bool track_armed(const crosspad::AudioSong& song, int t)
{
    const int audio = song.armedAudioTrack();
    return audio >= 0 ? audio == t : song.armedSeqTrack() == t;
}

/// The kit list's name for @p kitPath, or "" when the card has no such kit.
std::string kit_name(const std::string& kitPath)
{
    crosspad::IKitManager* km = crosspad::getKitManager();
    if (!km || kitPath.empty()) return {};
    for (int i = 0; i < km->getKitCount(); ++i) {
        const crosspad::KitInfo* k = km->getKit(i);
        if (k && k->path == kitPath) return k->name;
    }
    return {};
}

/// The kit seq track @p t plays: its own on the second kit's route, else the
/// song's. Empty for any other track.
std::string track_kit(const crosspad::AudioSong& song, int t)
{
    const int route = song.seqRoute(t);
    if (route < 0 || song.tracks[t].source != crosspad::SeqSource::Kit) return {};
    return route == 1 ? song.tracks[t].kit : song.kit;
}

lv_obj_t* s_root = nullptr;
lv_obj_t* s_timeline = nullptr;
lv_obj_t* s_playhead = nullptr;
lv_obj_t* s_projLabel = nullptr;
lv_obj_t* s_mixBtn = nullptr;
lv_obj_t* s_loopBtn = nullptr;
lv_obj_t* s_syncBtn = nullptr;
lv_obj_t* s_editBtn = nullptr;   ///< the timeline's EDIT, beside SYNC
lv_obj_t* s_followBtn = nullptr; ///< FOLLOW: the timeline keeps the playhead in view while playing
/* Kept across openings of the screen, as the player left it: on, the lanes
 * scroll with the song; off, they stay put for editing while it plays. */
bool      s_follow = true;
lv_obj_t* s_heads[kTracks] = {};
lv_obj_t* s_headNum[kTracks] = {};
lv_obj_t* s_headName[kTracks] = {};
lv_obj_t* s_headIce[kTracks] = {};

/* A frozen track's mark beside its name: a snowflake, 13 px of alpha drawn in
 * the image's recolour (kIce), so no font has to carry the glyph. */
static const uint8_t kSnowflakeA8[13 * 13] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x4c, 0xc3, 0xff, 0xc3, 0x4c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x6c, 0x3c, 0x00, 0x8f, 0xff, 0x8f, 0x00, 0x3c, 0x6c, 0x00, 0x00,
    0x20, 0xe7, 0xc7, 0x93, 0x00, 0x00, 0xff, 0x00, 0x00, 0x93, 0xc7, 0xe7, 0x20,
    0x00, 0x80, 0xff, 0xe3, 0x3c, 0x00, 0xff, 0x00, 0x3c, 0xe3, 0xff, 0x80, 0x00,
    0x00, 0xa7, 0x74, 0x60, 0xe3, 0xa7, 0xff, 0xa7, 0xe3, 0x60, 0x74, 0xa7, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0xdf, 0xff, 0xdf, 0x10, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xa7, 0x74, 0x60, 0xe3, 0xa7, 0xff, 0xa7, 0xe3, 0x60, 0x74, 0xa7, 0x00,
    0x00, 0x80, 0xff, 0xe3, 0x3c, 0x00, 0xff, 0x00, 0x3c, 0xe3, 0xff, 0x80, 0x00,
    0x20, 0xe7, 0xc7, 0x93, 0x00, 0x00, 0xff, 0x00, 0x00, 0x93, 0xc7, 0xe7, 0x20,
    0x00, 0x00, 0x6c, 0x3c, 0x00, 0x8f, 0xff, 0x8f, 0x00, 0x3c, 0x6c, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x4c, 0xc3, 0xff, 0xc3, 0x4c, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const lv_image_dsc_t* snowflake_image()
{
    static lv_image_dsc_t dsc;
    if (!dsc.data) {
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_A8;
        dsc.header.w = 13;
        dsc.header.h = 13;
        dsc.header.stride = 13;
        dsc.data_size = sizeof(kSnowflakeA8);
        dsc.data = kSnowflakeA8;
    }
    return &dsc;
}
lv_obj_t* s_headMute[kTracks] = {};
lv_obj_t* s_headSolo[kTracks] = {};
lv_obj_t* s_headArm[kTracks] = {};   ///< ARM: REC takes onto the track (its pads, or its input)
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
 * the slot the first track nobody has made would occupy. */
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
bool        s_kit2Pads     = false;   ///< kKit2Logic is pushed
lv_timer_t* s_soundTimer   = nullptr;
bool        s_visible      = false;   ///< this screen is the one on top
bool        s_soundHeld    = false;   ///< the instrument was borrowed for the song
bool        s_soundAsked   = false;   ///< a load was asked for since the host was last Idle
std::string s_soundPath;               ///< the instrument the keeper asks for
crosspad::RetryBackoff s_soundRetry;   ///< after a load of it failed, on lv_tick
bool        s_soundSaid    = false;   ///< its failure was said
bool        s_soundPads    = false;   ///< kSoundLogic is pushed
bool        s_soundBounce  = false;   ///< a bounce of the Pitch track is capturing it
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
static lv_timer_t* s_timer = nullptr;

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

Pending   s_pending = Pending::None;
uint32_t  s_pendingGen = 0;
lv_obj_t* s_loading = nullptr;
/* A project asked for and not open yet: the timeline waits for it. */
bool      s_opening = false;
bool      s_editing = false;
EditKind  s_editKind = EditKind::Tempo;
/* The song window's tempo row, the MIX window's dB and M; they live and die
 * with the overlay. */
lv_obj_t* s_tempoItem = nullptr;
lv_obj_t* s_songList = nullptr;     // a song window's list, under its header
lv_obj_t* s_mixDb[kTracks] = {};
lv_obj_t* s_mixMute[kTracks] = {};
lv_obj_t* s_mixSolo[kTracks] = {};
lv_obj_t* s_mixPan[kTracks] = {};          ///< the pan's reading: C, L25, R40; a tap centres it
lv_obj_t* s_mixPanSlider[kTracks] = {};
lv_obj_t* s_mixMeter[kTracks][2] = {};     ///< a bar a side beside the fader
float     s_mixShown[kTracks][2] = {};

#ifdef ARR_USE_ENCODER
lv_group_t* s_encGroup = nullptr;
static lv_group_t* s_prevEncGroup = nullptr;
lv_group_t* s_overlayGroup = nullptr;
lv_group_t* s_toolGroup = nullptr;    // MOVE/DUP/DEL while a block is clicked
lv_group_t* s_regionGroup = nullptr;  // the region handle while L or R is turned
#endif
bool s_toolMode = false;
MoveAxis  s_moveAxis = MoveAxis::None;
lv_obj_t* s_moveArrows[4] = {};       // left, right, up, down; children of the timeline

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
uint32_t  s_mergeMask = (uint32_t)((1ull << kTracks) - 1);
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
    /* The kit badges: a kit loaded in the kit browser is the song's too. */
    for (char c : s.kit) mix((uint8_t)c);
    for (int t = 0; t < kTracks; ++t) {
        for (char c : s.tracks[t].kit) mix((uint8_t)c);
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
            for (char ch : c.element) mix((uint8_t)ch);   // an element's bake lands in file, its title in the list
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

lv_obj_t* s_nameArea = nullptr;
int s_afterPick    = -1;   ///< a track made by the pick: picked once this screen is back
// ── building ───────────────────────────────────────────────────────────────

/// Focus as a frame inside the button: the theme's ring stands 4 px out of it,
/// and a row the button fills cuts that off at the top and bottom.
static void focus_frame(lv_obj_t* b)
{
    lv_obj_set_style_outline_width(b, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(b, kHead, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(b, 2, LV_STATE_FOCUS_KEY);
}

lv_obj_t* make_button(lv_obj_t* parent, int32_t w, const char* text, lv_event_cb_t cb, void* ud)
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
static lv_obj_t* tool_button(lv_obj_t* parent, int32_t w, const char* text, lv_event_cb_t cb)
{
    lv_obj_t* b = make_button(parent, w, text, cb);
    lv_obj_set_height(b, LV_PCT(100));
    lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_14, 0);
    return b;
}

/* The columns beside the timeline, sized with it to the rows in use. */
static lv_obj_t* s_headCol = nullptr;
static lv_obj_t* s_meterCol = nullptr;

void size_lanes(int rows)
{
    const int32_t h = kRulerH + 4 + rows * (kLaneH + kLaneGap);
    for (lv_obj_t* o : {s_headCol, s_timeline, s_meterCol})
        if (o && lv_obj_get_height(o) != h) lv_obj_set_height(o, h);
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

} // namespace arrange_ui

using namespace arrange_ui;

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
    s_projBtn = make_button(s_root, 58, "", project_cb);
    place(s_projBtn, 6, ARR_Y(35));
    s_projLabel = lv_obj_get_child(s_projBtn, 0);
    lv_label_set_long_mode(s_projLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_projLabel, 52);
    lv_obj_set_style_text_align(s_projLabel, LV_TEXT_ALIGN_CENTER, 0);

    /* MIX beside the song, where the export's dim "18 bars" was: the
     * tracks' desk one press away (it was only a long press on a track's
     * button), and the position is the top bar's anyway. */
    s_mixBtn = make_button(s_root, 42, "MIX", mix_cb);
    place(s_mixBtn, 67, ARR_Y(35));

    /* EDIT here is the timeline's: the loop region, copy / paste, merging,
     * the song's length, a clip from the library (edit_cb). The key at the
     * foot is the clip's (clip_edit_cb). */
    /* FOLLOW, an arrow beside the position it follows; the keys after it a
     * little narrower than the export's, to make the room. */
    s_followBtn = make_button(s_root, 24, LV_SYMBOL_RIGHT, follow_cb);
    place(s_followBtn, kCtxSong.follow, ARR_Y(35));
    s_editBtn = make_button(s_root, 38, "EDIT", edit_cb);
    place(s_editBtn, 139, ARR_Y(35));
    s_syncBtn = make_button(s_root, 42, "SYNC", sync_cb);
    place(s_syncBtn, 180, ARR_Y(35));
    s_loopBtn = make_button(s_root, 42, "LOOP", loop_cb);
    place(s_loopBtn, kCtxSong.loop, ARR_Y(35));
    s_zoomBtn = make_button(s_root, 42, "ZOOM", zoom_cycle_cb);
    place(s_zoomBtn, kCtxSong.zoom, ARR_Y(35));
    elements_build_crumbs(s_root);

    /* ── lanes: the track buttons fixed, the timeline scrolling beside them ──
     * The export's LANE_X=48 and a lane block from screen y=62 to 200: four
     * lanes, and the rest of the tracks scroll up and down under them, headers,
     * lanes and meters together (size_lanes()). */
    constexpr int32_t kBodyH = kRulerH + 4 + kShownTracks * (kLaneH + kLaneGap);
    lv_obj_t* body = lv_obj_create(s_root);
    lv_obj_remove_style_all(body);
    place(body, 4, ARR_Y(62));
    lv_obj_set_size(body, 312, kBodyH);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(body, 4, 0);   // 4 + 80 + 4: the lanes from x 88 to 302, the meters on 306

    lv_obj_t* heads = lv_obj_create(body);
    lv_obj_remove_style_all(heads);
    lv_obj_set_width(heads, kHeadW);
    lv_obj_remove_flag(heads, LV_OBJ_FLAG_SCROLLABLE);
    elements_build_solo(heads);
    s_headCol = heads;
    for (int t = 0; t < kTracks; ++t) {
        lv_obj_t* h = lv_button_create(heads);
        lv_obj_set_pos(h, 0, kRulerH + 4 + t * (kLaneH + kLaneGap));
        lv_obj_set_size(h, kHeadW, kLaneH);
        lv_obj_add_flag(h, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
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
        lv_obj_add_event_cb(h, head_hold_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)t);
        /* One line over the name: "01" in the type's colour, S and M beside
         * it, then ARM on a seq track. They only show: a touch anywhere on the
         * header opens its menu, where all three are keys -- small buttons
         * side by side were hit for one another. */
        lv_obj_t* l = lv_label_create(h);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, kAudioT, 0);
        set_text_fmt(l, "%02d", t + 1);
        lv_obj_set_pos(l, 5, 1);
        lv_obj_t* name = lv_label_create(h);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(name, kText, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(name, kHeadW - 10);
        lv_label_set_text(name, "");
        lv_obj_set_pos(name, 5, 11);            // 17 px of line in a 28 px lane
        /* Frozen: the snowflake where the name began, the name after it
         * (rebuild_timeline()). */
        lv_obj_t* ice = lv_image_create(h);
        lv_image_set_src(ice, snowflake_image());
        lv_obj_set_style_image_recolor(ice, kIce, 0);
        lv_obj_set_style_image_recolor_opa(ice, LV_OPA_COVER, 0);
        lv_obj_set_pos(ice, 5, 13);
        lv_obj_remove_flag(ice, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(ice, LV_OBJ_FLAG_HIDDEN);
        s_headIce[t] = ice;
        auto toggle = [&](int32_t x, const char* text) {
            lv_obj_t* b = lv_obj_create(h);
            lv_obj_remove_style_all(b);
            lv_group_remove_obj(b);
            lv_obj_set_size(b, 14, 11);
            lv_obj_set_pos(b, x, 1);
            lv_obj_set_style_radius(b, 3, 0);
            lv_obj_set_style_bg_color(b, kWell, 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);   // the header's touch, not its own
            lv_obj_t* bl = lv_label_create(b);
            lv_obj_set_style_text_font(bl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(bl, kDim, 0);
            lv_label_set_text(bl, text);
            lv_obj_center(bl);
            return b;
        };
        s_headSolo[t] = toggle(20, "S");
        s_headMute[t] = toggle(38, "M");
        /* ARM: a dot after M, red while REC takes onto the track -- the pads
         * onto a seq track, whose kit they then play, or an audio track's
         * input. */
        lv_obj_t* arm = lv_obj_create(h);
        lv_obj_remove_style_all(arm);
        lv_group_remove_obj(arm);
        lv_obj_set_size(arm, 10, 10);
        lv_obj_set_pos(arm, 57, 1);
        lv_obj_set_style_radius(arm, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(arm, kWell, 0);
        lv_obj_set_style_bg_opa(arm, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(arm, kRec, 0);
        lv_obj_set_style_border_width(arm, 1, 0);
        lv_obj_remove_flag(arm, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(arm, LV_OBJ_FLAG_CLICKABLE);
        s_headArm[t] = arm;
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
     * one. In the lanes' block but out of its row of columns, so it scrolls
     * up and down with the tracks and not sideways with the timeline. */
    s_addRow = lv_button_create(body);
    lv_obj_remove_style_all(s_addRow);
    lv_obj_add_flag(s_addRow, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_addRow, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_size(s_addRow, 312, kLaneH);
    place(s_addRow, 0, kRulerH + 4);
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
    lv_obj_set_flex_grow(s_timeline, 1);
    lv_obj_set_style_bg_color(s_timeline, kPanel, 0);
    lv_obj_set_style_bg_opa(s_timeline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_timeline, 4, 0);
    lv_obj_set_style_pad_top(s_timeline, 2, 0);
    lv_obj_set_style_pad_hor(s_timeline, 2, 0);
    lv_obj_set_scroll_dir(s_timeline, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_timeline, LV_SCROLLBAR_MODE_OFF);
    elements_build_minimap(s_root);

    /* The tracks' stereo meters, beside the lanes: a well a track, two
     * columns of ten segments bottom up -- green, two amber, one red -- at a
     * fifth until lit (show_meters). */
    lv_obj_t* meters = lv_obj_create(body);
    lv_obj_remove_style_all(meters);
    lv_obj_set_width(meters, kMeterW);
    lv_obj_remove_flag(meters, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(meters, LV_OBJ_FLAG_CLICKABLE);
    s_meterCol = meters;
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
    size_lanes(kShownTracks);
    /* Over the timeline and the meters, made after it: a sibling drawn
     * later covers it, and the add row was a strip of outline under the
     * timeline's panel. */
    lv_obj_move_foreground(s_addRow);

    /* ── tools on the selected block ──────────────────────────────────────
     * The export's five keys at 58 px on a 62 px pitch, filling the width.
     * Z- and Z+ became ZOOM in the context row and MIX a key there beside the
     * song (and a long press on a track button); everything else is where it
     * was. MOVE stays an editable
     * button -- the encoder nudges the block with it, which no plain key
     * could do. */
    constexpr int32_t kToolW = 74, kToolPitch = 78, kToolH = 28;
    s_tools[0] = lv_CreateEditableButton(s_root);
    lv_obj_set_size(s_tools[0], kToolW, kToolH);
    place(s_tools[0], 6, ARR_Y(204));
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
        place(s_tools[i + 1], 6 + (i + 1) * kToolPitch, ARR_Y(204));
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
            /* Back from the kit loader with a new kit track: picked, as a
             * track made here is. */
            if (s_afterPick >= 0) {
                const int made = s_afterPick;
                s_afterPick = -1;
                seq_track_made(made);
            }
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
    if (s_kit2Pads && crosspad::getPadManager().getActivePadLogic() == kKit2Logic) {
        crosspad::getPadManager().popPadLogic();
    }
    s_kit2Pads = false;
    /* The song keeps playing when the screen closes -- it is the song
     * engine's, not this screen's. Only the widgets go. */
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) { lv_group_delete(s_overlayGroup); s_overlayGroup = nullptr; }
    if (s_toolGroup) { lv_group_delete(s_toolGroup); s_toolGroup = nullptr; }
    if (s_regionGroup) { lv_group_delete(s_regionGroup); s_regionGroup = nullptr; }
#endif
    elements_destroy();
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
    mix_forget();
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
    for (auto& i : s_headIce) i = nullptr;
    for (auto& b : s_headMute) b = nullptr;
    for (auto& b : s_headSolo) b = nullptr;
    for (auto& w : s_meterWell) w = nullptr;
    for (auto& tr : s_meterSeg) for (auto& side : tr) for (auto& s : side) s = nullptr;
    s_procBox = s_procLabel = nullptr;
    for (auto& t : s_tools) t = nullptr;
    s_blockCount = 0;
    /* the status line went with the screen */
    s_root = s_timeline = s_playhead = s_projLabel = s_mixBtn = nullptr;
    s_headCol = s_meterCol = nullptr;
    s_zoomBtn = s_loopBtn = s_syncBtn = s_editBtn = s_projBtn = s_followBtn = nullptr;

#ifdef ARR_USE_ENCODER
    if (indev_encoderTop && s_prevEncGroup) lv_indev_restore_group(indev_encoderTop, s_prevEncGroup);
    if (s_encGroup) {
        lv_group_delete(s_encGroup);
        s_encGroup = nullptr;
    }
    s_prevEncGroup = nullptr;
#endif

    if (root) lv_obj_delete_async(root);
}

// No pad logic of its own: the pads play the kit over the song, or a Pitch
// track's instrument while one is in it (sound_keeper_cb).
REGISTER_APP(Arrange, nullptr, "arrange.png",
             Arrange_create, Arrange_destroy,
             nullptr, nullptr, nullptr, 0)
