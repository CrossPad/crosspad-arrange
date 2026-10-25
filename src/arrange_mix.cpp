// Arrange -- MIX: the four tracks' levels, a strip each, as on a desk.

#include "arrange_internal.hpp"

namespace arrange_ui {

// ── MIX: the four tracks' levels ───────────────────────────────────────────

/* Fader travel as the Mixer app's: 0 is -inf, then 0.6 dB a step up to +6 dB
 * at the top, which puts unity at 90. */
constexpr int32_t kFaderMax   = 100;
constexpr int32_t kFaderUnity = 90;
constexpr float   kDbPerStep  = 0.6f;

static float fader_to_gain(int32_t v)
{
    if (v <= 0) return 0.0f;
    return powf(10.0f, ((float)(v - kFaderUnity) * kDbPerStep) / 20.0f);
}

static int32_t gain_to_fader(float g)
{
    if (g <= 0.0f) return 0;
    const int32_t v = (int32_t)lroundf(20.0f * log10f(g) / kDbPerStep) + kFaderUnity;
    return v < 1 ? 1 : (v > kFaderMax ? kFaderMax : v);
}

/// The MIX window's widgets go with it.
void mix_forget()
{
    for (int t = 0; t < kTracks; ++t) {
        s_mixDb[t] = s_mixMute[t] = s_mixSolo[t] = s_mixPan[t] = s_mixPanSlider[t] = nullptr;
        s_mixMeter[t][0] = s_mixMeter[t][1] = nullptr;
        s_mixShown[t][0] = s_mixShown[t][1] = 0.0f;
    }
}

/// Each track's dB, pan, M and S as the song has them.
static void mix_show()
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
        if (s_mixPan[t]) {
            const int p = std::min(50, std::max(-50, (int)lroundf(tr.pan * 50.0f)));
            char buf[12];
            if (p == 0) std::snprintf(buf, sizeof(buf), "C");
            else        std::snprintf(buf, sizeof(buf), "%c%d", p < 0 ? 'L' : 'R', p < 0 ? -p : p);
            set_text(s_mixPan[t], buf);
        }
        style_toggle(s_mixMute[t], tr.muted, kMute);
        style_toggle(s_mixSolo[t], tr.solo, kKitT);
    }
}

/// The meters beside the faders, as the ones beside the lanes: what each
/// track plays, after its fader and pan, straight up and down at ~28 dB a
/// second, over the lanes' -48 dB to 0 -- green, amber from -6 dB, red from -1.
void mix_meters(crosspad::ISongEngine* se)
{
    constexpr float kFall = 0.85f;
    for (int t = 0; t < kTracks; ++t) {
        float lv[2] = {0.0f, 0.0f};
        se->trackLevels(t, lv[0], lv[1]);
        for (int c = 0; c < 2; ++c) {
            lv_obj_t* bar = s_mixMeter[t][c];
            if (!bar) continue;
            s_mixShown[t][c] = std::max(lv[c], s_mixShown[t][c] * kFall);
            const float db = s_mixShown[t][c] > 0.0f ? 20.0f * std::log10(s_mixShown[t][c]) : -96.0f;
            const int32_t v = db <= -48.0f ? 0 : db >= 0.0f ? 100 : (int32_t)lroundf((db + 48.0f) * 100.0f / 48.0f);
            if (lv_bar_get_value(bar) == v) continue;
            lv_bar_set_value(bar, v, LV_ANIM_OFF);
            const lv_color_t col = db >= -1.0f ? kMute : db >= -6.0f ? kKitT : kWave;
            if (!lv_color_eq(lv_obj_get_style_bg_color(bar, LV_PART_INDICATOR), col)) {
                lv_obj_set_style_bg_color(bar, col, LV_PART_INDICATOR);
            }
        }
    }
}

static void mix_fader_cb(lv_event_t* e)
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

static void mix_mute_cb(lv_event_t* e)
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

static void mix_solo_cb(lv_event_t* e)
{
    toggle_solo((int)(intptr_t)lv_event_get_user_data(e));
    mix_show();
}

