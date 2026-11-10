// Arrange -- a Pitch seq track's sound (the keeper that borrows the instrument),
// auto-freeze of the seq tracks ARM is not on, and which kit track the kits' engine plays.

#include "arrange_internal.hpp"
#include "crosspad/instrument/InstrumentLoadNote.hpp"
#include "crosspad/song/SongSounds.hpp"

#include "crosspad/song/AutoFreeze.hpp"

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

/// The song has a Pitch seq track in use -- muted or soloed out too. A mute
/// is not a reason to give the instrument back: core silences the muted
/// route's notes itself (applySeqSound), while letting go of it reset the
/// engine arena under the playing song. With the Pitch track armed, every
/// M on it swapped the engines (PitchedKit out, the kit in, and back on
/// the un-mute) and the arena reset ended the playback.
static bool song_has_pitch_track()
{
    auto* se = engine();
    if (!se) return false;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const auto& song = se->song();
    for (int i = 0; i < kTracks; ++i) {
        const auto& t = song.tracks[i];
        if (t.inUse && t.kind == crosspad::TrackKind::Seq &&
            t.source == crosspad::SeqSource::Pitch) return true;
    }
    return false;
}

/* Auto-freeze (ISongEngine::freezeTrack): every seq track ARM is not on that is
 * not frozen as it is -- the one ARM just left, a new track added, and one gone
 * stale since (its notes, its kit, the tempo) -- is frozen while the song is
 * stopped, with this screen up (the seq view and Pitch Play use the sequencer a
 * freeze takes), one at a time (crosspad::autoFreezeNext()), so the tracks not
 * being played cost a file's stream instead of an engine. PLAY, REC and DUB
 * cancel a freeze, and the next stop finishes it from the renders already on
 * the card; a freeze that failed is tried again later, later still each time
 * (crosspad::RetryBackoff). A song opened carries nothing over. */
static std::string s_freezeSong;
/// How long a mixdown waits for a kit track's kit to come into the kits' engine.
constexpr uint32_t kExportWaitMs = 20000;
static int         s_freezeRunning = -1;   ///< the track a freeze asked for here is making
static crosspad::RetryBackoff s_freezeRetry[kTracks];   ///< after a failed freeze, on lv_tick
static uint64_t    s_freezeShown = UINT64_MAX;   ///< freezeState() of each track, two bits, as last drawn
/// How long the kits' engine waits on a track it went to freeze for that
/// track's kit to come in (live_kit()).
constexpr uint32_t kVisitLoadMs = 30000;
static int         s_visit = -1;          ///< the kit track the engine went to, to freeze it
static uint32_t    s_visitSince = 0;      ///< when, on lv_tick

/// The engine track @p t plays through holds its sound, loaded: what a freeze
/// renders is the track's. The kits' engine, the track's kit; Pitch, the
/// instrument.
static bool engine_holds(const crosspad::AudioSong& song, int t)
{
    switch (song.liveRoute(t)) {
    case 0: {
        crosspad::IKitManager* km = crosspad::getKitManager();
        const crosspad::KitInfo* cur = km ? km->getCurrentKit() : nullptr;
        return cur && cur->path == song.tracks[t].kit;
    }
    case 2:
        return crosspad::seqTrackSound().pitch.ready();
    default:
        return false;
    }
}

/* One kits' engine for every kit track (AudioSong::liveKit): it plays the
 * armed one -- whose kit is the song's, what the pads play and the Sampler
 * edits -- and the others play their freeze. ARM on another kit track, or a
 * new one, hands it over only once the track it leaves is frozen: its kit is
 * the one loaded, so it is frozen first (auto_freeze(), the song stopped and
 * this screen up -- a new kit track stops the song) and then the next kit
 * loads; the track is heard from its freeze right away. Not waited for: a
 * holder with nothing to freeze, or whose kit is not in the engine, or whose
 * freeze failed and is not due again. With the armed track live and another
 * kit track still to freeze (a tempo turned, a song from before), the engine
 * goes to that one while @p canFreeze and the player is away (player_away()),
 * it is frozen, and the engine comes back: under a player's fingers the
 * engine stays the armed track's, or a freeze giving way to the pads swung
 * it back and forth, loading one kit after the other under them. A track the
 * engine went to keeps it while its kit comes in, as long as the player stays
 * away: taken back at once, the engine would go to it and back every tick with
 * a kit track armed, its kit would never come and it would never freeze. A kit
 * that has not come in kVisitLoadMs is tried again later. @p st is
 * freezeState() of each track, @p skip the tracks whose last freeze failed
 * and is not due again. */
