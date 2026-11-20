/**
 * @file StudioTimelineView.cpp
 * @brief Timeline coordinate mapping, hit-testing, hover tooltips/cursors,
 *        waveform/spectrogram tile caching, and timeline rendering for `Studio`.
 *
 * Coordinate spaces and units:
 * - Horizontal axis: project time in seconds (`viewStart` to `viewStart + viewLength()`),
 *   44.1 kHz half-open `[start, end)` project samples (`carta::sessionRate`), and
 *   screen X pixels in `[area.getX() + labelWidth, area.getRight())`, where
 *   `labelWidth = trackLabelWidth + scaleWidth` (110 + 52 = 162 px).
 * - Vertical axis: `timeline()` begins below the menu and transport bar
 *   (`menuHeight() + 32`), starting with a 32 px time ruler and an optional
 *   24 px MiniDisc track ribbon (`timelineHeaderHeight()`), followed by track
 *   rows starting at `firstTrack`. `marqueeDocumentY()` tracks virtual document
 *   pixels relative to track 0 so vertical scrolling preserves marquee anchors.
 *
 * Threading and cache ownership invariants:
 * - All view geometry, hit-testing, `waveforms`, `spectra`, and painting run
 *   strictly on the JUCE message thread; `paint()` briefly snapshots `position`
 *   under `Studio::lock`.
 * - Single-threaded `waveformPool` and `spectrumPool` workers read immutable
 *   `std::shared_ptr<carta::Audio>` buffers and deliver results via
 *   `MessageManager::callAsync` guarded by `SafePointer<Studio>` and cancellation
 *   tokens (`waveformPaintEpoch` cancels jobs that scrolled off-screen).
 * - Cache limits: `waveforms` is capped at 16 MiB / 256 entries and `spectra`
 *   at 64 MiB / 192 entries via LRU eviction.
 */

#include "Studio/Studio.h"

namespace {

constexpr int kTimelineRulerHeight = 32;
constexpr int kMdRibbonHeight = 24;
constexpr int kSnapBoundaryRadiusPx = 7;
constexpr int kClipEdgeHitPx = 7;

namespace HeaderHit {
constexpr int none = 0;
constexpr int collapse = 1;
constexpr int mute = 2;
constexpr int solo = 3;
constexpr int gain = 4;
constexpr int balance = 5;
} // namespace HeaderHit

} // namespace

// ============================================================================
// Ruler, MD Ribbon Geometry & Hit-Testing
// ============================================================================

std::vector<int> Studio::timelineMarkerPixels(juce::Rectangle<int> area, double span) const {
    std::vector<int> pixels(session.markers.size(), std::numeric_limits<int>::min());
    for (size_t i = 0; i < session.markers.size(); ++i) {
        if (std::isfinite(session.markers[i].position) && session.markers[i].name.isNotEmpty()) {
            pixels[i] = timeToX(session.markers[i].position, area, span);
        }
    }
    return pixels;
}

int Studio::visibleMdRibbonHeight() const {
    return discWorkspaceVisible && visibleDiscWorkspaceHeight() > 0 &&
                   !session.mdDisc.tracks.empty()
               ? kMdRibbonHeight
               : 0;
}

int Studio::timelineHeaderHeight() const {
    return kTimelineRulerHeight + visibleMdRibbonHeight();
}

juce::Rectangle<int> Studio::mdRibbonArea() const {
    const auto area = timeline();
    const int h = visibleMdRibbonHeight();
    return {area.getX(), area.getY() + kTimelineRulerHeight, area.getWidth(),
            std::min(h, std::max(0, area.getHeight() - kTimelineRulerHeight))};
}

juce::Colour Studio::mdRibbonGroupColour(int colorIdx, bool dark) {
    static const juce::uint32 darkColors[] = {0xff315e8a, 0xff2b6e66, 0xff70548c, 0xff805a33};
    static const juce::uint32 lightColors[] = {0xff3b7ddd, 0xff1e8275, 0xff7c4da3, 0xffb36c24};
    if (colorIdx < 0) {
        return dark ? juce::Colour(0xff484848) : juce::Colour(0xff7a7a7a);
    }
    const int idx = colorIdx % 4;
    return juce::Colour(dark ? darkColors[idx] : lightColors[idx]);
}

std::vector<Studio::MdRibbonHit> Studio::layoutMdRibbonHits(juce::Rectangle<int> area, double span,
                                                            juce::Rectangle<int> ribbon) const {
    std::vector<MdRibbonHit> hits;
    if (ribbon.getHeight() <= 0 || session.mdDisc.tracks.empty()) {
        return hits;
    }

    const int leftEdge = ribbon.getX() + labelWidth;
    const int rightEdge = ribbon.getRight();
    if (rightEdge <= leftEdge) {
        return hits;
    }

    std::vector<int> groupColorForTrack(session.mdDisc.tracks.size(), -1);
    for (size_t gi = 0; gi < session.mdDisc.groups.size(); ++gi) {
        const auto& g = session.mdDisc.groups[gi];
        for (int t1 = std::max(1, g.startTrack);
             t1 <= std::min(static_cast<int>(session.mdDisc.tracks.size()), g.endTrack); ++t1) {
            groupColorForTrack[static_cast<size_t>(t1 - 1)] = static_cast<int>(gi);
        }
    }

    std::map<std::pair<std::int64_t, std::int64_t>, int> spanCounts;
    for (const auto& tr : session.mdDisc.tracks) {
        for (const auto& part : tr.parts) {
            ++spanCounts[{part.startSample, part.endSample}];
        }
    }

    for (size_t ti = 0; ti < session.mdDisc.tracks.size(); ++ti) {
        const auto& tr = session.mdDisc.tracks[ti];
        const auto id = juce::String::fromUTF8(tr.id.c_str());
        auto title = juce::String::fromUTF8(tr.halfWidthTitle.c_str()).trim();
        if (title.isEmpty()) {
            title = juce::String::fromUTF8(tr.fullWidthTitle.c_str()).trim();
        }
        if (title.isEmpty()) {
            title = "Track " + juce::String(static_cast<int>(ti + 1));
        }

        for (size_t pi = 0; pi < tr.parts.size(); ++pi) {
            const auto& part = tr.parts[pi];
            const int x0 =
                timeToX(static_cast<double>(part.startSample) / carta::sessionRate, area, span);
            const int x1 =
                timeToX(static_cast<double>(part.endSample) / carta::sessionRate, area, span);
            const int clamped0 = std::max(leftEdge, x0);
            const int clamped1 = std::min(rightEdge, std::max(x0 + 2, x1));
            if (clamped1 <= clamped0) {
                continue;
            }
            const bool shared = spanCounts[{part.startSample, part.endSample}] > 1;
            hits.push_back({static_cast<int>(ti),
                            static_cast<int>(pi),
                            {clamped0, ribbon.getY() + 2, clamped1 - clamped0,
                             std::max(1, ribbon.getHeight() - 4)},
                            id,
                            title,
                            groupColorForTrack[ti],
                            part.startSample,
                            part.endSample,
                            shared});
        }
    }
    return hits;
}

std::optional<Studio::MdRibbonHit> Studio::mdRibbonHitAt(juce::Point<int> point) const {
    const auto ribbon = mdRibbonArea();
    if (ribbon.getHeight() <= 0 || !ribbon.contains(point) ||
        point.x < ribbon.getX() + labelWidth) {
        return {};
    }

    const auto hits = layoutMdRibbonHits(timeline(), viewLength(), ribbon);
    const auto primary = discWorkspace.primaryTrackId();
    std::optional<MdRibbonHit> fallback;
    for (const auto& h : hits) {
        if (!h.bounds.contains(point)) {
            continue;
        }
        if (primary.isNotEmpty() && h.trackId == primary) {
            return h;
        }
        if (!fallback) {
            fallback = h;
        }
    }
    return fallback;
}

Studio::MdRibbonPointerInfo Studio::classifyMdRibbonPointer(juce::Point<int> point,
                                                            const juce::ModifierKeys& mods) const {
    const auto ribbon = mdRibbonArea();
    if (ribbon.getHeight() <= 0 || !ribbon.contains(point) ||
        point.x < ribbon.getX() + labelWidth) {
        return {};
    }

    const auto area = timeline();
    const double span = viewLength();
    const auto primary = discWorkspace.primaryTrackId();
    struct PartRef {
        int ti = 0;
        int pi = 0;
        std::int64_t start = 0;
        std::int64_t end = 0;
        int x0 = 0;
        int x1 = 0;
        juce::String id;
        juce::String title;
    };
    std::vector<PartRef> parts;

    const auto titleFor = [&](size_t ti) {
        const auto& tr = session.mdDisc.tracks[ti];
        auto title = juce::String::fromUTF8(tr.halfWidthTitle.c_str()).trim();
        if (title.isEmpty()) {
            title = juce::String::fromUTF8(tr.fullWidthTitle.c_str()).trim();
        }
        if (title.isEmpty()) {
            title = "Track " + juce::String(static_cast<int>(ti + 1));
        }
        return title;
    };

    for (size_t ti = 0; ti < session.mdDisc.tracks.size(); ++ti) {
        const auto& tr = session.mdDisc.tracks[ti];
        const auto id = juce::String::fromUTF8(tr.id.c_str());
        const auto title = titleFor(ti);
        for (size_t pi = 0; pi < tr.parts.size(); ++pi) {
            const auto& pt = tr.parts[pi];
            const int x0 =
                timeToX(static_cast<double>(pt.startSample) / carta::sessionRate, area, span);
            const int x1 =
                timeToX(static_cast<double>(pt.endSample) / carta::sessionRate, area, span);
            parts.push_back({static_cast<int>(ti), static_cast<int>(pi), pt.startSample,
                             pt.endSample, x0, x1, id, title});
        }
    }

    // Check shared sequential split handles first (+/- 6 px).
    struct SplitCand {
        size_t l = 0;
        size_t r = 0;
        int sx = 0;
        int dist = 0;
        bool focused = false;
    };
    std::optional<SplitCand> bestSplit;
    auto considerSplit = [&](size_t l, size_t r) {
        if (parts[l].end != parts[r].start) {
            return;
        }
        const int sx = parts[l].x1;
        const int dist = std::abs(point.x - sx);
        if (dist > 6) {
            return;
        }
        const bool focused =
            primary.isNotEmpty() && (parts[l].id == primary || parts[r].id == primary);
        if (!bestSplit ||
            std::make_pair(!focused, dist) < std::make_pair(!bestSplit->focused, bestSplit->dist)) {
            bestSplit = SplitCand{l, r, sx, dist, focused};
        }
    };

    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const bool consecutiveSameTrack =
            parts[i].ti == parts[i + 1].ti && parts[i].pi + 1 == parts[i + 1].pi;
        const bool consecutiveTrack =
            parts[i].ti + 1 == parts[i + 1].ti && parts[i + 1].pi == 0 &&
            parts[i].pi + 1 ==
                static_cast<int>(
                    session.mdDisc.tracks[static_cast<size_t>(parts[i].ti)].parts.size());
        if (consecutiveSameTrack || consecutiveTrack) {
            considerSplit(i, i + 1);
        }
    }

    if (bestSplit) {
        const auto& L = parts[bestSplit->l];
        const auto& R = parts[bestSplit->r];
        if (!mods.isAltDown()) {
            const bool useRightId = primary.isNotEmpty() && R.id == primary;
            return {
                MdRibbonGesture::sharedSplit,   L.ti, L.pi, R.ti, R.pi, useRightId ? R.id : L.id,
                useRightId ? R.title : L.title, L.end};
        }
        // Option/Alt-drag splits the shared boundary into an independent one-sided trim.
        const bool pickRight =
            (point.x > bestSplit->sx) ||
            (point.x == bestSplit->sx && primary.isNotEmpty() && R.id == primary);
        if (pickRight) {
            return {MdRibbonGesture::leftEdge, R.ti, R.pi, -1, -1, R.id, R.title, R.start};
        }
        return {MdRibbonGesture::rightEdge, L.ti, L.pi, -1, -1, L.id, L.title, L.end};
    }

    // Check outer / unshared edge handles (+/- 6 px).
    struct EdgeCand {
        int ti = -1;
        int pi = -1;
        bool right = false;
        int dist = 0;
        bool focused = false;
        std::int64_t sample = 0;
        juce::String id;
        juce::String title;
    };
    std::optional<EdgeCand> bestEdge;
    for (const auto& p : parts) {
        const bool focused = primary.isNotEmpty() && p.id == primary;
        const int spanPx = std::max(2, p.x1 - p.x0);
        const int innerMargin = std::min(6, std::max(1, spanPx / 4));
        const bool nearLeft = (point.x >= p.x0 - 6 && point.x <= p.x0 + innerMargin);
        const bool nearRight = (point.x >= p.x1 - innerMargin && point.x <= p.x1 + 6);
        if (!nearLeft && !nearRight) {
            continue;
        }
        const int dL = std::abs(point.x - p.x0);
        const int dR = std::abs(point.x - p.x1);
        const bool pickRight = nearRight && (!nearLeft || dR < dL);
        const int dist = pickRight ? dR : dL;
        if (!bestEdge ||
            std::make_pair(!focused, dist) < std::make_pair(!bestEdge->focused, bestEdge->dist)) {
            bestEdge = EdgeCand{p.ti, p.pi,   pickRight, dist, focused, pickRight ? p.end : p.start,
                                p.id, p.title};
        }
    }

    if (bestEdge) {
        return {bestEdge->right ? MdRibbonGesture::rightEdge : MdRibbonGesture::leftEdge,
                bestEdge->ti,
                bestEdge->pi,
                -1,
                -1,
                bestEdge->id,
                bestEdge->title,
                bestEdge->sample};
    }

    if (const auto hit = mdRibbonHitAt(point)) {
        return {mods.isAltDown() ? MdRibbonGesture::duplicateAlias : MdRibbonGesture::slideBody,
                hit->trackIndex,
                hit->partIndex,
                -1,
                -1,
                hit->trackId,
                hit->title,
                hit->startSample};
    }
    return {MdRibbonGesture::emptySpace, -1, -1, -1, -1, {}, {}, 0};
}

