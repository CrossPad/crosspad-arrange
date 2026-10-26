// Arrange -- elements: arrangements inside the song, entered with CLIP EDIT, left with
// the power button, and the breadcrumb that says which one is open.
//
// An element is a project of its own under the song's .elements/ folder, played in the
// song as a block made of a bake of it. Everything that moves between them is
// crosspad::elementNav(), a job of requests to the song engine that this screen's
// timer walks (elements_tick), so nothing here touches the card.

#include "arrange_internal.hpp"

#include "crosspad/event/IEventBus.hpp"

#include <atomic>
#include <map>

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
constexpr int32_t     kBakeBarH       = 3;      ///< the progress bar along an element block's bottom edge
constexpr int32_t     kHatchH         = 5;      ///< a stale block's hatched edge
constexpr int32_t     kHatchPitch     = 4;
constexpr int32_t     kHatchLineW     = 1;
constexpr char        kUntitled[]     = "ELEMENT";   ///< a block whose element is not named (yet)
constexpr char        kCrumbKeyNone[] = "\x01";   ///< no crumbs drawn yet
constexpr bool        kKeepEdit       = true;   ///< show_draft's Continue, as draft_cb reads it
constexpr size_t      kReportLinesShown = 6;    ///< a refused delete names its users, this many lines
constexpr int32_t     kSoloH          = kRulerH + 3;   ///< the SOLO key: the free cell above the heads, less a pixel under it
constexpr int32_t     kMiniTapSlackPx = 6;      ///< a finger on the strip this close to a pass is on it
constexpr int32_t     kMiniOutlineW   = 1;
constexpr int32_t     kMiniFocusH     = 6;      ///< the focus's passes on the strip: the rest of the song is a line a track under them
constexpr int32_t     kMiniProgressH  = 2;      ///< the context's bake, along the strip's bottom edge
constexpr int32_t     kMiniTimelinePad = 2;     ///< the timeline's own top padding, where the ruler begins
constexpr uint32_t    kHashSeed       = 2166136261u;    ///< FNV-1a
constexpr uint32_t    kHashPrime      = 16777619u;

/// Which of ElementNav's questions the overlay in s_overlay is (the window is
/// the screen's, the question the engine's: it is put up again whenever the
/// question is waiting and nothing shows it).
enum class Ask : uint8_t { None, Question, Draft };
Ask         s_ask = Ask::None;
std::string s_deleteRef;                      ///< the element a long press in the picker offered to delete

lv_obj_t* s_crumbRow = nullptr;
lv_obj_t* s_moreKey  = nullptr;
std::string s_crumbKey = kCrumbKeyNone;       ///< what the row was last built from

/// The element library as the engine last listed it: block and crumb titles,
/// the next "ELEMENT n", the picker's rows.
std::vector<crosspad::ElementInfo> s_library;
/// The songs the picker drew, by name: a row picked is the song it showed,
/// whatever the engine has listed since.
std::vector<std::string> s_pickSongs;
bool     s_libraryWanted = true;          ///< a title is not known: ask the engine for the list
bool     s_libraryKnown  = false;         ///< the list came in since an element was last made or opened
bool     s_libraryAsked  = false;
uint32_t s_libraryGen    = 0;

/// A "Rebake now" is an enter of the Stale block's element and, the moment that
/// ends, its leave: the leave is what bakes (processing box and all).
bool     s_rebaking = false;

/// A block that can be baked only on the engine -- its element's kit tracks frozen at
/// another tempo, after the top bar's BPM turned -- played its old bake, pitched by
/// the varispeed, until someone found "Rebake now" in EDIT (26.10.2026). With the
/// song stopped and nothing touched for kAutoRebakeMs, the rebake runs by itself,
/// one element at a time, its processing box up; one that fails waits its turn
/// again on a RetryBackoff of its own.
constexpr uint32_t kAutoRebakeMs = 3000;
std::string s_autoRebake;                                   ///< the element the running rebake is for
std::map<std::string, crosspad::RetryBackoff> s_rebakeRetry;

/// The last pad a finger played, in the clock's microseconds (from the bus), and
/// what idleBaking() was last told: the call is made on a change only.
std::atomic<int64_t>         s_lastPadUs{0};
crosspad::SubscriptionHandle s_padSub = nullptr;
bool                         s_idleSent = false;
constexpr int64_t            kUsPerMs = 1000;

/// Where the level being left sat in the song above, for the click that lands there.
int      s_returnTrack = -1;
uint32_t s_returnBar   = 0;

