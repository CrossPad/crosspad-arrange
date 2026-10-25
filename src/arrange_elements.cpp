// Arrange -- elements: arrangements inside the song, entered with CLIP EDIT, left with
// the power button, and the breadcrumb that says which one is open.
//
// An element is a project of its own under the song's .elements/ folder, played in the
// song as a block made of a bake of it. Everything that moves between them is
// crosspad::elementNav(), a job of requests to the song engine that this screen's
// timer walks (elements_tick), so nothing here touches the card.

#include "arrange_internal.hpp"

namespace arrange_ui {

namespace {

constexpr size_t      kLevelsShown    = 3;      ///< crumbs on the row; deeper, "..." stands for the first
constexpr int32_t     kCrumbH         = 22;     ///< as every key on the context row
constexpr int32_t     kCrumbSongW     = 38;
constexpr int32_t     kCrumbMoreW     = 20;
constexpr int32_t     kCrumbMidW      = 52;
constexpr int32_t     kSepW           = 6;
constexpr int32_t     kCrumbPad       = 4;      ///< a crumb's text, less its key
constexpr int32_t     kMoreKeyW       = 42;     ///< ZOOM's width, the place it takes
constexpr int32_t     kGlyphW         = 7;      ///< the layers glyph at an element block's left
constexpr int32_t     kGlyphBarH      = 2;
constexpr int32_t     kGlyphGap       = 2;
constexpr int         kGlyphBars      = 3;
constexpr int32_t     kGlyphTop       = 3;
constexpr int32_t     kGlyphLabelGap  = 4;
constexpr uint32_t    kToastMs        = 3000;
constexpr int         kLibraryRows    = 32;
constexpr char        kCrumbKeyNone[] = "\x01";   ///< no crumbs drawn yet

lv_obj_t* s_crumbRow = nullptr;
lv_obj_t* s_moreKey  = nullptr;
std::string s_crumbKey = kCrumbKeyNone;       ///< what the row was last built from

/// The element library as the engine last listed it: block and crumb titles,
/// the next "ELEMENT n", the picker's rows.
std::vector<crosspad::ElementInfo> s_library;
bool     s_libraryWanted = true;          ///< a title is not known: ask the engine for the list
bool     s_libraryKnown  = false;         ///< the list came in since an element was last made or opened
bool     s_libraryAsked  = false;
uint32_t s_libraryGen    = 0;

/// Where the level being left sat in the song above, for the click that lands there.
int      s_returnTrack = -1;
uint32_t s_returnBar   = 0;

bool       s_busy = false;                 ///< the shield and the empty encoder group are up
lv_obj_t*  s_shield = nullptr;
#ifdef ARR_USE_ENCODER
lv_group_t* s_busyGroup = nullptr;
#endif

const crosspad::ElementInfo* find_element(const std::string& ref)
{
    for (const auto& e : s_library)
        if (e.ref == ref) return &e;
    return nullptr;
}

/// The title of the level, from the library when the level has none yet.
std::string level_title(const crosspad::ElementNav::Level& lv)
{
    if (!lv.title.empty()) return lv.title;
    return element_title(lv.open);
}

void upper(char* s)
{
    for (; *s; ++s) *s = (char)std::toupper((unsigned char)*s);
}

// ── the breadcrumb ─────────────────────────────────────────────────────────

void leave_to(int level)
{
    auto& nav = crosspad::elementNav();
    const auto& path = nav.path();
    if (level < 0 || level >= nav.depth()) return;
    const auto& below = path[(size_t)level + 1];
    s_returnTrack = below.track;
    s_returnBar = below.bar;
    if (!nav.leaveTo(level)) note(nav.lastError().c_str());
}

void crumb_cb(lv_event_t* e)
{
    leave_to((int)(intptr_t)lv_event_get_user_data(e));
}

void level_pick_cb(lv_event_t* e)
{
    const int level = (int)(intptr_t)lv_event_get_user_data(e);
    close_overlay();
    leave_to(level);
}

/// "...": the whole path, a level a row; a tap on one goes up to it.
void path_cb(lv_event_t*)
{
    if (s_overlay) return;
    auto& nav = crosspad::elementNav();
    overlay_list("Where you are");
    const auto& path = nav.path();
    for (size_t k = 0; k < path.size(); ++k) {
        const bool here = (int)k == nav.depth();
        const std::string name = k == 0 ? "SONG" : level_title(path[k]);
        overlay_item(name.c_str(), here ? cancel_cb : level_pick_cb, (void*)(intptr_t)k, here);
    }
    overlay_item(LV_SYMBOL_CLOSE " Cancel", cancel_cb, nullptr);
}

lv_obj_t* make_sep(lv_obj_t* row)
{
    lv_obj_t* l = lv_label_create(row);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(l, kDim, 0);
    lv_obj_set_width(l, kSepW);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, LV_SYMBOL_RIGHT);
    return l;
}

/// A crumb: a key in the context row's size. The last, where the player is, lit
/// as a lit LOOP is.
void make_crumb(lv_obj_t* row, int32_t w, const std::string& text, int level, bool last)
{
    lv_obj_t* b = make_button(row, w, "", crumb_cb, (void*)(intptr_t)level);
    lv_obj_t* l = lv_obj_get_child(b, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, w - kCrumbPad, lv_font_get_line_height(&lv_font_montserrat_10));   // one line: DOT only cuts a label that cannot grow
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text.c_str());
    if (last) {
        lv_obj_set_style_bg_color(b, kAccent, 0);
        lv_obj_set_style_text_color(l, kInk, 0);
    }
    lv_obj_set_width(b, w);
}