void Studio::drawMdRibbon(juce::Graphics& g, juce::Rectangle<int> area, double span,
                          bool dark) const {
    const auto ribbon = mdRibbonArea();
    if (ribbon.getHeight() <= 0) {
        return;
    }

    g.setColour(dark ? juce::Colour(0xff222222) : juce::Colour(0xffe8e8e8));
    g.fillRect(ribbon);
    g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.16f));
    g.drawHorizontalLine(ribbon.getY(), static_cast<float>(ribbon.getX()),
                         static_cast<float>(ribbon.getRight()));
    g.drawHorizontalLine(ribbon.getBottom() - 1, static_cast<float>(ribbon.getX()),
                         static_cast<float>(ribbon.getRight()));

    const auto header = ribbon.withWidth(std::min(labelWidth, ribbon.getWidth()));
    g.setColour(dark ? juce::Colour(0xff2c2c2c) : juce::Colour(0xffdedede));
    g.fillRect(header);
    g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.16f));
    g.drawVerticalLine(header.getRight() - 1, static_cast<float>(ribbon.getY()),
                       static_cast<float>(ribbon.getBottom()));
    g.setColour(dark ? juce::Colour(0xffe0e0e0) : juce::Colour(0xff2b2b2b));
    g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
    g.drawText("MD Tracks", header.reduced(6, 0), juce::Justification::centredLeft, true);

    const auto countText = juce::String(static_cast<int>(session.mdDisc.tracks.size())) + "T";
    g.setFont(juce::FontOptions(10.0f));
    g.setColour(dark ? juce::Colour(0xffa6a6a6) : juce::Colour(0xff5c5c5c));
    g.drawText(countText, header.reduced(6, 0), juce::Justification::centredRight, true);

    const auto body = ribbon.withTrimmedLeft(labelWidth);
    if (body.getWidth() <= 0) {
        return;
    }

    const juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(body);
    const auto hits = layoutMdRibbonHits(area, span, ribbon);
    const auto primary = (dragMode == kDragMdRibbon && mdDragFocusTrackId.isNotEmpty())
                             ? mdDragFocusTrackId
                             : discWorkspace.primaryTrackId();

    // Paint unselected tracks first, then the active track on top so focus outlines stay crisp.
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& h : hits) {
            const bool selected = primary.isNotEmpty() && h.trackId == primary;
            if (selected != (pass == 1)) {
                continue;
            }
            const auto base = mdRibbonGroupColour(h.groupColorIndex, dark);
            const auto selectedFill =
                (h.groupColorIndex >= 0)
                    ? base.brighter(dark ? 0.25f : 0.10f).withAlpha(0.95f)
                    : juce::Colour(dark ? 0xff5c5c5c : 0xff8c8c8c).withAlpha(0.95f);
            g.setColour(selected ? selectedFill : base.withAlpha(dark ? 0.62f : 0.52f));
            g.fillRoundedRectangle(h.bounds.toFloat(), 3.0f);

            g.setColour(
                selected ? (dark ? juce::Colours::white.withAlpha(0.95f) : juce::Colour(0xff222222))
                         : (dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.30f));
            g.drawRoundedRectangle(h.bounds.toFloat(), 3.0f, selected ? 1.3f : 0.8f);

            if (h.bounds.getWidth() >= 24) {
                g.setColour(selected
                                ? juce::Colours::white
                                : (dark ? juce::Colour(0xffeeeeee) : juce::Colour(0xff222222)));
                g.setFont(
                    juce::FontOptions(10.5f, selected ? juce::Font::bold : juce::Font::plain));
                const auto label =
                    "T" + juce::String(h.trackIndex + 1) +
                    (session.mdDisc.tracks[static_cast<size_t>(h.trackIndex)].parts.size() > 1
                         ? "." + juce::String(h.partIndex + 1)
                         : juce::String()) +
                    " " + h.title +
                    (h.sharesPhysical && h.bounds.getWidth() >= 92 ? juce::String(" [Shared]")
                                                                   : juce::String());
                g.drawText(label, h.bounds.reduced(5, 0), juce::Justification::centredLeft, true);
            }
        }
    }

    // Draw shared T-MARK split grips and unshared outer edge handles.
    std::vector<int> splitXs;
    for (size_t i = 0; i + 1 < session.mdDisc.tracks.size(); ++i) {
        const auto& leftTr = session.mdDisc.tracks[i];
        const auto& rightTr = session.mdDisc.tracks[i + 1];
        if (leftTr.parts.empty() || rightTr.parts.empty()) {
            continue;
        }
        if (leftTr.parts.back().endSample == rightTr.parts.front().startSample) {
            const int sx =
                timeToX(static_cast<double>(leftTr.parts.back().endSample) / carta::sessionRate,
                        area, span);
            if (sx >= body.getX() && sx < body.getRight()) {
                splitXs.push_back(sx);
            }
        }
    }
    for (const auto& tr : session.mdDisc.tracks) {
        for (size_t pi = 0; pi + 1 < tr.parts.size(); ++pi) {
            if (tr.parts[pi].endSample == tr.parts[pi + 1].startSample) {
                const int sx = timeToX(
                    static_cast<double>(tr.parts[pi].endSample) / carta::sessionRate, area, span);
                if (sx >= body.getX() && sx < body.getRight()) {
                    splitXs.push_back(sx);
                }
            }
        }
    }
    for (const int sx : splitXs) {
        g.setColour(dark ? juce::Colour(0xff181818) : juce::Colours::white);
        g.fillRect(sx - 2, ribbon.getY() + 2, 4, ribbon.getHeight() - 4);
        g.setColour(dark ? juce::Colour(0xffd4d4d4) : juce::Colour(0xff444444));
        g.drawVerticalLine(sx, static_cast<float>(ribbon.getY() + 2),
                           static_cast<float>(ribbon.getBottom() - 2));
        const int cy = ribbon.getCentreY();
        for (int dy : {-4, 0, 4}) {
            g.fillRect(sx - 1, cy + dy, 2, 2);
        }
    }
}

// ============================================================================
// Main Timeline Paint
// ============================================================================