/// Where the last insert or make laid its block, to say on a failure whether it
/// did: the track (-1: whichever the job chose), the bar, and how many element
/// blocks the song held before.
int      s_laidTrack = -1;
uint32_t s_laidBar   = 0;
size_t   s_laidBefore = 0;
int      s_laidDepth  = 0;      ///< where the job was started: inside an element it is not 0

lv_obj_t* s_soloKey = nullptr;                ///< SOLO, in the cell above the track heads
lv_obj_t* s_mini = nullptr;                   ///< the level above in miniature, over the ruler
lv_obj_t* s_miniProgress = nullptr;           ///< the context's bake along its edge
uint32_t  s_miniKey = 0;                      ///< what the strip was last drawn from
bool      s_solo = false;                     ///< the focus alone is heard
int       s_soloDepth = -1;                   ///< the depth SOLO was last seen at
crosspad::ContextState s_ctxState = crosspad::ContextState::None;   ///< as last seen, to toast an Incomplete once

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
    for (lv_obj_t* o : {s_projBtn, s_mixBtn, s_syncBtn, s_editBtn}) {
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

/// "ELEMENT n" for the one about to be made: the next number the library has not used.
std::string next_title()
{
    return "ELEMENT " + std::to_string(s_library.size() + 1);
}

/// Element blocks on @p track, or on every track when it is -1.
size_t element_blocks(const crosspad::AudioSong& song, int track)
{
    size_t n = 0;
    for (int t = 0; t < kTracks; ++t) {
        if (track >= 0 && t != track) continue;
        for (const auto& c : song.tracks[t].clips) n += c.isElement() && c.bar == s_laidBar;
    }
    return n;
}

/// The job is about to lay a block on @p track (-1: wherever it picks) at @p bar.
void expect_block(crosspad::ISongEngine* se, int track, uint32_t bar)
{
    s_laidTrack = track;
    s_laidBar = bar;
    s_laidDepth = crosspad::elementNav().depth();
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    s_laidBefore = element_blocks(se->song(), track);
}

/// A job that failed left its block on a track: the engine is a level deeper
/// than where the job started (the block was laid before it was entered), or
/// the song holds one more block at that bar than it did. An insert that
/// failed at the copy, and a make that was refused, laid none.
bool block_was_laid(crosspad::ISongEngine* se)
{
    if (crosspad::elementNav().depth() > s_laidDepth) return true;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    return element_blocks(se->song(), s_laidTrack) > s_laidBefore;
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
    expect_block(se, track, bar);
    if (!nav.insertExisting(track, bar, s_library[i].ref)) note(nav.lastError().c_str());
}

void song_pick_cb(lv_event_t* e)
{
    const size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    auto* se = engine();
    close_overlay();
    if (!se) return;
    auto& nav = crosspad::elementNav();
    if (nav.busy()) return;
    if (i >= s_pickSongs.size()) return;
    const std::string song = s_pickSongs[i];
    const uint32_t bar = playhead_bar();
    int track = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        track = free_track(se->song(), bar, 1);    // its length is known once it is copied; the job refuses an overlap
    }
    if (track < 0) { note("No audio track is free there."); return; }
    expect_block(se, track, bar);
    if (!nav.insertSong(track, bar, song)) note(nav.lastError().c_str());
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


// ── the question a leave asks, and the unfinished edit ─────────────────────

/// A line of small text in the overlay, as wide as it.
void overlay_note(const char* text)
{
    lv_obj_t* l = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(l, kDim, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_text(l, text);
}

void answer_cb(lv_event_t* e)
{
    const auto c = (crosspad::ElementNav::Choice)(intptr_t)lv_event_get_user_data(e);
    close_overlay();
    auto& nav = crosspad::elementNav();
    if (!nav.choose(c)) note(nav.lastError().c_str());
}

void draft_cb(lv_event_t* e)
{
    const bool keep = (bool)(intptr_t)lv_event_get_user_data(e);
    close_overlay();
    auto& nav = crosspad::elementNav();
    if (!nav.resolveDraft(keep)) note(nav.lastError().c_str());
}

/// "REFREN is used in 3 other places": the leave waits for Make a copy or
/// Replace everywhere. The power button calls the leave off (elements_ask_back()).
void show_question()
{
    const auto& q = crosspad::elementNav().question();
    close_overlay();
    char head[96];
    std::snprintf(head, sizeof(head), "%s is used in %u other place%s", q.title.empty() ? kUntitled : q.title.c_str(),
                  (unsigned)q.uses, q.uses == 1 ? "" : "s");
    overlay_keys(head);
    std::string copies = "Copy makes: ";
    for (size_t i = 0; i < q.copies.size(); ++i) copies += (i ? ", " : "") + q.copies[i];
    overlay_note(copies.c_str());
    overlay_note("Replace everywhere: they all get this edit. Power button: stay in the element.");
    lv_obj_t* copy = overlay_key("Make a copy", answer_cb, (void*)(intptr_t)crosspad::ElementNav::Choice::Copy);
    overlay_key("Replace everywhere", answer_cb, (void*)(intptr_t)crosspad::ElementNav::Choice::Replace);
    s_ask = Ask::Question;
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_focus_obj(copy);
#else
    (void)copy;
#endif
}

/// An edit a power cut ended: kept, or the element put back as it was.
void show_draft()
{
    const auto& lv = crosspad::elementNav().path().back();
    const std::string head = "Unfinished edit of " + level_title(lv);
    close_overlay();
    overlay_keys(head.c_str());
    overlay_note("Continue keeps the edit. Restore puts the element back as it was before it.");
    lv_obj_t* keep = overlay_key("Continue", draft_cb, (void*)(intptr_t)kKeepEdit);
    overlay_key("Restore", draft_cb, nullptr);
    s_ask = Ask::Draft;
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_focus_obj(keep);
#else
    (void)keep;
#endif
}

/// What ElementNav waits for, shown: also when Arrange was closed meanwhile and
/// is opened again -- the job stays busy until it is answered.
void ask_tick()
{
    auto& nav = crosspad::elementNav();
    const Ask want = nav.asking() ? Ask::Question : nav.draftWaiting() ? Ask::Draft : Ask::None;
    if (want == s_ask && (want == Ask::None || s_overlay)) return;
    if (want == Ask::None) { close_overlay(); return; }
    if (want == Ask::Question) show_question();
    else                       show_draft();
}

// ── deleting an element in the library ─────────────────────────────────────

void element_delete_cb(lv_event_t*)
{
    auto* se = engine();
    close_overlay();
    if (!se) return;
    if (!se->requestDeleteElement(s_deleteRef)) {
        note(se->playing() ? "Stop the song first." : "Not deleted: the card is busy.");
        return;
    }
    s_editing = true;
    s_editKind = EditKind::ElementDelete;
    note("Deleting ...");
}

/// A long press on an element of the picker: asked once more, "Keep it" first.
void library_hold_cb(lv_event_t* e)
{
    const size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    close_overlay();
    if (i >= s_library.size()) return;
    s_deleteRef = s_library[i].ref;
    const std::string head = std::string("Delete ") + element_title(s_deleteRef) + "?";
    overlay_keys(head.c_str());
    overlay_note("It goes from the card with its bakes. One that a song or another element plays is refused, and the refusal names them.");
    lv_obj_t* keep = overlay_key("Keep it", cancel_cb);
    lv_obj_t* del = overlay_key(LV_SYMBOL_TRASH " Delete", element_delete_cb);
    lv_obj_set_style_bg_color(del, kMute, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(del, 0), kInk, 0);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_focus_obj(keep);
#else
    (void)keep;
#endif
}

} // namespace

lv_group_t* elements_group_after_overlay(lv_group_t* base)
{
#ifdef ARR_USE_ENCODER
    return s_busy && s_busyGroup ? s_busyGroup : base;
#else
    return base;
#endif
}

void elements_overlay_closed()
{
    s_ask = Ask::None;
}

bool elements_ask_back()
{
    if (s_ask == Ask::None) return false;
    /* The question is called off and the user stays in the element; the unfinished
     * edit has no such way out, it is answered with a key. */
    if (s_ask == Ask::Question) {
        crosspad::elementNav().choose(crosspad::ElementNav::Choice::None);
        close_overlay();
    } else {
        note("Answer the unfinished edit first.");
    }
    return true;
}

void element_delete_done(bool ok)
{
    s_libraryWanted = true;
    s_libraryKnown = false;
    if (ok) { note("Element deleted."); return; }
    auto* se = engine();
    const std::vector<std::string> report = se ? se->editReport() : std::vector<std::string>{};
    if (s_overlay) { note(report.empty() ? "Not deleted." : report[0].c_str()); return; }
    overlay_list("Not deleted");
    for (size_t i = 0; i < report.size() && i < kReportLinesShown; ++i) overlay_note(report[i].c_str());
    if (report.empty()) overlay_note("No card, or the card kept some of its files.");
    lv_obj_t* done = overlay_item(LV_SYMBOL_OK " OK", cancel_cb, nullptr, true);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_focus_obj(done);
#else
    (void)done;
#endif
}

const char* element_title(const std::string& ref)
{
    const auto* e = find_element(ref);
    if (e && !e->title.empty()) return e->title.c_str();
    /* Asked for once a change: an element the list does not hold (or holds
     * unnamed) is not asked for again each time the timeline is drawn. */
    if (!e && !s_libraryKnown) s_libraryWanted = true;
    return kUntitled;
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

// ── the mini-map and SOLO ──────────────────────────────────────────────────

namespace {

void solo_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se || crosspad::elementNav().depth() == 0) return;
    s_solo = !s_solo;
    se->contextSolo(s_solo);
    style_toggle(s_soloKey, s_solo, kKitT);
}

/// Where a bar of the level above falls on the strip.
struct MiniScale {
    int32_t  w = 0;
    uint32_t bars = 1;
    int32_t x(uint32_t bar) const { return (int32_t)((uint64_t)bar * (uint32_t)w / bars); }
};

/// How many bars of the level above an occurrence sounds for, at least one.
uint32_t occurrence_bars(const crosspad::ElementNav::Occurrence& o, uint32_t framesPerBar)
{
    if (!framesPerBar) return 1;
    return std::max<uint32_t>(1, (o.frames + framesPerBar - 1) / framesPerBar);
}

/// A tap on the strip: the pass under the finger (a few pixels either side of it), chosen,
/// with the playhead at where in the element it begins.
void mini_cb(lv_event_t*)
{
    auto* se = engine();
    auto& nav = crosspad::elementNav();
    lv_indev_t* in = lv_indev_active();
    if (!se || !in || !s_mini || nav.depth() == 0 || nav.busy()) return;
    const auto& lv = nav.path().back();
    uint32_t total = 1;
    for (const auto& b : lv.parentBlocks) total = std::max<uint32_t>(total, b.bar + b.bars);
    const uint32_t fpb = se->framesPerBar();
    for (const auto& o : lv.occurrences) total = std::max(total, o.parentBar + occurrence_bars(o, fpb));
    lv_area_t a;
    lv_obj_get_coords(s_mini, &a);
    lv_point_t p;
    lv_indev_get_point(in, &p);
    const MiniScale sc{lv_area_get_width(&a), total};
    const int32_t px = p.x - a.x1;
    for (size_t i = 0; i < lv.occurrences.size(); ++i) {
        const auto& o = lv.occurrences[i];
        if (px < sc.x(o.parentBar) - kMiniTapSlackPx || px > sc.x(o.parentBar + occurrence_bars(o, fpb)) + kMiniTapSlackPx) continue;
        if (!nav.chooseOccurrence(i)) { note(nav.lastError().c_str()); return; }
        if (fpb) crosspad::songTransport().setStartBar(o.offset / fpb);
        return;
    }
}

/// What the strip shows: the level's blocks, its passes, the chosen one, SOLO and the
/// context's state -- anything else moving leaves it as drawn.
uint32_t mini_key(const crosspad::ElementNav::Level& lv, int depth, crosspad::ContextState st, int32_t w)
{
    uint32_t h = kHashSeed;
    auto mix = [&h](uint64_t v) { h = (h ^ (uint32_t)v) * kHashPrime; h = (h ^ (uint32_t)(v >> 32)) * kHashPrime; };
    mix((uint64_t)depth);
    mix(lv.chosen);
    mix((uint64_t)st);
    mix(s_solo);
    mix((uint64_t)w);
    for (const auto& b : lv.parentBlocks) {
        mix(b.track);
        mix(b.bar);
        mix(b.bars);
        mix((uint64_t)b.focus | (uint64_t)b.ready << 1 | (uint64_t)b.seq << 2);
    }
    for (const auto& o : lv.occurrences) {
        mix(o.parentBar);
        mix(o.frames);
    }
    return h;
}

void draw_mini(lv_obj_t* parent, const crosspad::ElementNav::Level& lv, uint32_t fpb)
{
    uint32_t total = 1;
    for (const auto& b : lv.parentBlocks) total = std::max<uint32_t>(total, b.bar + b.bars);
    for (const auto& o : lv.occurrences) total = std::max(total, o.parentBar + occurrence_bars(o, fpb));
    const MiniScale sc{lv_obj_get_width(parent), total};
    constexpr int32_t kBand = (kRulerH - kMiniFocusH) / kTracks;      // the blocks of a track: its own line
    auto rect = [parent](int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t c, lv_opa_t opa) {
        lv_obj_t* o = lv_obj_create(parent);
        lv_obj_remove_style_all(o);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_size(o, std::max<int32_t>(1, w), h);
        lv_obj_set_style_bg_color(o, c, 0);
        lv_obj_set_style_bg_opa(o, opa, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        return o;
    };
    /* The parent as it is heard around the element: SOLO dims everything that is not the focus. */
    const lv_opa_t aside = s_solo ? LV_OPA_20 : LV_OPA_COVER;
    for (const auto& b : lv.parentBlocks) {
        if (b.focus) continue;
        rect(sc.x(b.bar), kMiniFocusH + b.track * kBand, sc.x(b.bar + b.bars) - sc.x(b.bar), kBand, b.ready ? kIce : kMissing, aside);
    }
    for (const auto& b : lv.parentBlocks)
        if (b.focus) rect(sc.x(b.bar), 0, sc.x(b.bar + b.bars) - sc.x(b.bar), kMiniFocusH, kFocus, LV_OPA_COVER);
    if (lv.chosen < lv.occurrences.size()) {
        const auto& o = lv.occurrences[lv.chosen];
        lv_obj_t* ring = rect(sc.x(o.parentBar), 0, sc.x(o.parentBar + occurrence_bars(o, fpb)) - sc.x(o.parentBar), kRulerH, kHead, LV_OPA_TRANSP);
        lv_obj_set_style_border_color(ring, kHead, 0);
        lv_obj_set_style_border_width(ring, kMiniOutlineW, 0);
    }
    s_miniProgress = rect(0, kRulerH - kMiniProgressH, 0, kMiniProgressH, kAudioT, LV_OPA_COVER);
}

} // namespace

void elements_build_solo(lv_obj_t* heads)
{
    s_soloKey = make_button(heads, kHeadW, "S  SOLO", solo_cb);
    lv_obj_set_height(s_soloKey, kSoloH);
    lv_obj_set_pos(s_soloKey, 0, 0);
    lv_obj_add_flag(s_soloKey, LV_OBJ_FLAG_HIDDEN);
    style_toggle(s_soloKey, false, kKitT);
    s_solo = false;
    s_soloDepth = -1;
}

void elements_build_minimap(lv_obj_t* parent)
{
    s_mini = lv_obj_create(parent);
    lv_obj_remove_style_all(s_mini);
    lv_obj_add_flag(s_mini, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_mini, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_mini, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_mini, kPanel, 0);
    lv_obj_set_style_bg_opa(s_mini, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_mini, mini_cb, LV_EVENT_CLICKED, nullptr);
    s_miniKey = 0;
}

/// Run by the screen's timer: SOLO off on any change of level, the strip over
/// the ruler redrawn when what it shows changed, the context's bake along it,
/// a context that could not be made said once.
static void mini_tick(crosspad::ISongEngine* se)
{
    auto& nav = crosspad::elementNav();
    const int depth = nav.depth();
    if (depth != s_soloDepth) {
        s_soloDepth = depth;
        s_solo = false;
        se->contextSolo(false);
        s_miniKey = 0;
        style_toggle(s_soloKey, false, kKitT);
    }
    const bool inside = depth > 0;
    if (s_soloKey) {
        if (inside) lv_obj_remove_flag(s_soloKey, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(s_soloKey, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_mini || !s_timeline) return;
    if (!inside) {
        if (!lv_obj_has_flag(s_mini, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_mini, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clean(s_mini);
            s_miniProgress = nullptr;
        }
        s_miniKey = 0;
        s_ctxState = crosspad::ContextState::None;
        return;
    }
    const auto st = se->contextState();
    if (st != s_ctxState) {
        if (st == crosspad::ContextState::Incomplete)
            ui_toast_show(se->contextWhy().c_str(), kToastMs, UI_TOAST_ERROR);
        s_ctxState = st;
    }
    lv_area_t t, r;
    lv_obj_get_coords(s_timeline, &t);
    lv_obj_get_coords(s_root, &r);
    const int32_t w = lv_area_get_width(&t);
    const auto& lv = nav.path().back();
    const uint32_t key = mini_key(lv, depth, st, w);
    if (key != s_miniKey) {
        s_miniKey = key;
        lv_obj_set_pos(s_mini, t.x1 - r.x1, t.y1 - r.y1 + kMiniTimelinePad);
        lv_obj_set_size(s_mini, w, kRulerH);
        lv_obj_clean(s_mini);
        draw_mini(s_mini, lv, se->framesPerBar());
        lv_obj_remove_flag(s_mini, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_miniProgress) {
        if (st == crosspad::ContextState::Baking) {
            lv_obj_remove_flag(s_miniProgress, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(s_miniProgress, std::max<int32_t>(1, w * se->backgroundProgress() / 100));
        } else {
            lv_obj_add_flag(s_miniProgress, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

#ifdef ARR_USE_ENCODER
void elements_group_add(lv_group_t* g)
{
    if (s_soloKey) lv_group_add_obj(g, s_soloKey);
    if (s_crumbRow) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(s_crumbRow); ++i) {
            lv_obj_t* c = lv_obj_get_child(s_crumbRow, (int32_t)i);
            if (lv_obj_check_type(c, &lv_button_class)) lv_group_add_obj(g, c);
        }
    }
    if (s_moreKey) lv_group_add_obj(g, s_moreKey);
}
#endif

/// An element block's bake moving on: a change of state redraws the timeline, a
/// bake under way moves its bar.
static void watch_bakes(crosspad::ISongEngine* se)
{
    bool moved = false;
    const uint8_t progress = se->backgroundProgress();
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        for (int k = 0; k < s_blockCount; ++k) {
            Block& b = s_blocks[k];
            if (!b.element) continue;
            if (se->elementClipState((uint8_t)b.track, (size_t)b.index) != b.elementState) { moved = true; break; }
            if (b.bakeBar) lv_obj_set_width(b.bakeBar, lv_pct(progress));
        }
    }
    if (moved) rebuild_timeline();
}

/// The player is away -- the song stopped, no element job running and no pad
/// played for kIdleBakeMs (core's, the one its own docs name) -- so the engine
/// may bake the other songs' blocks of an element changed since. The press
/// time comes off the bus: PadManager keeps none.
static void idle_tick(crosspad::ISongEngine* se)
{
    if (!s_padSub) {
        s_lastPadUs.store(crosspad::getClock().getTimeUs());
        s_padSub = crosspad::getEventBus().subscribe(crosspad::EventType::PadPressed,
            [](const void*, crosspad::EventType, void*) {
                s_lastPadUs.store(crosspad::getClock().getTimeUs());
            });
    }
    const int64_t quietUs = crosspad::getClock().getTimeUs() - s_lastPadUs.load();
    const bool away = !se->playing() && !crosspad::elementNav().busy()
                      && quietUs >= (int64_t)crosspad::kIdleBakeMs * kUsPerMs;
    if (away != s_idleSent) {
        s_idleSent = away;
        se->idleBaking(away);
    }
}

/// The first element block only the engine can bake (see kAutoRebakeMs), as a
/// "Rebake now" on it: selected, entered, and left at once by elements_tick().
static void auto_rebake(crosspad::ISongEngine* se)
{
    auto& nav = crosspad::elementNav();
    if (!s_visible || s_overlay || nav.busy() || se->playing() || !s_autoRebake.empty()) return;
    if (lv_display_get_inactive_time(nullptr) < kAutoRebakeMs) return;
    const int64_t quietUs = crosspad::getClock().getTimeUs() - s_lastPadUs.load();
    if (quietUs < (int64_t)kAutoRebakeMs * kUsPerMs) return;
    const uint32_t now = lv_tick_get();
    for (int t = 0; t < kTracks; ++t) {
        std::vector<std::string> refs;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            for (const auto& c : se->song().tracks[t].clips) refs.push_back(c.element);
        }
        for (size_t i = 0; i < refs.size(); ++i) {
            if (refs[i].empty()) continue;
            const auto st = se->elementClipState((uint8_t)t, i);
            if (st != crosspad::ElementClipState::Stale && st != crosspad::ElementClipState::Missing) continue;
            /* Only what entering and leaving mends: not an edit undecided, a child's
             * state or a song.json that cannot be read -- and the card's baker does
             * the rest by itself. */
            if (!se->elementClipRebakes((uint8_t)t, i)) continue;
            auto retry = s_rebakeRetry.find(refs[i]);
            if (retry != s_rebakeRetry.end() && !retry->second.due(now)) continue;
            s_selTrack = t;
            s_selIndex = (int)i;
            const std::string why = se->elementClipWhy((uint8_t)t, i);
            if (!elements_clip_edit()) continue;
            s_rebaking = nav.busy();
            if (s_rebaking) {
                s_autoRebake = refs[i];
                /* The screen goes in and out by itself: said why. */
                note_fmt("Baking %s again: %s", element_title(refs[i]), why.c_str());
            } else {
                s_rebakeRetry[refs[i]].failed(now);
            }
            return;
        }
    }
}

void elements_tick(crosspad::ISongEngine* se)
{
    auto& nav = crosspad::elementNav();
    const auto r = nav.poll();
    set_busy(nav.busy());
    ask_tick();
    /* The job waits for a stopped song (a leave while it plays, fitting copies
     * of stretched audio), and the top bar's PLAY is outside the shield. */
    if (nav.busy() && se->playing()) se->stop();
    if (r.done && s_rebaking) {
        /* The enter of a "Rebake now" is only the way in: straight on out, no
         * new level to settle into. The leave's own end is the job it reports. */
        s_rebaking = false;
        if (r.ok && r.job == crosspad::ElementNav::Job::Enter) {
            leave_to(nav.depth() - 1);
            return;
        }
    }
    if (r.done && !s_autoRebake.empty()) {
        if (r.ok) s_rebakeRetry.erase(s_autoRebake);
        else      s_rebakeRetry[s_autoRebake].failed(lv_tick_get());
        s_autoRebake.clear();
    }
    if (r.done) {
        if (r.calledOff) {
            /* The user's own answer: nothing failed, nothing to say. */
        } else if (!r.ok) {
            std::string msg = r.error.empty() ? nav.lastError() : r.error;
            /* A block is laid before its job bakes it: it stays, unbaked. A job that
             * failed before that left the song as it was. */
            const bool lays = r.job == crosspad::ElementNav::Job::Insert || r.job == crosspad::ElementNav::Job::Make;
            if (lays && block_was_laid(se)) msg += "\nThe block stays on the track, not baked.";
            ui_toast_show(msg.c_str(), kToastMs, UI_TOAST_ERROR);
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
    watch_bakes(se);
    ask_library(se);
    mini_tick(se);
    idle_tick(se);
    auto_rebake(se);
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

/// The hatching along a stale block's bottom edge: slanted lines, as the song's
/// own "old" mark -- the block plays the bake it has, not the element as it is.
static void stale_edge_cb(lv_event_t* e)
{
    lv_obj_t* o = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = kInk;
    d.width = kHatchLineW;
    d.opa = LV_OPA_COVER;
    for (int32_t x = a.x1; x <= a.x2; x += kHatchPitch) {
        d.p1.x = x;
        d.p1.y = a.y2;
        d.p2.x = x + kHatchH;
        d.p2.y = a.y2 - kHatchH;
        lv_draw_line(layer, &d);
    }
}

void elements_style_block(lv_obj_t* block, lv_obj_t* label, const crosspad::SongClip& c, Block& b)
{
    using crosspad::ElementClipState;
    auto* se = engine();
    lv_obj_set_style_bg_color(block, kIce, 0);
    const ElementClipState st = se ? se->elementClipState((uint8_t)b.track, (size_t)b.index) : ElementClipState::Fresh;
    b.elementState = st;
    /* Not baked yet, or its bake is not on the card: it has no sound of its
     * own to show, and says so. A silenced track's block is already faded; that
     * is kept. */
    const bool baked = !c.file.empty();
    const bool silent = st == ElementClipState::Missing || !baked;
    if (silent && lv_obj_get_style_bg_opa(block, LV_PART_MAIN) == LV_OPA_COVER)
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
    const char* title = element_title(c.element);
    if (st == ElementClipState::Missing)      set_text_fmt(label, "%s - no bake", title);
    else if (st == ElementClipState::Stale)   set_text_fmt(label, "%s  old", title);
    else if (st == ElementClipState::Baking)  set_text_fmt(label, "%s", title);
    else                                      set_text_fmt(label, baked ? "%s" : "%s - not baked", title);
    if (st == ElementClipState::Stale) lv_obj_add_event_cb(block, stale_edge_cb, LV_EVENT_DRAW_MAIN_END, nullptr);
    if (st == ElementClipState::Baking) {
        lv_obj_t* bar = lv_obj_create(block);
        lv_obj_remove_style_all(bar);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(bar, lv_pct(se->backgroundProgress()), kBakeBarH);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_bg_color(bar, kAudioT, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        b.bakeBar = bar;
    }
}

void element_rebake_cb(lv_event_t*)
{
    close_overlay();
    auto& nav = crosspad::elementNav();
    if (nav.busy() || !elements_clip_edit()) return;
    s_rebaking = nav.busy();
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
    expect_block(se, track, bar);
    if (!nav.insertNew(track, bar, next_title())) note(nav.lastError().c_str());
}

void element_library_cb(lv_event_t*)
{
    close_overlay();
    request_list(Pending::Elements, "Element from library");
}

/// One row of the picker: an element of the library or a song to copy into it
/// (@c index into s_library or s_pickSongs).
struct PickRow { bool song; size_t index; };

static bool s_songsBusy = false;       ///< the card would not list the songs

static void render_picker(size_t from);

static void picker_page_cb(lv_event_t* e)
{
    const size_t from = (size_t)(intptr_t)lv_event_get_user_data(e);
    close_overlay();
    overlay_list("Element from library");
    render_picker(from);
}

/// The library, then the songs (the root left out), kLibraryRows rows of
/// objects from @p from on: the rest is a "More..." away, the ones before a
/// "Back..." -- a card with 50 songs does not build 50 buttons.
static void render_picker(size_t from)
{
    auto* se = engine();
    if (!se || !s_overlay) return;
    const std::string root = crosspad::elementNav().path().front().open;
    const auto songs = se->projects();
    std::vector<PickRow> rows;
    for (size_t i = 0; i < s_library.size(); ++i) rows.push_back({false, i});
    s_pickSongs.clear();
    if (!s_songsBusy)
        for (const std::string& song : songs)
            if (song != root) {
                rows.push_back({true, s_pickSongs.size()});
                s_pickSongs.push_back(song);
            }
    if (from >= rows.size()) from = 0;
    const size_t to = std::min(rows.size(), from + (size_t)kLibraryRows);

    if (from > 0) overlay_item(LV_SYMBOL_UP " Back...", picker_page_cb,
                               (void*)(intptr_t)(from > (size_t)kLibraryRows ? from - kLibraryRows : 0));
    if (s_library.empty() && from == 0) {
        lv_obj_t* l = lv_label_create(s_overlay);
        lv_obj_set_style_text_color(l, kDim, 0);
        lv_label_set_text(l, "No elements yet -- EDIT > New element here.");
    }
    if (!s_library.empty() && from == 0) overlay_note("Hold an element to delete it.");
    bool songsHeaded = false;
    for (size_t k = from; k < to; ++k) {
        if (rows[k].song && !songsHeaded) {
            lv_obj_t* head = lv_label_create(s_overlay);
            lv_obj_set_style_text_color(head, kDim, 0);
            lv_label_set_text(head, "Songs");
            songsHeaded = true;
        }
        if (!rows[k].song) {
            char row[64];
            lv_snprintf(row, sizeof(row), "%s  (%u bars)", element_title(s_library[rows[k].index].ref),
                        (unsigned)s_library[rows[k].index].bars);
            lv_obj_t* b = overlay_item(row, library_pick_cb, (void*)(intptr_t)rows[k].index);
            lv_obj_add_event_cb(b, library_hold_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)rows[k].index);
        } else {
            overlay_item(s_pickSongs[rows[k].index].c_str(), song_pick_cb, (void*)(intptr_t)rows[k].index);
        }
    }
    if (to < rows.size()) overlay_item(LV_SYMBOL_DOWN " More...", picker_page_cb, (void*)(intptr_t)to);
    if (s_songsBusy && to == rows.size()) {
        lv_obj_t* l = lv_label_create(s_overlay);
        lv_obj_set_style_text_color(l, kDim, 0);
        lv_label_set_text(l, "Songs: card busy, try again");
    }
    overlay_item(LV_SYMBOL_CLOSE " Cancel", cancel_cb, nullptr);
}

void fill_elements()
{
    auto* se = engine();
    s_library = se->elements();
    s_libraryWanted = false;
    s_libraryKnown = true;
    /* The songs are read next, so the pages are laid out once with both. */
    s_pendingGen = se->listGeneration();
    s_songsBusy = !se->requestProjects();
    if (s_songsBusy) { render_picker(0); return; }
    s_pending = Pending::Songs;
    s_loading = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(s_loading, kDim, 0);
    lv_label_set_text(s_loading, "Reading songs ...");
}

void fill_library_songs()
{
    s_songsBusy = false;
    render_picker(0);
}

void element_region_cb(lv_event_t*)
{
    auto* se = engine();
    close_overlay();
    auto& nav = crosspad::elementNav();
    if (!se || nav.busy()) return;
    uint32_t from = 0, to = 0;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        if (!se->song().hasRegion()) return;
        from = se->song().loopStart;
        to = se->song().loopEnd;
    }
    expect_block(se, -1, from);
    if (!nav.makeFromRegion(from, to, next_title())) note(nav.lastError().c_str());
}

bool element_progress(char* buf, size_t cap)
{
    auto& nav = crosspad::elementNav();
    if (!nav.busy() || nav.asking()) return false;     // asking: the question is on the screen
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
    s_crumbRow = s_moreKey = s_shield = s_soloKey = s_mini = s_miniProgress = nullptr;
    s_miniKey = 0;
    s_ctxState = crosspad::ContextState::None;
    s_crumbKey = kCrumbKeyNone;
    s_busy = false;
    s_ask = Ask::None;
    s_libraryAsked = false;
    /* The screen is the player's eyes on the pads and the clock: gone, there is
     * no one to be away, and the engine is told so. */
    if (s_padSub) {
        crosspad::getEventBus().unsubscribe(s_padSub);
        s_padSub = nullptr;
    }
    if (s_idleSent) {
        s_idleSent = false;
        if (auto* se = engine()) se->idleBaking(false);
    }
#ifdef ARR_USE_ENCODER
    if (s_busyGroup) { lv_group_delete(s_busyGroup); s_busyGroup = nullptr; }
#endif
}

} // namespace arrange_ui