static void live_kit(crosspad::ISongEngine* se, const crosspad::FreezeState* st, uint32_t skip, bool canFreeze,
                     uint32_t now)
{
    int holder = -1, next = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        auto plays = [&](int t) {
            return song.seqRoute(t) == 0 && !song.seqPlaysNothing(t) && !song.tracks[t].kit.empty();
        };
        auto toFreeze = [&](int t) {
            return plays(t) && st[t] != crosspad::FreezeState::Frozen && !(skip & (1u << t));
        };
        holder = song.seqRoute(song.liveKit) == 0 ? song.liveKit : -1;
        const int want = song.seqRoute(song.armed) == 0 ? song.armed : -1;
        const bool away = canFreeze && player_away();
        bool holderWaits = false;
        if (holder >= 0 && holder != want && toFreeze(holder)) {
            if (engine_holds(song, holder)) {
                holderWaits = true;
            } else if (away && holder == s_visit) {
                if (now - s_visitSince < kVisitLoadMs) holderWaits = true;
                else                                   s_freezeRetry[holder].failed(now);
            }
        }
        if (want >= 0 && want != holder) {
            if (holderWaits) return;
            next = want;
        } else if (away && !holderWaits) {
            for (int t = 0; t < kTracks && next < 0; ++t)
                if (t != holder && t != want && toFreeze(t)) next = t;
        }
        if (next < 0 || next == holder) return;
        s_visit = next == want ? -1 : next;
        s_visitSince = now;
    }
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        se->song().setLiveKit(next);
    }
    se->commit();
}