void Studio::paint(juce::Graphics& g) {
    double displayPosition;
    {
        const juce::ScopedLock guard(lock);
        displayPosition = position;
    }

    const bool dark = juce::Desktop::getInstance().isDarkModeActive();
    const auto textColour = dark ? juce::Colour(0xffdddddd) : juce::Colour(0xff303030);
    g.fillAll(dark ? juce::Colour(0xff252525) : juce::Colour(0xffeeeeee));
    g.setColour(textColour);

    const auto area = timeline();
    const double span = viewLength();
    ++waveformPaintEpoch;

    {
        const juce::Graphics::ScopedSaveState timelineClip(g);
        g.reduceClipRegion(area);
        g.setColour(dark ? juce::Colour(0xff1d1d1d) : juce::Colour(0xfffafafa));
        g.fillRect(area);

        // 1. Time ruler, saved loop region, and ruler ticks.
        const auto ruler =
            juce::Rectangle<int>(area.getX(), area.getY(), area.getWidth(), kTimelineRulerHeight);
        g.setColour(dark ? juce::Colour(0xff333333) : juce::Colour(0xffdedede));
        g.fillRect(ruler);

        const auto displayedLoop = dragMode == kDragSavedLoop ? loopDragPreview : savedLoop;
        if (displayedLoop.end > displayedLoop.start) {
            const int startX =
                timeToX(static_cast<double>(displayedLoop.start) / carta::sessionRate);
            const int endX = timeToX(static_cast<double>(displayedLoop.end) / carta::sessionRate);
            const int left = std::max(ruler.getX() + labelWidth, startX);
            const int right = std::min(ruler.getRight(), endX);
            if (right > left) {
                g.setColour(textColour.withAlpha(
                    displayedLoop.end <= carta::projectEndSample(session) ? 0.65f : 0.25f));
                g.fillRect(left, ruler.getY() + 1, right - left, 2);
            }
            if (displayedLoop.end <= carta::projectEndSample(session)) {
                for (int x : {startX, endX}) {
                    if (x >= ruler.getX() + labelWidth && x < ruler.getRight()) {
                        g.fillRect(x - 2, ruler.getY() + 1, 5, 7);
                    }
                }
            }
        }

        updateSpectrumViewport(span);
        int lastLabelRight = std::numeric_limits<int>::min();
        const auto ticks =
            carta::makeRulerTicks(viewStart, span, std::max(0, area.getWidth() - labelWidth));
        for (const auto tick : ticks.samples) {
            const auto x = timeToX(static_cast<double>(tick) / carta::sessionRate, area, span);
            g.setColour(dark ? juce::Colour(0xff777777) : juce::Colour(0xffaaaaaa));
            g.drawVerticalLine(x, static_cast<float>(ruler.getY() + 23),
                               static_cast<float>(ruler.getBottom()));
            g.setColour(textColour);
            g.setFont(juce::FontOptions(11.0f));
            if (x >= lastLabelRight) {
                const auto label = carta::formatRulerTick(tick, ticks.interval, timelineTimeFormat);
                const int textWidth = std::clamp(label.length() * 7 + 8, 100, 180);
                g.drawText(label, x + 3, ruler.getY() + 3, textWidth, 12,
                           juce::Justification::left);
                lastLabelRight = x + textWidth;
            }
        }

        // 2. Visible track rows, headers, clips, time selection, and drag ghosts.
        bool incompleteWaveformPaint = false;
        const auto visibleRows = carta::visibleTrackRows(
            session, firstTrack, std::max(0, area.getHeight() - timelineHeaderHeight()));
        for (size_t t = static_cast<size_t>(visibleRows.getStart());
             t < static_cast<size_t>(visibleRows.getEnd()); ++t) {
            const auto row = trackArea(static_cast<int>(t));
            if (!row.intersects(area)) {
                continue;
            }

            g.setColour(dark ? (t % 2 ? juce::Colour(0xff242424) : juce::Colour(0xff292929))
                             : (t % 2 ? juce::Colour(0xfff3f3f3) : juce::Colour(0xffeaeaea)));
            g.fillRect(row);

            const bool independentlySelected = timelineTrackRevision == editGeneration &&
                                               timelineTracks.count(static_cast<int>(t)) > 0;
            if (static_cast<int>(t) == selectedTrack || independentlySelected) {
                g.setColour((dark ? juce::Colours::white : juce::Colours::black)
                                .withAlpha(independentlySelected ? 0.16f : 0.09f));
                g.fillRect(row.withWidth(trackLabelWidth));
            }
            if (independentlySelected) {
                g.setColour(textColour.withAlpha(0.75f));
                g.fillRect(row.getX(), row.getY(), 3, row.getHeight());
            }

            const auto& trackState = session.tracks[t];
            const auto headerCtrls = trackHeaderControls(static_cast<int>(t));
            g.setColour(textColour);
            g.setFont(juce::FontOptions(12.0f));
            g.drawText(trackState.name,
                       trackState.collapsed
                           ? row.withWidth(trackLabelWidth - 24).reduced(8, 0).withHeight(18)
                           : row.withWidth(trackLabelWidth - 24)
                                 .reduced(8, 0)
                                 .withTrimmedTop(4)
                                 .withHeight(16),
                       juce::Justification::topLeft, true);

            const auto drawHeaderPill = [&](juce::Rectangle<int> bounds, const juce::String& label,
                                            bool active, juce::Colour activeFill) {
                if (bounds.isEmpty()) {
                    return;
                }
                g.setColour(active ? activeFill
                                   : (dark ? juce::Colour(0xff343434) : juce::Colour(0xffdcdcdc)));
                g.fillRoundedRectangle(bounds.toFloat(), 3.0f);
                g.setColour((dark ? juce::Colours::white : juce::Colour(0xff222222))
                                .withAlpha(active ? 0.85f : 0.32f));
                g.drawRoundedRectangle(bounds.toFloat(), 3.0f, 1.0f);
                g.setColour(active ? juce::Colours::white : textColour);
                g.setFont(juce::FontOptions(10.0f));
                g.drawText(label, bounds, juce::Justification::centred, false);
            };
            drawHeaderPill(headerCtrls.collapse, utf8Str(trackState.collapsed ? "▸" : "▾"), false,
                           textColour);
            if (!trackState.collapsed && !headerCtrls.mute.isEmpty()) {
                drawHeaderPill(headerCtrls.mute, "M", trackState.muted, juce::Colour(0xffb5483a));
                drawHeaderPill(headerCtrls.solo, "S", trackState.solo, juce::Colour(0xff3b78b8));
                const bool customGain = std::abs(trackState.gain - 1.0f) > 1e-3f;
                drawHeaderPill(headerCtrls.gain, formatHeaderGainDb(trackState.gain), customGain,
                               juce::Colour(0xff4a675a));
                const bool customBal = std::abs(trackState.pan) > 5e-3f;
                drawHeaderPill(headerCtrls.balance, formatHeaderBalance(trackState.pan), customBal,
                               juce::Colour(0xff525f7a));
            }

            g.setFont(juce::FontOptions(10.5f));
            const int visibleChannel = carta::isolatedDisplayChannel(trackState);
            const auto overlaps = carta::clipOverlapRanges(trackState, viewStart, viewStart + span);
            const auto trackChannels = carta::maxTrackChannels(trackState);
            const auto stateText =
                juce::String(sourceChecks.trackHasIssue(trackState) ? "FILE ISSUE  " : "") +
                (trackState.collapsed ? juce::String(trackState.muted ? "MUTE  " : "") +
                                            (trackState.solo ? "SOLO  " : "")
                                      : juce::String()) +
                (visibleChannel >= 0
                     ? "CH " + juce::String(visibleChannel + 1) + "  "
                     : (trackState.displaySourceChannel >= 0 ? juce::String("ALL CH  ")
                                                             : juce::String())) +
                (!trackState.channelHeightWeights.empty() &&
                         static_cast<int>(trackState.channelHeightWeights.size()) != trackChannels
                     ? "HEIGHTS: EQUAL  "
                     : "") +
                (overlaps.empty() ? "" : "OVERLAP: SUM");
            g.setColour(textColour.withAlpha(0.8f));
            if (trackState.collapsed || headerCtrls.mute.isEmpty()) {
                g.drawText(stateText,
                           row.withWidth(trackLabelWidth)
                               .withTrimmedTop(trackState.collapsed ? 19 : 27)
                               .reduced(8, 0)
                               .withHeight(17),
                           juce::Justification::left, true);
                if (!trackState.collapsed && trackChannels > 0 &&
                    !carta::clipRoutingConfirmed(trackState, trackChannels)) {
                    g.drawText("ROUTING?",
                               row.withWidth(trackLabelWidth)
                                   .withTrimmedTop(44)
                                   .reduced(8, 0)
                                   .withHeight(17),
                               juce::Justification::left, true);
                }
            } else {
                const bool needRouting =
                    trackChannels > 0 && !carta::clipRoutingConfirmed(trackState, trackChannels);
                const auto badgeText =
                    (needRouting ? juce::String("ROUTING? ") : juce::String()) + stateText;
                if (badgeText.isNotEmpty()) {
                    g.drawText(badgeText,
                               juce::Rectangle<int>(row.getX() + 46, row.getY() + 42,
                                                    trackLabelWidth - 50, 15),
                               juce::Justification::left, true);
                }
            }

            const auto& clips = session.tracks[t].clips;
            const int track = static_cast<int>(t);
            const double spectrumRate = carta::spectrogramTrackRate(trackState);
            drawScale(g, track, dark, visibleChannel, trackChannels, spectrumRate);

            for (size_t c = 0; c < clips.size(); ++c) {
                if (!(track == selectedTrack && static_cast<int>(c) == selectedClip)) {
                    drawClip(g, clips[c], track, static_cast<int>(c), visibleChannel, trackChannels,
                             spectrumRate, row, area, span, dark, &incompleteWaveformPaint);
                }
            }
            if (track == selectedTrack && validSelection()) {
                drawClip(g, clips[static_cast<size_t>(selectedClip)], track, selectedClip,
                         visibleChannel, trackChannels, spectrumRate, row, area, span, dark,
                         &incompleteWaveformPaint);
            }

            if (trackInTimeScope(track)) {
                const auto left = timeToX(
                    static_cast<double>(timeSelection.start) / carta::sessionRate, area, span);
                const auto right = timeToX(
                    static_cast<double>(timeSelection.end) / carta::sessionRate, area, span);
                const auto selected =
                    juce::Rectangle<int>(left, row.getY() + 4 + titleHeight,
                                         std::max(1, right - left),
                                         std::max(0, row.getHeight() - 8 - titleHeight))
                        .getIntersection(row.withTrimmedLeft(labelWidth));
                g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.22f));
                g.fillRect(selected);
                g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.7f));
                g.drawRect(selected, 1);

                const int bodyTop = row.getY() + 4 + titleHeight;
                if (left >= row.getX() + labelWidth && left < row.getRight()) {
                    g.fillRect(left - 2, bodyTop, 5, 9);
                }
                if (right >= row.getX() + labelWidth && right < row.getRight()) {
                    g.fillRect(right - 2, bodyTop, 5, 9);
                }
            }

            if (timeSelection.hasPoint() && timeSelection.firstTrack == track) {
                const int pointX = timeToX(
                    static_cast<double>(timeSelection.start) / carta::sessionRate, area, span);
                const int top = row.getY() + 4 + titleHeight;
                const int bottom = row.getBottom() - 4;
                if (pointX >= row.getX() + labelWidth && pointX < row.getRight() && bottom > top) {
                    g.setColour(textColour.withAlpha(0.9f));
                    const float dashes[] = {3.0f, 3.0f};
                    g.drawDashedLine({static_cast<float>(pointX), static_cast<float>(top),
                                      static_cast<float>(pointX), static_cast<float>(bottom)},
                                     dashes, 2, 1.0f);
                    g.fillRect(pointX - 2, top, 5, 5);
                }
            }

            for (const auto& region : titleRegions(track, row, area, span)) {
                drawTitle(g, clips[static_cast<size_t>(region.clip)], track, region, row, dark);
            }

            // A narrow neutral stripe marks the exact horizontal overlap; the
            // track label explains that overlapping signals sum when audible.
            if (!trackState.collapsed) {
                const auto body = row.withTrimmedLeft(labelWidth);
                g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.75f));
                for (const auto& overlap : overlaps) {
                    const int left = timeToX(overlap.getStart(), area, span);
                    const int right = timeToX(overlap.getEnd(), area, span);
                    if (right > left) {
                        g.fillRect(juce::Rectangle<int>(left, row.getY() + 4 + titleHeight,
                                                        right - left, 3)
                                       .getIntersection(body));
                    }
                }
            }

            if (track == selectedTrack && validSelection() &&
                timelineClipSelection.get(editGeneration).empty()) {
                drawFadeHandles(g, clips[static_cast<size_t>(selectedClip)], track, dark);
            }

            if (dragMode == kDragClipMove && clipPointerDragging && track == clipDropTrack &&
                validSelection()) {
                const auto& moving = session.tracks[static_cast<size_t>(selectedTrack)]
                                         .clips[static_cast<size_t>(selectedClip)];
                const auto ghost = clipRect(moving, row, area, span)
                                       .getIntersection(row.withTrimmedLeft(labelWidth));
                if (!ghost.isEmpty()) {
                    g.setColour(
                        (dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.14f));
                    g.fillRect(ghost);
                    g.setColour(textColour.withAlpha(0.9f));
                    g.drawRect(ghost, 1);
                    const bool overlap =
                        clipOverlapReady && clipOverlapPreview.overlapsSingle(moving.start, track);
                    g.setFont(juce::FontOptions(11.0f));
                    g.drawText((overlap ? utf8Str("Overlap: sum — ") : "Move: ") +
                                   carta::clipDisplayName(moving),
                               ghost.reduced(5, 0).withHeight(titleHeight),
                               juce::Justification::centredLeft, true);
                }
            }

            if (dragMode == kDragGroupEdge && clipPointerDragging && groupDragValid &&
                groupDragGeneration == editGeneration) {
                for (size_t i = 0; i < groupDragClips.size() && i < groupEdgePreview.size(); ++i) {
                    if (groupDragClips[i].track == track) {
                        const auto ghost = clipRect(groupEdgePreview[i], row, area, span)
                                               .getIntersection(row.withTrimmedLeft(labelWidth));
                        if (!ghost.isEmpty()) {
                            g.setColour(textColour.withAlpha(0.14f));
                            g.fillRect(ghost);
                            g.setColour(textColour.withAlpha(0.9f));
                            g.drawRect(ghost, 1);
                        }
                    }
                }
            }

            if (dragMode == kDragGroupMove && clipPointerDragging && groupDragValid &&
                groupDragGeneration == editGeneration) {
                for (const auto& ref : groupDragClips) {
                    if (ref.track + groupDragTracks == track) {
                        auto ghostClip = session.tracks[static_cast<size_t>(ref.track)]
                                             .clips[static_cast<size_t>(ref.clip)];
                        ghostClip.start =
                            carta::shiftTimelineBySamples(ghostClip.start, groupDragSamples);
                        const auto ghost = clipRect(ghostClip, row, area, span)
                                               .getIntersection(row.withTrimmedLeft(labelWidth));
                        if (!ghost.isEmpty()) {
                            g.setColour(textColour.withAlpha(0.14f));
                            g.fillRect(ghost);
                            g.setColour(textColour.withAlpha(0.9f));
                            g.drawRect(ghost, 1);
                            if (groupDragOverlaps && ref.track == groupDragAnchorTrack &&
                                ref.clip == groupDragAnchorClip) {
                                g.setFont(juce::FontOptions(11.0f));
                                g.drawText("Group overlap: sum",
                                           ghost.reduced(5, 0).withHeight(titleHeight),
                                           juce::Justification::centredLeft, true);
                            }
                        }
                    }
                }
            }
        }

        // 3. Master end boundary, markers, live in-point, MD guides/ribbon, and overlays.
        const auto endSample = carta::projectEndSample(session);
        if (endSample > 0) {
            const int endX =
                timeToX(static_cast<double>(endSample) / carta::sessionRate, area, span);
            if (endX >= ruler.getX() + labelWidth && endX < ruler.getRight()) {
                g.setColour(textColour.withAlpha(0.55f));
                const float dashes[] = {3.0f, 4.0f};
                g.drawDashedLine({static_cast<float>(endX), static_cast<float>(ruler.getBottom()),
                                  static_cast<float>(endX), static_cast<float>(area.getBottom())},
                                 dashes, 2, 1.0f);
                g.setColour(textColour);
                g.fillRect(endX - 4, ruler.getBottom() - 8, 8, 8);
            }
        }

        if (!incompleteWaveformPaint) {
            cancelUnseenWaveforms();
        }

        const auto markerPixels = timelineMarkerPixels(area, span);
        for (size_t i = 0; i < session.markers.size(); ++i) {
            const auto& m = session.markers[i];
            if (!std::isfinite(m.position)) {
                continue;
            }
            const int x = timeToX(m.position, area, span);
            if (x < ruler.getX() + labelWidth || x >= ruler.getRight()) {
                continue;
            }
            g.setColour(dark ? juce::Colour(0xffdddddd) : juce::Colour(0xff555555));
            g.drawVerticalLine(x, static_cast<float>(ruler.getY()),
                               static_cast<float>(area.getBottom()));
        }

        const auto markerLabels = carta::layoutMarkerLabels(
            markerPixels,
            {ruler.getX() + labelWidth, std::max(ruler.getX() + labelWidth, ruler.getRight())},
            selectedMarker);
        for (const auto& label : markerLabels) {
            const juce::Rectangle<int> bounds(label.bounds.getStart(), ruler.getY() + 17,
                                              label.bounds.getLength(), 14);
            const bool selected = label.marker == selectedMarker;
            g.setColour(selected ? (dark ? juce::Colour(0xff606060) : juce::Colour(0xffb8b8b8))
                                 : (dark ? juce::Colour(0xff333333) : juce::Colour(0xffdedede)));
            g.fillRect(bounds);
            g.setColour(dark ? juce::Colour(0xffdddddd) : juce::Colour(0xff555555));
            g.setFont(juce::FontOptions(11.0f));
            const auto index = static_cast<size_t>(label.marker);
            g.drawText(session.markers[index].name, bounds, juce::Justification::left, true);
        }

        if (liveInPresent && liveInGeneration == editGeneration) {
            const int markX =
                timeToX(static_cast<double>(liveInSample) / carta::sessionRate, area, span);
            if (markX >= ruler.getX() + labelWidth && markX < ruler.getRight()) {
                g.setColour(textColour.withAlpha(0.85f));
                g.drawVerticalLine(markX, static_cast<float>(ruler.getY()),
                                   static_cast<float>(ruler.getBottom()));
                g.setFont(juce::FontOptions(10.0f));
                g.drawText("In", markX + 3, ruler.getY() + 2, 20, 12,
                           juce::Justification::centredLeft);
            }
        }

        if (visibleMdRibbonHeight() > 0) {
            std::vector<std::pair<juce::int64, bool>> guides;
            const auto primaryId = (dragMode == kDragMdRibbon && mdDragFocusTrackId.isNotEmpty())
                                       ? mdDragFocusTrackId
                                       : discWorkspace.primaryTrackId();
            for (const auto& tr : session.mdDisc.tracks) {
                const bool focused =
                    primaryId.isNotEmpty() && juce::String::fromUTF8(tr.id.c_str()) == primaryId;
                for (const auto& pt : tr.parts) {
                    guides.emplace_back(pt.startSample, focused);
                    guides.emplace_back(pt.endSample, focused);
                }
            }
            std::sort(guides.begin(), guides.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });

            const int topY = area.getY() + timelineHeaderHeight();
            const int botY = area.getBottom();
            if (botY > topY) {
                size_t idx = 0;
                while (idx < guides.size()) {
                    const juce::int64 smp = guides[idx].first;
                    bool focused = guides[idx].second;
                    while (++idx < guides.size() && guides[idx].first == smp) {
                        focused = focused || guides[idx].second;
                    }
                    const int gx =
                        timeToX(static_cast<double>(smp) / carta::sessionRate, area, span);
                    if (gx < area.getX() + labelWidth || gx >= area.getRight()) {
                        continue;
                    }
                    g.setColour(juce::Colour(dark ? 0xff70a8ff : 0xff2563eb)
                                    .withAlpha(focused ? 0.24f : 0.11f));
                    for (int dy = topY; dy < botY; dy += 6) {
                        g.fillRect(gx, dy, 1, std::min(3, botY - dy));
                    }
                }
            }
        }

        drawMdRibbon(g, area, span, dark);

        const bool dragShowsSnapGuide =
            dragMode == kDragClipMove || dragMode == kDragClipTrimLeft ||
            dragMode == kDragClipTrimRight || dragMode == kDragMarkerOrMasterEnd ||
            dragMode == kDragTimeSelection || dragMode == kDragTimeSelectionResize ||
            dragMode == kDragPlayhead || dragMode == kDragGroupMove || dragMode == kDragGroupEdge ||
            dragMode == kDragSavedLoop || dragMode == kDragMdRibbon;
        if (snapGuideVisible && dragShowsSnapGuide) {
            const int guideX = timeToX(snapGuideTime, area, span);
            if (guideX >= ruler.getX() + labelWidth && guideX < ruler.getRight()) {
                g.setColour(textColour.withAlpha(0.8f));
                g.drawVerticalLine(guideX, static_cast<float>(ruler.getY()),
                                   static_cast<float>(area.getBottom()));
                g.setFont(juce::FontOptions(10.0f));
                g.drawText("Snap", guideX + 3, ruler.getY() + 15, 45, 15,
                           juce::Justification::left);
            }
        }

        if (dragMode == kDragTrackReorder && trackDragMoved && trackDragSlot >= 0) {
            const int count = static_cast<int>(session.tracks.size());
            const int y = trackDragSlot < count ? trackArea(trackDragSlot).getY()
                                                : trackArea(count - 1).getBottom();
            if (y >= area.getY() + timelineHeaderHeight() && y < area.getBottom()) {
                g.setColour(textColour);
                g.fillRect(area.getX(), y - 1, area.getWidth(), 2);
            }
        }

        if (dragMode == kDragMarquee && dragged) {
            const auto box = marqueeBounds();
            g.setColour(textColour.withAlpha(0.10f));
            g.fillRect(box);
            g.setColour(textColour.withAlpha(0.8f));
            g.drawRect(box, 1);
        }

        const int x = timeToX(displayPosition, area, span);
        g.setColour(textColour);
        if (x >= ruler.getX() + labelWidth && x < ruler.getRight()) {
            g.drawVerticalLine(x, static_cast<float>(ruler.getY()),
                               static_cast<float>(area.getBottom()));
        }

        if (session.tracks.empty()) {
            g.setColour(juce::Colours::grey);
            g.drawText("Import audio to start Layer 1", area, juce::Justification::centred);
        }

        if (fileDropGuideVisible) {
            const int dropX =
                timeToX(static_cast<double>(fileDropSample) / carta::sessionRate, area, span);
            g.setColour(textColour.withAlpha(0.85f));
            g.drawVerticalLine(dropX, static_cast<float>(ruler.getBottom()),
                               static_cast<float>(area.getBottom()));
            const auto label =
                fileDropTrack >= 0 ? ("Layer " + juce::String(fileDropTrack + 1) + " at sample " +
                                      juce::String(fileDropSample))
                : session.tracks.empty()
                    ? juce::String("Start Layer 1 at sample 0")
                    : ("Append to Layer 1 at sample " + juce::String(fileDropSample));
            const int labelX =
                juce::jlimit(area.getX() + labelWidth,
                             std::max(area.getX() + labelWidth, area.getRight() - 230), dropX + 4);
            g.setColour(dark ? juce::Colour(0xff252525) : juce::Colour(0xffeeeeee));
            g.fillRect(labelX, ruler.getBottom() + 3, 226, 18);
            g.setColour(textColour);
            g.setFont(juce::FontOptions(11.0f));
            g.drawText(label, labelX + 3, ruler.getBottom() + 3, 220, 18,
                       juce::Justification::centredLeft, true);
        }
    }
}

// ============================================================================
// Viewport, Marquee, Scroll & Track Layout Helpers
// ============================================================================

juce::String Studio::getTooltip() {
    return dragMode == kDragNone ? hoverHelpAt(getMouseXYRelative()) : juce::String();
}

juce::Rectangle<int> Studio::timeline() const {
    return {0, menuHeight() + kTimelineRulerHeight, getWidth(),
            std::max(0, getHeight() - menuHeight() - 69 - visibleDiscWorkspaceHeight())};
}

juce::int64 Studio::marqueeDocumentY(int y) const {
    const auto area = timeline();
    const int top = std::min(area.getBottom(), area.getY() + timelineHeaderHeight());
    juce::int64 result = std::clamp(y, top, area.getBottom()) - top;
    for (int track = 0; track < firstTrack && track < static_cast<int>(session.tracks.size());
         ++track) {
        result += carta::trackDisplayHeight(session.tracks[static_cast<size_t>(track)]);
    }
    return result;
}

juce::Rectangle<int> Studio::marqueeBounds() const {
    const auto area = timeline();
    const int left = std::min(area.getRight(), area.getX() + labelWidth);
    const int top = std::min(area.getBottom(), area.getY() + timelineHeaderHeight());
    const int anchorX = std::clamp(timeToX(marqueeAnchorTime), left, area.getRight());
    const int endX = std::clamp(marqueeEnd.x, left, area.getRight());
    const auto documentTop = marqueeDocumentY(top);
    const int anchorY = static_cast<int>(std::clamp(marqueeAnchorY - documentTop + top,
                                                    static_cast<juce::int64>(top),
                                                    static_cast<juce::int64>(area.getBottom())));
    const int endY = std::clamp(marqueeEnd.y, top, area.getBottom());
    return juce::Rectangle<int>::leftTopRightBottom(
        std::min(anchorX, endX), std::min(anchorY, endY), std::max(anchorX, endX),
        std::max(anchorY, endY));
}

