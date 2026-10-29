// Arrange -- the tracks a player makes: ADD TRACK, a seq track's source, the track menu,
// its input and bounce sheets, a kit track's kit.

#include "arrange_internal.hpp"

namespace arrange_ui {

// ── Arrange is where a song starts ────────────────────────────────────────
/*
 * The one flow in this screen that is not editing: making a track.
 *
 *   + ADD TRACK ──> "New track": AUDIO or KITS
 *                      |            |
 *                      |            +--> "plays with": KIT or PITCH
 *                      |                      |
 *                      +----------------------+--> an empty track, armed:
 *                                                   REC records into it --
 *                                                   its input over the song,
 *                                                   the pads on a seq track
 *                    (KIT: the kit loader first; PITCH with no instrument
 *                     yet: Pitch Play, to pick one)
 *
 * Every dialog says the same two things, because they are the rules a player
 * has to know before choosing and nobody reads them anywhere else: kTracks
 * tracks, up to two seqs -- two kits, or one Pitch -- and any track can be
 * bounced to audio at any time, which is also how a seq slot comes free.
 */

/// A sheet over the timeline: the overlay machinery (its own encoder group,
/// the power button's click closing it) with the column layout off, because
/// a sheet places its cards rather than stacking them.
static lv_obj_t* sheet_open()
{
    overlay_open();
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_layout(s_overlay, LV_LAYOUT_NONE);
    return s_overlay;
}

static lv_obj_t* sheet_text(lv_obj_t* parent, int32_t x, int32_t y, const char* text,
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
static lv_obj_t* sheet_card(int32_t x, const char* tag, const char* name, const char* l1,
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

/// A half-height card, for four choices in two rows: @p row 0 or 1.
static lv_obj_t* sheet_tile(int32_t x, int row, const char* tag, const char* name, const char* line,
                     lv_color_t col, bool enabled, lv_event_cb_t cb, void* ud)
{
    lv_obj_t* c = sheet_card(x, tag, name, "", "", col, enabled, cb, ud);
    lv_obj_set_pos(c, x, ARR_Y(74) + row * 50);
    lv_obj_set_height(c, 46);
    /* The card's own lines, moved up for the lower height: tag, name, line. */
    lv_obj_set_pos(lv_obj_get_child(c, 0), 10, 6);
    lv_obj_set_pos(lv_obj_get_child(c, 1), 10, 20);
    lv_obj_add_flag(lv_obj_get_child(c, 2), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lv_obj_get_child(c, 3), LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* l = sheet_text(c, 0, 8, line, &lv_font_montserrat_10, kDim);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -8, 7);
    return c;
}

/// The box that carries the rules, under the cards.
static void sheet_rules(const char* line, const char* accented)
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
static void sheet_keys(const char* const* labels, const lv_event_cb_t* cbs, int n)
{
    const int32_t gap = 4, avail = 320 - 8 - gap * (n - 1);
    int32_t x = 4;
    for (int i = 0; i < n; ++i) {
        const int32_t w = (i == n - 1) ? (316 - x) : avail / n;
        lv_obj_t* b = lv_button_create(s_overlay);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, x, ARR_Y(204));
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

static void new_track_sheet();
static void seq_source_sheet();
static void open_seq_track(int track);
static void pick_kit_for(int track, crosspad::KitList list = crosspad::KitList::Any);

/// Is the selected track one that holds a seq? What a bounce needs.
static bool selected_is_seq()
{
    auto* se = engine();
    if (!se || s_selTrack < 0 || s_selTrack >= kTracks) return false;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const auto& t = se->song().tracks[s_selTrack];
    return t.inUse && t.kind == crosspad::TrackKind::Seq;
}

static void cancel_sheet_cb(lv_event_t*) { close_overlay(); }
static void back_to_add_cb(lv_event_t*)  { close_overlay(); new_track_sheet(); }

static void choose_audio_cb(lv_event_t*)
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
    if (made < 0) { note("No room: all tracks are in use."); return; }
    /* An empty audio track is filled by recording into it, here: armed, the
     * next REC records its input over the song (ArrangeAudioTake). */
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        se->song().armed = (int8_t)made;
    }
    se->commit();
    select(made, -1);
    rebuild_timeline();
    if (crosspad::getPlatformServices().sampleTools) {
        note_fmt("Track %d armed: REC records the mics into it. Hold its header for another input.", made + 1);
    } else {
        note_fmt("Track %d added. No recorder in this firmware -- EDIT it to add a clip.", made + 1);
    }
}

static void choose_seq_cb(lv_event_t*)
{
    close_overlay();
    seq_source_sheet();
}

/// A seq track on @p src on the lowest free track, playing @p kit (see
/// AudioSong::addSeqTrack()), armed -- the newest track is the one played
/// into next. Its index, or -1 when there is no room for it.
static int make_seq_track(crosspad::SeqSource src, const std::string& kit)
{
    auto* se = engine();
    auto* sq = crosspad::getPlatformServices().sequencer;
    if (!se) return -1;
    /* A scene of its own, so the track keeps playing what was put on it when
     * the player switches scenes somewhere else -- the one just added, not
     * the active one: while the transport runs that is still the old scene
     * until the loop ends, another track's, and the new track's seq view
     * cleared it and kept its notes (22.10.2026). With all eight there, the
     * lowest scene no track plays. */
    const bool added = sq && sq->addScene();
    int made = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        const int scene = song.sceneForNewSeq(sq ? sq->getSceneCount() : 1, added);
        made = song.freeTrack();
        if (made < 0 || scene < 0 || !song.addSeqTrack(made, src, (uint8_t)scene, kit)) made = -1;
        /* A song that starts as a seq track has no clip to take its tempo
         * from: its grid is the sequencer's, or the two drift apart. */
        if (made >= 0 && song.bpm <= 0.0f && sq) song.bpm = sq->getBpm();
        if (made >= 0) song.armed = (int8_t)made;
    }
    if (made >= 0) se->commit();
    return made;
}

/// What KITS offers: the kit types, then Pitch Play.
enum class NewSeq : uint8_t { Perf, Drums, Slice, Pitch };

/// A seq track just made: picked, on the timeline, and nothing opened -- it
/// is empty and armed, so the next REC records into it, and CLIP EDIT is
/// there for whoever wants the seq view instead.
void seq_track_made(int made)
{
    select(made, -1);
    rebuild_timeline();
    auto* se = engine();
    int holder = -1;
    if (se) {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        if (song.seqRoute(made) == 0 && song.seqRoute(song.liveKit) == 0 && song.liveKit != made) holder = song.liveKit;
    }
    if (holder >= 0 && se->freezeState((uint8_t)holder) != crosspad::FreezeState::Frozen) {
        note_fmt("Track %d armed: track %d freezes first, then its kit loads.", made + 1, holder + 1);
        return;
    }
    note_fmt("Track %d armed: REC records the pads, CLIP EDIT opens its seq view.", made + 1);
}

static void choose_source_cb(lv_event_t* e)
{
    const auto what = (NewSeq)(intptr_t)lv_event_get_user_data(e);
    close_overlay();
    /* A kit track plays the kit picked for it, from the list of its kind. */
    if (what != NewSeq::Pitch) {
        pick_kit_for(-1, what == NewSeq::Drums ? crosspad::KitList::Drums
                       : what == NewSeq::Slice ? crosspad::KitList::Slice : crosspad::KitList::Perf);
        return;
    }
    const auto src = crosspad::SeqSource::Pitch;
    const int made = make_seq_track(src, {});
    if (made < 0) { note("No room: all tracks are in use, or a seq is already in the song."); return; }
    /* With no instrument picked yet the track has no sound: Pitch Play is
     * where one is picked, as the kit loader is for a kit track. */
    if (crosspad::instrumentPlayState().instrumentPath.empty()) {
        select(made, -1);
        rebuild_timeline();
        open_seq_track(made);
        return;
    }
    seq_track_made(made);
}

// ── a kit track's kit ──────────────────────────────────────────────────────
/*
 * The kit loader -- the board's kit browser (IKitPicker) -- for a new kit
 * track or one whose kit changes. The kit picked is the track's; while the
 * track is the live one (AudioSong::liveKit) it is the song's too -- what the
 * pads and the Sampler play, loaded as a song's kit is. The browser is up
 * while this screen is down: the pick changes the song, and the screen, back,
 * picks the new track (s_afterPick).
 */

static int s_kitPickTrack = -1;   ///< the track the kit loader picks for; -1 a new one
static crosspad::KitList s_kitPickList = crosspad::KitList::Any;   ///< the list the loader opens on

static void kit_picked(const std::string& path, void*)
{
    auto* se = engine();
    if (!se) return;
    const int track = s_kitPickTrack;
    if (track < 0) {
        /* A new kit track: the song stops, the track holding the kits' engine
         * is frozen, then the new kit loads (live_kit()). */
        if (se->playing()) se->stop();
        s_afterPick = make_seq_track(crosspad::SeqSource::Kit, path);
        /* A toast, not this screen's note: the screen is down while the loader is up. */
        if (s_afterPick < 0) ui_toast_show("No room: all tracks or all eight scenes are in use.", 3000, UI_TOAST_WARNING);
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        if (song.seqRoute(track) != 0) return;
        song.setTrackKit(track, path);
    }
    se->commit();
}

static void pick_kit_for(int track, crosspad::KitList list)
{
    auto* se = engine();
    if (!se) return;
    int route = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        route = track < 0 ? song.freeSeqRoute(crosspad::SeqSource::Kit) : song.seqRoute(track);
    }
    if (route < 0) { note("No room for another kit track."); return; }
    if (!crosspad::getPlatformServices().kitPicker) { note("No kit loader in this firmware."); return; }
    s_kitPickTrack = track;
    s_kitPickList = list;
    /* Not inside this screen's own click: the browser takes the screen down. */
    lv_async_call([](void*) {
        crosspad::IKitPicker* picker = crosspad::getPlatformServices().kitPicker;
        if (!picker || !picker->open(&kit_picked, nullptr, s_kitPickList)) note("The kit loader cannot open now.");
    }, nullptr);
}

