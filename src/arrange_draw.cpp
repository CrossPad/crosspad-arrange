// Arrange -- the timeline: lanes, track headers, blocks, their waveforms and notes,
// the meters and the info line, and the rebuild that draws them again.

#include "arrange_internal.hpp"

namespace arrange_ui {

// ── drawing ────────────────────────────────────────────────────────────────

/// A block's waveform, under its name: the clip at the speed it plays, so a
/// file shorter than its block ends early, and one made at another tempo
/// shows where its beats really fall -- unless its track follows the tempo
/// (stretch or repitch), when the clip fills the block's bars, as the copy
/// that plays does. Drawn to the clip's loudest value --
/// the lanes are too short for a quiet clip at full scale -- and only the
/// bars in the area being redrawn.
static void block_draw_cb(lv_event_t* e)
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
static void seq_draw_cb(lv_event_t* e)
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
static void set_peaks(Block& b, const crosspad::ClipPeaks& cp)
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
        if (b.peaks || !b.obj || b.element || b.file.empty() || !se->clipPeaks(b.file, cp)) continue;
        set_peaks(b, cp);
        lv_obj_invalidate(b.obj);
    }
}

static void paint_block(const Block& b)
{
    if (!b.obj) return;
    const bool sel = b.track == s_selTrack && b.index == s_selIndex;
    lv_obj_set_style_outline_width(b.obj, sel ? 2 : 0, 0);
    lv_obj_set_style_outline_color(b.obj, kHead, 0);
    lv_obj_set_style_outline_pad(b.obj, 1, 0);
}

/// Whether the block standing for track @p tr's clip @p idx has no file.
static bool block_missing(int tr, int idx)
{
    for (int i = 0; i < s_blockCount; ++i) {
        if (s_blocks[i].track == tr && s_blocks[i].index == idx) return s_blocks[i].missing;
    }
    return false;
}

/// What this screen has to say that the position does not. A toast, because
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
        if (valid && !s_selQuiet && (s_selTrack != locTrack || s_selIndex != locIndex)) {
            crosspad::songTransport().setStartBar(sel[s_selIndex].bar);
        }
        s_selQuiet = false;
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
    /* The position is the top bar's. So the status line speaks only where it
     * has something that is not in it: a region being edited, a clip whose
     * file is gone, an empty song. */
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
        return;   // the position already says this; see the note above
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

/// The seq over the whole song, as a block: picked by focus, and a click
/// makes it one (seqBlocksExplicit, as a cut does) and opens MOVE / DUP / DEL
/// on it -- on the next turn of the loop, since that rebuilds the timeline
/// and the lane being clicked with it.
static void seq_lane_cb(lv_event_t* e)
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

/* Where the finger went down on a block: a release far from it was a drag
 * with nothing to scroll (the whole song in view), not a tap on the block. */
static lv_point_t s_blockPressAt{};

static void block_cb(lv_event_t* e)
{
    const int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= s_blockCount) return;
    lv_indev_t* in = lv_indev_active();
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        if (in) lv_indev_get_point(in, &s_blockPressAt);
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && in && lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) {
        lv_point_t p;
        lv_indev_get_point(in, &p);
        if (std::abs(p.x - s_blockPressAt.x) > 12 || std::abs(p.y - s_blockPressAt.y) > 12) return;
    }
    select(s_blocks[k].track, s_blocks[k].index);
    if (s_blocks[k].obj) lv_obj_scroll_to_view(s_blocks[k].obj, LV_ANIM_ON);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) enter_tools();
}