void Studio::updateMarqueePreview() {
    marqueePreviewValid = false;
    if (dragMode != kDragMarquee || !dragged || marqueeRevision != editGeneration) {
        return;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (marqueePreviewHook) {
        marqueePreviewHook();
    }
#endif
    if (dragMode != kDragMarquee || marqueeRevision != editGeneration) {
        return;
    }

    const auto area = timeline();
    const int left = area.getX() + labelWidth;
    const int right = area.getRight();
    const int anchorX = timeToX(marqueeAnchorTime);
    const int endX = std::clamp(marqueeEnd.x, left, std::max(left, right));
    const int minX = std::min(anchorX, endX);
    const int maxX = std::max(anchorX, endX);
    const auto endY = marqueeDocumentY(marqueeEnd.y);
    const auto minY = std::min(marqueeAnchorY, endY);
    const auto maxY = std::max(marqueeAnchorY, endY);

    std::vector<carta::ClipReference> hits;
    if (maxX > minX && maxY > minY) {
        const double span = viewLength();
        juce::int64 rowY = 0;
        for (int track = 0; track < static_cast<int>(session.tracks.size()); ++track) {
            const int height =
                carta::trackDisplayHeight(session.tracks[static_cast<size_t>(track)]);
            const auto clipTop = rowY + 4;
            const auto clipBottom = rowY + height - 6;
            rowY += height;
            if (clipTop >= maxY) {
                break;
            }
            if (clipBottom <= minY) {
                continue;
            }
            const auto& clips = session.tracks[static_cast<size_t>(track)].clips;
            for (size_t clip = 0; clip < clips.size(); ++clip) {
                const auto& item = clips[clip];
                const int x = carta::timelineTimeToPixel(item.start, viewStart, span, left,
                                                         area.getWidth() - labelWidth);
                const int end =
                    carta::timelineTimeToPixel(item.start + item.duration, viewStart, span, left,
                                               area.getWidth() - labelWidth);
                if (static_cast<juce::int64>(x) + std::max(2, end - x) > minX && x < maxX) {
                    hits.push_back({track, static_cast<int>(clip)});
                }
            }
        }
    }

    carta::ClipSelection next;
    if (!hits.empty() && next.selectExpanded(session, hits, marqueeRevision) &&
        dragMode == kDragMarquee && marqueeRevision == editGeneration) {
        marqueePreview = std::move(next);
        marqueePreviewValid = true;
        const auto count = marqueePreview.get(marqueeRevision).size();
        status.setText(juce::String(count) + (count == 1 ? " clip" : " clips") +
                           " will be selected. Release to apply; Escape cancels.",
                       juce::dontSendNotification);
    } else {
        status.setText("No clips in selection rectangle; existing selection will remain.",
                       juce::dontSendNotification);
    }
}

const carta::ClipSelection& Studio::displayedClipSelection() const noexcept {
    return dragMode == kDragMarquee && marqueePreviewValid && marqueeRevision == editGeneration
               ? marqueePreview
               : timelineClipSelection;
}

bool Studio::clipAppearsSelected(int track, int clip) const {
    const auto& selected = displayedClipSelection();
    return selected.get(editGeneration).empty() ? (track == selectedTrack && clip == selectedClip)
                                                : selected.contains(track, clip, editGeneration);
}

void Studio::syncTimelineScroll() {
    const double full = fullLength();
    const double span = viewLength();
    timelineScroll.setRangeLimits(0, full, juce::dontSendNotification);
    timelineScroll.setCurrentRange(viewStart, span, juce::dontSendNotification);
    timelineScroll.setSingleStepSize(span / 10.0);
    timelineScroll.setEnabled(dragMode == kDragNone && full > span);
}

void Studio::scrollBarMoved(juce::ScrollBar* bar, double start) {
    if (bar != &timelineScroll || dragMode != kDragNone || !std::isfinite(start)) {
        return;
    }
    const double next = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()), start);
    if (std::abs(next - viewStart) > 1e-12) {
        viewStart = next;
        suspendFollowingForNavigation();
        repaint();
    }
}

juce::Rectangle<int> Studio::trackArea(int track) const {
    const auto a = timeline();
    int y = a.getY() + timelineHeaderHeight();
    for (int i = firstTrack; i < track && i < static_cast<int>(session.tracks.size()); ++i) {
        y += carta::trackDisplayHeight(session.tracks[static_cast<size_t>(i)]);
    }
    return {a.getX(), y, a.getWidth(),
            carta::trackDisplayHeight(session.tracks[static_cast<size_t>(track)]) - 2};
}

int Studio::trackAt(int y) const {
    for (int t = firstTrack; t < static_cast<int>(session.tracks.size()); ++t) {
        const auto row = trackArea(t);
        if (y >= row.getY() && y < row.getBottom() + 2) {
            return t;
        }
        if (row.getY() > y) {
            break;
        }
    }
    return -1;
}

int Studio::trackResizeAt(juce::Point<int> point) const {
    if (!timeline().contains(point)) {
        return -1;
    }
    for (int t = firstTrack; t < static_cast<int>(session.tracks.size()); ++t) {
        const auto row = trackArea(t);
        if (!session.tracks[static_cast<size_t>(t)].collapsed &&
            std::abs(point.y - row.getBottom()) <= 4) {
            return t;
        }
        if (row.getY() > point.y + 4) {
            break;
        }
    }
    return -1;
}

Studio::ChannelBoundary Studio::channelBoundaryAt(juce::Point<int> point) const {
    const auto area = timeline();
    if (!area.contains(point) || point.x < area.getX() + trackLabelWidth ||
        point.x >= area.getX() + labelWidth || trackResizeAt(point) >= 0) {
        return {};
    }

    const int trackIndex = trackAt(point.y);
    if (trackIndex < 0 || trackIndex >= static_cast<int>(session.tracks.size())) {
        return {};
    }

    const auto& track = session.tracks[static_cast<size_t>(trackIndex)];
    const int channels = carta::maxTrackChannels(track);
    if (track.collapsed || track.clips.empty() || channels < 2 ||
        carta::isolatedDisplayChannel(track) >= 0) {
        return {};
    }

    const auto rect = clipRect(track.clips.front(), trackIndex);
    const int header = std::min(titleHeight, std::max(0, rect.getHeight() - 8));
    const auto content = rect.withTrimmedTop(header);
    const auto sections = carta::trackViewLayout(content.getHeight(), track.view, spectrumView);
    const auto find = [&](int offset, int height) -> ChannelBoundary {
        if (height <= 0 || point.y < content.getY() + offset ||
            point.y >= content.getY() + offset + height) {
            return {};
        }
        const auto lanes = carta::channelLaneLayout(height, channels, track.channelHeightWeights);
        const int top = content.getY() + offset;
        for (int lane = 0; lane + 1 < channels; ++lane) {
            const int boundary = top + lanes[static_cast<size_t>(lane)].getEnd();
            const int pairTop = top + lanes[static_cast<size_t>(lane)].getStart();
            const int pairBottom = top + lanes[static_cast<size_t>(lane + 1)].getEnd();
            if (pairBottom - pairTop >= 8 && std::abs(point.y - boundary) <= 3) {
                return {trackIndex, lane, pairTop, pairBottom};
            }
        }
        return {};
    };

    auto result = find(sections.waveformTop, sections.waveformHeight);
    return result.track >= 0 ? result : find(sections.spectrumTop, sections.spectrumHeight);
}

// ============================================================================
// Coordinate Conversion, Markers, Snapping & Handle Hit-Testing
// ============================================================================

int Studio::timeToX(double t, juce::Rectangle<int> area, double span) const {
    return carta::timelineTimeToPixel(t, viewStart, span, area.getX() + labelWidth,
                                      area.getWidth() - labelWidth);
}

int Studio::timeToX(double t) const {
    return timeToX(t, timeline(), viewLength());
}

int Studio::markerHitAt(int x, bool cycle, int preferred, int* nearby) const {
    struct Hit {
        int index;
        juce::int64 distance;
    };
    const auto area = timeline();
    const double span = viewLength();
    std::vector<Hit> hits;
    for (int i = static_cast<int>(session.markers.size()) - 1; i >= 0; --i) {
        const auto value = session.markers[static_cast<size_t>(i)].position;
        if (!std::isfinite(value)) {
            continue;
        }
        const int px = carta::timelineTimeToPixel(value, viewStart, span, area.getX() + labelWidth,
                                                  area.getWidth() - labelWidth);
        const auto distance = std::abs(static_cast<juce::int64>(px) - static_cast<juce::int64>(x));
        if (distance <= 6) {
            hits.push_back({i, distance});
        }
    }
    if (nearby) {
        *nearby = static_cast<int>(hits.size());
    }
    if (hits.empty()) {
        return -1;
    }

    const auto nearest = std::min_element(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        return a.distance < b.distance;
    });
    const auto found = std::find_if(hits.begin(), hits.end(),
                                    [preferred](const Hit& hit) { return hit.index == preferred; });
    if (found == hits.end()) {
        return nearest->index;
    }
    if (cycle) {
        return hits[(static_cast<size_t>(std::distance(hits.begin(), found)) + 1) % hits.size()]
            .index;
    }
    return found->distance == nearest->distance ? found->index : nearest->index;
}

bool Studio::focusAdjacentMarker(bool next) {
    const auto generation = editGeneration;
    const int selected = selectedMarker;
    double cursor = 0;
    {
        const juce::ScopedLock guard(lock);
        cursor = position;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif

    const bool anchored = selected >= 0 && selected < static_cast<int>(session.markers.size()) &&
                          std::isfinite(session.markers[static_cast<size_t>(selected)].position);
    const double origin =
        anchored ? session.markers[static_cast<size_t>(selected)].position : cursor;
    const double end = static_cast<double>(carta::projectEndSample(session)) / carta::sessionRate;
    int hit = -1;

    if (std::isfinite(origin)) {
        for (int i = 0; i < static_cast<int>(session.markers.size()); ++i) {
            const double value = session.markers[static_cast<size_t>(i)].position;
            if (!std::isfinite(value) || value < 0 || value >= end) {
                continue;
            }
            const bool eligible =
                anchored ? (next ? value > origin ||
                                       (std::equal_to<double>{}(value, origin) && i > selected)
                                 : value < origin ||
                                       (std::equal_to<double>{}(value, origin) && i < selected))
                         : (next ? value >= origin : value <= origin);
            if (!eligible) {
                continue;
            }
            if (hit < 0) {
                hit = i;
                continue;
            }
            const double best = session.markers[static_cast<size_t>(hit)].position;
            if (next ? value < best || (std::equal_to<double>{}(value, best) && i < hit)
                     : value > best || (std::equal_to<double>{}(value, best) && i > hit)) {
                hit = i;
            }
        }
    }

    if (generation != editGeneration || selected != selectedMarker) {
        status.setText("Project or marker focus changed; try again.", juce::dontSendNotification);
        announceNavigation(status.getText());
        return false;
    }
    if (hit < 0) {
        status.setText(next ? "No next marker to focus." : "No previous marker to focus.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return false;
    }
    focusMarker(hit);
    return true;
}

void Studio::focusMarker(int index) {
    selectedMarker = index;
    const auto& target = session.markers[static_cast<size_t>(index)];
    if (std::isfinite(target.position) && target.position >= 0 && target.position <= fullLength() &&
        (target.position < viewStart || target.position >= viewStart + viewLength())) {
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()),
                                 target.position - viewLength() * 0.5);
        suspendFollowingForNavigation();
    }
    const auto message =
        "Focused marker " + juce::String(index + 1) + ": " +
        (target.name.trim().isEmpty() ? juce::String("unnamed") : target.name) + ". " +
        carta::describeMarkerPosition(session, index, carta::projectEndSample(session));
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
}

juce::int64 Studio::sampleAtX(int x) const {
    return static_cast<juce::int64>(std::llround(xToTime(x) * carta::sessionRate));
}

juce::int64 Studio::snappedPointerSample(int x, bool allowSnap) {
    if (snapMode == 0 || !allowSnap) {
        snapGuideVisible = false;
        return sampleAtX(x);
    }
    const double radius = snapMode == 2
                              ? static_cast<double>(kSnapBoundaryRadiusPx) * viewLength() /
                                    std::max(1, timeline().getWidth() - labelWidth)
                              : 0.0;
    const auto& targets = snapMode == 2 ? dragTargets : emptySnapTargets;
    const auto result = carta::snapTimelinePosition(xToTime(x), 0, fullLength(), true, radius,
                                                    targets, 0, snapGridSamples);
    snapGuideVisible = result.hasGuide;
    snapGuideTime = result.guide;
    return static_cast<juce::int64>(std::llround(result.time * carta::sessionRate));
}

int Studio::selectionEdgeAt(juce::Point<int> point) const {
    if (!timeSelection.hasRange() || point.x < timeline().getX() + labelWidth) {
        return 0;
    }
    const int track = trackAt(point.y);
    if (!trackInTimeScope(track)) {
        return 0;
    }
    const auto row = trackArea(track);
    const int top = row.getY() + 4 + titleHeight;
    if (point.y < top || point.y >= top + 10) {
        return 0;
    }
    const int left = timeToX(static_cast<double>(timeSelection.start) / carta::sessionRate);
    const int right = timeToX(static_cast<double>(timeSelection.end) / carta::sessionRate);
    const int dl = std::abs(point.x - left);
    const int dr = std::abs(point.x - right);
    if (std::min(dl, dr) > 6) {
        return 0;
    }
    return dl <= dr ? -1 : 1;
}

double Studio::xToTime(int x, juce::Rectangle<int> area, double span) const {
    return juce::jlimit(0.0, viewStart + span,
                        viewStart + (x - area.getX() - labelWidth) * span /
                                        std::max(1, area.getWidth() - labelWidth));
}

double Studio::xToTime(int x) const {
    return xToTime(x, timeline(), viewLength());
}

void Studio::updateFileDropGuide(int x, int y) {
    const auto area = timeline().withTrimmedTop(timelineHeaderHeight()).withTrimmedLeft(labelWidth);
    const bool valid = area.contains(x, y) && !activeOpen;
    const int targetLayer = valid ? trackAt(y) : -1;
    const auto sample =
        valid
            ? (targetLayer >= 0 ? sampleAtX(x)
                                : (session.tracks.empty() ? 0 : layerEndSample(session.tracks[0])))
            : 0;
    if (valid != fileDropGuideVisible ||
        (valid && (sample != fileDropSample || targetLayer != fileDropTrack))) {
        fileDropGuideVisible = valid;
        fileDropSample = sample;
        fileDropTrack = targetLayer;
        repaint();
    }
}

juce::Rectangle<int> Studio::clipRect(const carta::Clip& c, juce::Rectangle<int> row,
                                      juce::Rectangle<int> area, double span) const {
    const int left = area.getX() + labelWidth;
    const int width = area.getWidth() - labelWidth;
    const int x = carta::timelineTimeToPixel(c.start, viewStart, span, left, width);
    const int end = carta::timelineTimeToPixel(c.start + c.duration, viewStart, span, left, width);
    return {x, row.getY() + 4, std::max(2, end - x), row.getHeight() - 8};
}

juce::Rectangle<int> Studio::clipRect(const carta::Clip& c, int track) const {
    return clipRect(c, trackArea(track), timeline(), viewLength());
}

std::vector<int> Studio::clipHitsAt(const std::vector<carta::Clip>& clips, juce::Point<int> point,
                                    juce::Rectangle<int> row, juce::Rectangle<int> area,
                                    double span) const {
    std::vector<int> hits;
    for (int i = static_cast<int>(clips.size()) - 1; i >= 0; --i) {
        if (clipRect(clips[static_cast<size_t>(i)], row, area, span).contains(point)) {
            hits.push_back(i);
        }
    }
    return hits;
}

std::vector<carta::ClipTitleRegion> Studio::titleRegions(int track, juce::Rectangle<int> row,
                                                         juce::Rectangle<int> area,
                                                         double span) const {
    const auto& clips = session.tracks[static_cast<size_t>(track)].clips;
    std::vector<juce::Range<int>> ranges;
    ranges.reserve(clips.size());
    for (const auto& clip : clips) {
        const auto rect = clipRect(clip, row, area, span);
        ranges.emplace_back(rect.getX(), rect.getRight());
    }
    return carta::layoutClipTitles(ranges, {area.getX() + labelWidth, area.getRight()});
}

std::vector<carta::ClipTitleRegion> Studio::titleRegions(int track) const {
    return titleRegions(track, trackArea(track), timeline(), viewLength());
}

bool Studio::masterEndHandleAt(juce::Point<int> point) const {
    const auto area = timeline();
    const auto end = carta::projectEndSample(session);
    if (end <= 0 || point.y < area.getY() + 24 || point.y >= area.getY() + kTimelineRulerHeight ||
        point.x < area.getX() + labelWidth || point.x >= area.getRight()) {
        return false;
    }
    return std::abs(point.x - timeToX(static_cast<double>(end) / carta::sessionRate)) <= 5;
}