void layout_context_row(bool inside)
{
    for (lv_obj_t* o : {s_projBtn, s_posLabel, s_syncBtn, s_editBtn}) {
        if (!o) continue;
        if (inside) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_crumbRow) {
        if (inside) lv_obj_remove_flag(s_crumbRow, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(s_crumbRow, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_moreKey) {
        if (inside) lv_obj_remove_flag(s_moreKey, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(s_moreKey, LV_OBJ_FLAG_HIDDEN);
    }
    const CtxKeys& k = inside ? kCtxElement : kCtxSong;
    if (s_followBtn) lv_obj_set_x(s_followBtn, k.follow);
    if (s_loopBtn)   lv_obj_set_x(s_loopBtn, k.loop);
    if (s_zoomBtn)   lv_obj_set_x(s_zoomBtn, k.zoom);
}

// ── the engine's list of elements ──────────────────────────────────────────

void ask_library(crosspad::ISongEngine* se)
{
    if (s_libraryAsked) {
        if (se->listGeneration() == s_libraryGen) return;
        s_libraryAsked = false;
        s_library = se->elements();
        s_libraryWanted = false;
        s_libraryKnown = true;
        rebuild_timeline();
        return;
    }
    /* Not under a list another request is waiting for (Pending), whose
     * generation this one would move. */
    if (!s_libraryWanted || s_overlay || s_pending != Pending::None) return;
    s_libraryGen = se->listGeneration();
    s_libraryAsked = se->requestElements();
}

// ── the lane an element is laid on ─────────────────────────────────────────

/// A track with no block over bars [bar, bar + bars): the selected audio track
/// if that is free there, else the first track nobody has made, else any audio
/// track free there. -1 when there is none.
int free_track(const crosspad::AudioSong& song, uint32_t bar, uint32_t bars)
{
    auto freeAt = [&](int t) {
        for (const auto& c : song.tracks[t].clips)
            if (bar < c.bar + c.bars && c.bar < bar + bars) return false;
        return true;
    };
    const auto& sel = song.tracks[s_selTrack];
    if (sel.inUse && sel.kind == crosspad::TrackKind::Audio && freeAt(s_selTrack)) return s_selTrack;
    for (int t = 0; t < kTracks; ++t)
        if (!song.tracks[t].inUse) return t;
    for (int t = 0; t < kTracks; ++t)
        if (song.tracks[t].kind == crosspad::TrackKind::Audio && freeAt(t)) return t;
    return -1;
}

void library_pick_cb(lv_event_t* e)
{
    const size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    auto* se = engine();
    close_overlay();
    if (!se || i >= s_library.size()) return;
    auto& nav = crosspad::elementNav();
    if (nav.busy()) return;
    const uint32_t bar = playhead_bar();
    int track = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        track = free_track(se->song(), bar, std::max<uint16_t>(1, s_library[i].bars));
    }
    if (track < 0) { note("No audio track is free there."); return; }
    if (!nav.insertExisting(track, bar, s_library[i].ref)) note(nav.lastError().c_str());
}

// ── while a job runs ───────────────────────────────────────────────────────

/// A job runs: a touch lands on a shield over the screen and the encoder on a
/// group with nothing in it, so no key does anything until the song is back.
void set_busy(bool busy)
{
    if (busy == s_busy) return;
    s_busy = busy;
    if (busy) {
        s_shield = lv_obj_create(s_root);
        lv_obj_remove_style_all(s_shield);
        lv_obj_set_size(s_shield, LV_PCT(100), LV_PCT(100));
        lv_obj_add_flag(s_shield, LV_OBJ_FLAG_FLOATING);
        lv_obj_add_flag(s_shield, LV_OBJ_FLAG_CLICKABLE);
        if (s_procBox) lv_obj_move_foreground(s_procBox);
#ifdef ARR_USE_ENCODER
        if (!s_busyGroup) s_busyGroup = lv_group_create();
        if (indev_encoderTop) lv_indev_set_group(indev_encoderTop, s_busyGroup);
#endif
        return;
    }
    if (s_shield) { lv_obj_delete_async(s_shield); s_shield = nullptr; }
#ifdef ARR_USE_ENCODER
    if (indev_encoderTop && s_encGroup) lv_indev_set_group(indev_encoderTop, s_overlayGroup && s_overlay ? s_overlayGroup : s_encGroup);
    if (s_busyGroup) { lv_group_delete_deferred(s_busyGroup); s_busyGroup = nullptr; }
#endif
}

/// The element block the leave came from, picked again in the song it returned to.
void select_returned()
{
    auto* se = engine();
    if (!se || s_returnTrack < 0 || s_returnTrack >= kTracks) return;
    int index = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& cs = se->song().tracks[s_returnTrack].clips;
        for (size_t i = 0; i < cs.size(); ++i)
            if (cs[i].isElement() && cs[i].bar == s_returnBar) { index = (int)i; break; }
    }
    s_selTrack = s_returnTrack;
    s_selIndex = index;
    crosspad::songTransport().setStartBar(s_returnBar);
    s_returnTrack = -1;
}

} // namespace

