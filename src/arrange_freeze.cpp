// Arrange -- a Pitch seq track's sound (the keeper that borrows the instrument) and
// auto-freeze of the seq tracks ARM leaves.

#include "arrange_internal.hpp"

namespace arrange_ui {

// ── A Pitch seq track's sound ──────────────────────────────────────────────
//
// A seq track that plays through Pitch Play needs the pitched engine, which
// the kits' engine gives up to it (one arena between them). The seq view used
// to be the only place that borrowed it, so a Pitch track was silent in
// Arrange and played only in EDIT. This keeper holds the instrument while the
// arrangement can play it -- this screen up, the song playing, or a bounce of
// the track running -- and tells the sequencer's sink to route the song's
// notes to it (SeqTrackSound). With this screen up the pads play it too: the
// kit has given up the arena, so the kit's pads would be silent here anyway.
//
// It outlives the screen (the song does), so it is a timer of its own and
// never deleted. LVGL task only.

/// The song has a Pitch seq track that plays (in use, not silenced).
static bool song_has_pitch_track()
{
    auto* se = engine();
    if (!se) return false;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const auto& song = se->song();
    for (int i = 0; i < kTracks; ++i) {
        const auto& t = song.tracks[i];
        if (t.inUse && t.kind == crosspad::TrackKind::Seq &&
            t.source == crosspad::SeqSource::Pitch && !song.silenced(i)) return true;
    }
    return false;
}

/// The pads play the second kit: its track armed with this screen up, or
/// the seq view open on that track.
static bool kit2_pads_wanted()
{
    auto* se = engine();
    if (!se || !crosspad::getPlatformServices().secondKit) return false;
    const crosspad::ArrangeFocus& f = crosspad::arrangeFocus();
    crosspad_gui::ILvglApp* fg = crosspad_gui::AppOrchestrator::getInstance().getForegroundApp();
    const bool seqView = f.held() && f.seq && fg && std::strcmp(fg->getName(), "Sequencer") == 0;
    if (!s_visible && !seqView) return false;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const auto& song = se->song();
    return song.seqRoute(seqView ? f.track : song.armedSeqTrack()) == 1;
}

/* Auto-freeze (ISongEngine::freezeTrack): the seq track ARM leaves -- another
 * armed in its place, a new track added -- is frozen at once while the song
 * is stopped (else when it stops), with this screen up
 * (the seq view and Pitch Play use the sequencer a freeze takes), so the
 * tracks not being played cost a file's stream instead of an engine. One
 * already frozen as it is stays; PLAY, REC and DUB cancel a freeze, and the
 * next stop finishes it from the renders already on the card. A song opened
 * carries nothing over. */
static std::string s_freezeSong;
static int         s_freezeArmed = -2;
static uint8_t     s_freezeWanted = 0;     ///< a bit per track
static int         s_freezeRunning = -1;   ///< the track a freeze asked for here is making
static uint8_t     s_freezeShown = 0xFF;   ///< freezeState() of each track, two bits, as last drawn

static void auto_freeze(crosspad::ISongEngine* se)
{
    if (!se || se->openState() != crosspad::SongOpState::Ready) return;
    int armed = -1;
    std::string dir;
    bool live[kTracks] = {};
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        armed = song.armed;
        dir = song.dir;
        for (int t = 0; t < kTracks; ++t) {
            const auto& tr = song.tracks[t];
            live[t] = tr.inUse && song.seqRoute(t) >= 0 && (!tr.seqBlocks || !tr.clips.empty());
        }
    }
    if (dir != s_freezeSong) {
        s_freezeSong = dir;
        s_freezeArmed = armed;
        s_freezeWanted = 0;
        s_freezeRunning = -1;
    }
    if (armed != s_freezeArmed) {
        if (s_freezeArmed >= 0 && s_freezeArmed < kTracks) s_freezeWanted |= (uint8_t)(1u << s_freezeArmed);
        s_freezeArmed = armed;
    }
    if (armed >= 0 && armed < kTracks) s_freezeWanted &= (uint8_t)~(1u << armed);