int Studio::savedLoopHandleAt(juce::Point<int> point) const {
    const auto area = timeline();
    if (savedLoop.end <= savedLoop.start || savedLoop.end > carta::projectEndSample(session) ||
        point.y < area.getY() || point.y >= area.getY() + 9 || point.x < area.getX() + labelWidth ||
        point.x >= area.getRight()) {
        return 0;
    }
    const int startX = timeToX(static_cast<double>(savedLoop.start) / carta::sessionRate);
    const int endX = timeToX(static_cast<double>(savedLoop.end) / carta::sessionRate);
    const int a = std::abs(point.x - startX);
    const int b = std::abs(point.x - endX);
    if (std::min(a, b) > 5) {
        return 0;
    }
    return a <= b ? -1 : 1;
}

std::pair<juce::Point<int>, juce::Point<int>> Studio::fadeHandlePoints(const carta::Clip& clip,
                                                                       int track) const {
    const auto rect = clipRect(clip, track);
    const auto at = [&](double source, int y) {
        const double relative = std::clamp(source - clip.offset, 0.0, clip.duration);
        return juce::Point<int>(
            juce::jlimit(rect.getX() + 5, rect.getRight() - 5, timeToX(clip.start + relative)),
            rect.getY() + titleHeight + y);
    };
    return {
        at(clip.fadeIn > 0 ? clip.fadeInStart + clip.fadeIn : clip.offset, 7),
        at(clip.fadeOut > 0 ? clip.fadeOutEnd - clip.fadeOut : clip.offset + clip.duration, 19)};
}

int Studio::fadeHandleAt(juce::Point<int> point) const {
    if (!validSelection() || !timelineClipSelection.get(editGeneration).empty() ||
        session.tracks[static_cast<size_t>(selectedTrack)].collapsed) {
        return 0;
    }
    const auto& clip =
        session.tracks[static_cast<size_t>(selectedTrack)].clips[static_cast<size_t>(selectedClip)];
    const auto rect = clipRect(clip, selectedTrack);
    if (rect.getWidth() < 20 || rect.getHeight() < titleHeight + 26 ||
        !rect.getIntersection(trackArea(selectedTrack).withTrimmedLeft(labelWidth))
             .contains(point)) {
        return 0;
    }
    const auto points = fadeHandlePoints(clip, selectedTrack);
    const int a = std::abs(point.x - points.first.x);
    const int b = std::abs(point.x - points.second.x);
    if (std::abs(point.y - points.first.y) <= 5 && a <= 6) {
        return -1;
    }
    if (std::abs(point.y - points.second.y) <= 5 && b <= 6) {
        return 1;
    }
    return 0;
}

void Studio::drawFadeHandles(juce::Graphics& g, const carta::Clip& clip, int track,
                             bool dark) const {
    const auto rect = clipRect(clip, track);
    if (session.tracks[static_cast<size_t>(track)].collapsed || rect.getWidth() < 20 ||
        rect.getHeight() < titleHeight + 26) {
        return;
    }
    const juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(trackArea(track).withTrimmedLeft(labelWidth));
    const auto points = fadeHandlePoints(clip, track);
    for (const auto point : {points.first, points.second}) {
        juce::Path diamond;
        diamond.startNewSubPath(static_cast<float>(point.x), static_cast<float>(point.y - 5));
        diamond.lineTo(static_cast<float>(point.x + 5), static_cast<float>(point.y));
        diamond.lineTo(static_cast<float>(point.x), static_cast<float>(point.y + 5));
        diamond.lineTo(static_cast<float>(point.x - 5), static_cast<float>(point.y));
        diamond.closeSubPath();
        g.setColour(dark ? juce::Colours::black : juce::Colours::white);
        g.fillPath(diamond);
        g.setColour(dark ? juce::Colours::white : juce::Colours::black);
        g.strokePath(diamond, juce::PathStrokeType(1.0f));
    }
}

Studio::TrackHeaderControls Studio::trackHeaderControls(int track) const {
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        return {};
    }
    const auto row = trackArea(track).withWidth(trackLabelWidth);
    const auto& tr = session.tracks[static_cast<size_t>(track)];
    TrackHeaderControls c;
    c.track = track;
    c.collapse = {row.getX() + trackLabelWidth - 22, row.getY() + 4, 18, 15};
    if (!tr.collapsed && row.getHeight() >= 64) {
        c.mute = {row.getX() + 6, row.getY() + 24, 18, 16};
        c.solo = {row.getX() + 26, row.getY() + 24, 18, 16};
        c.gain = {row.getX() + 46, row.getY() + 24, 58, 16};
        c.balance = {row.getX() + 6, row.getY() + 42, 38, 15};
    }
    return c;
}

int Studio::trackHeaderHit(juce::Point<int> point, int* outTrack) const {
    const auto area = timeline();
    if (!area.contains(point) || point.y < area.getY() + timelineHeaderHeight() ||
        point.x >= area.getX() + trackLabelWidth) {
        return HeaderHit::none;
    }
    const int track = trackAt(point.y);
    if (track < 0) {
        return HeaderHit::none;
    }
    if (outTrack) {
        *outTrack = track;
    }
    const auto c = trackHeaderControls(track);
    if (c.collapse.contains(point)) {
        return HeaderHit::collapse;
    }
    if (c.mute.contains(point)) {
        return HeaderHit::mute;
    }
    if (c.solo.contains(point)) {
        return HeaderHit::solo;
    }
    if (c.gain.contains(point)) {
        return HeaderHit::gain;
    }
    if (c.balance.contains(point)) {
        return HeaderHit::balance;
    }
    return HeaderHit::none;
}

