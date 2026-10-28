// Arrange -- the screen's clock: the tick that follows the transport, the playhead,
// the position and the freeze's processing box.

#include "arrange_internal.hpp"

namespace arrange_ui {

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
    set_bg(s_followBtn, s_follow ? kAccent : kPanel);
    set_ink(lv_obj_get_child(s_followBtn, 0), s_follow ? kInk : kDim);
}

/// The meters beside the lanes, from what each track is playing: straight
/// up, down at ~28 dB a second. A segment lights at its level -- green
/// below -6 dB, amber to -1, red above -- and only those that change are
/// touched.
static void show_meters(crosspad::ISongEngine* se)
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

static bool s_procElement = false;  ///< the box says an element's enter, leave or bake
static int  s_progTrack = -1;       ///< the header whose foot shows a freeze's progress

/// A freeze or bounce of ours: a line along its track header's foot.
static void show_freeze(crosspad::ISongEngine* se)
{
    int track = -1;
    unsigned pct = 0;
    if (!freeze_progress(se, track, pct)) track = -1;
    if (s_progTrack >= 0 && s_progTrack != track && s_headProg[s_progTrack]) {
        lv_obj_add_flag(s_headProg[s_progTrack], LV_OBJ_FLAG_HIDDEN);
    }
    s_progTrack = track;
    if (track < 0 || !s_headProg[track]) return;
    lv_obj_t* p = s_headProg[track];
    const int32_t w = std::max<int32_t>(2, (int32_t)((kHeadW - 4) * pct / 100));
    if (lv_obj_get_width(p) != w) lv_obj_set_width(p, w);
    if (lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
}

/// Stretched or repitched blocks still being fitted to the tempo, or an
/// element entered, left or baked: a box over the lanes for as long as either
/// runs -- a stretch is made only while stopped, and until then those blocks
/// play as they were. A freeze says so on its track's header (show_freeze()).
static void show_processing(crosspad::ISongEngine* se)
{
    show_freeze(se);
    if (!s_procBox) return;
    const bool up = !lv_obj_has_flag(s_procBox, LV_OBJ_FLAG_HIDDEN);
    char fbuf[72];
    if (element_progress(fbuf, sizeof(fbuf))) {
        s_procElement = true;
        set_text(s_procLabel, fbuf);
        if (!up) {
            lv_obj_remove_flag(s_procBox, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_procBox);
        }
        return;
    }
    if (s_procElement) {
        s_procElement = false;
        if (up && !se->stretchWaiting()) {
            lv_obj_add_flag(s_procBox, LV_OBJ_FLAG_HIDDEN);
            return;
        }
    }
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

void tick_cb(lv_timer_t*)
{
    auto* se = engine();
    if (!se) return;
    if (s_pending != Pending::None && se->listGeneration() != s_pendingGen) {
        const Pending what = s_pending;
        s_pending = Pending::None;
        if (s_loading) { lv_obj_delete(s_loading); s_loading = nullptr; }
        if (what == Pending::Projects) fill_projects();
        else if (what == Pending::Clips) fill_clips();
        else if (what == Pending::Songs) fill_library_songs();
        else                           fill_elements();
    }
    elements_tick(se);
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
    /* An audio take that has ended goes onto its track (ArrangeAudioTake). */
    const crosspad::ArrangeAudioTake::Result landed = crosspad::arrangeAudioTake().poll();
    if (landed.done) {
        if (landed.ok) {
            select(landed.track, -1);
            rebuild_timeline();
            if (landed.passes > 1) {
                note_fmt("%u passes on track %d: the last from bar %u, the others are clips (EDIT).",
                         (unsigned)landed.passes, landed.track + 1, (unsigned)landed.bar + 1);
            } else {
                note_fmt("Take on track %d from bar %u.", landed.track + 1, (unsigned)landed.bar + 1);
            }
        } else {
            note_fmt("No take: %s.", landed.error.c_str());
        }
    }
    /* A REC that could not start, wherever it was pressed. */
    static uint32_t refused = crosspad::arrangeAudioTake().refusals();
    if (crosspad::arrangeAudioTake().refusals() != refused) {
        refused = crosspad::arrangeAudioTake().refusals();
        note_fmt("No REC: %s.", crosspad::arrangeAudioTake().lastError().c_str());
    }
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
    if (s_editing && s_editKind == EditKind::ExportWav && se->editState() == crosspad::SongOpState::Busy) {
        note_fmt("Writing the mix ... %u%%", (unsigned)se->editProgress());
    }
    if (s_editing && se->editState() != crosspad::SongOpState::Busy) {
        s_editing = false;
        const bool failed = se->editState() == crosspad::SongOpState::Failed;
        show_tempo();                 // the tempo row, if its window is still open
        rebuild_timeline();
        update_info();
        if (s_editKind == EditKind::Export || s_editKind == EditKind::ExportWav) export_done(!failed);
        else if (s_editKind == EditKind::ElementDelete) element_delete_done(!failed);
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
            /* How far: its header's foot (show_freeze()). */
        } else {
            const int track = s_bouncing;
            s_bouncing = -1;
            if (st.state == crosspad::BounceState::Done) {
                /* The engine wrote the WAV, put its block on this track and
                 * made the track audio, which frees its seq slot. */
                rebuild_timeline();
                note_fmt("Track %d is audio now -- its seq slot is free.", track + 1);
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
    if (s_overlay) {
        if (s_mixMeter[0][0]) mix_meters(se);
        return;
    }
    uint32_t shape = 0;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        shape = song_shape(se->song());
    }
    if (shape != s_shape) rebuild_timeline();
    show_meters(se);
    sync_transport();

    if (!s_playhead) return;
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
        return;
    }
    const double pos = se->positionBars();
    /* A take past the last block runs off the lanes: they are drawn on. */
    if ((uint32_t)pos + 2 >= s_drawnBars) {
        rebuild_timeline();
        if (!s_playhead) return;
    }
    const int32_t x = (int32_t)(pos * s_pxPerBar);
    /* Red while the pads or an input are being recorded onto the song. */
    set_bg(s_playhead, se->seqTake() != crosspad::SyncTake::None || crosspad::arrangeAudioTake().recording()
                           ? kRec : kHead);
    if (lv_obj_has_flag(s_playhead, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(s_playhead, LV_OBJ_FLAG_HIDDEN);
    if (lv_obj_get_x(s_playhead) != x) lv_obj_set_pos(s_playhead, x, 0);

    /* FOLLOW off: the lanes stay where the player put them, and the playhead
     * may run off them. */
    if (!s_follow) return;
    const int32_t view = lv_obj_get_content_width(s_timeline);
    const int32_t left = lv_obj_get_scroll_x(s_timeline);
    if (x < left || x > left + view * 2 / 3) {
        lv_obj_scroll_to_x(s_timeline, x > view / 3 ? x - view / 3 : 0, LV_ANIM_ON);
    }
}

} // namespace arrange_ui
