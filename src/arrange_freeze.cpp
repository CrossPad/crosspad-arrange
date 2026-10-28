// Arrange -- a Pitch seq track's sound (the keeper that borrows the instrument),
// auto-freeze of the seq tracks ARM is not on, and which kit tracks the two kit engines play.

#include "arrange_internal.hpp"
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

/// The track the seq view has open, or -1 when it is not on screen.
static int seq_view_track()
{
    const crosspad::ArrangeFocus& f = crosspad::arrangeFocus();
    crosspad_gui::ILvglApp* fg = crosspad_gui::AppOrchestrator::getInstance().getForegroundApp();
    return f.held() && f.seq && fg && std::strcmp(fg->getName(), "Sequencer") == 0 ? f.track : -1;
}

/// The pads play a kit slot's engine: a track with a kit of its own armed
/// with this screen up, or the seq view open on such a track -- while the
/// track holds a slot. Aims SecondKitPadLogic at that slot.
static bool kit2_pads_wanted()
{
    auto* se = engine();
    if (!se || !crosspad::getPlatformServices().secondKit) return false;
    const int seqView = seq_view_track();
    if (!s_visible && seqView < 0) return false;
    int route = -1;
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        const auto& song = se->song();
        route = song.liveRoute(seqView >= 0 ? seqView : song.armedSeqTrack());
    }
    if (route != 1 && route != 3) return false;
    crosspad::seqTrackSound().kit2.setSlot(route == 3 ? 1 : 0);
    return true;
}

/// The engine of kit slot @p slot: 0 the second kit, 1 the third.
static crosspad::ISecondKit* slot_engine(int slot)
{
    const auto& ps = crosspad::getPlatformServices();
    return slot ? ps.thirdKit : ps.secondKit;
}

/* Auto-freeze (ISongEngine::freezeTrack): every seq track ARM is not on that is
 * not frozen as it is -- the one ARM just left, a new track added, and one gone
 * stale since (its notes, its kit, the tempo) -- is frozen while the song is
 * stopped, with this screen up (the seq view and Pitch Play use the sequencer a
 * freeze takes), one at a time (crosspad::autoFreezeNext()), so the tracks not
 * being played cost a file's stream instead of an engine. Only an ARM change
 * froze before: a tempo turned or a kit picked left every track live for good
 * (26.10.2026). PLAY, REC and DUB cancel a freeze, and the next stop finishes it
 * from the renders already on the card; a freeze that failed is tried again
 * later, later still each time (crosspad::RetryBackoff). A song opened carries
 * nothing over. */
static std::string s_freezeSong;
/// How long a mixdown waits for a kit track's kit to come into a slot.
constexpr uint32_t kExportWaitMs = 20000;
static int         s_freezeRunning = -1;   ///< the track a freeze asked for here is making
static crosspad::RetryBackoff s_freezeRetry[kTracks];   ///< after a failed freeze, on lv_tick
static uint64_t    s_freezeShown = UINT64_MAX;   ///< freezeState() of each track, two bits, as last drawn

/// The engine track @p t plays through holds its sound, loaded: what a freeze
/// renders is the track's. The second kit, its own kit; the pads' kit, the
/// song's; Pitch, the instrument.
static bool engine_holds(const crosspad::AudioSong& song, int t)
{
    switch (song.liveRoute(t)) {
    case 0: {
        crosspad::IKitManager* km = crosspad::getKitManager();
        const crosspad::KitInfo* cur = km ? km->getCurrentKit() : nullptr;
        return cur && cur->path == song.kit;
    }
    case 1:
    case 3: {
        const crosspad::ISecondKit* k = slot_engine(song.kitSlotOf(t));
        return k && k->state() == crosspad::ISecondKit::State::Ready && k->kit() == song.tracks[t].kit;
    }
    case 2:
        return crosspad::seqTrackSound().pitch.ready();
    default:
        return false;
    }
}

/* The two kit engines play the kit tracks with a kit of their own two at a
 * time (AudioSong::kitSlots), and any number of them are in a song: the rest
 * play their freeze. The track being played into -- the one the seq view has
 * open, else the armed one -- takes one at once, playing or stopped, and the
 * one it left keeps the other until it is frozen (crosspad::kitSlotsNext()).
 * The new holder's kit is asked for here and now, not on the song keeper's
 * next step: ARM to the pads playing it was ~1 s, a third of it that wait.
 * @p st is freezeState() of each track, @p skip the tracks whose last freeze
 * failed and is not due again. */
static int s_slotWant = -1, s_slotPrevWant = -1;