static void rebuild_group()
{
#ifdef ARR_USE_ENCODER
    if (!s_encGroup) return;
    lv_obj_t* focused = lv_group_get_focused(s_encGroup);
    lv_group_remove_all_objs(s_encGroup);
    /* The context row's keys the open element hides are not reachable: a hidden
     * key left in the group could take the focus back and open the project list. */
    for (lv_obj_t* k : {s_projBtn, s_mixBtn, s_followBtn, s_editBtn, s_syncBtn, s_loopBtn, s_zoomBtn})
        if (k && !lv_obj_has_flag(k, LV_OBJ_FLAG_HIDDEN)) lv_group_add_obj(s_encGroup, k);
    elements_group_add(s_encGroup);
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

/// S or M on a header: lit in its colour, or dark.
void style_toggle(lv_obj_t* b, bool on, lv_color_t col)
{
    if (!b) return;
    set_bg(b, on ? col : kWell);
    set_ink(lv_obj_get_child(b, 0), on ? kInk : kDim);
}

/* The ruler held, then dragged: a loop region from the bar it was held on
 * to the bar under the finger, drawn as it grows and set, looping, on the
 * release. */
static lv_obj_t* s_rulerBand = nullptr;
static int32_t   s_rulerFrom = -1;     ///< the bar held on; -1 while no region is being drawn
static int32_t   s_rulerTo = -1;

/// The bar under the finger on the ruler, or -1.
static int32_t ruler_bar(lv_event_t* e)
{
    lv_indev_t* in = lv_indev_active();
    if (!in || s_pxPerBar <= 0) return -1;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t a;
    lv_obj_get_coords((lv_obj_t*)lv_event_get_target(e), &a);
    const int32_t x = p.x - a.x1;
    return x < 0 ? -1 : x / s_pxPerBar;
}

/// A tap on the ruler: the playhead to that bar. Held and dragged: a loop
/// region (rebuild_timeline()).
static void ruler_cb(lv_event_t* e)
{
    auto* se = engine();
    if (!se || !s_timeline) return;
    const lv_event_code_t code = lv_event_get_code(e);
    const int32_t bar = ruler_bar(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        if (bar < 0) return;
        s_rulerFrom = s_rulerTo = bar;
        lv_obj_remove_flag((lv_obj_t*)lv_event_get_target(e), LV_OBJ_FLAG_SCROLL_CHAIN);   // the lanes stay put
        s_rulerBand = lv_obj_create(s_timeline);
        lv_obj_remove_style_all(s_rulerBand);
        lv_obj_set_style_bg_color(s_rulerBand, kAccent, 0);
        lv_obj_set_style_bg_opa(s_rulerBand, LV_OPA_50, 0);
        lv_obj_remove_flag(s_rulerBand, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(s_rulerBand, bar * s_pxPerBar, 0);
        lv_obj_set_size(s_rulerBand, s_pxPerBar, kRulerH);
        note_fmt("Loop region: bar %d", (int)bar + 1);
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (s_rulerFrom < 0 || bar < 0 || bar == s_rulerTo || !s_rulerBand) return;
        s_rulerTo = bar;
        const int32_t a = std::min(s_rulerFrom, s_rulerTo), b = std::max(s_rulerFrom, s_rulerTo) + 1;
        lv_obj_set_pos(s_rulerBand, a * s_pxPerBar, 0);
        lv_obj_set_width(s_rulerBand, (b - a) * s_pxPerBar);
        note_fmt("Loop region: bars %d - %d", (int)a + 1, (int)b);
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_rulerFrom < 0) return;
        const uint32_t a = (uint32_t)std::min(s_rulerFrom, s_rulerTo), b = (uint32_t)std::max(s_rulerFrom, s_rulerTo) + 1;
        s_rulerFrom = s_rulerTo = -1;
        s_rulerBand = nullptr;           // the rebuild deletes it with the lanes
        if (code == LV_EVENT_PRESS_LOST) {
            lv_async_call([](void*) { rebuild_timeline(); }, nullptr);   // not from inside the ruler's own event
            return;
        }
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            auto& song = se->song();
            song.loopStart = a;
            song.loopEnd = b;
            song.loop = true;
        }
        se->commit();
        /* Next turn of the loop: the rebuild deletes the ruler whose event this is. */
        lv_async_call([](void*) { rebuild_timeline(); sync_transport(); update_info(); }, nullptr);
        note_fmt("Looping bars %u - %u.", (unsigned)a + 1, (unsigned)b);
        return;
    }
    /* CLICKED: a tap -- but not the release of a region just drawn. */
    if (bar < 0) return;
    if (se->playing() && se->seqTake() == crosspad::SyncTake::None && !crosspad::arrangeAudioTake().recording()) {
        se->play((uint32_t)bar);
    } else if (!se->playing()) {
        crosspad::songTransport().setStartBar((uint32_t)bar);
    }
}