    const bool bouncing = se->bounceStatus().state == crosspad::BounceState::Running;
    if (s_freezeRunning >= 0 && !bouncing) {
        /* Cancelled (PLAY) or failed: wanted again, made at the next stop
         * unless it is armed by then. */
        if (se->freezeState((uint8_t)s_freezeRunning) != crosspad::FreezeState::Frozen &&
            s_freezeRunning != armed) {
            s_freezeWanted |= (uint8_t)(1u << s_freezeRunning);
        }
        s_freezeRunning = -1;
    }
    if (s_freezeWanted && s_visible && !bouncing && !se->playing()) {
        for (int t = 0; t < kTracks; ++t) {
            if (!(s_freezeWanted & (1u << t))) continue;
            s_freezeWanted &= (uint8_t)~(1u << t);
            if (!live[t] || se->freezeState((uint8_t)t) == crosspad::FreezeState::Frozen) continue;
            if (se->freezeTrack((uint8_t)t)) s_freezeRunning = t;
            break;
        }
    }

    uint8_t shown = 0;
    for (int t = 0; t < kTracks; ++t) shown |= (uint8_t)((uint8_t)se->freezeState((uint8_t)t) << (2 * t));
    if (shown != s_freezeShown) {
        s_freezeShown = shown;
        if (s_visible && s_timeline) rebuild_timeline();
    }
}

/// The processing box's text for a freeze Arrange started, while it runs:
/// the track, the renders through of those its plan needs (core finds the
/// ones it has on the card and renders only the rest) and how far the whole
/// is. False when no freeze of ours runs.
bool freeze_progress(crosspad::ISongEngine* se, char* buf, size_t cap)
{
    /* A bounce from BOUNCE is a freeze whose renders become the track's
     * audio (core 2.14): the same box, the same count. */
    const int track = s_bouncing >= 0 ? s_bouncing : s_freezeRunning;
    if (!se || track < 0 || se->freezeState((uint8_t)track) != crosspad::FreezeState::Freezing) return false;
    const crosspad::BounceStatus st = se->bounceStatus();
    const uint32_t pieces = st.pieces ? st.pieces : 1;
    const uint64_t part = st.framesTotal ? (uint64_t)st.framesDone * 100 / st.framesTotal : 0;
    const unsigned pct = (unsigned)std::min<uint64_t>(100, (st.piecesDone * 100 + part) / pieces);
    std::snprintf(buf, cap, "%s TRACK %d\nrender %u of %u  -  %u%%", s_bouncing >= 0 ? "BOUNCING" : "FREEZING",
                  track + 1, (unsigned)std::min<uint32_t>(st.piecesDone + 1, pieces), (unsigned)pieces, pct);
    return true;
}

