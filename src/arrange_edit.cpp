// Arrange -- editing: block tools (MOVE, DUP, DEL), the header and key callbacks, the loop region,
// merging tracks and the EDIT menu.

#include "arrange_internal.hpp"

namespace arrange_ui {

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

static void move_arrow_cb(lv_event_t* e);

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
static void set_move_axis(MoveAxis a);

static void leave_tools()
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

static void scroll_to_selected()
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
    const auto& src = song.tracks[from];
    if (src.kind != crosspad::TrackKind::Audio) {
        /* A seq block goes to the other kit track and plays its notes through
         * that track's kit. One that plays its track's scene has no notes of
         * its own to take along, so it stays. */
        const bool own = s_selIndex >= 0 && s_selIndex < (int)src.clips.size() && src.clips[(size_t)s_selIndex].notes;
        if (src.source != crosspad::SeqSource::Kit || !own) return -1;
        for (int t = from + dir; t >= 0 && t < kTracks; t += dir) {
            if (song.seqRoute(t) >= 0 && song.tracks[t].source == crosspad::SeqSource::Kit) return t;
        }
        return -1;
    }
    for (int t = from + dir; t >= 0 && t < kTracks; t += dir) {
        const auto& tr = song.tracks[t];
        if (tr.inUse && tr.kind == crosspad::TrackKind::Audio) return t;
    }
    return -1;
}

/// The selected block to the track above (-1) or below (+1), on its bar.
static void move_track(int dir)
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

static void move_bars(int step)
{
    edit_selected([step](crosspad::AudioSong&, crosspad::SongClip& c) {
        if (step < 0 && c.bar) --c.bar;
        else if (step > 0)     ++c.bar;
        return c;
    });
}

static void set_move_axis(MoveAxis a)
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
static void move_arrow_cb(lv_event_t* e)
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

static void leave_region_edit();

/// The power button's click goes back one step, and only one: a window, the
/// loop region being turned, MOVE being turned, the keys a block opened --
/// and out of the app with none of them left. A block that is only picked is
/// not a step: the encoder picks one by walking over it, the pick outlives
/// the screen, and a click spent unpicking it showed nothing but a 2 px
/// outline going, so getting out took two clicks or more (12.10.2026). (The
/// encoder's edit mode and the top bar's focus are stepped out of before this
/// is asked: powerBack().) Inside an element, what is left goes up one level
/// (elements_back()); while that is being baked, a click is spent on waiting.
bool back_handler(lv_obj_t*)
{
    if (crosspad::elementNav().busy()) return true;
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
    return elements_back();
}

/// A long press on a track's header: an audio track's input (with MIX beside
/// it in the same sheet), MIX straight away on any other.
void head_hold_cb(lv_event_t* e)
{
    auto* se = engine();
    if (!se) return;
    const int t = (int)(intptr_t)lv_event_get_user_data(e);
    bool audio = false;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& tr = se->song().tracks[t];
        audio = tr.inUse && tr.kind == crosspad::TrackKind::Audio;
    }
    if (audio) input_sheet(t);
    else       mix_cb(e);
}

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

/// FOLLOW: the lanes scroll with the playhead while the song plays, or stay
/// put so a block can be edited while it does. Turned back on, they go to
/// the playhead at once.
void follow_cb(lv_event_t*)
{
    s_follow = !s_follow;
    sync_transport();
    auto* se = engine();
    if (!s_follow || !se || !s_timeline) return;
    const double at = se->playing() ? se->positionBars() : (double)crosspad::songTransport().startBar();
    const int32_t x = (int32_t)(at * s_pxPerBar);
    const int32_t view = lv_obj_get_content_width(s_timeline);
    lv_obj_scroll_to_x(s_timeline, x > view / 3 ? x - view / 3 : 0, LV_ANIM_ON);
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
static void zoom(int dir)
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

// ── the loop region ────────────────────────────────────────────────────────

static void region_changed()
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
static void enter_region_edit()
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

static void leave_region_edit()
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
static uint32_t prepare_bar_edit(crosspad::AudioSong& song)
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

static void region_copy_cb(lv_event_t*)
{
    region_edit([](crosspad::AudioSong& song) {
        const uint32_t spb = prepare_bar_edit(song);
        s_clipboard = song.copyRange(song.loopStart, song.loopEnd, spb);
        s_clipboardBars = song.loopEnd - song.loopStart;
    });
}

/// The copied bars in at the region's start; what was there moves on, cut
/// where the region starts if it crossed it.
static void region_paste_cb(lv_event_t*)
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
static void region_dup_cb(lv_event_t*)
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
static void cut_cb(lv_event_t*)
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

static void region_clear_cb(lv_event_t*)
{
    region_edit([](crosspad::AudioSong& song) { song.loopStart = song.loopEnd = 0; });
}

static void region_block_cb(lv_event_t*)
{
    region_edit([](crosspad::AudioSong& song) {
        const auto& cs = song.tracks[s_selTrack].clips;
        if (s_selIndex < 0 || s_selIndex >= (int)cs.size()) return;
        song.loopStart = cs[s_selIndex].bar;
        song.loopEnd = cs[s_selIndex].bar + cs[s_selIndex].bars;
    });
}

static void region_set_cb(lv_event_t*)
{
    close_overlay();
    enter_region_edit();
}

// ── merging tracks ─────────────────────────────────────────────────────────

/// The tracks in @p mask, over the region or the whole song, into one clip.
static void run_merge(uint8_t mask)
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

static void paint_merge_toggles()
{
    for (int t = 0; t < kTracks; ++t) {
        lv_obj_t* b = s_mergeToggles[t];
        if (!b) continue;
        const bool on = s_mergeMask & (1u << t);
        lv_obj_set_style_bg_color(b, on ? kAccent : kPanel, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(b, 0), on ? kInk : kText, 0);
    }
}

static void merge_toggle_cb(lv_event_t* e)
{
    s_mergeMask ^= (uint8_t)(1u << (int)(intptr_t)lv_event_get_user_data(e));
    paint_merge_toggles();
}

static void merge_tracks_cb(lv_event_t*)
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

static void merge_all_cb(lv_event_t*) { run_merge(0x0F); }

// ── the EDIT menu ──────────────────────────────────────────────────────────

/// UNDO / REDO: the arrangement a step back or forward (ISongEngine::undo).
/// The timeline repaints from the song's shape on the next tick.
static void undo_redo(bool back)
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
    overlay_item(LV_SYMBOL_PLUS " New element here", element_new_cb, nullptr);
    overlay_item(LV_SYMBOL_LIST " Element from library ...", element_library_cb, nullptr);
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
        overlay_item(LV_SYMBOL_LIST " Make element from region", element_region_cb, nullptr);
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

} // namespace arrange_ui