/// A seq track's screen, on its scene and knowing it came from here: a Pitch
/// track opens in Pitch Play -- the very screen the MODE map's Pitch opens,
/// which takes the transport for this scene so REC and DUB record what is
/// played on its keys -- and a kit track in the sequencer's seq view, armed
/// and given the kits' engine at once: what the pads play there, and what
/// the Sampler's EDIT edits, is this track's kit.
static void open_seq_track(int track)
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
            const bool added = sq->addScene();
            const int scene = se->song().sceneForNewSeq(sq->getSceneCount(), added);
            if (scene >= 0) {
                t.scene = (uint8_t)scene;
                moved = true;
            }
        }
        if (t.source == crosspad::SeqSource::Kit) {
            se->song().armed = (int8_t)track;
            se->song().setLiveKit(track);
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

static void new_track_sheet()
{
    auto* se = engine();
    if (!se || s_overlay || s_merging) return;
    int used = 0;
    bool seqFree = true;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        used = se->song().usedTracks();
        seqFree = se->song().seqSlotFree() || se->song().freeSeqRoute(crosspad::SeqSource::Pitch) >= 0;
    }
    if (used >= kTracks) { note("All tracks are in use."); return; }

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
    sheet_card(164, "KITS", "Kits",
               seqFree ? "drums, slices, a kit," : "no scene is free:",
               seqFree ? "or Pitch Play"         : "bounce one to free it",
               kAccent, seqFree, choose_seq_cb, nullptr);

    sheet_rules("Only the armed track plays live; the rest frozen.",
                "Any track can be bounced to audio at any time.");
    static const char* kLabels[] = {"CANCEL"};
    static const lv_event_cb_t kCbs[] = {cancel_sheet_cb};
    sheet_keys(kLabels, kCbs, 1);
}