void sound_keeper_cb(lv_timer_t*)
{
    auto& ts = crosspad::seqTrackSound();
    auto* se = engine();
    auto* host = crosspad::getPlatformServices().instrumentHost;
    auto& pm = crosspad::getPadManager();

    if (s_soundBounce && (!se || se->bounceStatus().state != crosspad::BounceState::Running)) {
        s_soundBounce = false;
    }
    auto_freeze(se);
    const bool playing = se && se->playing();
    const std::string path = crosspad::instrumentPlayState().instrumentPath;
    const bool want = host && !path.empty() && song_has_pitch_track() &&
                      (s_visible || playing || s_soundBounce);

    if (want) {
        /* Asked once each time the kits have the engine back: Pitch Play and
         * the seq view give it back when they close, whatever else played it. */
        const auto st = host->state();
        if (st == crosspad::IInstrumentHost::State::Idle && !s_soundAsked) {
            s_soundAsked = host->requestOpen(path);
        } else if (st == crosspad::IInstrumentHost::State::Ready) {
            s_soundAsked = false;
        }
        s_soundHeld = true;
    } else {
        s_soundAsked = false;
    }
    /* The route's notes play whenever the instrument is up, whoever holds the
     * screen: the seq view of a Pitch block plays it with Arrange gone and
     * the song stopped, and gated on `want` it was silent there. */
    const bool ready = host && !path.empty() && song_has_pitch_track() &&
                       host->state() == crosspad::IInstrumentHost::State::Ready;
    ts.pitch.setReady(ready);

    /* The fingers, here, while the Pitch track is armed: pushed only with the
     * MODE map down -- the map keeps its own entry on the stack while it is
     * up -- and popped only while it is ours on top. With a kit track armed
     * the pads play its kit, as they do without a Pitch track. */
    bool pitchArmed = false;
    if (se) {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        pitchArmed = song.seqRoute(song.armedSeqTrack()) == 2;
    }
    /* The base logic on top means nothing pushed is left -- ours went with a
     * screen that popped past it (the kit picker, the seq view) -- so a flag
     * still set would keep the pads off the armed track for good. */
    if (pm.getActivePadLogic() == pm.getBasePadLogic()) {
        s_soundPads = false;
        s_kit2Pads = false;
    }
    const bool padsWanted = want && pitchArmed && s_visible && !crosspad_gui::mode_overlay_is_open();
    if (padsWanted && !s_soundPads) {
        pm.pushPadLogic(kSoundLogic);
        s_soundPads = true;
    } else if (!padsWanted && s_soundPads && pm.getActivePadLogic() == kSoundLogic) {
        pm.popPadLogic();
        s_soundPads = false;
    }

    /* The second kit's pads: here while its track is armed, and in the seq
     * view of one of its blocks -- what is played is what the take keeps. */
    const bool kit2Wanted = kit2_pads_wanted() && !crosspad_gui::mode_overlay_is_open();
    if (kit2Wanted && !s_kit2Pads) {
        pm.pushPadLogic(kKit2Logic);
        s_kit2Pads = true;
    } else if (!kit2Wanted && s_kit2Pads && pm.getActivePadLogic() == kKit2Logic) {
        pm.popPadLogic();
        s_kit2Pads = false;
    }

    if (!want && s_soundHeld) {
        s_soundHeld = false;
        /* Not out from under the screen that plays it now: Pitch Play and the
         * seq view borrow the same engine and give it back themselves. */
        crosspad_gui::ILvglApp* fg = crosspad_gui::AppOrchestrator::getInstance().getForegroundApp();
        const char* name = fg ? fg->getName() : nullptr;
        const bool theirs = name && (std::strcmp(name, "PitchPlay") == 0 || std::strcmp(name, "Sequencer") == 0);
        if (!theirs && host && host->state() != crosspad::IInstrumentHost::State::Idle) host->release();
    }
}

/// The fingers on the Pitch track's instrument while it is armed. Only the
/// fingers, as SecondKitPadLogic: the song's notes on the pads' route (the
/// Drums track, EventSource::SequencerCallback) go on to the base logic --
/// pushed over it, the instrument played the drum track's notes and the kit
/// fell silent (hil_perf_song C, 2026-10-22).
class PitchFingers final : public crosspad::IPadLogicHandler {
public:
    void onActivate(crosspad::PadManager& pm) override { pitch().onActivate(pm); }
    void onDeactivate(crosspad::PadManager& pm) override { pitch().onDeactivate(pm); }
    void onPadPress(crosspad::PadManager& pm, uint8_t pad, uint8_t velocity) override
    {
        if (pm.pressSource(pad) == crosspad::EventSource::SequencerCallback) {
            if (auto base = beneath(pm)) base->onPadPress(pm, pad, velocity);
            return;
        }
        pitch().onPadPress(pm, pad, velocity);
    }
    void onPadRelease(crosspad::PadManager& pm, uint8_t pad) override
    {
        if (pm.pressSource(pad) == crosspad::EventSource::SequencerCallback) {
            if (auto base = beneath(pm)) base->onPadRelease(pm, pad);
            return;
        }
        pitch().onPadRelease(pm, pad);
    }
    void onPadPressure(crosspad::PadManager& pm, uint8_t pad, uint8_t pressure) override
    {
        pitch().onPadPressure(pm, pad, pressure);
    }

private:
    static crosspad::IPadLogicHandler& pitch() { return crosspad::seqTrackSound().pitch; }
    std::shared_ptr<crosspad::IPadLogicHandler> beneath(crosspad::PadManager& pm) const
    {
        auto base = pm.getPadLogic(pm.getBasePadLogic());
        return base.get() == this ? nullptr : base;
    }
};

void sound_keeper_start()
{
    if (s_soundTimer) return;
    crosspad::getPadManager().registerPadLogic(kSoundLogic, std::make_shared<PitchFingers>());
    crosspad::getPadManager().registerPadLogic(
        kKit2Logic, std::shared_ptr<crosspad::IPadLogicHandler>(&crosspad::seqTrackSound().kit2,
                                                                [](crosspad::IPadLogicHandler*) {}));
    s_soundTimer = lv_timer_create(sound_keeper_cb, 100, nullptr);
}

} // namespace arrange_ui