bool Studio::selectedGroupEdgeAt(juce::Point<int> point) const {
    const int track = trackAt(point.y);
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        return false;
    }
    const auto& selected = timelineClipSelection.get(editGeneration);
    if (selected.empty()) {
        return false;
    }
    const auto row = trackArea(track);
    const auto area = timeline();
    const double span = viewLength();
    for (const auto& ref : selected) {
        if (ref.track == track) {
            const auto rect = clipRect(
                session.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(ref.clip)],
                row, area, span);
            if (rect.contains(point) && (point.x < rect.getX() + kClipEdgeHitPx ||
                                         point.x > rect.getRight() - kClipEdgeHitPx)) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// Cursor Feedback & Hover Help
// ============================================================================

void Studio::updateCursor(juce::Point<int> point) {
    const auto area = timeline();
    if (mdRibbonArea().contains(point)) {
        if (point.x < area.getX() + labelWidth) {
            setMouseCursor(juce::MouseCursor::NormalCursor);
            return;
        }
        const auto info =
            classifyMdRibbonPointer(point, juce::ModifierKeys::getCurrentModifiersRealtime());
        switch (info.gesture) {
        case MdRibbonGesture::sharedSplit:
        case MdRibbonGesture::leftEdge:
        case MdRibbonGesture::rightEdge:
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            return;
        case MdRibbonGesture::duplicateAlias:
            setMouseCursor(juce::MouseCursor::CopyingCursor);
            return;
        case MdRibbonGesture::slideBody:
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            return;
        case MdRibbonGesture::emptySpace:
        case MdRibbonGesture::none:
            setMouseCursor(juce::MouseCursor::CrosshairCursor);
            return;
        }
    }

    if (!area.contains(point)) {
        setMouseCursor(juce::MouseCursor::NormalCursor);
        return;
    }
    if (trackResizeAt(point) >= 0) {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        return;
    }
    if (channelBoundaryAt(point).track >= 0) {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        return;
    }
    if (point.x < area.getX() + labelWidth) {
        if (point.x < area.getX() + trackLabelWidth &&
            point.y >= area.getY() + timelineHeaderHeight()) {
            const int hit = trackHeaderHit(point);
            if (hit == HeaderHit::gain || hit == HeaderHit::balance) {
                setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
                return;
            }
            setMouseCursor(trackAt(point.y) >= 0 ? juce::MouseCursor::PointingHandCursor
                                                 : juce::MouseCursor::NormalCursor);
            return;
        }
        setMouseCursor(juce::MouseCursor::NormalCursor);
        return;
    }
    if (point.y < area.getY() + kTimelineRulerHeight) {
        setMouseCursor((masterEndHandleAt(point) || savedLoopHandleAt(point) != 0 ||
                        markerHitAt(point.x, false, selectedMarker) >= 0)
                           ? juce::MouseCursor::LeftRightResizeCursor
                           : juce::MouseCursor::CrosshairCursor);
        return;
    }
    if (selectedGroupEdgeAt(point)) {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        return;
    }
    if (fadeHandleAt(point) != 0) {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        return;
    }
    if (selectionEdgeAt(point) != 0) {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        return;
    }

    const int track = trackAt(point.y);
    if (track < 0) {
        setMouseCursor(juce::MouseCursor::NormalCursor);
        return;
    }
    const auto& clips = session.tracks[static_cast<size_t>(track)].clips;
    const auto row = trackArea(track);
    const double span = viewLength();
    int hit = -1;
    if (!clips.empty() && point.y < row.getY() + 4 + titleHeight) {
        for (const auto& region : titleRegions(track, row, area, span)) {
            if (region.bounds.contains(point.x)) {
                hit = region.clip;
                break;
            }
        }
    }
    if (hit < 0 && track == selectedTrack && validSelection() &&
        clipRect(clips[static_cast<size_t>(selectedClip)], row, area, span).contains(point)) {
        hit = selectedClip;
    }
    if (hit < 0) {
        for (int i = static_cast<int>(clips.size()) - 1; i >= 0; --i) {
            if (clipRect(clips[static_cast<size_t>(i)], row, area, span).contains(point)) {
                hit = i;
                break;
            }
        }
    }
    if (hit < 0) {
        setMouseCursor(juce::MouseCursor::IBeamCursor);
        return;
    }

    const auto rect = clipRect(clips[static_cast<size_t>(hit)], row, area, span);
    if (point.y < rect.getY() + titleHeight &&
        timelineClipSelection.contains(track, hit, editGeneration)) {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        return;
    }
    if (point.x < rect.getX() + kClipEdgeHitPx || point.x > rect.getRight() - kClipEdgeHitPx) {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    } else {
        setMouseCursor(point.y < rect.getY() + titleHeight ? juce::MouseCursor::PointingHandCursor
                                                           : juce::MouseCursor::IBeamCursor);
    }
}

juce::String Studio::hoverHelpAt(juce::Point<int> point) const {
    const auto area = timeline();
    if (mdRibbonArea().contains(point)) {
        if (point.x < area.getX() + labelWidth) {
            return "MiniDisc Track Ribbon: shows MD tracks, titles, and movable split boundaries.";
        }
        const auto info =
            classifyMdRibbonPointer(point, juce::ModifierKeys::getCurrentModifiersRealtime());
        switch (info.gesture) {
        case MdRibbonGesture::sharedSplit:
            return "Shared MD split (T-MARK): drag to move split (snaps to clips & markers); "
                   "Option/Alt-drag trims one side; right-click to merge split.";
        case MdRibbonGesture::leftEdge:
        case MdRibbonGesture::rightEdge:
            return "MD track boundary: drag to trim start or end independently (hold Shift to "
                   "bypass snapping).";
        case MdRibbonGesture::duplicateAlias:
            return "Option/Alt-drag to duplicate a shared MD track (uses zero extra disc time).";
        case MdRibbonGesture::slideBody:
            return "MD Track " + juce::String(info.trackIndex + 1) + ": " + info.title +
                   ". Drag to move; Option/Alt-drag duplicates; double-click to focus; "
                   "right-click for actions.";
        case MdRibbonGesture::emptySpace:
        case MdRibbonGesture::none:
            return "Empty MD Tracks gap: double-click to insert an MD track here; click to "
                   "seek.";
        }
    }

    if (!area.contains(point)) {
        return {};
    }
    if (point.y < area.getY() + kTimelineRulerHeight) {
        if (point.x < area.getX() + labelWidth) {
            return {};
        }
        if (savedLoopHandleAt(point) != 0) {
            return "Drag the loop boundary. Hold Shift to bypass snapping; Escape cancels.";
        }
        if (masterEndHandleAt(point)) {
            return "Drag to change the master end. Escape cancels.";
        }
        int nearby = 0;
        const int markerIndex = markerHitAt(point.x, false, selectedMarker, &nearby);
        if (markerIndex >= 0) {
            const auto& name = session.markers[static_cast<size_t>(markerIndex)].name;
            return "Track marker: " + (name.trim().isEmpty() ? juce::String("unnamed") : name) +
                   ". Drag to move; right-click for marker actions." +
                   (nearby > 1 ? " Option/Alt-click cycles nearby markers." : "");
        }
        return "Click or drag to seek. Hold Shift to bypass snapping.";
    }

    if (trackResizeAt(point) >= 0) {
        return "Drag to resize the layer. Escape cancels.";
    }
    if (channelBoundaryAt(point).track >= 0) {
        return "Drag to resize adjacent source channels. Escape cancels; other channel proportions "
               "are retained within integer rounding.";
    }

    const int track = trackAt(point.y);
    if (track < 0) {
        return {};
    }
    if (point.x < area.getX() + trackLabelWidth) {
        const int hit = trackHeaderHit(point);
        if (hit == HeaderHit::collapse) {
            return "Collapse or expand layer " + session.tracks[static_cast<size_t>(track)].name +
                   ".";
        }
        if (hit == HeaderHit::mute) {
            return "Toggle mute on layer " + session.tracks[static_cast<size_t>(track)].name + ".";
        }
        if (hit == HeaderHit::solo) {
            return "Toggle solo on layer " + session.tracks[static_cast<size_t>(track)].name + ".";
        }
        if (hit == HeaderHit::gain) {
            return "Drag left or right to adjust layer gain (up to +24.1 dB). Double-click to "
                   "reset to 0 dB.";
        }
        if (hit == HeaderHit::balance) {
            return "Drag left or right to adjust layer stereo balance. Double-click to center.";
        }
        return "Layer " + session.tracks[static_cast<size_t>(track)].name +
               ". Drag to reorder; Command/Control+Shift-click selects layers; right-click for "
               "actions.";
    }
    if (point.x < area.getX() + labelWidth) {
        return scaleHoverHelp(track, point.y);
    }
    if (selectedGroupEdgeAt(point)) {
        return "Drag selected clip edges together. Hold Shift to bypass snapping; Escape cancels.";
    }
    const int fade = fadeHandleAt(point);
    if (fade != 0) {
        return fade < 0 ? "Drag to adjust fade in. Escape cancels."
                        : "Drag to adjust fade out. Escape cancels.";
    }
    const int selectionEdge = selectionEdgeAt(point);
    if (selectionEdge != 0) {
        return "Drag to adjust the selection boundary; crossing reverses it. Escape cancels.";
    }

    const auto& clips = session.tracks[static_cast<size_t>(track)].clips;
    const auto row = trackArea(track);
    const double span = viewLength();
    int hit = -1;
    if (!clips.empty() && point.y < row.getY() + 4 + titleHeight) {
        for (const auto& region : titleRegions(track, row, area, span)) {
            if (region.bounds.contains(point.x)) {
                hit = region.clip;
                break;
            }
        }
    }
    if (hit < 0 && track == selectedTrack && validSelection() &&
        clipRect(clips[static_cast<size_t>(selectedClip)], row, area, span).contains(point)) {
        hit = selectedClip;
    }
    if (hit < 0) {
        for (int i = static_cast<int>(clips.size()) - 1; i >= 0; --i) {
            if (clipRect(clips[static_cast<size_t>(i)], row, area, span).contains(point)) {
                hit = i;
                break;
            }
        }
    }
    if (hit < 0) {
        return "Drag to select time across layers. Double-click a clip-free gap to select it. "
               "Hold Shift to bypass snapping.";
    }

    const auto& clip = clips[static_cast<size_t>(hit)];
    const auto rect = clipRect(clip, row, area, span);
    if (point.y < rect.getY() + titleHeight &&
        timelineClipSelection.contains(track, hit, editGeneration)) {
        return "Drag selected clips together; Option/Alt-drag copies them (Control-drag also on "
               "Linux). Command/Control+Shift-click toggles a clip; Escape cancels.";
    }
    if (point.x < rect.getX() + kClipEdgeHitPx || point.x > rect.getRight() - kClipEdgeHitPx) {
        juce::int64 first = 0;
        juce::int64 last = 0;
        const bool left = point.x < rect.getX() + kClipEdgeHitPx;
        const auto bounds = carta::clipSampleRange(clip, first, last)
                                ? " Project samples [" + juce::String(first) + ", " +
                                      juce::String(last) + ") (end exclusive)."
                                : juce::String();
        return (left ? juce::String("Start edge.") : juce::String("End edge.")) + bounds +
               (clip.groupId.isNotEmpty()
                    ? " Drag to trim grouped clips together. Hold Shift to bypass snapping; Escape "
                      "cancels."
                    : " Drag to trim or untrim. Hold Shift to bypass snapping; Escape cancels.");
    }
    if (point.y < rect.getY() + titleHeight) {
        return carta::clipDisplayName(clip) +
               (clip.groupId.isNotEmpty()
                    ? ". Click to select; Drag to move the group; Option/Alt-drag copies it "
                      "(Control-drag also on Linux); double-click to zoom; right-click for clip "
                      "actions."
                    : ". Click to select; Drag to move; Option/Alt-drag copies (Control-drag also "
                      "on Linux); double-click to zoom; right-click for clip actions.");
    }
    return "Click to place playback cursor; drag to select time across layers. Alt/Option-click "
           "cycles overlapping clips.";
}

juce::String Studio::scaleHoverHelp(int track, int y) const {
    const auto& state = session.tracks[static_cast<size_t>(track)];
    const auto explanation =
        juce::String("Waveform zoom ") +
        juce::String(carta::trackWaveformZoom(state, waveformVerticalZoom)) + utf8Str("×") +
        (state.waveformZoom == 0 ? " (default). " : ". ") +
        (waveformHalf ? "Half-wave shows absolute amplitude; polarity is hidden. "
                      : "Full-wave shows positive and negative amplitude. ") +
        (!state.channelHeightWeights.empty() &&
                 static_cast<int>(state.channelHeightWeights.size()) !=
                     carta::maxTrackChannels(state)
             ? "Saved channel heights do not match the current channel count; equal heights are "
               "displayed. "
             : "") +
        "Source channel lanes and spectrum frequencies are shared across the layer; missing "
        "channels and frequencies above a source's Nyquist limit are blank. An asterisk indicates "
        "mixed source rates or channel counts.";

    if (state.clips.empty() || state.collapsed) {
        return "Layer scale. " + explanation;
    }

    const int visibleChannel = carta::isolatedDisplayChannel(state);
    const int channels = visibleChannel >= 0 ? 1 : carta::maxTrackChannels(state);
    if (channels <= 0) {
        return "Layer scale. " + explanation;
    }

    const auto rect = clipRect(state.clips.front(), track);
    const int header = std::min(titleHeight, std::max(0, rect.getHeight() - 8));
    const auto content = rect.withTrimmedTop(header);
    const auto layout = carta::trackViewLayout(content.getHeight(), state.view, spectrumView);
    const auto section = [&](int top, int height, bool spectrum) -> juce::String {
        const int local = y - content.getY() - top;
        if (local < 0 || local >= height) {
            return {};
        }
        const auto lanes = carta::channelLaneLayout(height, channels, state.channelHeightWeights);
        for (int lane = 0; lane < static_cast<int>(lanes.size()); ++lane) {
            if (lanes[static_cast<size_t>(lane)].contains(local)) {
                juce::String range;
                double low = 0;
                double high = 0;
                if (spectrum && carta::spectrogramFrequencyRange(
                                    carta::spectrogramTrackRate(state), spectrumBand, low, high,
                                    spectrumCustomLow, spectrumCustomHigh)) {
                    range = " Range " + carta::formatSpectrumFrequency(low) + "Hz to " +
                            carta::formatSpectrumFrequency(high) + "Hz.";
                }
                return "Source channel " +
                       juce::String((visibleChannel >= 0 ? visibleChannel : lane) + 1) + ", " +
                       (spectrum ? "spectrogram" : "waveform") + " scale." + range + " " +
                       explanation;
            }
        }
        return {};
    };

    auto result = section(layout.waveformTop, layout.waveformHeight, false);
    if (result.isEmpty()) {
        result = section(layout.spectrumTop, layout.spectrumHeight, true);
    }
    return result.isEmpty() ? "Layer scale. " + explanation : result;
}

// ============================================================================
// Scale Gutter Rendering
// ============================================================================

void Studio::drawScale(juce::Graphics& g, int track, bool dark, int visibleChannel,
                       int trackChannels, double spectrumRate) {
    const auto row = trackArea(track);
    const auto& clips = session.tracks[static_cast<size_t>(track)].clips;
    const juce::Rectangle<int> gutter(row.getX() + trackLabelWidth, row.getY(), scaleWidth,
                                      row.getHeight());
    g.setColour(dark ? juce::Colour(0xff303030) : juce::Colour(0xffdfdfdf));
    g.fillRect(gutter);
    g.setColour(dark ? juce::Colour(0xffbbbbbb) : juce::Colour(0xff444444));
    g.setFont(juce::FontOptions(9.0f));

    if (session.tracks[static_cast<size_t>(track)].collapsed) {
        const auto& state = session.tracks[static_cast<size_t>(track)];
        if (carta::maxTrackChannels(state) > 0 &&
            !carta::clipRoutingConfirmed(state, carta::maxTrackChannels(state))) {
            g.drawText("!", gutter.withHeight(titleHeight), juce::Justification::centredRight);
        }
        return;
    }
    if (clips.empty()) {
        return;
    }

    const auto& reference = (track == selectedTrack && validSelection())
                                ? clips[static_cast<size_t>(selectedClip)]
                                : clips.front();
    const int sourceChannels = reference.audio->numChannels();
    if (sourceChannels <= 0) {
        return;
    }

    const int channels = visibleChannel >= 0 ? 1 : trackChannels;
    const double rate = reference.audio->sampleRate;
    bool mixed = false;
    for (const auto& clip : clips) {
        if (clip.audio->numChannels() != sourceChannels ||
            std::abs(clip.audio->sampleRate - rate) > 0.01) {
            mixed = true;
        }
    }

    const auto& state = session.tracks[static_cast<size_t>(track)];
    const int trackZoom = carta::trackWaveformZoom(state, waveformVerticalZoom);
    g.drawText(juce::String(state.view == carta::TrackView::combined         ? "dB/Hz"
                            : carta::trackShowsSpectrum(state, spectrumView) ? "Hz"
                                                                             : "dBFS") +
                   (mixed ? "*" : ""),
               gutter.withHeight(titleHeight), juce::Justification::centredRight);

    const auto rect = clipRect(reference, track);
    const int header = std::min(titleHeight, std::max(0, rect.getHeight() - 8));
    const auto content = rect.withTrimmedTop(header);
    const auto layout = carta::trackViewLayout(content.getHeight(), state.view, spectrumView);

    auto drawSection = [&](bool showSpectrum, int offset, int height) {
        if (height <= 0) {
            return;
        }
        const juce::Graphics::ScopedSaveState savedState(g);
        const int top = content.getY() + offset;
        const auto lanes = carta::channelLaneLayout(height, channels, state.channelHeightWeights);

        for (int channel = 0; channel < channels; ++channel) {
            const auto bounds = lanes[static_cast<size_t>(channel)];
            const float lane = static_cast<float>(bounds.getLength());
            const float y0 = static_cast<float>(top + bounds.getStart());
            const float mid = y0 + lane * 0.5f;
            const bool showChannel = (channels > 1 || visibleChannel >= 0) && lane >= 20;
            if (showChannel) {
                g.drawText(juce::String((visibleChannel >= 0 ? visibleChannel : channel) + 1),
                           gutter.getX(), juce::roundToInt(y0) + 1, 18, 10,
                           juce::Justification::centredLeft, true);
            }

            auto tick = [&](float y, const juce::String& text) {
                const int yy = juce::roundToInt(y);
                g.drawHorizontalLine(yy, static_cast<float>(gutter.getRight() - 5),
                                     static_cast<float>(gutter.getRight()));
                const int inset = showChannel ? 18 : 0;
                const int labelHeight = std::min(12, bounds.getLength());
                const int labelY = juce::jlimit(
                    top + bounds.getStart(),
                    std::max(top + bounds.getStart(), top + bounds.getEnd() - labelHeight),
                    yy - labelHeight / 2);
                g.drawText(text, gutter.getX() + inset, labelY, gutter.getWidth() - 7 - inset,
                           labelHeight, juce::Justification::centredRight, true);
            };

            if (!showSpectrum) {
                const auto outer = juce::String(
                    juce::roundToInt(carta::waveformDisplayDb(1.0, trackZoom, waveformScale)));
                const auto inner = juce::String(
                    juce::roundToInt(carta::waveformDisplayDb(0.5, trackZoom, waveformScale)));
                if (waveformHalf) {
                    if (lane < 24) {
                        tick(y0 + lane * 0.96f, "-inf");
                        continue;
                    }
                    tick(y0 + lane * 0.04f, outer);
                    if (lane >= 65) {
                        tick(mid, inner);
                    }
                    tick(y0 + lane * 0.96f, "-inf");
                    continue;
                }
                if (lane < 24) {
                    tick(mid, "-inf");
                    continue;
                }
                tick(mid - lane * 0.46f, outer);
                if (lane >= 65) {
                    tick(mid - lane * 0.23f, inner);
                }
                tick(mid, "-inf");
                if (lane >= 65) {
                    tick(mid + lane * 0.23f, inner);
                }
                if (channel == channels - 1) {
                    tick(mid + lane * 0.46f, outer);
                }
            } else {
                double low = 0;
                double high = 0;
                if (!carta::spectrogramFrequencyRange(spectrumRate, spectrumBand, low, high,
                                                      spectrumCustomLow, spectrumCustomHigh)) {
                    continue;
                }
                for (const auto& mark :
                     carta::spectrogramFrequencyTicks(low, high, bounds.getLength())) {
                    tick(y0 + static_cast<float>(std::max(0, bounds.getLength() - 1)) *
                                  static_cast<float>(mark.position),
                         carta::formatSpectrumFrequency(mark.frequency));
                }
            }
        }

        if (channels > 1 && visibleChannel < 0) {
            g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.45f));
            for (int channel = 0; channel + 1 < channels; ++channel) {
                const int y = top + lanes[static_cast<size_t>(channel)].getEnd();
                if (lanes[static_cast<size_t>(channel + 1)].getEnd() -
                        lanes[static_cast<size_t>(channel)].getStart() >=
                    8) {
                    g.drawHorizontalLine(y, static_cast<float>(gutter.getX() + 2),
                                         static_cast<float>(gutter.getX() + 12));
                }
            }
        }
    };

    drawSection(false, layout.waveformTop, layout.waveformHeight);
    drawSection(true, layout.spectrumTop, layout.spectrumHeight);
}

// ============================================================================
// Waveform & Spectrogram Async Tile Caching
// ============================================================================

void Studio::cancelPendingSpectrum() {
    for (auto it = spectra.begin(); it != spectra.end();) {
        if (it->second.pending) {
            it->second.cancelled->store(true);
            it = spectra.erase(it);
        } else {
            ++it;
        }
    }
}

void Studio::cancelPendingWaveforms() {
    for (auto it = waveforms.begin(); it != waveforms.end();) {
        if (it->second.pending) {
            it->second.cancelled->store(true);
            it = waveforms.erase(it);
        } else {
            ++it;
        }
    }
}