static void auto_freeze(crosspad::ISongEngine* se)
{
    /* An element's enter or leave freezes the tracks it needs itself. */
    if (!se || se->openState() != crosspad::SongOpState::Ready || crosspad::elementNav().busy()) return;
    crosspad::FreezeState st[kTracks];
    for (int t = 0; t < kTracks; ++t) st[t] = se->freezeState((uint8_t)t);
    std::string dir;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        dir = se->song().dir;
    }
    if (dir != s_freezeSong) {
        s_freezeSong = dir;
        s_freezeRunning = -1;
        s_visit = -1;
        if (s_exportWav) {
            s_exportWav = false;          // asked of the song that was open, not of this one
            note("No mix: another song was opened.");
        }
        for (auto& r : s_freezeRetry) r.reset();
    }

    const bool bouncing = se->bounceStatus().state == crosspad::BounceState::Running;
    const uint32_t now = lv_tick_get();
    if (s_freezeRunning >= 0 && !bouncing) {
        /* Over: made, or failed -- tried again later, each time later still. A
         * PLAY's cancel is a failure too, and the stop that follows comes after
         * the first wait anyway. */
        const int t = s_freezeRunning;
        if (st[t] == crosspad::FreezeState::Frozen) s_freezeRetry[t].reset();
        else                                         s_freezeRetry[t].failed(now);
        s_freezeRunning = -1;
    }
    uint32_t skip = 0;
    for (int t = 0; t < kTracks; ++t)
        if (!s_freezeRetry[t].due(now)) skip |= 1u << t;
    /* Never under a render: the kit it renders through would change. */
    const bool canFreeze = s_visible && !bouncing && !se->playing() && s_freezeRunning < 0;
    if (!bouncing && s_freezeRunning < 0) live_kit(se, st, skip, canFreeze, now);
    /* A mixdown asked for: every seq track that is heard frozen -- the armed
     * one too, which auto-freeze leaves live -- then the mix is written. */
    if (s_exportWav && !bouncing && !se->playing() && s_freezeRunning < 0 &&
        se->editState() != crosspad::SongOpState::Busy) {
        int next = -1, stuck = -1;
        bool waiting = false;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            const auto& song = se->song();
            for (int t = 0; t < kTracks && next < 0; ++t) {
                const auto& tr = song.tracks[t];
                if (!tr.inUse || tr.kind != crosspad::TrackKind::Seq || song.silenced(t) || song.seqPlaysNothing(t) ||
                    st[t] == crosspad::FreezeState::Frozen) continue;
                /* Its sound must be in an engine first, or the freeze is of
                 * silence or of another kit: waited for, as live_kit() hands
                 * the kits' engine to each track still to freeze. */
                if (song.liveRoute(t) < 0 || !engine_holds(song, t)) { waiting = true; continue; }
                if (skip & (1u << t)) stuck = t;
                else                  next = t;
            }
        }
        const bool engineComing = next < 0 && waiting && now - s_exportAskedMs < kExportWaitMs;
        if (next < 0 && waiting && !engineComing) stuck = 0;
        if (engineComing) {
            /* the engine on its way: asked again on the next tick */
        } else if (next >= 0) {
            if (se->freezeTrack((uint8_t)next)) s_freezeRunning = next;
            else                                s_freezeRetry[next].failed(now);
        } else if (stuck >= 0) {
            s_exportWav = false;
            if (waiting) note("No mix: a kit track's kit did not load in time -- try again.");
            else         note_fmt("No mix: track %d would not freeze (its sound is not loaded?).", stuck + 1);
        } else {
            s_exportWav = false;
            if (se->requestExportWav()) {
                s_editing = true;
                s_editKind = EditKind::ExportWav;
                note("Writing the mix ...");
            }
        }
    }
    if (canFreeze) {
        int next = -1;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            const auto& song = se->song();
            /* Its sound not in its engine yet (a kit loading, the engine just
             * handed over): waited for, not a failure. */
            for (int t = 0; t < kTracks; ++t)
                if (song.liveRoute(t) >= 0 && !engine_holds(song, t)) skip |= 1u << t;
            next = crosspad::autoFreezeNext(song, st, skip);
        }
        if (next >= 0) {
            if (se->freezeTrack((uint8_t)next)) s_freezeRunning = next;
            else                                s_freezeRetry[next].failed(now);
        }
    }

    uint64_t shown = 0;
    for (int t = 0; t < kTracks; ++t) shown |= (uint64_t)st[t] << (2 * t);
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
    /* Not while a song's sounds are still coming: the path is then the last
     * song's, and SongSounds is loading this one's. */
    const crosspad::SongSounds* sounds = crosspad::getPlatformServices().songSounds;
    const bool want = host && !path.empty() && song_has_pitch_track() && !(sounds && sounds->pending()) &&
                      (s_visible || playing || s_soundBounce);

    if (want) {
        /* Asked once each time the kits have the engine back: Pitch Play and
         * the seq view give it back when they close, whatever else played it. */
        const auto st = host->state();
        const uint32_t now = lv_tick_get();
        if (path != s_soundPath) {
            s_soundPath = path;
            s_soundRetry.reset();
            s_soundFails = 0;
            s_soundSaid = false;
        }
        if (st == crosspad::IInstrumentHost::State::Ready) {
            s_soundAsked = false;
            s_soundRetry.reset();
            s_soundFails = 0;
        } else if (st == crosspad::IInstrumentHost::State::Failed) {
            /* It would not load -- no memory beside the kits, say: asked for again
             * later, later still each time, and said once the retry failed too
             * (the first try often fails only while the auto-freeze bounces).
             * A host left Failed by someone else's load is asked at once: it
             * never turns Idle by itself, and the Pitch track would stay silent
             * for good. */
            if (s_soundAsked) {
                s_soundAsked = false;
                s_soundRetry.failed(now);
                if (s_soundFails < crosspad::InstrumentLoadNote::kSayAfter) ++s_soundFails;
                if (s_soundFails >= crosspad::InstrumentLoadNote::kSayAfter && !s_soundSaid) {
                    s_soundSaid = true;
                    char msg[96];
                    crosspad::InstrumentLoadNote::text(host->message(), msg, sizeof(msg));
                    note(msg);
                }
            } else if (s_soundRetry.due(now)) {
                s_soundAsked = host->requestOpen(path);
            }
        } else if (st == crosspad::IInstrumentHost::State::Idle && !s_soundAsked) {
            s_soundAsked = host->requestOpen(path);
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
    if (pm.getActivePadLogic() == pm.getBasePadLogic()) s_soundPads = false;
    const bool padsWanted = want && pitchArmed && s_visible && !crosspad_gui::mode_overlay_is_open();
    if (padsWanted && !s_soundPads) {
        pm.pushPadLogic(kSoundLogic);
        s_soundPads = true;
    } else if (!padsWanted && s_soundPads && pm.getActivePadLogic() == kSoundLogic) {
        pm.popPadLogic();
        s_soundPads = false;
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
/// fingers: the song's notes on the kits' route (the
/// Drums track, EventSource::SequencerCallback) go on to the base logic --
/// pushed over it, the instrument would play the drum track's notes and the
/// kit would fall silent.
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
    s_soundTimer = lv_timer_create(sound_keeper_cb, 100, nullptr);
}

} // namespace arrange_ui