static void seq_source_sheet()
{
    auto* se = engine();
    if (!se || s_overlay) return;
    int used = 0, kitRoute = -1, pitchRoute = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        used = song.usedTracks();
        kitRoute = song.freeSeqRoute(crosspad::SeqSource::Kit);
        pitchRoute = song.freeSeqRoute(crosspad::SeqSource::Pitch);
    }
    sheet_open();
    char title[48];
    std::snprintf(title, sizeof(title), "Kit track %d  -  plays with", used + 1);
    sheet_text(s_overlay, 8, ARR_Y(36), title, &lv_font_montserrat_16, kText);

    /* The kit types of the kit browser, in its colours, then Pitch Play. A
     * kit track picks its kit from the list of its kind. */
    const bool kit = kitRoute >= 0, pitch = pitchRoute >= 0;
    sheet_tile(4,   0, "PERF",   "Kit",   kit ? "long + short" : "no kit slot",
               lv_color_hex(0xC6FF47), kit, choose_source_cb, (void*)(intptr_t)NewSeq::Perf);
    sheet_tile(164, 0, "DRUMS",  "Drums", kit ? "hits, 12 voices" : "no kit slot",
               lv_color_hex(0xFFC857), kit, choose_source_cb, (void*)(intptr_t)NewSeq::Drums);
    sheet_tile(4,   1, "SLICE",  "Slice", kit ? "one loop, 1 voice" : "no kit slot",
               lv_color_hex(0xFF8A3D), kit, choose_source_cb, (void*)(intptr_t)NewSeq::Slice);
    sheet_tile(164, 1, "PITCH PLAY", "Pitch", pitch ? "melody, 4 voices" : "slot taken",
               kPitchT, pitch, choose_source_cb, (void*)(intptr_t)NewSeq::Pitch);

    sheet_rules("A kit of its own: plays live armed, frozen when not.",
                "Bounce turns a track into audio and frees its slot.");
    static const char* kLabels[] = {LV_SYMBOL_LEFT " BACK", "CANCEL"};
    static const lv_event_cb_t kCbs[] = {back_to_add_cb, cancel_sheet_cb};
    sheet_keys(kLabels, kCbs, 2);
}