const char* element_title(const std::string& ref)
{
    const auto* e = find_element(ref);
    if (e && !e->title.empty()) return e->title.c_str();
    /* Asked for once a change: an element the list does not hold (or holds
     * unnamed) is not asked for again each time the timeline is drawn. */
    if (!e && !s_libraryKnown) s_libraryWanted = true;
    const size_t skip = ref.rfind('/');
    return ref.c_str() + (skip == std::string::npos ? 0 : skip + 1);
}

void elements_build_crumbs(lv_obj_t* parent)
{
    s_crumbRow = lv_obj_create(parent);
    lv_obj_remove_style_all(s_crumbRow);
    lv_obj_set_size(s_crumbRow, kCrumbsW, kCrumbH);
    lv_obj_set_pos(s_crumbRow, 6, ARR_Y(35));
    lv_obj_remove_flag(s_crumbRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_crumbRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_crumbRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_crumbRow, LV_OBJ_FLAG_HIDDEN);
    /* EDIT, where ZOOM stood: the song's own EDIT is hidden inside an element. */
    s_moreKey = make_button(parent, kMoreKeyW, "...", edit_cb);
    lv_obj_set_style_text_font(lv_obj_get_child(s_moreKey, 0), &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_moreKey, kCtxMoreX, ARR_Y(35));
    lv_obj_add_flag(s_moreKey, LV_OBJ_FLAG_HIDDEN);
    s_crumbKey = kCrumbKeyNone;
}