void rebuild_timeline()
{
    auto* se = engine();
    if (!se || !s_timeline) return;
    /* The lanes go, the ruler and a region being drawn on it with them. */
    s_rulerBand = nullptr;
    s_rulerFrom = s_rulerTo = -1;
    drag_forget();
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
                std::any_of(cs.begin(), cs.end(), [](const crosspad::SongClip& c) { return !c.file.empty() && !c.isElement(); })) {
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

    /* Only the tracks somebody made. They are always the first of them:
     * a track is taken from the lowest free one and removing one closes the
     * gap (AudioSong::freeTrack(), removeTrack()), so "in use" and "before
     * usedTracks()" are the same set. */
    const int used = song.usedTracks();
    /* The lanes in use, and the empty ones under them up to a screenful. */
    const int lanes = std::max(used, kShownTracks);
    /* Drawn blocks per track: a long track must not use up the ones the others need. */
    const int perTrack = kMaxBlocks / lanes;
    size_lanes(std::max(used < kTracks ? used + 1 : used, kShownTracks));
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
            lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
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
        /* A seq track's takes are lettered in the order they first appear:
         * a DUP shares its notes (copy-on-write), so every copy of a part
         * carries the part's letter and the song's form reads off the lane --
         * A A B A. */
        std::vector<const void*> parts;
        for (int i = 0; i < (int)cs.size() && i < perTrack && s_blockCount < kMaxBlocks; ++i) {
            const auto& c = cs[i];
            lv_obj_t* o = lv_obj_create(s_timeline);
            lv_obj_remove_style_all(o);
            lv_obj_set_pos(o, (int32_t)c.bar * s_pxPerBar, y);
            lv_obj_set_size(o, std::max<int32_t>(2, (int32_t)c.bars * s_pxPerBar - 1), kLaneH);
            lv_obj_set_style_radius(o, 3, 0);
            /* Coloured as its track: its type's, like the badge (UX 2000 section 7), or the player's. */
            lv_obj_set_style_bg_color(o, track_color(song.tracks[tr]), 0);
            lv_obj_set_style_bg_opa(o, song.silenced(tr) ? LV_OPA_30 : LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(o, 2, 0);
            lv_obj_set_style_pad_top(o, 1, 0);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
            lv_obj_add_event_cb(o, block_cb, LV_EVENT_FOCUSED, (void*)(intptr_t)s_blockCount);
            lv_obj_add_event_cb(o, block_cb, LV_EVENT_CLICKED, (void*)(intptr_t)s_blockCount);
            lv_obj_add_event_cb(o, block_cb, LV_EVENT_PRESSED, (void*)(intptr_t)s_blockCount);
            for (lv_event_code_t ev : {LV_EVENT_LONG_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
                lv_obj_add_event_cb(o, block_drag_cb, ev, (void*)(intptr_t)s_blockCount);
            /* An element has no waveform and no notes of its own to draw
             * here: it is a whole arrangement, baked. */
            if (!c.isElement()) {
                lv_obj_add_event_cb(o, seq ? seq_draw_cb : block_draw_cb, LV_EVENT_DRAW_MAIN_END,
                                    (void*)(intptr_t)s_blockCount);
            }

            lv_obj_t* lb = lv_label_create(o);
            lv_obj_set_style_text_font(lb, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(lb, kInk, 0);
            lv_label_set_long_mode(lb, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(lb, LV_PCT(100));
            /* A block of its own notes is the seq track's part; one that
             * plays the scene says which; an element's is styled with its Block. */
            if (!c.isElement()) {
                if (seq && c.notes) {
                    auto it = std::find(parts.begin(), parts.end(), (const void*)c.notes.get());
                    const size_t n = (size_t)(it - parts.begin());
                    if (it == parts.end()) parts.push_back(c.notes.get());
                    if (n < 26) set_text_fmt(lb, "%c", 'A' + (int)n);
                    else        set_text_fmt(lb, "%c%u", 'A' + (int)(n % 26), (unsigned)(n / 26));
                }
                else if (seq)       set_text_fmt(lb, "Seq %u", (unsigned)(song.tracks[tr].scene + 1));
                else                lv_label_set_text(lb, short_name(c.file).c_str());
            }

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
            b.element = c.isElement();
            if (b.element) elements_style_block(o, lb, c, b);
            b.seq = seq;
            b.notes = c.notes;
            b.scene = song.tracks[tr].scene;
            b.from = c.from;
            crosspad::ClipPeaks cp;
            if (!seq && !b.element && se->clipPeaks(c.file, cp)) set_peaks(b, cp);
            s_blocks[s_blockCount++] = std::move(b);
        }
    }

    /* The end of the song: a line, and what lies past it dimmed -- blocks
     * there are kept but not played. */
    if (end) {
        const int32_t h = kRulerH + 2 + lanes * (kLaneH + kLaneGap);
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
        const int32_t h = kRulerH + 2 + lanes * (kLaneH + kLaneGap);
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

    /* The ruler takes a finger: a tap puts the playhead on that bar -- where
     * the next PLAY starts, or, playing, where the song goes on from. A drag
     * still scrolls the lanes (no CLICKED after a scroll). Taller than the
     * ruler's 10 px, over the top of the lanes, so a fingertip finds it. */
    {
        lv_obj_t* r = lv_obj_create(s_timeline);
        lv_obj_remove_style_all(r);
        lv_obj_set_pos(r, 0, 0);
        lv_obj_set_size(r, (int32_t)bars * s_pxPerBar, kRulerH + 4);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_group_remove_obj(r);
        lv_obj_add_flag(r, LV_OBJ_FLAG_PRESS_LOCK);
        for (lv_event_code_t ev : {LV_EVENT_SHORT_CLICKED, LV_EVENT_LONG_PRESSED, LV_EVENT_PRESSING,
                                   LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
            lv_obj_add_event_cb(r, ruler_cb, ev, nullptr);
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
            const std::string title = track_title(song, t);
            /* Dim while it has only its kind for a name. */
            const bool named = !tr.name.empty() || !kit_name(track_kit(song, t)).empty();
            /* Frozen, it plays its file, not its engine (auto_freeze()). */
            const auto fz = tr.kind == crosspad::TrackKind::Seq ? se->freezeState((uint8_t)t)
                                                                : crosspad::FreezeState::None;
            const bool frozen = fz == crosspad::FreezeState::Frozen && song.armed != t;
            /* A kit track the kits' engine is not playing: its last freeze
             * stands in (faint snowflake), and with none it is silent until
             * one is made (live_kit()). */
            const bool noEngine = song.seqRoute(t) == 0 && song.liveRoute(t) < 0 && song.armed != t;
            const bool standIn = noEngine && fz == crosspad::FreezeState::Stale;
            const bool parked = noEngine && fz == crosspad::FreezeState::None;
            /* Played by its engine (not its freeze): whether that has its sound. */
            const TrackSound sound = frozen || noEngine ? TrackSound::Ok : track_sound(song, t);
            /* A snowflake says frozen, and a faint one freezing: the name
             * stays readable beside it instead of losing its start to "FRZ". */
            const bool bouncing = fz == crosspad::FreezeState::Freezing && s_bouncing == t;
            const bool ice = (frozen || standIn || fz == crosspad::FreezeState::Freezing) && !bouncing;
            if (s_headIce[t]) {
                if (ice) lv_obj_remove_flag(s_headIce[t], LV_OBJ_FLAG_HIDDEN);
                else     lv_obj_add_flag(s_headIce[t], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_image_opa(s_headIce[t], frozen ? LV_OPA_COVER : LV_OPA_50, 0);
            }
            const bool mon = track_monitored(song, t);
            if (s_headMon[t]) {
                if (mon) lv_obj_remove_flag(s_headMon[t], LV_OBJ_FLAG_HIDDEN);
                else     lv_obj_add_flag(s_headMon[t], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_set_x(s_headName[t], ice || mon ? 21 : 5);
            lv_obj_set_width(s_headName[t], ice || mon ? kHeadW - 26 : kHeadW - 10);
            const std::string shownText = bouncing ? std::string("BOUNCING...")
                                        : parked   ? "PARKED " + title
                                        : sound == TrackSound::Loading ? "LOADING " + title
                                        : sound == TrackSound::Failed  ? "! " + title
                                                   : title;
            set_text(s_headName[t], shownText.c_str());
            /* A name that does not fit is set smaller, not cut: "VINTAG" and
             * "sub_bas" were two kits nobody could tell apart. */
            {
                lv_point_t sz;
                lv_text_get_size(&sz, shownText.c_str(), &lv_font_montserrat_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                const lv_font_t* f = sz.x > lv_obj_get_width(s_headName[t]) ? &lv_font_montserrat_10 : &lv_font_montserrat_14;
                if (lv_obj_get_style_text_font(s_headName[t], LV_PART_MAIN) != f) {
                    lv_obj_set_style_text_font(s_headName[t], f, 0);
                    lv_obj_set_y(s_headName[t], f == &lv_font_montserrat_14 ? 11 : 14);
                    /* Too long even small: it ends in dots, so it reads as cut, not as a name. */
                    lv_label_set_long_mode(s_headName[t], f == &lv_font_montserrat_14 ? LV_LABEL_LONG_CLIP : LV_LABEL_LONG_DOT);
                    lv_obj_set_height(s_headName[t], lv_font_get_line_height(f));   // one line, or DOT wraps it
                }
            }
            set_ink(s_headName[t], fz == crosspad::FreezeState::Freezing || frozen ? kIce
                                   : sound == TrackSound::Failed ? kRec
                                   : parked || sound == TrackSound::Loading || s_trackQuiet[t] || !named ? kDim : kText);
        }
        style_toggle(s_headSolo[t], tr.solo, kKitT);
        style_toggle(s_headMute[t], tr.muted, kMute);
        if (s_headArm[t]) {
            const bool seq = song.seqRoute(t) >= 0;
            const bool audio = tr.inUse && tr.kind == crosspad::TrackKind::Audio;
            if (seq || audio) lv_obj_remove_flag(s_headArm[t], LV_OBJ_FLAG_HIDDEN);
            else              lv_obj_add_flag(s_headArm[t], LV_OBJ_FLAG_HIDDEN);
            set_bg(s_headArm[t], track_armed(song, t) ? kRec : kWell);
        }
    }

    /* "+ ADD TRACK", in the slot of the first track nobody has made -- and
     * gone once all of them are. The counter beside it is the rule the player
     * needs before choosing: how many tracks are left, and how many seqs. */
    if (s_addRow) {
        if (used >= kTracks) {
            lv_obj_add_flag(s_addRow, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_addRow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(s_addRow, kRulerH + 4 + used * (kLaneH + kLaneGap));
            if (s_addInfo)
                set_text_fmt(s_addInfo, "%d / %d tracks  -  seq %d", used, kTracks, song.seqTracks());
        }
    }
    lv_label_set_text(s_projLabel, song.name.empty() ? "(no card)" : song.name.c_str());

    /* The selection by what it was, not where: an edit may have re-sorted. */
    const auto& sel = song.tracks[s_selTrack].clips;
    if (s_selIndex >= (int)sel.size()) s_selIndex = (int)sel.size() - 1;
    s_shape = song_shape(song);
    elements_refresh_crumbs();
    rebuild_group();
    place_move_arrows();
    select(s_selTrack, s_selIndex);
}

} // namespace arrange_ui