/// Track @p t's pan, -50..+50: heard at once, saved once it rests.
static void mix_set_pan(int t, int32_t v)
{
    auto* se = engine();
    if (!se) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        se->song().tracks[t].pan = (float)v / 50.0f;
    }
    se->commit();
    mix_show();
}

static void mix_pan_cb(lv_event_t* e)
{
    mix_set_pan((int)(intptr_t)lv_event_get_user_data(e),
                lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e))));
}

/// A tap on the pan's reading puts it back in the centre, as a double click
/// on a desk's pan does: a slider being held follows the finger, so it
/// cannot be the one to do it.
static void mix_pan_centre_cb(lv_event_t* e)
{
    const int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_mixPanSlider[t]) lv_slider_set_value(s_mixPanSlider[t], 0, LV_ANIM_OFF);
    mix_set_pan(t, 0);
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

/// ARM: REC takes onto @p t -- a seq track's pads, whose kit they then play
/// here, or an audio track's input.
void arm_track(int t)
{
    auto* se = engine();
    if (!se || t < 0 || t >= kTracks) return;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        const auto& tr = song.tracks[t];
        const bool audio = tr.inUse && tr.kind == crosspad::TrackKind::Audio;
        if ((song.seqRoute(t) < 0 && !audio) || song.armed == t) return;
        song.armed = (int8_t)t;
    }
    se->commit();
    rebuild_timeline();
}