void elements_refresh_crumbs()
{
    auto& nav = crosspad::elementNav();
    const bool inside = nav.depth() > 0;
    layout_context_row(inside);
    if (!s_crumbRow) return;
    const auto& path = nav.path();
    std::string key;
    for (const auto& lv : path) key += level_title(lv) + '\n';
    if (key == s_crumbKey) return;
    s_crumbKey = key;
    lv_obj_clean(s_crumbRow);
    if (!inside) return;

    /* The last three levels; deeper, "..." is the ones before them. */
    const size_t n = path.size();
    const bool cut = n > kLevelsShown;
    const size_t first = cut ? n - (kLevelsShown - 1) : 0;
    int32_t used = 0;
    auto sep = [&] { make_sep(s_crumbRow); used += kSepW; };
    if (cut) {
        lv_obj_t* b = make_button(s_crumbRow, kCrumbMoreW, "...", path_cb);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_14, 0);
        used += kCrumbMoreW;
        sep();
    }
    for (size_t k = first; k < n; ++k) {
        const bool last = k == n - 1;
        const int32_t w = last ? std::max<int32_t>(kCrumbMoreW, kCrumbsW - used)
                        : k == 0 ? kCrumbSongW : kCrumbMidW;
        make_crumb(s_crumbRow, w, k == 0 ? std::string("SONG") : level_title(path[k]), (int)k, last);
        used += w;
        if (!last) sep();
    }
}

#ifdef ARR_USE_ENCODER
void elements_group_add(lv_group_t* g)
{
    if (s_crumbRow) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(s_crumbRow); ++i) {
            lv_obj_t* c = lv_obj_get_child(s_crumbRow, (int32_t)i);
            if (lv_obj_check_type(c, &lv_button_class)) lv_group_add_obj(g, c);
        }
    }
    if (s_moreKey) lv_group_add_obj(g, s_moreKey);
}
#endif

void elements_tick(crosspad::ISongEngine* se)
{
    auto& nav = crosspad::elementNav();
    const auto r = nav.poll();
    set_busy(nav.busy());
    /* The job waits for a stopped song (a leave while it plays, fitting copies
     * of stretched audio), and the top bar's PLAY is outside the shield. */
    if (nav.busy() && se->playing()) se->stop();
    if (r.done) {
        if (!r.ok) {
            ui_toast_show(r.error.empty() ? nav.lastError().c_str() : r.error.c_str(), kToastMs, UI_TOAST_ERROR);
        } else if (r.job == crosspad::ElementNav::Job::Leave) {
            select_returned();
        } else {
            /* In a new level the song's playhead is not where this one begins. */
            select(s_selTrack, -1);
            crosspad::songTransport().setStartBar(0);
        }
        s_libraryWanted = true;
        s_libraryKnown = false;
        rebuild_timeline();
    }
    ask_library(se);
}

bool elements_back()
{
    auto& nav = crosspad::elementNav();
    if (nav.depth() == 0) return false;
    leave_to(nav.depth() - 1);
    return true;
}

bool elements_clip_edit()
{
    auto* se = engine();
    if (!se) return false;
    uint32_t bar = 0;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& cs = se->song().tracks[s_selTrack].clips;
        if (s_selIndex < 0 || s_selIndex >= (int)cs.size() || !cs[(size_t)s_selIndex].isElement()) return false;
        bar = cs[(size_t)s_selIndex].bar;
    }
    auto& nav = crosspad::elementNav();
    if (!nav.enter(s_selTrack, bar)) note(nav.lastError().c_str());
    return true;
}