static void bounce_go_cb(lv_event_t*)
{
    auto* se = engine();
    const int track = s_selTrack;
    close_overlay();
    if (!se) return;
    /* A Pitch track is bounced through its instrument, so it has to be in:
     * notes before the bank is built would be silence in the clip. */
    bool pitch = false, kit = false, parked = false;
    std::string trackKit;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        pitch = song.tracks[track].source == crosspad::SeqSource::Pitch;
        kit = song.seqRoute(track) == 0;
        parked = kit && song.liveRoute(track) < 0;
        trackKit = song.tracks[track].kit;
    }
    if (parked) {
        note("The kits' engine plays another track now -- ARM this one, then BOUNCE.");
        return;
    }
    if (pitch && !crosspad::seqTrackSound().pitch.ready()) {
        note("The track's instrument is still loading -- BOUNCE again in a moment.");
        return;
    }
    /* A kit track is bounced through the kits' engine, once its kit is in. */
    crosspad::IKitManager* km = crosspad::getKitManager();
    const crosspad::KitInfo* cur = km ? km->getCurrentKit() : nullptr;
    if (kit && (!cur || cur->path != trackKit)) {
        note("The track's kit is still loading -- BOUNCE again in a moment.");
        return;
    }
    /* What the track plays, rendered as a freeze is -- a looping block's two
     * passes, the renders it already has on the card taken, off the clock --
     * becomes its audio when it lands (core 2.14). */
    /* Its notes reach the instrument on their own route; it stays loaded. */
    if (pitch) s_soundBounce = true;
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
static void confirm_sheet(const char* title, const char* line, const char* line2, lv_color_t line2Col,
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
                  "Its seq slot is free again for a new idea.", kAccent,
                  kKitT, kInk, "BOUNCE", bounce_go_cb);
}

