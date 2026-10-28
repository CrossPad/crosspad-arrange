// Arrange -- the overlays: the project picker and the clip library, the song windows
// and the open song's settings (tempo, name, length).

#include "arrange_internal.hpp"

namespace arrange_ui {

// ── overlays: project picker, clip library ─────────────────────────────────

void close_overlay()
{
    s_pending = Pending::None;
    s_loading = nullptr;
    elements_overlay_closed();
    if (!s_overlay) return;
#ifdef ARR_USE_ENCODER
    if (indev_encoderTop && s_encGroup) lv_indev_set_group(indev_encoderTop, elements_group_after_overlay(s_encGroup));
    lv_group_delete_deferred(s_overlayGroup);     // the click that closed it is still being read
    s_overlayGroup = nullptr;
#endif
    lv_obj_delete_async(s_overlay);
    s_overlay = nullptr;
    s_songList = nullptr;
    s_tempoItem = nullptr;
    s_trackNameArea = nullptr;
    mix_forget();
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

lv_obj_t* overlay_item(const char* text, lv_event_cb_t cb, void* ud, bool accent)
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

/// A menu of keys rather than rows: two a row, as tall as a dialog's keys
/// (BOUNCE, CANCEL) -- a menu that is touched more than it is turned.
lv_obj_t* overlay_keys(const char* title)
{
    overlay_list(title);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s_overlay, 4, 0);
    lv_obj_set_style_pad_row(s_overlay, 4, 0);
    lv_obj_set_width(lv_obj_get_child(s_overlay, 0), LV_PCT(100));
    return s_overlay;
}

constexpr int32_t kMenuKeyW = (320 - 2 * 6 - 4) / 2;   ///< half the overlay, less its gap
constexpr int32_t kMenuKeyH = 36;

lv_obj_t* overlay_key(const char* text, lv_event_cb_t cb, void* ud)
{
    lv_obj_t* b = overlay_item(text, cb, ud);
    lv_obj_set_size(b, kMenuKeyW, kMenuKeyH);
    return b;
}

// ── the song windows: a header with the way back, a list under it ────────

constexpr int32_t kHeadH = 28;
constexpr int32_t kRowH = 30;

/// A header button: "< SONGS" on the left, an action like "+ NEW" on the right.
static lv_obj_t* head_button(lv_obj_t* head, const char* text, bool accent, lv_event_cb_t cb)
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
static void song_window(const char* back, lv_event_cb_t backCb, const char* action = nullptr,
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
static void style_row(lv_obj_t* r)
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
static lv_obj_t* list_row(const char* name, const char* value, lv_event_cb_t cb, void* ud = nullptr,
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
static lv_obj_t* list_note(const char* text, const lv_font_t* font = &lv_font_montserrat_10, lv_color_t c = kDim)
{
    lv_obj_t* l = lv_label_create(s_songList);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_text(l, text);
    return l;
}

static std::vector<std::string> s_projects;
static std::vector<crosspad::ClipInfo> s_library;

void cancel_cb(lv_event_t*) { close_overlay(); }

// ── the open song's settings: tempo and name ───────────────────────────────

static int32_t   s_editBpm = 120;          // the tempo being dialled, applied on the click that ends it

void show_tempo()
{
    if (!s_tempoItem) return;
    set_text_fmt(lv_obj_get_child(s_tempoItem, 1), "%03d BPM", (int)s_editBpm);
}

static void song_edit_cb(lv_event_t*);

/// Turned to dial, clicked to set: a tempo change resizes every block (see
/// ISongEngine::requestTempo), so it is made once, not per detent.
static void tempo_cb(lv_event_t* e)
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
static void emphasise_row(lv_obj_t* r, lv_color_t bg)
{
    lv_obj_set_style_bg_color(r, bg, 0);
    lv_obj_set_style_text_color(r, kInk, 0);
}

static std::string open_song_name()
{
    auto* se = engine();
    if (!se) return {};
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    return se->song().name;
}

void rename_failed()
{
    if (s_overlay) return;
    song_window("Rename", song_edit_cb);
    list_note("That name is taken, or not one a folder can have.", &lv_font_montserrat_14, kText);
    emphasise_row(list_row(LV_SYMBOL_OK "  OK", nullptr, song_edit_cb), kAccent);
}

static void rename_done_cb(lv_event_t* e)
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

static void rename_cb(lv_event_t*)
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

static void delete_confirmed_cb(lv_event_t*)
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
static void delete_cb(lv_event_t*)
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
    const bool wav = s_editKind == EditKind::ExportWav;
    song_window(wav ? "Mixdown" : "Ableton Live", song_edit_cb);
    list_note(ok ? (wav ? LV_SYMBOL_OK "  Mix written" : LV_SYMBOL_OK "  Live Set written")
                 : (wav ? LV_SYMBOL_WARNING "  No mix written" : LV_SYMBOL_WARNING "  No Live Set written"),
              &lv_font_montserrat_14, ok ? kAccent : kMute);
    for (size_t i = 0; i < report.size() && i < 6; ++i) {
        list_note(report[i].c_str(), i ? &lv_font_montserrat_10 : &lv_font_montserrat_14, i ? kDim : kText);
    }
    if (report.size() > 6) {
        list_note(("... and " + std::to_string(report.size() - 6) + " more").c_str());
    }
    if (ok && wav)  list_note("Beside the song on the card: copy it off, or over USB (Settings -> USB -> SD card as USB drive).");
    else if (ok)    list_note("Open the .als on the card in Live 12, or copy the song's folder off it first.");
    lv_obj_t* done = list_row(LV_SYMBOL_OK "  OK", nullptr, [](lv_event_t*) { close_overlay(); });
    emphasise_row(done, kAccent);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_focus_obj(done);
#endif
}

static void export_cb(lv_event_t*)
{
    close_overlay();
    auto* se = engine();
    if (!se || !se->requestExportLiveSet()) return;
    s_editing = true;
    s_editKind = EditKind::Export;
    note("Writing the Ableton Live Set ...");
}

/// The song mixed down to a WAV beside it: what is not frozen is frozen
/// first, in the background (auto_freeze()), then the mix is written.
static void export_wav_cb(lv_event_t*)
{
    close_overlay();
    auto* se = engine();
    if (!se) return;
    if (se->playing()) crosspad::songTransport().stop();
    s_exportWav = true;
    note("Mixing the song down -- frozen first where it is not ...");
}

/// The open song's settings, a row each: tempo (click, turn, click), name,
/// the export and the delete. Back is the songs list.
static void song_edit_cb(lv_event_t*)
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
    list_row("Export mix to WAV", ".wav  " LV_SYMBOL_RIGHT, export_wav_cb);
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
static void pick_project(int i)
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
    crosspad::elementNav().reset();     // another song: the breadcrumb starts over
    s_opening = se->requestOpen(name);
    if (s_opening) note_fmt("Opening %s ...", name.c_str());
}

static void project_pick_cb(lv_event_t* e) { pick_project((int)(intptr_t)lv_event_get_user_data(e)); }

/// "+ NEW" is in the header from the start, but a new name is only known
/// once the list is: until then it does nothing.
static void new_song_cb(lv_event_t*)
{
    if (s_pending != Pending::Projects) pick_project(-1);
}

void request_list(Pending what, const char* title)
{
    auto* se = engine();
    if (!se || s_overlay) return;
    s_pendingGen = se->listGeneration();
    /* One generation counts every list the engine reads: an elements read still
     * running would move it and fill this list from the old one. */
    const bool asked = !elements_list_pending() && (what == Pending::Projects ? se->requestProjects()
                     : what == Pending::Clips  ? se->requestClips()
                                               : se->requestElements());
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

/// Songs one page of the list builds: a row is a few kB of objects, and a card
/// with fifty songs built none past the 24th -- the newest "Song N" could not
/// be opened from here (26.10.2026). The rest is a "More ..." away.
static constexpr size_t kSongRows = 24;

static void fill_projects_from(size_t from);

static void projects_page_cb(lv_event_t* e)
{
    const size_t from = (size_t)(intptr_t)lv_event_get_user_data(e);
    close_overlay();
    song_window("Songs", cancel_cb, LV_SYMBOL_PLUS " NEW", new_song_cb);
    fill_projects_from(from);
}

/// The songs on the card, from the page the open one is on: it is marked and
/// first to the encoder; a click on it edits it, on any other opens that one.
void fill_projects()
{
    s_projects = engine()->projects();
    const auto it = std::find(s_projects.begin(), s_projects.end(), open_song_name());
    const size_t at = it == s_projects.end() ? 0 : (size_t)(it - s_projects.begin());
    fill_projects_from(at / kSongRows * kSongRows);
}

static void fill_projects_from(size_t from)
{
    const std::string current = open_song_name();
    lv_obj_t* focus = nullptr;
    if (from > 0) list_row(LV_SYMBOL_UP "  Back ...", nullptr, projects_page_cb, (void*)(intptr_t)(from - kSongRows));
    const size_t to = std::min(s_projects.size(), from + kSongRows);
    for (size_t i = from; i < to; ++i) {
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
    if (to < s_projects.size()) list_row(LV_SYMBOL_DOWN "  More ...", nullptr, projects_page_cb, (void*)(intptr_t)to);
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
static void library_pick_cb(lv_event_t* e)
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
static void show_length()
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

static void length_step_cb(lv_event_t* e)
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

static void length_done_cb(lv_event_t*)
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

} // namespace arrange_ui