void Studio::setWaveformZoom(int value) {
    waveformVerticalZoom = value;
    status.setText("Waveform vertical zoom: " + juce::String(value) +
                       utf8Str("×; audio is unchanged."),
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    menuItemsChanged();
    repaint();
}

void Studio::setWaveformScale(carta::WaveformScale value) {
    waveformScale = value;
    status.setText(value == carta::WaveformScale::linear
                       ? "Waveform scale: linear amplitude."
                       : "Waveform scale: logarithmic dB, 60 dB visible range.",
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    menuItemsChanged();
    repaint();
}

void Studio::setWaveformHalf(bool enabled) {
    waveformHalf = enabled;
    status.setText(enabled ? "Half-wave display: absolute source amplitude; polarity is hidden."
                           : "Full-wave display: positive and negative source amplitude.",
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    menuItemsChanged();
    repaint();
}

void Studio::setWaveformRms(bool enabled) {
    if (showWaveformRms == enabled) {
        return;
    }
    showWaveformRms = enabled;
    cancelPendingWaveforms();
    const auto message =
        enabled ? juce::String("Clip RMS overlay on. It measures source channels after clip gain "
                               "and fades, before track mixing.")
                : juce::String("Clip RMS overlay off.");
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
}

void Studio::cancelUnseenWaveforms() {
    for (auto it = waveforms.begin(); it != waveforms.end();) {
        if (it->second.pending && it->second.seen != waveformPaintEpoch) {
            it->second.cancelled->store(true);
            it = waveforms.erase(it);
        } else {
            ++it;
        }
    }
}

void Studio::pruneWaveformCache() {
    constexpr size_t maxBytes = 16u * 1024u * 1024u;
    constexpr size_t maxEntries = 256;
    for (auto it = waveforms.begin(); it != waveforms.end();) {
        if (!it->second.pending && it->second.source.expired()) {
            it = waveforms.erase(it);
        } else {
            ++it;
        }
    }

    for (;;) {
        size_t bytes = 0;
        auto oldest = waveforms.end();
        for (auto it = waveforms.begin(); it != waveforms.end(); ++it) {
            bytes += it->second.bytes;
            if (!it->second.pending &&
                (oldest == waveforms.end() || it->second.lastUse < oldest->second.lastUse)) {
                oldest = it;
            }
        }
        if ((bytes <= maxBytes && waveforms.size() <= maxEntries) || oldest == waveforms.end()) {
            break;
        }
        waveforms.erase(oldest);
    }
}

std::shared_ptr<const std::vector<carta::WaveformColumn>>
Studio::waveformColumnsFor(const carta::Clip& clip, int channel, int firstX, int columns,
                           bool includeRms, juce::Rectangle<int> area, double span) {
    const auto& audio = clip.audio;
    if (!audio || channel < 0 || channel >= audio->numChannels() || columns <= 0 ||
        columns > 8192 || !std::isfinite(viewStart) || !std::isfinite(span) || span <= 0 ||
        !std::isfinite(clip.start) || !std::isfinite(clip.offset) ||
        !std::isfinite(clip.duration) || !std::isfinite(clip.gain) || !std::isfinite(clip.fadeIn) ||
        !std::isfinite(clip.fadeOut) || !std::isfinite(clip.fadeInStart) ||
        !std::isfinite(clip.fadeOutEnd)) {
        return {};
    }

    const WaveformKey key{audio.get(),
                          channel,
                          area.getX() + labelWidth,
                          std::max(1, area.getWidth() - labelWidth),
                          firstX,
                          columns,
                          viewStart,
                          span,
                          clip.start,
                          clip.offset,
                          clip.duration,
                          clip.fadeIn,
                          clip.fadeOut,
                          clip.fadeInStart,
                          clip.fadeOutEnd,
                          clip.gain,
                          includeRms,
                          clip.fadeInShape,
                          clip.fadeOutShape};
    auto it = waveforms.find(key);
    if (it != waveforms.end() && it->second.source.lock() != audio) {
        if (it->second.pending) {
            it->second.cancelled->store(true);
        }
        waveforms.erase(it);
        it = waveforms.end();
    }
    if (it != waveforms.end()) {
        it->second.lastUse = ++waveformUse;
        it->second.seen = waveformPaintEpoch;
        return it->second.columns;
    }
    if (dragMode != kDragNone || waveformPool.getNumJobs() >= 16) {
        return {};
    }

    auto token = std::make_shared<std::atomic<bool>>(false);
    auto& entry = waveforms[key];
    entry.source = audio;
    entry.pending = true;
    entry.cancelled = token;
    entry.lastUse = ++waveformUse;
    entry.seen = waveformPaintEpoch;

    waveformPool.addJob(
        [safe = juce::Component::SafePointer<Studio>(this), audio, clip, key, token] {
            std::shared_ptr<std::vector<carta::WaveformColumn>> result;
            bool ok = false;
            try {
                result = std::make_shared<std::vector<carta::WaveformColumn>>();
                ok = carta::exactWaveformColumns(
                    clip, key.channel, key.viewStart, key.span, key.left, key.width, key.firstX,
                    key.columns, *result,
                    [token] {
                        auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob();
                        return token->load() || (job != nullptr && job->shouldExit());
                    },
                    key.rms);
            } catch (...) {
                ok = false;
            }
            juce::MessageManager::callAsync([safe, audio, key, token, result, ok] {
                if (safe == nullptr) {
                    return;
                }
                auto found = safe->waveforms.find(key);
                if (found != safe->waveforms.end() && found->second.cancelled == token &&
                    found->second.source.lock() == audio) {
                    found->second.pending = false;
                    found->second.failed = !ok;
                    if (ok) {
                        found->second.columns = result;
                        found->second.bytes = result->size() * sizeof(carta::WaveformColumn);
                    }
                    safe->pruneWaveformCache();
                }
                safe->repaint();
            });
        });

    pruneWaveformCache();
    return {};
}

void Studio::updateSpectrumViewport(double span) {
    const int width = timeline().getWidth();
    if (spectrumViewportValid && spectrumViewportGeneration == editGeneration &&
        spectrumViewportEnabled == spectrumView && spectrumViewportWidth == width &&
        std::abs(spectrumViewportStart - viewStart) < 1e-12 &&
        std::abs(spectrumViewportSpan - span) < 1e-12) {
        return;
    }
    spectrumViewportValid = true;
    spectrumViewportGeneration = editGeneration;
    spectrumViewportEnabled = spectrumView;
    spectrumViewportWidth = width;
    spectrumViewportStart = viewStart;
    spectrumViewportSpan = span;
    cancelPendingSpectrum();
    cancelPendingWaveforms();
}

void Studio::pruneSpectrumCache() {
    constexpr size_t maxBytes = 64u * 1024u * 1024u;
    constexpr size_t maxEntries = 192;
    for (auto it = spectra.begin(); it != spectra.end();) {
        if (!it->second.pending && it->second.source.expired()) {
            it = spectra.erase(it);
        } else {
            ++it;
        }
    }

    for (;;) {
        size_t bytes = 0;
        auto oldest = spectra.end();
        for (auto it = spectra.begin(); it != spectra.end(); ++it) {
            bytes += it->second.bytes;
            if (!it->second.pending &&
                (oldest == spectra.end() || it->second.lastUsed < oldest->second.lastUsed)) {
                oldest = it;
            }
        }
        if ((bytes <= maxBytes && spectra.size() <= maxEntries) || oldest == spectra.end()) {
            break;
        }
        spectra.erase(oldest);
    }
}

juce::Image Studio::spectrumFor(const carta::Clip& clip, juce::int64 start, juce::int64 step,
                                int columns, double displayRate, bool& failed) {
    const auto& audio = clip.audio;
    if (!audio || !std::isfinite(clip.offset) || !std::isfinite(clip.duration) ||
        !std::isfinite(clip.gain) || !std::isfinite(clip.fadeIn) || !std::isfinite(clip.fadeOut) ||
        !std::isfinite(clip.fadeInStart) || !std::isfinite(clip.fadeOutEnd)) {
        failed = true;
        return {};
    }

    const SpectrumKey key{audio.get(),
                          start,
                          step,
                          columns,
                          spectrumFftOrder,
                          spectrumBand,
                          displayRate,
                          clip.offset,
                          clip.duration,
                          clip.gain,
                          clip.fadeIn,
                          clip.fadeOut,
                          clip.fadeInStart,
                          clip.fadeOutEnd,
                          spectrumCustomLow,
                          spectrumCustomHigh,
                          spectrumWindow,
                          spectrumFloorDb,
                          clip.fadeInShape,
                          clip.fadeOutShape};
    auto it = spectra.find(key);
    if (it != spectra.end() && it->second.source.lock() != audio) {
        if (it->second.pending) {
            it->second.cancelled->store(true);
        }
        spectra.erase(it);
        it = spectra.end();
    }
    if (it != spectra.end()) {
        it->second.lastUsed = ++spectrumUse;
        failed = failed || it->second.failed;
        return it->second.image;
    }
    if (spectrumPool.getNumJobs() >= 32) {
        return {};
    }

    auto token = std::make_shared<std::atomic<bool>>(false);
    auto& entry = spectra[key];
    entry.source = audio;
    entry.pending = true;
    entry.cancelled = token;
    entry.lastUsed = ++spectrumUse;

    spectrumPool.addJob(
        [safe = juce::Component::SafePointer<Studio>(this), audio, clip, key, token] {
            juce::Image image;
            try {
                image = carta::makeClipSpectrogramTile(
                    clip, key.start, key.step, key.columns,
                    [token] {
                        auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob();
                        return token->load() || (job != nullptr && job->shouldExit());
                    },
                    key.fftOrder, key.band, key.displayRate, key.customLow, key.customHigh,
                    key.window, key.floorDb);
            } catch (...) {
                image = {};
            }
            juce::MessageManager::callAsync([safe, audio, key, token, image] {
                if (safe == nullptr) {
                    return;
                }
                auto found = safe->spectra.find(key);
                if (found != safe->spectra.end() && found->second.cancelled == token &&
                    found->second.source.lock() == audio) {
                    found->second.pending = false;
                    found->second.image = image;
                    found->second.failed = !image.isValid();
                    found->second.bytes = image.isValid()
                                              ? static_cast<size_t>(image.getWidth()) *
                                                    static_cast<size_t>(image.getHeight()) * 4u
                                              : 0;
                    safe->pruneSpectrumCache();
                }
                safe->repaint();
            });
        });

    pruneSpectrumCache();
    return {};
}

// ============================================================================
// Clip & Title Rendering
// ============================================================================

void Studio::drawSpectrumTiles(juce::Graphics& g, const carta::Clip& clip,
                               juce::Rectangle<int> content, juce::Rectangle<int> visible,
                               int visibleChannel, int trackChannels, double displayRate,
                               const std::vector<int>& weights, juce::Rectangle<int> area,
                               double span) {
    const auto& audio = *clip.audio;
    const auto rate = audio.sampleRate;
    if (rate <= 0 || audio.numSamples() <= 0 || audio.numChannels() <= 0 || content.isEmpty()) {
        return;
    }

    const double left = std::max({clip.offset, 0.0, clip.offset + viewStart - clip.start});
    const double right = std::min(
        {clip.offset + clip.duration, audio.length(), clip.offset + viewStart + span - clip.start});
    if (!(right > left)) {
        return;
    }

    const double samplesPerPixel = rate * span / std::max(1, area.getWidth() - labelWidth);
    juce::int64 step = 1;
    while (static_cast<double>(step) < samplesPerPixel && step < (1LL << 31)) {
        step *= 2;
    }
    const int columns = std::max(1, std::min(128, 2000000 / (96 * audio.numChannels())));
    const juce::int64 tileFrames = step * columns;
    const auto first = static_cast<juce::int64>(std::floor(left * rate));
    const auto last = static_cast<juce::int64>(std::ceil(right * rate));

    bool pending = false;
    bool failed = false;
    const int lanes = visibleChannel >= 0 ? 1 : trackChannels;
    const auto laneBounds = carta::channelLaneLayout(content.getHeight(), lanes, weights);

    for (auto start = (first / tileFrames) * tileFrames; start < last && start < audio.numSamples();
         start += tileFrames) {
        const auto image = spectrumFor(clip, start, step, columns, displayRate, failed);
        if (!image.isValid()) {
            pending = true;
            continue;
        }
        const int x1 =
            timeToX(clip.start + static_cast<double>(start) / rate - clip.offset, area, span);
        const int x2 = timeToX(
            clip.start + static_cast<double>(start + tileFrames) / rate - clip.offset, area, span);
        if (x2 > x1) {
            const int sourceHeight = image.getHeight() / audio.numChannels();
            const int shown = visibleChannel >= 0 ? 1 : audio.numChannels();
            g.setOpacity(0.75f);
            for (int lane = 0; lane < shown; ++lane) {
                const int sourceChannel = visibleChannel >= 0 ? visibleChannel : lane;
                const auto bounds = laneBounds[static_cast<size_t>(lane)];
                if (bounds.getLength() > 0) {
#if JUCE_LINUX || CARTA_STUDIO_COMPONENT_TEST
                    if (useStableSpectrumRaster()) {
                        const auto tileWidth = static_cast<juce::int64>(x2) - x1;
                        if (tileWidth > 0 && tileWidth <= std::numeric_limits<int>::max()) {
                            const auto clipped =
                                visible.getIntersection(g.getClipBounds())
                                    .getIntersection({x1, content.getY() + bounds.getStart(),
                                                      static_cast<int>(tileWidth),
                                                      bounds.getLength()});
                            const auto firstPixel = std::max<juce::int64>(
                                0, static_cast<juce::int64>(clipped.getX()) - x1);
                            const auto lastPixel = std::min<juce::int64>(
                                tileWidth, static_cast<juce::int64>(clipped.getRight()) - x1);
                            g.setOpacity(1.0f);
                            for (auto offset =
                                     (firstPixel / carta::StableSpectrumRaster::chunkSize) *
                                     carta::StableSpectrumRaster::chunkSize;
                                 offset < lastPixel;
                                 offset += carta::StableSpectrumRaster::chunkSize) {
                                const int chunkX =
                                    static_cast<int>(static_cast<juce::int64>(x1) + offset);
                                const auto raster = stableSpectrumRaster.chunk(
                                    image, sourceChannel * sourceHeight, sourceHeight,
                                    static_cast<int>(tileWidth), bounds.getLength(),
                                    static_cast<int>(offset));
                                if (raster.isValid()) {
                                    g.drawImageAt(raster, chunkX,
                                                  content.getY() + bounds.getStart());
                                } else {
                                    const juce::Graphics::ScopedSaveState fallback(g);
                                    g.reduceClipRegion({chunkX, content.getY() + bounds.getStart(),
                                                        static_cast<int>(std::min<juce::int64>(
                                                            carta::StableSpectrumRaster::chunkSize,
                                                            tileWidth - offset)),
                                                        bounds.getLength()});
                                    g.setOpacity(0.75f);
                                    g.drawImage(image, x1, content.getY() + bounds.getStart(),
                                                static_cast<int>(tileWidth), bounds.getLength(), 0,
                                                sourceChannel * sourceHeight, image.getWidth(),
                                                sourceHeight);
                                }
                            }
                            g.setOpacity(0.75f);
                        }
                    } else
#endif
                        g.drawImage(image, x1, content.getY() + bounds.getStart(), x2 - x1,
                                    bounds.getLength(), 0, sourceChannel * sourceHeight,
                                    image.getWidth(), sourceHeight);
                }
            }
            g.setOpacity(1.0f);
        }
    }

    if (pending) {
        const bool dark = juce::Desktop::getInstance().isDarkModeActive();
        g.setColour((dark ? juce::Colours::lightgrey : juce::Colours::darkgrey).withAlpha(0.8f));
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(failed ? "Spectrum analysis unavailable." : "Analyzing spectrum...",
                   visible.reduced(5), juce::Justification::centredLeft, true);
    }
}

void Studio::drawSpectrumTiles(juce::Graphics& g, const carta::Clip& clip,
                               juce::Rectangle<int> content, juce::Rectangle<int> visible,
                               int visibleChannel, int trackChannels, double displayRate,
                               const std::vector<int>& weights) {
    drawSpectrumTiles(g, clip, content, visible, visibleChannel, trackChannels, displayRate,
                      weights, timeline(), viewLength());
}

std::pair<juce::Rectangle<int>, juce::Rectangle<int>>
Studio::clipTrimGripBounds(juce::Rectangle<int> rect) {
    const int header = std::min(titleHeight, std::max(0, rect.getHeight() - 8));
    const auto body = rect.withTrimmedTop(header);
    if (rect.getWidth() < 18 || body.getHeight() < 12) {
        return {};
    }
    const int height = std::min(14, body.getHeight() - 6);
    const int y = body.getY() + (body.getHeight() - height) / 2;
    return {{rect.getX() + 1, y, 3, height}, {rect.getRight() - 4, y, 3, height}};
}

void Studio::drawClip(juce::Graphics& g, const carta::Clip& c, int t, int index, int visibleChannel,
                      int trackChannels, double spectrumRate, juce::Rectangle<int> row,
                      juce::Rectangle<int> area, double span, bool dark,
                      bool* incompleteWaveformPaint) {
    const auto rect = clipRect(c, row, area, span);
    const auto visible = rect.getIntersection(row.withTrimmedLeft(labelWidth));
    if (visible.isEmpty()) {
        return;
    }
    if (!g.clipRegionIntersects(visible)) {
        if (incompleteWaveformPaint) {
            *incompleteWaveformPaint = true;
        }
        return;
    }

    const juce::Graphics::ScopedSaveState saveState(g);
    g.reduceClipRegion(visible);
    const bool selected = clipAppearsSelected(t, index);
    const int trackZoom =
        carta::trackWaveformZoom(session.tracks[static_cast<size_t>(t)], waveformVerticalZoom);
    if (c.lockedOnDisc) {
        g.setColour((dark ? (selected ? juce::Colour(0xff2b5968) : juce::Colour(0xff1e414d))
                          : (selected ? juce::Colour(0xff6b9aa8) : juce::Colour(0xff89b2bf)))
                        .withAlpha(0.65f));
        g.fillRect(rect);
        g.setColour(dark ? juce::Colour(0xff4dd0e1).withAlpha(0.80f)
                         : juce::Colour(0xff00796b).withAlpha(0.85f));
        g.fillRect(rect.getX(), rect.getY(), rect.getWidth(), 2);
    } else {
        g.setColour((dark ? (selected ? juce::Colour(0xff8b8b8b) : juce::Colour(0xff666666))
                          : (selected ? juce::Colour(0xff888888) : juce::Colour(0xffaaaaaa)))
                        .withAlpha(0.56f));
        g.fillRect(rect);
    }

    const int header = std::min(titleHeight, std::max(0, rect.getHeight() - 8));
    const auto content = rect.withTrimmedTop(header);
    if (session.tracks[static_cast<size_t>(t)].collapsed) {
        // Keep the clip title and selection outline without drawing its lanes.
    } else if (c.audio && c.audio->onDiscPlaceholder) {
        if (content.getHeight() > 6 && visible.getWidth() > 24) {
            const int midY = content.getCentreY();
            g.setColour(
                (dark ? juce::Colour(0xff4dd0e1) : juce::Colour(0xff00695c)).withAlpha(0.28f));
            g.drawHorizontalLine(midY, static_cast<float>(visible.getX()),
                                 static_cast<float>(visible.getRight()));
            if (visible.getWidth() > 54 && content.getHeight() > 14) {
                g.setColour(
                    (dark ? juce::Colour(0xffb2ebf2) : juce::Colour(0xff004d40)).withAlpha(0.85f));
                g.setFont(juce::FontOptions(10.5f));
                const juce::String label =
                    visible.getWidth() >= 150 ? "On-Disc Audio (UTOC loaded)" : "On-Disc";
                g.drawText(label, content.getIntersection(visible).reduced(6, 0),
                           juce::Justification::centredLeft, true);
            }
        }
    } else {
        const auto layout = carta::trackViewLayout(
            content.getHeight(), session.tracks[static_cast<size_t>(t)].view, spectrumView);
        if (layout.spectrumHeight > 0) {
            const auto spectrumContent = content.withY(content.getY() + layout.spectrumTop)
                                             .withHeight(layout.spectrumHeight);
            drawSpectrumTiles(g, c, spectrumContent, visible.getIntersection(spectrumContent),
                              visibleChannel, trackChannels, spectrumRate,
                              session.tracks[static_cast<size_t>(t)].channelHeightWeights, area,
                              span);
        }

        const auto waveformContent =
            content.withY(content.getY() + layout.waveformTop).withHeight(layout.waveformHeight);
        if (layout.waveformHeight > 0 && !g.clipRegionIntersects(waveformContent)) {
            if (incompleteWaveformPaint) {
                *incompleteWaveformPaint = true;
            }
        } else if (layout.waveformHeight > 0) {
            const juce::Graphics::ScopedSaveState waveState(g);
            g.reduceClipRegion(waveformContent);
            const auto dirtyWaveform = visible.getIntersection(g.getClipBounds().expanded(4, 0));
            g.setColour(
                (dark ? juce::Colour(0xffeeeeee) : juce::Colour(0xff303030)).withAlpha(0.82f));
            const int channels = visibleChannel >= 0 ? 1 : trackChannels;
            const int shown = visibleChannel >= 0 ? 1 : c.audio->numChannels();
            const auto laneBounds = carta::channelLaneLayout(
                waveformContent.getHeight(), channels,
                session.tracks[static_cast<size_t>(t)].channelHeightWeights);

            for (int lane = 0; lane < shown; ++lane) {
                const int ch = visibleChannel >= 0 ? visibleChannel : lane;
                const auto bounds = laneBounds[static_cast<size_t>(lane)];
                const float laneHeight = static_cast<float>(bounds.getLength());
                if (laneHeight <= 0) {
                    continue;
                }

                const float top = static_cast<float>(waveformContent.getY() + bounds.getStart());
                const float centre = top + (waveformHalf ? 0.96f : 0.5f) * laneHeight;
                const float amplitudeHeight = laneHeight * (waveformHalf ? 0.92f : 0.46f);
                juce::Path confirmedPeaks;
                juce::Path possiblePeaks;
                const auto recordPeak = [&](float x, bool positive, bool negative, bool exact) {
                    if (!showClipPeakWarnings) {
                        return;
                    }
                    auto& path = exact ? confirmedPeaks : possiblePeaks;
                    if (positive || (waveformHalf && negative)) {
                        path.addRectangle(x, centre - amplitudeHeight, 1.5f, 4.0f);
                    }
                    if (!waveformHalf && negative) {
                        path.addRectangle(x, centre + amplitudeHeight - 4.0f, 1.5f, 4.0f);
                    }
                };
                const auto paintPeaks = [&] {
                    if (!possiblePeaks.isEmpty()) {
                        g.setColour(dark ? juce::Colour(0xffffc463) : juce::Colour(0xff875000));
                        g.fillPath(possiblePeaks);
                    }
                    if (!confirmedPeaks.isEmpty()) {
                        g.setColour(dark ? juce::Colour(0xffff6b5c) : juce::Colour(0xffad2118));
                        g.fillPath(confirmedPeaks);
                    }
                };

                const auto waveColour =
                    (dark ? juce::Colour(0xffeeeeee) : juce::Colour(0xff303030)).withAlpha(0.82f);
                g.setColour(waveColour.withAlpha(0.25f));
                g.drawHorizontalLine(juce::roundToInt(centre), static_cast<float>(visible.getX()),
                                     static_cast<float>(visible.getRight()));
                g.setColour(waveColour);

                const double samplesPerPixel =
                    c.audio->sampleRate * span / std::max(1, area.getWidth() - labelWidth);
                if (samplesPerPixel <= 1.0) {
                    int first = 0;
                    int end = 0;
                    if (!carta::waveformPointRange(c, xToTime(dirtyWaveform.getX(), area, span),
                                                   xToTime(dirtyWaveform.getRight(), area, span),
                                                   first, end)) {
                        continue;
                    }
                    bool havePrevious = false;
                    float previousX = 0;
                    float previousY = 0;
                    float previousValue = 0;
                    for (int sample = first; sample < end; ++sample) {
                        const float px = static_cast<float>(timeToX(
                            c.start + static_cast<double>(sample) / c.audio->sampleRate - c.offset,
                            area, span));
                        const float value =
                            c.audio->channelData(ch)[sample] *
                            carta::clipLevel(c, static_cast<double>(sample) / c.audio->sampleRate);
                        const float py = centre - carta::waveformDisplayValue(
                                                      waveformHalf ? std::abs(value) : value,
                                                      trackZoom, waveformScale) *
                                                      amplitudeHeight;
                        if (havePrevious) {
                            if (waveformHalf && std::isfinite(previousValue) &&
                                std::isfinite(value) &&
                                ((previousValue < 0 && value > 0) ||
                                 (previousValue > 0 && value < 0))) {
                                const double a = static_cast<double>(previousValue);
                                const double b = static_cast<double>(value);
                                const float crossing =
                                    previousX + (px - previousX) * static_cast<float>(-a / (b - a));
                                g.drawLine(previousX, previousY, crossing, centre, 1.0f);
                                g.drawLine(crossing, centre, px, py, 1.0f);
                            } else {
                                g.drawLine(previousX, previousY, px, py, 1.0f);
                            }
                        }
                        g.fillEllipse(px - 1.5f, py - 1.5f, 3.0f, 3.0f);
                        recordPeak(px, value > 1.0f, value < -1.0f, true);
                        previousX = px;
                        previousY = py;
                        previousValue = value;
                        havePrevious = true;
                    }
                    paintPeaks();
                    continue;
                }

                const auto block = c.audio->peakCache->blockSize;
                const auto needed =
                    block > 0
                        ? static_cast<size_t>(
                              (static_cast<juce::int64>(c.audio->numSamples()) + block - 1) / block)
                        : 0;
                const bool cached =
                    block > 0 && static_cast<size_t>(ch) < c.audio->peakCache->peaks.size() &&
                    c.audio->peakCache->peaks[static_cast<size_t>(ch)].size() >= needed;
                const bool expensive = samplesPerPixel > 4096 && (!cached || block > 4096);
                const auto precise =
                    (showWaveformRms ||
                     (samplesPerPixel > 4096 && (expensive || c.fadeIn > 0 || c.fadeOut > 0)))
                        ? waveformColumnsFor(c, ch, visible.getX(), visible.getWidth(),
                                             showWaveformRms, area, span)
                        : nullptr;
                bool missingWaveform = false;
                bool missingRms = false;
                // Pending/unavailable labels describe the whole visible clip, so
                // retain a full scan when their presence depends on any column.
                const auto scan = showWaveformRms || expensive ? visible : dirtyWaveform;
                for (int x = scan.getX() - rect.getX(); x < scan.getRight() - rect.getX(); ++x) {
                    const int screenX = rect.getX() + x;
                    int lo = 0;
                    int hi = 0;
                    if (!carta::waveformSampleRange(c, xToTime(screenX, area, span),
                                                    xToTime(screenX + 1, area, span), lo, hi)) {
                        continue;
                    }

                    bool exact = false;
                    juce::Range<float> range;
                    const auto column = static_cast<size_t>(screenX - visible.getX());
                    if (precise && column < precise->size() && (*precise)[column].hasSamples) {
                        range = (*precise)[column].range;
                        exact = true;
                    } else if (expensive) {
                        if (!carta::cachedWaveformBounds(c, ch, lo, hi, range)) {
                            missingWaveform = true;
                            continue;
                        }
                    } else {
                        range = carta::clipWaveformRange(c, ch, lo, hi, &exact);
                    }

                    const float low = waveformHalf
                                          ? 0.0f
                                          : carta::waveformDisplayValue(range.getStart(), trackZoom,
                                                                        waveformScale);
                    const float high = carta::waveformDisplayValue(
                        waveformHalf
                            ? std::max(std::abs(range.getStart()), std::abs(range.getEnd()))
                            : range.getEnd(),
                        trackZoom, waveformScale);
                    const bool haveRms = showWaveformRms && precise && column < precise->size() &&
                                         (*precise)[column].hasRms;
                    if (showWaveformRms && !haveRms) {
                        missingRms = true;
                    }
                    if (haveRms) {
                        g.setColour(waveColour.withAlpha(0.32f));
                    }
                    g.drawVerticalLine(rect.getX() + x, centre - high * amplitudeHeight,
                                       centre - low * amplitudeHeight + 1.0f);
                    if (haveRms) {
                        const float magnitude = carta::waveformDisplayValue(
                            (*precise)[column].rms, trackZoom, waveformScale);
                        const auto rmsBounds = carta::waveformRmsDisplayRange(low, high, magnitude);
                        g.setColour(waveColour);
                        g.drawVerticalLine(rect.getX() + x,
                                           centre - rmsBounds.getEnd() * amplitudeHeight,
                                           centre - rmsBounds.getStart() * amplitudeHeight + 1.0f);
                    }
                    recordPeak(static_cast<float>(screenX), range.getEnd() > 1.0f,
                               range.getStart() < -1.0f, exact);
                }

                paintPeaks();
                if (missingWaveform) {
                    g.setColour(waveColour);
                    g.setFont(juce::FontOptions(10.0f));
                    g.drawText("Waveform pending or unavailable", visible.getX() + 4,
                               juce::roundToInt(top), std::max(0, visible.getWidth() - 8),
                               juce::roundToInt(laneHeight), juce::Justification::centredLeft,
                               true);
                } else if (missingRms) {
                    g.setColour(waveColour);
                    g.setFont(juce::FontOptions(10.0f));
                    g.drawText("RMS pending or unavailable", visible.getX() + 4,
                               juce::roundToInt(top), std::max(0, visible.getWidth() - 8),
                               juce::roundToInt(laneHeight), juce::Justification::centredLeft,
                               true);
                }
            }
        }
    }

    if (!c.lockedOnDisc && !session.tracks[static_cast<size_t>(t)].collapsed &&
        content.getHeight() > 4) {
        const float topY = static_cast<float>(content.getY());
        const float bottomY = static_cast<float>(content.getBottom());
        const auto drawFadeOverlay = [&](double segStart, double segDuration,
                                         carta::FadeShape shape, bool isFadeIn) {
            if (!(segDuration > 0.0) || !std::isfinite(segStart) || !std::isfinite(segDuration)) {
                return;
            }
            const double clipSrcStart = c.offset;
            const double clipSrcEnd = c.offset + c.duration;
            const double visStart = std::max(clipSrcStart, segStart);
            const double visEnd = std::min(clipSrcEnd, segStart + segDuration);
            if (!(visEnd > visStart)) {
                return;
            }
            const float xLeft = static_cast<float>(rect.getX()) +
                                static_cast<float>((visStart - clipSrcStart) /
                                                   std::max(1e-12, c.duration) * rect.getWidth());
            const float xRight = static_cast<float>(rect.getX()) +
                                 static_cast<float>((visEnd - clipSrcStart) /
                                                    std::max(1e-12, c.duration) * rect.getWidth());
            if (xRight - xLeft < 1.5f) {
                return;
            }

            constexpr int steps = 24;
            juce::Path shade;
            juce::Path stroke;
            for (int step = 0; step <= steps; ++step) {
                const double frac = static_cast<double>(step) / steps;
                const double src = visStart + frac * (visEnd - visStart);
                const double u = isFadeIn ? (src - segStart) / segDuration
                                          : (segStart + segDuration - src) / segDuration;
                const float env = carta::fadeCurveGain(u, shape);
                const float px = xLeft + static_cast<float>(frac) * (xRight - xLeft);
                const float py = bottomY - env * (bottomY - topY);
                if (step == 0) {
                    shade.startNewSubPath(px, topY);
                    shade.lineTo(px, py);
                    stroke.startNewSubPath(px, py);
                } else {
                    shade.lineTo(px, py);
                    stroke.lineTo(px, py);
                }
            }
            shade.lineTo(xRight, topY);
            shade.closeSubPath();
            g.setColour(juce::Colours::black.withAlpha(dark ? 0.30f : 0.16f));
            g.fillPath(shade);
            g.setColour((dark ? juce::Colours::white : juce::Colour(0xff202020)).withAlpha(0.75f));
            g.strokePath(stroke, juce::PathStrokeType(1.25f));
        };

        if (c.fadeIn > 0.0) {
            drawFadeOverlay(c.fadeInStart, c.fadeIn, c.fadeInShape, true);
        }
        if (c.fadeOut > 0.0) {
            drawFadeOverlay(c.fadeOutEnd - c.fadeOut, c.fadeOut, c.fadeOutShape, false);
        }
    }

    if (selected) {
        g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.8f));
        g.drawRect(rect, 1);
    }
    if (!c.lockedOnDisc && t == selectedTrack && index == selectedClip &&
        !session.tracks[static_cast<size_t>(t)].collapsed) {
        const auto grips = clipTrimGripBounds(rect);
        g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.85f));
        if (!grips.first.isEmpty()) {
            g.fillRoundedRectangle(grips.first.toFloat(), 1.5f);
        }
        if (!grips.second.isEmpty()) {
            g.fillRoundedRectangle(grips.second.toFloat(), 1.5f);
        }
    }
    if (!displayedClipSelection().get(editGeneration).empty() && t == selectedTrack &&
        index == selectedClip && rect.getWidth() > 4 && rect.getHeight() > 4) {
        g.setColour((dark ? juce::Colours::white : juce::Colours::black).withAlpha(0.85f));
        g.drawRect(rect.reduced(2), 1);
    }
}