void elements_style_block(lv_obj_t* block, lv_obj_t* label, const crosspad::SongClip& c)
{
    lv_obj_set_style_bg_color(block, kIce, 0);
    /* Not baked yet: it has no sound of its own to show, and says so. A silenced
     * track's block is already faded; that is kept. */
    const bool baked = !c.file.empty();
    if (!baked && lv_obj_get_style_bg_opa(block, LV_PART_MAIN) == LV_OPA_COVER)
        lv_obj_set_style_bg_opa(block, LV_OPA_50, 0);
    /* Three bars stacked: layers. Inert, so a touch lands on the block. */
    for (int i = 0; i < kGlyphBars; ++i) {
        lv_obj_t* bar = lv_obj_create(block);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, kGlyphW, kGlyphBarH);
        lv_obj_set_pos(bar, 0, kGlyphTop + i * (kGlyphBarH + kGlyphGap));
        lv_obj_set_style_bg_color(bar, kInk, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_x(label, kGlyphW + kGlyphLabelGap);
    set_text_fmt(label, baked ? "%s" : "%s - not baked", element_title(c.element));
}

void element_new_cb(lv_event_t*)
{
    auto* se = engine();
    close_overlay();
    auto& nav = crosspad::elementNav();
    if (!se || nav.busy()) return;
    const uint32_t bar = playhead_bar();
    int track = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        track = free_track(se->song(), bar, crosspad::ElementNav::kNewElementBars);
    }
    if (track < 0) { note("No audio track is free there."); return; }
    char title[32];
    std::snprintf(title, sizeof(title), "ELEMENT %u", (unsigned)s_library.size() + 1);
    if (!nav.insertNew(track, bar, title)) note(nav.lastError().c_str());
}

void element_library_cb(lv_event_t*)
{
    close_overlay();
    request_list(Pending::Elements, "Element from library");
}

void fill_elements()
{
    auto* se = engine();
    s_library = se->elements();
    s_libraryWanted = false;
    s_libraryKnown = true;
    for (size_t i = 0; i < s_library.size() && i < (size_t)kLibraryRows; ++i) {
        char row[64];
        lv_snprintf(row, sizeof(row), "%s  (%u bars)", element_title(s_library[i].ref), (unsigned)s_library[i].bars);
        overlay_item(row, library_pick_cb, (void*)(intptr_t)i);
    }
    if (s_library.empty()) {
        lv_obj_t* l = lv_label_create(s_overlay);
        lv_obj_set_style_text_color(l, kDim, 0);
        lv_label_set_text(l, "No elements yet -- EDIT > New element here.");
    }
    overlay_item(LV_SYMBOL_CLOSE " Cancel", cancel_cb, nullptr);
}

bool element_progress(char* buf, size_t cap)
{
    auto& nav = crosspad::elementNav();
    if (!nav.busy()) return false;
    char stage[16];
    std::snprintf(stage, sizeof(stage), "%s", nav.stage()[0] ? nav.stage() : "working");
    upper(stage);
    std::snprintf(buf, cap, "%s ELEMENT\n%u%%", stage, (unsigned)nav.progress());
    return true;
}

bool elements_list_pending()
{
    return s_libraryAsked;
}

void elements_destroy()
{
    /* The widgets go with the screen's root; the job does not -- it is the
     * engine's, and the timer picks it up when the screen is back. */
    s_crumbRow = s_moreKey = s_shield = nullptr;
    s_crumbKey = kCrumbKeyNone;
    s_busy = false;
    s_libraryAsked = false;
#ifdef ARR_USE_ENCODER
    if (s_busyGroup) { lv_group_delete(s_busyGroup); s_busyGroup = nullptr; }
#endif
}

} // namespace arrange_ui
