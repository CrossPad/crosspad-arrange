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

/// Each track's dB and M as the song has them.
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
        if (s_mixMute[t]) {
            set_bg(s_mixMute[t], tr.muted ? kMute : kTrack);          // a button on the strip's panel
            set_ink(lv_obj_get_child(s_mixMute[t], 0), tr.muted ? kInk : kText);
        }
    }
}

/// A fader moved: heard at once; commit() saves the song once it rests.
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

/// A track's strip, as on a desk: its number, a tall fader, the dB, M.
static void mix_strip(lv_obj_t* strips, int t, float volume)
{
    lv_obj_t* strip = lv_obj_create(strips);
    lv_obj_remove_style_all(strip);
    lv_obj_set_height(strip, LV_PCT(100));
    lv_obj_set_flex_grow(strip, 1);
    lv_obj_set_style_bg_color(strip, kPanel, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(strip, 6, 0);
    lv_obj_set_style_pad_ver(strip, 4, 0);
    lv_obj_set_style_pad_row(strip, 4, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* num = lv_label_create(strip);
    lv_obj_set_style_text_font(num, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(num, kText, 0);
    set_text_fmt(num, "%d", t + 1);

    /* Taller than wide: LVGL makes it a vertical slider, the bottom its 0. */
    lv_obj_t* s = lv_slider_create(strip);
    lv_obj_set_width(s, 8);
    lv_obj_set_flex_grow(s, 1);
    lv_slider_set_range(s, 0, kFaderMax);
    lv_slider_set_value(s, gain_to_fader(volume), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, kTrack, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, kAccent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, kHead, LV_PART_KNOB);
    lv_obj_set_style_radius(s, 4, LV_PART_KNOB);
    lv_obj_set_style_pad_hor(s, 10, LV_PART_KNOB);     // a fader cap, wider than its slot
    lv_obj_set_style_pad_ver(s, 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s, kAccent, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(s, kAccent, (lv_style_selector_t)LV_PART_KNOB | LV_STATE_EDITED);
    lv_obj_set_style_outline_width(s, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_ext_click_area(s, 16);
    lv_obj_add_event_cb(s, mix_fader_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)t);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, s);
#endif

    s_mixDb[t] = lv_label_create(strip);
    lv_obj_set_style_text_font(s_mixDb[t], &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_mixDb[t], kText, 0);
    lv_label_set_text(s_mixDb[t], "");

    s_mixMute[t] = make_button(strip, 40, "M", mix_mute_cb, (void*)(intptr_t)t);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, s_mixMute[t]);
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
    float vol[kTracks];
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        for (int t = 0; t < kTracks; ++t) vol[t] = se->song().tracks[t].volume;
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
    for (int t = 0; t < kTracks; ++t) mix_strip(strips, t, vol[t]);
#ifdef ARR_USE_ENCODER
    if (s_overlayGroup) lv_group_add_obj(s_overlayGroup, done);   // after the strips: the first fader has the focus
#endif
    mix_show();
}

} // namespace arrange_ui