void Studio::drawClip(juce::Graphics& g, const carta::Clip& c, int t, int index, int visibleChannel,
                      int trackChannels, double spectrumRate) {
    drawClip(g, c, t, index, visibleChannel, trackChannels, spectrumRate, trackArea(t), timeline(),
             viewLength(), juce::Desktop::getInstance().isDarkModeActive());
}

void Studio::drawTitle(juce::Graphics& g, const carta::Clip& c, int track,
                       const carta::ClipTitleRegion& region, juce::Rectangle<int> row, bool dark) {
    const juce::Rectangle<int> title(region.bounds.getStart(), row.getY() + 4,
                                     region.bounds.getLength(), titleHeight);
    if (title.isEmpty()) {
        return;
    }

    const bool selected = clipAppearsSelected(track, region.clip);
    g.setColour((dark ? (selected ? juce::Colour(0xff777777) : juce::Colour(0xff3c3c3c))
                      : (selected ? juce::Colour(0xffaaaaaa) : juce::Colour(0xffdddddd)))
                    .withAlpha(0.94f));
    g.fillRect(title);
    g.setColour((dark ? juce::Colours::white : juce::Colour(0xff202020)).withAlpha(0.85f));
    g.drawRect(title, 1);
    if (!displayedClipSelection().get(editGeneration).empty() && track == selectedTrack &&
        region.clip == selectedClip && title.getWidth() > 4 && title.getHeight() > 4) {
        g.drawRect(title.reduced(2), 1);
    }
    g.setFont(juce::FontOptions(11.0f));
    g.drawText((c.lockedOnDisc ? juce::String("[MD] ") : juce::String()) +
                   (c.groupId.isNotEmpty() ? juce::String::fromUTF8("◇ ") : juce::String()) +
                   carta::clipDisplayName(c),
               title.reduced(5, 0), juce::Justification::centredLeft, true);
}