static void delete_track_go_cb(lv_event_t*)
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

static void track_name_done_cb(lv_event_t* e)
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
static void rename_track_sheet()
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
static void stretch_mode_cb(lv_event_t* e)
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

static void stretch_sheet()
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

/// An input picked for an audio track: recorded into it by the next REC, so
/// the track is armed with it.
static void input_pick_cb(lv_event_t* e)
{
    const auto in = (crosspad::RecInput)(intptr_t)lv_event_get_user_data(e);
    auto* se = engine();
    close_overlay();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        auto& t = song.tracks[s_selTrack];
        if (!t.inUse || t.kind != crosspad::TrackKind::Audio) return;
        t.input = in;
        song.armed = (int8_t)s_selTrack;
    }
    se->commit();
    rebuild_timeline();
    crosspad::ISampleTools* st = crosspad::getPlatformServices().sampleTools;
    if (st && !st->inputAvailable(in)) {
        note_fmt("Track %d armed on %s -- it records what the computer plays once Settings > USB is audio.",
                 s_selTrack + 1, input_name(in));
    } else {
        note_fmt("Track %d armed: REC records %s into it.", s_selTrack + 1, input_name(in));
    }
}

/// MONITOR in the input sheet: the track's input heard live on the speakers
/// and jacks, or not -- the Mixer's MONITOR at "all" or "off" for the
/// input's channels (IInputMonitor), so the Mixer shows it and its desk keeps
/// it. The sheet opens again with the new state.
static void monitor_toggle_cb(lv_event_t*)
{
    auto* se = engine();
    crosspad::IInputMonitor* mon = crosspad::getPlatformServices().inputMonitor;
    close_overlay();
    if (!se || !mon) return;
    crosspad::RecInput in = crosspad::RecInput::Mic;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        in = se->song().tracks[s_selTrack].input;
    }
    const bool on = !mon->monitoring(in);
    if (mon->setMonitoring(in, on)) {
        note_fmt(on ? "%s monitored: heard on the speakers and jacks." : "%s not monitored.", input_name(in));
    }
    input_sheet(s_selTrack);
}

/// A long press on an audio track's header: which input REC records into it
/// -- the mics, the line-in, both at once, what the computer plays over USB,
/// or what the pads play -- whether that input is heard live (MONITOR), and
/// MIX, which the same press opens on the others.
void input_sheet(int track)
{
    auto* se = engine();
    if (!se || s_overlay || s_merging || s_bouncing >= 0 || track < 0 || track >= kTracks) return;
    crosspad::RecInput cur = crosspad::RecInput::Mic;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& t = se->song().tracks[track];
        if (!t.inUse || t.kind != crosspad::TrackKind::Audio) return;
        cur = t.input;
    }
    select(track, s_selTrack == track ? s_selIndex : -1);
    crosspad::ISampleTools* st = crosspad::getPlatformServices().sampleTools;
    char title[40];
    std::snprintf(title, sizeof(title), "Track %d  -  records from", track + 1);
    overlay_list(title);
    for (const auto& i : kTrackInputs) {
        char text[64];
        const bool now = !st || st->inputAvailable(i.in);
        std::snprintf(text, sizeof(text), "%s %s%s", i.in == cur ? LV_SYMBOL_OK : "    ", i.name,
                      now ? "" : "  -  USB audio is off");
        overlay_item(text, input_pick_cb, (void*)(intptr_t)i.in);
    }
    crosspad::IInputMonitor* mon = crosspad::getPlatformServices().inputMonitor;
    if (mon && mon->canMonitor(cur)) {
        const bool on = mon->monitoring(cur);
        overlay_item(on ? LV_SYMBOL_VOLUME_MAX " Monitor: ON" : LV_SYMBOL_MUTE " Monitor: OFF",
                     monitor_toggle_cb, nullptr, on);
    }
    overlay_item(LV_SYMBOL_SETTINGS " MIX ...", [](lv_event_t* e) { close_overlay(); mix_cb(e); }, nullptr);
    overlay_item(LV_SYMBOL_CLOSE " Cancel", cancel_cb, nullptr);
}