/// A thin vertical bar for a meter: taller than wide, LVGL fills it upwards.
static lv_obj_t* mix_meter_bar(lv_obj_t* parent)
{
    lv_obj_t* b = lv_bar_create(parent);
    lv_obj_set_size(b, 4, LV_PCT(100));
    lv_bar_set_range(b, 0, 100);
    lv_obj_set_style_radius(b, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 1, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, kWell, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, kWave, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(b, 0, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

/// A track's strip, as on a DAW's mixer: its number and kind in its colour,
/// the pan, a tall fader with the track's meter beside it, the dB, M and S.
static void mix_strip(lv_obj_t* strips, int t, const crosspad::SongTrack& tr)
{
    lv_obj_t* strip = lv_obj_create(strips);
    lv_obj_remove_style_all(strip);
    lv_obj_set_height(strip, LV_PCT(100));
    lv_obj_set_flex_grow(strip, 1);
    lv_obj_set_style_bg_color(strip, kPanel, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(strip, 6, 0);
    lv_obj_set_style_pad_all(strip, 3, 0);
    lv_obj_set_style_pad_row(strip, 3, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    /* The name chip: the track's number and kind in the colour the timeline
     * gives it, dim for an empty track. */
    lv_obj_t* chip = lv_label_create(strip);
    lv_obj_set_width(chip, LV_PCT(100));
    lv_obj_set_style_text_font(chip, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(chip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_radius(chip, 3, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(chip, tr.inUse ? track_color(tr) : kWell, 0);
    lv_obj_set_style_text_color(chip, tr.inUse ? kInk : kDim, 0);
    lv_label_set_long_mode(chip, LV_LABEL_LONG_DOT);
    if (!tr.inUse)             set_text_fmt(chip, "%d  -", t + 1);
    else if (!tr.name.empty()) set_text_fmt(chip, "%d %s", t + 1, tr.name.c_str());
    else                       set_text_fmt(chip, "%d %s", t + 1, track_badge(tr));

    /* Pan: the reading over a short slider from the centre out. */
    s_mixPan[t] = lv_label_create(strip);
    lv_obj_set_style_text_font(s_mixPan[t], &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_mixPan[t], kDim, 0);
    lv_label_set_text(s_mixPan[t], "");
    lv_obj_add_flag(s_mixPan[t], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_mixPan[t], 6);
    lv_obj_add_event_cb(s_mixPan[t], mix_pan_centre_cb, LV_EVENT_CLICKED, (void*)(intptr_t)t);
    lv_obj_t* pan = lv_slider_create(strip);
    lv_obj_set_size(pan, LV_PCT(84), 6);
    lv_slider_set_range(pan, -50, 50);
    lv_slider_set_mode(pan, LV_SLIDER_MODE_SYMMETRICAL);
    lv_slider_set_value(pan, (int32_t)lroundf(tr.pan * 50.0f), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(pan, kTrack, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pan, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pan, kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(pan, kHead, LV_PART_KNOB);
    lv_obj_set_style_pad_all(pan, 3, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(pan, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_color(pan, kAccent, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(pan, kAccent, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_EDITED);
    lv_obj_set_style_outline_width(pan, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_ext_click_area(pan, 8);
    lv_obj_add_event_cb(pan, mix_pan_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)t);
    s_mixPanSlider[t] = pan;
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, pan);
#endif

    /* The fader, and the meter beside it, as a channel strip has them. */
    lv_obj_t* body = lv_obj_create(strip);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(body, 2, 0);
    lv_obj_set_style_pad_ver(body, 4, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    /* Taller than wide: LVGL makes it a vertical slider, the bottom its 0. */
    lv_obj_t* s = lv_slider_create(body);
    lv_obj_set_size(s, 8, LV_PCT(100));
    lv_slider_set_range(s, 0, kFaderMax);
    lv_slider_set_value(s, gain_to_fader(tr.volume), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, kTrack, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, kHead, LV_PART_KNOB);
    lv_obj_set_style_radius(s, 4, LV_PART_KNOB);
    lv_obj_set_style_pad_hor(s, 8, LV_PART_KNOB);     // a fader cap, wider than its slot
    lv_obj_set_style_pad_ver(s, 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s, kAccent, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(s, kAccent, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_EDITED);
    lv_obj_set_style_outline_width(s, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_ext_click_area(s, 12);
    lv_obj_add_event_cb(s, mix_fader_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)t);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, s);
#endif
    /* The cap's room, then L and R. */
    lv_obj_t* gap = lv_obj_create(body);
    lv_obj_remove_style_all(gap);
    lv_obj_set_size(gap, 8, 1);
    s_mixMeter[t][0] = mix_meter_bar(body);
    s_mixMeter[t][1] = mix_meter_bar(body);

    s_mixDb[t] = lv_label_create(strip);
    lv_obj_set_style_text_font(s_mixDb[t], &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_mixDb[t], kText, 0);
    lv_label_set_text(s_mixDb[t], "");

    lv_obj_t* ms = lv_obj_create(strip);
    lv_obj_remove_style_all(ms);
    lv_obj_set_size(ms, LV_PCT(100), 22);
    lv_obj_set_flex_flow(ms, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ms, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ms, 3, 0);
    lv_obj_remove_flag(ms, LV_OBJ_FLAG_SCROLLABLE);
    s_mixMute[t] = make_button(ms, 10, "M", mix_mute_cb, (void*)(intptr_t)t);
    lv_obj_set_flex_grow(s_mixMute[t], 1);
    s_mixSolo[t] = make_button(ms, 10, "S", mix_solo_cb, (void*)(intptr_t)t);
    lv_obj_set_flex_grow(s_mixSolo[t], 1);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) {
        lv_group_add_obj(s_overlayGroup, s_mixMute[t]);
        lv_group_add_obj(s_overlayGroup, s_mixSolo[t]);
    }
#endif
}

static void mix_done_cb(lv_event_t*)
{
    close_overlay();
    rebuild_timeline();                          // a muted track dims on the timeline
}

/// MIX: a strip per track side by side, Done beside the title.
void mix_cb(lv_event_t*)
{
    auto* se = engine();
    if (!se || s_overlay) return;
    crosspad::SongTrack tracks[kTracks];
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        for (int t = 0; t < kTracks; ++t) {
            const auto& src = se->song().tracks[t];
            tracks[t].inUse = src.inUse;
            tracks[t].kind = src.kind;
            tracks[t].source = src.source;
            tracks[t].name = src.name;
            tracks[t].volume = src.volume;
            tracks[t].pan = src.pan;
        }
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
    for (int t = 0; t < kTracks; ++t) mix_strip(strips, t, tracks[t]);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, done);   // after the strips: the first fader has the focus
#endif
    mix_show();
}

} // namespace arrange_ui