static void kit_slot(crosspad::ISongEngine* se, const crosspad::FreezeState* st, uint32_t skip)
{
    const int seqView = seq_view_track();
    int8_t slots[crosspad::AudioSong::kKitSlots];
    std::string kits[crosspad::AudioSong::kKitSlots];
    {
        std::lock_guard<std::recursive_mutex> l(se->songMutex());
        auto& song = se->song();
        const int want = seqView >= 0 ? seqView : song.armed;
        if (want != s_slotWant) {
            s_slotPrevWant = s_slotWant;
            s_slotWant = want;
        }
        const auto& ps = crosspad::getPlatformServices();
        const int usable = ps.thirdKit ? crosspad::AudioSong::kKitSlots : 1;
        /* A frozen kit stays cached in its slot while the song plays -- but
         * not while the Pitch instrument could not be built beside the kits:
         * the cached heads are the memory it is missing, and its keeper asks
         * again later (zone_0: no memory, hil_torture_song, 2026-10-28). */
        const bool starved = ps.instrumentHost && song_has_pitch_track() &&
                             ps.instrumentHost->state() == crosspad::IInstrumentHost::State::Failed;
        const bool cache = se->playing() && !starved;
        if (!crosspad::kitSlotsNext(song, st, want, s_slotPrevWant, skip, cache, slots, usable)) return;
        for (int s = 0; s < crosspad::AudioSong::kKitSlots; ++s) {
            song.kitSlots[s] = slots[s];
            if (slots[s] >= 0) kits[s] = song.tracks[slots[s]].kit;
        }
    }
    se->commit();
    for (int s = 0; s < crosspad::AudioSong::kKitSlots; ++s)
        if (crosspad::ISecondKit* k = slot_engine(s)) k->hold(kits[s]);
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
    /* Playing or stopped: ARM moves while the song plays (a live set). Not
     * under a freeze or a bounce, whose engine must stay the track's. */
    if ((s_visible || seq_view_track() >= 0) && !bouncing && s_freezeRunning < 0) kit_slot(se, st, skip);
    /* Playing: the next track a freeze would make is harvested instead --
     * recorded out of the song as it passes, in the background, no box on
     * the screen -- so a kit of its own gives its engine back without a stop
     * (ISongEngine::harvestTrack()). Only a kit of its own: the pads' kit and
     * Pitch stay loaded either way. */
    if (se->playing() && !bouncing && s_freezeRunning < 0 && se->harvestingTrack() < 0) {
        int next = -1;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            const auto& song = se->song();
            uint32_t own = skip;
            for (int t = 0; t < kTracks; ++t) {
                const int r = song.liveRoute(t);
                if ((r != 1 && r != 3) || !engine_holds(song, t)) own |= 1u << t;
            }
            next = crosspad::autoFreezeNext(song, st, own);
        }
        if (next >= 0 && !se->harvestTrack((uint8_t)next)) s_freezeRetry[next].failed(now);
    }
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
                 * silence or of another kit: waited for, as kit_slot() hands
                 * the slots out to the tracks that need them. */
                if (song.liveRoute(t) < 0 || !engine_holds(song, t)) { waiting = true; continue; }
                if (skip & (1u << t)) stuck = t;
                else                  next = t;
            }
        }
        const bool slotComing = next < 0 && waiting && now - s_exportAskedMs < kExportWaitMs;
        if (next < 0 && waiting && !slotComing) stuck = 0;
        if (slotComing) {
            /* a slot on its way: asked again on the next tick */
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
    if (s_visible && !bouncing && !se->playing() && s_freezeRunning < 0) {
        int next = -1;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            const auto& song = se->song();
            /* Its sound not in its engine yet (a kit loading, the second kit
             * just handed over): waited for, not a failure. */
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

/// A freeze Arrange started, while it runs: the track and how far the whole
/// is -- the renders through of those its plan needs (core finds the ones it
/// has on the card and renders only the rest). False when no freeze of ours
/// runs.
bool freeze_progress(crosspad::ISongEngine* se, int& track, unsigned& pct)
{
    /* A bounce from BOUNCE is a freeze whose renders become the track's
     * audio (core 2.14): the same count. */
    track = s_bouncing >= 0 ? s_bouncing : s_freezeRunning;
    if (!se || track < 0 || se->freezeState((uint8_t)track) != crosspad::FreezeState::Freezing) return false;
    const crosspad::BounceStatus st = se->bounceStatus();
    const uint32_t pieces = st.pieces ? st.pieces : 1;
    const uint64_t part = st.framesTotal ? (uint64_t)st.framesDone * 100 / st.framesTotal : 0;
    pct = (unsigned)std::min<uint64_t>(100, (st.piecesDone * 100 + part) / pieces);
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
            s_soundSaid = false;
        }
        if (st == crosspad::IInstrumentHost::State::Ready) {
            s_soundAsked = false;
            s_soundRetry.reset();
        } else if (st == crosspad::IInstrumentHost::State::Failed) {
            /* It would not load -- no memory beside the kits, say: asked for again
             * later, later still each time, and said once. A host left Failed by
             * someone else's load is asked at once: it never turns Idle by itself,
             * and the Pitch track stayed silent for good (26.10.2026). */
            if (s_soundAsked) {
                s_soundAsked = false;
                s_soundRetry.failed(now);
                if (!s_soundSaid) {
                    s_soundSaid = true;
                    note_fmt("Pitch sound did not load: %s", host->message().c_str());
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