/// What a tap on a track's header opens: what acts on the track as a whole.
/// Room for more; BOUNCE left the key row for it.
void track_menu()
{
    auto* se = engine();
    if (!se || s_overlay || s_merging || s_bouncing >= 0) return;
    bool seq = false, muted = false, solo = false, armed = false;
    crosspad::RecInput input = crosspad::RecInput::Mic;
    crosspad::StretchMode stretch = crosspad::StretchMode::Off;
    char title[32];
    std::string kit;
    bool kitTrack = false;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        const auto& t = song.tracks[s_selTrack];
        if (!t.inUse) return;
        seq = t.kind == crosspad::TrackKind::Seq;
        stretch = t.stretch;
        muted = t.muted;
        solo = t.solo;
        armed = track_armed(song, s_selTrack);
        input = t.input;
        kitTrack = seq && t.source == crosspad::SeqSource::Kit;
        if (kitTrack) kit = kit_name(track_kit(song, s_selTrack));
        std::snprintf(title, sizeof(title), "Track %d  -  %s", s_selTrack + 1, track_badge(t));
    }
    overlay_keys(title);
    if (kitTrack) {
        char item[64];
        std::snprintf(item, sizeof(item), LV_SYMBOL_DRIVE " %s", kit.empty() ? "Kit ..." : kit.c_str());
        overlay_key(item, [](lv_event_t*) { close_overlay(); pick_kit_for(s_selTrack); });
    }
    overlay_key(armed ? LV_SYMBOL_OK " Armed" : LV_SYMBOL_PLUS " Arm for REC",
                [](lv_event_t*) { close_overlay(); arm_track(s_selTrack); });
    if (!seq) {
        char item[32];
        std::snprintf(item, sizeof(item), LV_SYMBOL_AUDIO " Input: %s", input_name(input));
        overlay_key(item, [](lv_event_t*) { close_overlay(); input_sheet(s_selTrack); });
    }
    if (seq) overlay_key(LV_SYMBOL_AUDIO " Bounce ...", [](lv_event_t*) { close_overlay(); bounce_sheet(); });
    else overlay_key(stretch == crosspad::StretchMode::KeepPitch ? LV_SYMBOL_SHUFFLE " Stretch"
                     : stretch == crosspad::StretchMode::Repitch ? LV_SYMBOL_SHUFFLE " Repitch"
                                                                 : LV_SYMBOL_SHUFFLE " Stretch: OFF",
                     [](lv_event_t*) { close_overlay(); stretch_sheet(); });
    overlay_key(muted ? LV_SYMBOL_MUTE " Mute: ON" : LV_SYMBOL_VOLUME_MAX " Mute: OFF",
                [](lv_event_t*) { close_overlay(); toggle_mute(s_selTrack); });
    overlay_key(solo ? LV_SYMBOL_OK " Solo: ON" : LV_SYMBOL_AUDIO " Solo: OFF",
                [](lv_event_t*) { close_overlay(); toggle_solo(s_selTrack); });
    overlay_key(LV_SYMBOL_EDIT " Rename ...", [](lv_event_t*) { close_overlay(); rename_track_sheet(); });
    overlay_key(LV_SYMBOL_TRASH " Delete ...", [](lv_event_t*) { close_overlay(); delete_track_sheet(); });
    overlay_key(LV_SYMBOL_CLOSE " Cancel", cancel_cb);
}

/// CLIP EDIT: the selected track, in the app that makes it.
///
/// A seq track opens the sequencer on its scene (a Pitch track Pitch Play,
/// whose SEQ key leads on to the notes); an audio track opens the Recorder on
/// the block picked -- or the one under the playhead -- to trim it and put it
/// back where it was, or, on an empty bar, to record into the track. The
/// timeline's editing is the EDIT beside SYNC (edit_cb). An element block opens
/// as the song it is, one level down the breadcrumb.
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
    if (elements_clip_edit()) return;
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

} // namespace arrange_ui
