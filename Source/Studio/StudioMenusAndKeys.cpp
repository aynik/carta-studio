/**
 * @file StudioMenusAndKeys.cpp
 * @brief Context menus, menubar construction, `ApplicationCommandTarget`
 *        command dispatch, and keyboard shortcut handling for `Studio`.
 *
 * Coordinate spaces and units:
 * - Context menu hit-testing maps screen/component coordinates against
 *   timeline header/track regions and `timeToX()` / `xToTime()` pixel bounds.
 * - Split-at-pointer and sample-nudge commands operate in 44.1 kHz project
 *   samples (`carta::sessionRate`), validated against `2^53`
 *   (`9007199254740992.0`) for exact IEEE-754 double sample indexing.
 * - Dynamic undo/redo history menu items allocate IDs from
 *   `MenuId::historyBase` (`1000000`) via `historyCommands`.
 *
 * Threading and locking invariants:
 * - Menus, command queries, and key events execute on the JUCE message thread.
 * - `getMenuForIndex()` snapshots only `playing` under `Studio::lock` for the
 *   Transport menu; context menu callbacks and `menuItemSelected()` acquire
 *   `lock` briefly when checking revisions or stopping special auditions, and
 *   release it via `juce::ScopedUnlock` before invoking modal editors or actions.
 * - `perform()` ignores `InvocationInfo::fromMenu` because JUCE invokes
 *   `menuItemSelected()` separately for menu-bar clicks, avoiding double edits.
 */

#include "Studio/Studio.h"

namespace {

constexpr int kTimelineRulerHeight = 32;

namespace MenuBarIndex {
constexpr int file = 0;
constexpr int edit = 1;
constexpr int transport = 2;
constexpr int markers = 3;
constexpr int master = 4;
constexpr int view = 5;
constexpr int layer = 6;
constexpr int help = 7;
} // namespace MenuBarIndex

namespace MenuId {
// File, project & export
constexpr int fileImportAudio = 1;
constexpr int fileOpenProject = 2;
constexpr int fileSaveProject = 3;
constexpr int fileExportWav = 4;
constexpr int fileNewProject = 20;
constexpr int fileExportManifest = 44;
constexpr int fileSaveProjectAs = 45;
constexpr int fileExportSelWavFull = 51;
constexpr int fileExportSelWavLayers = 52;
constexpr int fileExportTrackAtPlayhead = 53;
constexpr int fileCancelExport = 55;
constexpr int fileCancelPendingImports = 58;
constexpr int fileCancelOpen = 66;
constexpr int fileCancelSave = 67;
constexpr int fileSaveRecoveryNow = 74;
constexpr int fileRecoverAutosave = 75;
constexpr int fileExportSelectedClipWav = 122;
constexpr int fileExportAllMasterTracks = 128;
constexpr int fileExportPortable = 198;
constexpr int fileExportSelectedClipsWav = 211;
constexpr int fileShowImportIssues = 263;
constexpr int fileClearImportIssues = 264;
constexpr int fileExportMdReviewWav = 1020;

// History & clipboard / range editing
constexpr int editUndo = 5;
constexpr int editSplit = 6;
constexpr int editDeleteClip = 7;
constexpr int editRedo = 18;
constexpr int editDuplicateClip = 19;
constexpr int editCutSelection = 24;
constexpr int editCopySelection = 25;
constexpr int editPasteOverlap = 26;
constexpr int editDeleteSelection = 27;
constexpr int editRippleDeleteSelected = 28;
constexpr int editRippleDeleteAll = 29;
constexpr int editZoomToSelection = 39;
constexpr int editSetTimeSelection = 42;
constexpr int editSelectAll = 43;
constexpr int editSplitSelToNewLayers = 107;
constexpr int editInsertGapSelected = 109;
constexpr int editInsertGapAll = 110;
constexpr int editSetSelectionStartLength = 115;
constexpr int editToBoundaryMasterStart = 116;
constexpr int editToBoundaryMasterEnd = 117;
constexpr int editToBoundaryPrevMarker = 118;
constexpr int editToBoundaryNextMarker = 119;
constexpr int editToBoundaryPrevClip = 120;
constexpr int editToBoundaryNextClip = 121;
constexpr int editTrimOutsideSelection = 123;
constexpr int editScopedToMasterStart = 135;
constexpr int editScopedToMasterEnd = 136;
constexpr int editScopedToPrevMarker = 137;
constexpr int editScopedToNextMarker = 138;
constexpr int editScopedToPrevClip = 139;
constexpr int editScopedToNextClip = 140;
constexpr int editRememberSelection = 147;
constexpr int editRestoreSelection = 148;
constexpr int editDuplicateSelectionAfter = 167;
constexpr int editPasteReplace = 168;
constexpr int editPasteInsertDest = 169;
constexpr int editPasteInsertAll = 170;
constexpr int editSelectMasterTrackAll = 171;
constexpr int editSelectMasterTrackScoped = 172;
constexpr int editRippleCutSelected = 174;
constexpr int editRippleCutAll = 175;
constexpr int editMoveSelection = 177;
constexpr int editFindZeroCrossingsAny = 255;
constexpr int editRippleDeleteFocused = 259;
constexpr int editRippleCutFocused = 260;
constexpr int editInsertGapFocused = 261;
constexpr int editMarkInAtPlayhead = 267;
constexpr int editMarkOutAtPlayhead = 268;
constexpr int editFindZeroCrossingsCommon = 368;
constexpr int editClearRememberedSelection = 369;
constexpr int editDeselectAll = 1025;

// Selection nudging & edit point
constexpr int selStartEarlier = 87;
constexpr int selStartLater = 88;
constexpr int selEndEarlier = 89;
constexpr int selEndLater = 90;
constexpr int selMoveEarlier = 91;
constexpr int selMoveLater = 92;
constexpr int selExtendLeft = 102;
constexpr int selExtendRight = 103;
constexpr int selExtendUp = 133;
constexpr int selExtendDown = 134;
constexpr int editPointNudgeLeft = 297;
constexpr int editPointNudgeRight = 298;
constexpr int editPointPasteOverlap = 299;
constexpr int editPointPasteReplace = 300;
constexpr int editPointPasteInsertDest = 301;
constexpr int editPointPasteInsertAll = 302;
constexpr int editPointSetAtPlayhead = 303;
constexpr int editPointAddMarker = 304;
constexpr int editPointNavMasterStart = 305;
constexpr int editPointNavMasterEnd = 306;
constexpr int editPointNavPrevMarker = 307;
constexpr int editPointNavNextMarker = 308;
constexpr int editPointNavPrevClipLayer = 309;
constexpr int editPointNavNextClipLayer = 310;
constexpr int editPointNavPrevClipAny = 311;
constexpr int editPointNavNextClipAny = 312;
constexpr int editPointSelectMasterStart = 313;
constexpr int editPointSelectMasterEnd = 314;
constexpr int editPointSelectPrevMarker = 315;
constexpr int editPointSelectNextMarker = 316;
constexpr int editPointSelectPrevClipLayer = 317;
constexpr int editPointSelectNextClipLayer = 318;
constexpr int editPointSelectPrevClipAny = 319;
constexpr int editPointSelectNextClipAny = 320;
constexpr int editPointSelectGapLayer = 326;
constexpr int editPointSelectGapIndependent = 327;
constexpr int editPointSelectGapAll = 328;

// Focused & multi-clip operations
constexpr int clipZoom = 23;
constexpr int clipGainAndFades = 49;
constexpr int clipCrossfadeTrack = 50;
constexpr int clipMoveToSample = 59;
constexpr int clipTrimToSamples = 60;
constexpr int clipNudgeLeft = 61;
constexpr int clipNudgeRight = 62;
constexpr int clipSelectTime = 86;
constexpr int clipRename = 93;
constexpr int clipJoinNext = 108;
constexpr int clipAlignStartPlayhead = 141;
constexpr int clipAlignEndPlayhead = 142;
constexpr int clipAlignStartSelStart = 143;
constexpr int clipAlignStartSelEnd = 144;
constexpr int clipAlignEndSelStart = 145;
constexpr int clipAlignEndSelEnd = 146;
constexpr int clipNudgeStartEarlier = 154;
constexpr int clipNudgeStartLater = 155;
constexpr int clipNudgeEndEarlier = 156;
constexpr int clipNudgeEndLater = 157;
constexpr int clipCrossfadeAcrossTracks = 176;
constexpr int clipSplitSelectedLayers = 186;
constexpr int clipSplitAllLayers = 187;
constexpr int selectedClipsCopy = 188;
constexpr int selectedClipsCut = 189;
constexpr int selectedClipsDelete = 190;
constexpr int selectedClipsDuplicate = 191;
constexpr int selectedClipsSplit = 192;
constexpr int selectedClipsMove = 193;
constexpr int selectedClipsClear = 194;
constexpr int selectedClipsNudgeLeft = 195;
constexpr int selectedClipsNudgeRight = 196;
constexpr int clipToggleSelection = 197;
constexpr int clipAlignStartMarker = 199;
constexpr int clipAlignEndMarker = 200;
constexpr int clipAlignStartPrevEdge = 201;
constexpr int clipAlignStartNextEdge = 202;
constexpr int clipAlignEndPrevEdge = 203;
constexpr int clipAlignEndNextEdge = 204;
constexpr int selectedClipsSequence = 205;
constexpr int selectClipsAllLayers = 206;
constexpr int selectClipsSelectedLayers = 207;
constexpr int selectClipsInTimeSel = 208;
constexpr int selectedClipsDistribute = 239;
constexpr int clipSelectVisiblePortion = 249;
constexpr int clipSelectBeforePlayhead = 250;
constexpr int clipSelectAfterPlayhead = 251;
constexpr int selectedClipsGainFades = 284;
constexpr int selectedClipsMoveStarts = 285;
constexpr int selectedClipsMoveEnds = 286;
constexpr int selectedClipsZoom = 288;
constexpr int selectedClipsTrimStartsPlayhead = 289;
constexpr int selectedClipsTrimEndsPlayhead = 290;
constexpr int selectedClipsJoin = 291;
constexpr int selectedClipsGroup = 295;
constexpr int selectedClipsUngroup = 296;
constexpr int selectedClipsCrossfadePair = 348;
constexpr int selectedClipsSelectSpan = 350;
constexpr int selectClipsFocusedSourceAll = 355;
constexpr int clipFindSilence = 356;
constexpr int selectedClipsSlip = 372;
constexpr int clipSlip = 373;
constexpr int selectClipsFocusedSourceLayers = 382;
constexpr int selectClipsFocusedSourceTime = 383;
constexpr int fadeInLinear = 390;
constexpr int fadeInEqualPower = 391;
constexpr int fadeInSCurve = 392;
constexpr int fadeInExponential = 393;
constexpr int fadeOutLinear = 394;
constexpr int fadeOutEqualPower = 395;
constexpr int fadeOutSCurve = 396;
constexpr int fadeOutExponential = 397;
constexpr int clipAmplify = 398;
constexpr int clipNormalize = 399;
constexpr int clipContextSplitAtPointer = 9000;
constexpr int clipContextOverlapBase = 10000;

// Snapping
constexpr int snapOff = 63;
constexpr int snapGrid = 64;
constexpr int snapGridAndBounds = 65;
constexpr int snapTargetClipEdges = 231;
constexpr int snapTargetMarkers = 232;
constexpr int snapTargetSelection = 233;
constexpr int snapTargetPlayhead = 234;
constexpr int snapTargetTimelineStart = 235;
constexpr int snapGridSamples1 = 236;
constexpr int snapGridSamples100Ms = 237;
constexpr int snapGridSamples1Sec = 238;
constexpr int snapGridCustom = 344;

// Transport, playback, audition & audio device
constexpr int transportPlayPause = 8;
constexpr int transportStop = 9;
constexpr int transportPlaySelFull = 40;
constexpr int transportLoopSelFull = 41;
constexpr int auditionPlayhead = 76;
constexpr int auditionSelStart = 77;
constexpr int auditionSelEnd = 78;
constexpr int auditionSetContext = 79;
constexpr int previewRippleSelected = 80;
constexpr int previewRippleAll = 81;
constexpr int transportPlaySelLayers = 94;
constexpr int transportLoopSelLayers = 95;
constexpr int transportPlayClip = 96;
constexpr int transportLoopClip = 97;
constexpr int audioSettings = 104;
constexpr int audioRescanDevices = 105;
constexpr int metersResetPeaks = 160;
constexpr int seekOneSampleEarlier = 161;
constexpr int seekOneSampleLater = 162;
constexpr int seekMasterStart = 163;
constexpr int seekMasterEnd = 164;
constexpr int transportPlaySelectedClips = 212;
constexpr int transportLoopSelectedClips = 213;
constexpr int previewRippleFocused = 262;
constexpr int previewCrossfadeSameLayer = 265;
constexpr int previewCrossfadeAcrossLayers = 266;
constexpr int savedLoopSetFromSel = 292;
constexpr int savedLoopPlay = 293;
constexpr int savedLoopClear = 294;
constexpr int auditionEditPoint = 321;
constexpr int transportStopAtPlayhead = 343;
constexpr int transportPlaySelContextFull = 345;
constexpr int transportPlaySelContextLayers = 346;
constexpr int previewCrossfadePair = 349;
constexpr int metersResetEditLockCount = 353;

// Markers & MiniDisc workspace commands
constexpr int markerAddAtPlayhead = 10;
constexpr int markerRename = 13;
constexpr int markerDelete = 14;
constexpr int markerSetPosition = 46;
constexpr int markerPrev = 47;
constexpr int markerNext = 48;
constexpr int markerTrackList = 54;
constexpr int markerImport = 68;
constexpr int markerExport = 69;
constexpr int markerList = 106;
constexpr int markerShiftAll = 132;
constexpr int markerNumberByTrack = 158;
constexpr int markerSnapToSamples = 173;
constexpr int markerShiftFromPlayhead = 209;
constexpr int markerShiftInSelection = 210;
constexpr int markerImportAudacity = 214;
constexpr int markerExportAudacity = 215;
constexpr int markerAddAtClipStarts = 240;
constexpr int markerAddSourceCues = 242;
constexpr int markerSuggestQuietGaps = 256;
constexpr int markerAddAtClipEnds = 287;
constexpr int markerNudgeLeft = 339;
constexpr int markerNudgeRight = 340;
constexpr int markerCancelExchange = 354;
constexpr int markerDeleteInSelection = 374;
constexpr int markerFocusPrev = 378;
constexpr int markerFocusNext = 379;
constexpr int markerAuditionBoundary = 380;
constexpr int mdGoToTrack = 1014;
constexpr int mdPreviewFromSelected = 1015;
constexpr int mdInsertTrackFromSelection = 1018;
constexpr int mdAppendPartFromSelection = 1019;
constexpr int mdEditTrackTitleForms = 1022;
constexpr int mdTracksFromClips = 1023;
constexpr int mdReserved1024 = 1024;

// Master & analysis
constexpr int masterStereo = 11;
constexpr int masterMono = 12;
constexpr int wavFmtPcm16 = 70;
constexpr int wavFmtPcm24 = 71;
constexpr int wavFmtFloat32 = 72;
constexpr int wavToggleDither = 73;
constexpr int analyzeMasterLevels = 180;
constexpr int analyzeSelLevelsFull = 181;
constexpr int analyzeCancel = 182;
constexpr int analyzeSelLevelsLayers = 183;
constexpr int analyzeTrackAtPlayhead = 184;
constexpr int wavToggleEmbedMarkers = 241;
constexpr int masterMinimumDuration = 258;

// View, zoom, waveform & spectrogram settings
constexpr int viewZoomIn = 15;
constexpr int viewZoomOut = 16;
constexpr int viewZoomFit = 17;
constexpr int viewModeWaveform = 21;
constexpr int viewModeSpectrogram = 22;
constexpr int navClipBoundaryPrevLayer = 82;
constexpr int navClipBoundaryNextLayer = 83;
constexpr int navClipBoundaryPrevAny = 84;
constexpr int navClipBoundaryNextAny = 85;
constexpr int viewFitLayersVertically = 111;
constexpr int viewToggleOutputPeaks = 159;
constexpr int viewSpectrumFft1024 = 216;
constexpr int viewSpectrumFft2048 = 217;
constexpr int viewSpectrumFft4096 = 218;
constexpr int viewSpectrumFft8192 = 219;
constexpr int viewSpectrumBandFull = 220;
constexpr int viewSpectrumBandBass = 221;
constexpr int viewSpectrumBandVoice = 222;
constexpr int viewSpectrumBandTreble = 223;
constexpr int viewTogglePeakWarnings = 224;
constexpr int viewWaveZoom1x = 225;
constexpr int viewWaveZoom2x = 226;
constexpr int viewWaveZoom4x = 227;
constexpr int viewWaveZoom8x = 228;
constexpr int viewWaveScaleLinear = 229;
constexpr int viewWaveScaleDb = 230;
constexpr int viewTogglePrevZoom = 243;
constexpr int viewTimeFormatAuto = 244;
constexpr int viewTimeFormatSamples = 245;
constexpr int viewTimeFormatClock = 246;
constexpr int viewTimeFormatSeconds = 247;
constexpr int viewBrowseOverlaps = 252;
constexpr int viewFollowOff = 329;
constexpr int viewFollowPage = 330;
constexpr int viewFollowPinned = 331;
constexpr int viewZoomMasterTrackAtPlayhead = 332;
constexpr int viewSpectrumCustomRange = 341;
constexpr int viewResumeFollowing = 342;
constexpr int viewToggleRms = 358;
constexpr int viewSpectrumWindowHann = 359;
constexpr int viewSpectrumWindowHamming = 360;
constexpr int viewSpectrumWindowBlackman = 361;
constexpr int viewSpectrumFloorMinus40 = 362;
constexpr int viewSpectrumFloorMinus60 = 363;
constexpr int viewSpectrumFloorMinus80 = 364;
constexpr int viewSpectrumFloorMinus100 = 365;
constexpr int viewSpectrumFloorMinus120 = 366;
constexpr int viewToggleHalfWave = 381;
constexpr int viewToggleMdWorkspace = 389;

// Layer management, routing, render & multi-layer selection
constexpr int layerMute = 30;
constexpr int layerSolo = 31;
constexpr int layerGainBalance = 32;
constexpr int layerAddEmpty = 33;
constexpr int layerRename = 34;
constexpr int layerDuplicate = 35;
constexpr int layerRemove = 36;
constexpr int layerMoveUp = 37;
constexpr int layerMoveDown = 38;
constexpr int layerChannelRouting = 56;
constexpr int layerClearRouting = 57;
constexpr int layerSelectPrev = 98;
constexpr int layerSelectNext = 99;
constexpr int layerSelectPrevClip = 100;
constexpr int layerSelectNextClip = 101;
constexpr int layerSetHeight = 112;
constexpr int layerSourceInfo = 114;
constexpr int layerViewDefault = 125;
constexpr int layerViewWaveform = 126;
constexpr int layerViewSpectrogram = 127;
constexpr int layerToggleCollapse = 129;
constexpr int layerCollapseAll = 130;
constexpr int layerExpandAll = 131;
constexpr int layerRowsMute = 150;
constexpr int layerRowsUnmute = 151;
constexpr int layerRowsSolo = 152;
constexpr int layerRowsClearSolo = 153;
constexpr int layerVerifySources = 165;
constexpr int layerCancelSourceCheck = 166;
constexpr int layerSwapStereo = 178;
constexpr int layerFoldStereoToMono = 179;
constexpr int layerBrowseNavigator = 185;
constexpr int layerViewCombined = 248;
constexpr int layerSplitStereo = 253;
constexpr int layerCombineChannel = 254;
constexpr int layerVerifyAllSources = 257;
constexpr int layerEditMixList = 269;
constexpr int layerToggleIndependent = 270;
constexpr int layerSelectAllIndependent = 271;
constexpr int layerClearIndependent = 272;
constexpr int layerIndMute = 273;
constexpr int layerIndUnmute = 274;
constexpr int layerIndSolo = 275;
constexpr int layerIndClearSolo = 276;
constexpr int layerIndCopyTime = 277;
constexpr int layerIndCutGap = 278;
constexpr int layerIndCutClose = 279;
constexpr int layerIndDeleteGap = 280;
constexpr int layerIndDeleteClose = 281;
constexpr int layerIndInsertGap = 282;
constexpr int layerIndPreviewClose = 283;
constexpr int layerRenderSelectedTime = 322;
constexpr int layerRenderSelectedTracks = 323;
constexpr int layerRenderSelectedClips = 324;
constexpr int layerConsolidateClips = 325;
constexpr int layerWaveZoomDefault = 333;
constexpr int layerWaveZoom1x = 334;
constexpr int layerWaveZoom2x = 335;
constexpr int layerWaveZoom4x = 336;
constexpr int layerWaveZoom8x = 337;
constexpr int layerChannelHeights = 338;
constexpr int layerRowsScaleGain = 347;
constexpr int layerHeightDecrease = 351;
constexpr int layerHeightIncrease = 352;
constexpr int layerRenderStereoPair = 357;
constexpr int layerRowsSetGain = 370;
constexpr int layerRowsSetBalance = 375;
constexpr int layerRowsSetGainDb = 376;
constexpr int layerRowsAdjustGainDb = 377;
constexpr int layerFocusPrevClip = 384;
constexpr int layerFocusNextClip = 385;
constexpr int layerFocusPrev = 386;
constexpr int layerFocusNext = 387;
constexpr int layerConsolidateAllowRouting = 388;

// Help & dynamic history
constexpr int helpKeyboardShortcuts = 124;
constexpr int helpAnnounceTimeline = 149;
constexpr int helpFocusTimeline = 367;
constexpr int helpSearchCommands = 371;
constexpr int historyBase = 1000000;
} // namespace MenuId

/**
 * @brief Returns true if dispatching @p id handles or preserves special audition
 *        playback rather than immediately halting it.
 */
bool preservesSpecialAuditionOnMenuCommand(int id) noexcept {
    switch (id) {
    case MenuId::mdInsertTrackFromSelection:
    case MenuId::mdAppendPartFromSelection:
    case MenuId::editDeselectAll:
    case MenuId::mdPreviewFromSelected:
    case MenuId::mdGoToTrack:
    case MenuId::mdEditTrackTitleForms:
    case MenuId::mdTracksFromClips:
    case MenuId::mdReserved1024:
    case MenuId::viewToggleMdWorkspace:
    case MenuId::editDuplicateClip:
    case MenuId::editCutSelection:
    case MenuId::editCopySelection:
    case MenuId::editPasteOverlap:
    case MenuId::editDeleteSelection:
    case MenuId::editRippleDeleteSelected:
    case MenuId::editRippleDeleteAll:
    case MenuId::editInsertGapSelected:
    case MenuId::editInsertGapAll:
    case MenuId::editTrimOutsideSelection:
    case MenuId::editDuplicateSelectionAfter:
    case MenuId::editPasteReplace:
    case MenuId::editPasteInsertDest:
    case MenuId::editPasteInsertAll:
    case MenuId::editRippleDeleteFocused:
    case MenuId::editRippleCutFocused:
    case MenuId::editInsertGapFocused:
    case MenuId::layerIndCopyTime:
    case MenuId::layerIndCutGap:
    case MenuId::layerIndCutClose:
    case MenuId::layerIndDeleteGap:
    case MenuId::layerIndDeleteClose:
    case MenuId::layerIndInsertGap:
    case MenuId::editPointPasteOverlap:
    case MenuId::editPointPasteReplace:
    case MenuId::editPointPasteInsertDest:
    case MenuId::editPointPasteInsertAll:
    case MenuId::editSplit:
    case MenuId::clipSplitSelectedLayers:
    case MenuId::clipSplitAllLayers:
    case MenuId::selectedClipsCopy:
    case MenuId::selectedClipsCut:
    case MenuId::selectedClipsDelete:
    case MenuId::selectedClipsDuplicate:
    case MenuId::selectedClipsSplit:
    case MenuId::selectedClipsMove:
    case MenuId::selectedClipsJoin:
    case MenuId::clipAlignStartPlayhead:
    case MenuId::clipAlignEndPlayhead:
    case MenuId::clipAlignStartSelStart:
    case MenuId::clipAlignStartSelEnd:
    case MenuId::clipAlignEndSelStart:
    case MenuId::clipAlignEndSelEnd:
    case MenuId::clipAlignStartMarker:
    case MenuId::clipAlignEndMarker:
    case MenuId::clipAlignStartPrevEdge:
    case MenuId::clipAlignStartNextEdge:
    case MenuId::clipAlignEndPrevEdge:
    case MenuId::clipAlignEndNextEdge:
    case MenuId::clipNudgeStartEarlier:
    case MenuId::clipNudgeStartLater:
    case MenuId::clipNudgeEndEarlier:
    case MenuId::clipNudgeEndLater:
    case MenuId::selectedClipsTrimStartsPlayhead:
    case MenuId::selectedClipsTrimEndsPlayhead:
    case MenuId::editDeleteClip:
    case MenuId::markerDelete:
    case MenuId::editSplitSelToNewLayers:
    case MenuId::clipJoinNext:
    case MenuId::markerNumberByTrack:
    case MenuId::markerSnapToSamples:
    case MenuId::layerSwapStereo:
    case MenuId::layerFoldStereoToMono:
    case MenuId::layerSplitStereo:
    case MenuId::layerCombineChannel:
    case MenuId::markerDeleteInSelection:
    case MenuId::transportPlayPause:
    case MenuId::transportStop:
    case MenuId::markerSetPosition:
    case MenuId::markerPrev:
    case MenuId::markerNext:
    case MenuId::markerFocusPrev:
    case MenuId::markerFocusNext:
    case MenuId::markerAuditionBoundary:
    case MenuId::layerFocusPrevClip:
    case MenuId::layerFocusNextClip:
    case MenuId::layerFocusPrev:
    case MenuId::layerFocusNext:
    case MenuId::auditionPlayhead:
    case MenuId::auditionSelStart:
    case MenuId::auditionSelEnd:
    case MenuId::auditionSetContext:
    case MenuId::previewRippleSelected:
    case MenuId::previewRippleAll:
    case MenuId::auditionEditPoint:
    case MenuId::navClipBoundaryPrevLayer:
    case MenuId::navClipBoundaryNextLayer:
    case MenuId::navClipBoundaryPrevAny:
    case MenuId::navClipBoundaryNextAny:
    case MenuId::seekOneSampleEarlier:
    case MenuId::seekOneSampleLater:
    case MenuId::seekMasterStart:
    case MenuId::seekMasterEnd:
    case MenuId::previewRippleFocused:
    case MenuId::layerIndPreviewClose:
    case MenuId::viewToggleOutputPeaks:
    case MenuId::metersResetPeaks:
    case MenuId::previewCrossfadeSameLayer:
    case MenuId::previewCrossfadeAcrossLayers:
    case MenuId::editMarkInAtPlayhead:
    case MenuId::editMarkOutAtPlayhead:
    case MenuId::previewCrossfadePair:
    case MenuId::viewFollowOff:
    case MenuId::viewFollowPage:
    case MenuId::viewFollowPinned:
    case MenuId::selectClipsAllLayers:
    case MenuId::selectClipsSelectedLayers:
    case MenuId::selectClipsInTimeSel:
    case MenuId::viewResumeFollowing:
    case MenuId::transportStopAtPlayhead:
    case MenuId::metersResetEditLockCount:
    case MenuId::markerCancelExchange:
    case MenuId::selectClipsFocusedSourceAll:
    case MenuId::selectClipsFocusedSourceLayers:
    case MenuId::selectClipsFocusedSourceTime:
    case MenuId::layerConsolidateAllowRouting:
    case MenuId::viewToggleRms:
    case MenuId::helpFocusTimeline:
    case MenuId::editRememberSelection:
    case MenuId::editRestoreSelection:
    case MenuId::editClearRememberedSelection:
    case MenuId::helpSearchCommands:
    case MenuId::fadeInLinear:
    case MenuId::fadeInEqualPower:
    case MenuId::fadeInSCurve:
    case MenuId::fadeInExponential:
    case MenuId::fadeOutLinear:
    case MenuId::fadeOutEqualPower:
    case MenuId::fadeOutSCurve:
    case MenuId::fadeOutExponential:
    case MenuId::clipAmplify:
    case MenuId::clipNormalize:
        return true;
    default:
        return false;
    }
}

} // namespace

// ============================================================================
// Context Menus (Marker, Layer, Selection, Clip)
// ============================================================================

bool Studio::showMarkerContextMenu(const juce::MouseEvent& event) {
    const auto area = timeline();
    if (!area.contains(event.getPosition()) || event.x < area.getX() + labelWidth ||
        event.y >= area.getY() + kTimelineRulerHeight) {
        return false;
    }

    const int markerIndex = markerHitAt(event.x, false, selectedMarker);
    if (markerIndex < 0) {
        return false;
    }

    const bool keepPlayback = selectionPlaybackContinues();
    selectedMarker = markerIndex;
    if (!keepPlayback) {
        position = session.markers[static_cast<size_t>(markerIndex)].position;
        auditionActive = false;
    }
    repaint();

    juce::PopupMenu menu;
    menu.addItem(MenuId::markerRename, "Rename marker (F2)...");
    menu.addItem(MenuId::markerSetPosition, "Set marker position (P)...");
    menu.addItem(MenuId::markerAuditionBoundary, "Audition marker boundary (B)");
    menu.addItem(MenuId::markerNudgeLeft, "Move left one sample (F9)");
    menu.addItem(MenuId::markerNudgeRight, "Move right one sample (F10)");
    menu.addItem(MenuId::markerDelete, "Delete marker (Delete)");
    menu.addSeparator();
    menu.addItem(MenuId::markerList, "Marker list and repair...");

    const auto revision = editGeneration;
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(
            {event.getScreenX(), event.getScreenY(), 1, 1}),
        [safe = juce::Component::SafePointer<Studio>(this), markerIndex, revision](int result) {
            if (safe == nullptr || result == 0) {
                return;
            }
            const juce::ScopedLock guard(safe->lock);
            if (safe->editGeneration != revision || safe->selectedMarker != markerIndex ||
                markerIndex >= static_cast<int>(safe->session.markers.size())) {
                safe->status.setText("The marker changed; open its menu again.",
                                     juce::dontSendNotification);
                return;
            }
            {
                const juce::ScopedUnlock unlock(safe->lock);
                safe->menuItemSelected(result, MenuBarIndex::edit);
            }
        });
    return true;
}

void Studio::addTrackWaveformZoomMenu(juce::PopupMenu& parent, int trackIndex) const {
    const bool valid = trackIndex >= 0 && trackIndex < static_cast<int>(session.tracks.size());
    const int current = valid ? session.tracks[static_cast<size_t>(trackIndex)].waveformZoom : 0;
    juce::PopupMenu zoomMenu;
    zoomMenu.addItem(MenuId::layerWaveZoomDefault, "Follow default", valid, current == 0);
    zoomMenu.addItem(MenuId::layerWaveZoom1x, utf8Str("1×"), valid, current == 1);
    zoomMenu.addItem(MenuId::layerWaveZoom2x, utf8Str("2×"), valid, current == 2);
    zoomMenu.addItem(MenuId::layerWaveZoom4x, utf8Str("4×"), valid, current == 4);
    zoomMenu.addItem(MenuId::layerWaveZoom8x, utf8Str("8×"), valid, current == 8);
    parent.addSubMenu("Waveform vertical zoom", zoomMenu, valid);
}

bool Studio::showTrackContextMenu(const juce::MouseEvent& event) {
    const auto area = timeline();
    if (!area.contains(event.getPosition()) || event.y < area.getY() + timelineHeaderHeight() ||
        event.x >= area.getX() + trackLabelWidth) {
        return false;
    }

    const int track = trackAt(event.y);
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        return false;
    }

    selectedTrack = track;
    selectedClip = -1;
    selectedMarker = -1;
    repaint();

    const auto& state = session.tracks[static_cast<size_t>(track)];
    juce::PopupMenu menu;
    menu.addItem(MenuId::layerMute, "Mute layer", true, state.muted);
    menu.addItem(MenuId::layerSolo, "Solo layer", true, state.solo);
    menu.addItem(MenuId::layerToggleCollapse, state.collapsed ? "Expand layer" : "Collapse layer");
    menu.addItem(MenuId::layerSetHeight, "Set layer height...");
    menu.addItem(MenuId::layerChannelHeights, "Channel heights...",
                 carta::maxTrackChannels(state) >= 2 || !state.channelHeightWeights.empty());

    juce::PopupMenu display;
    display.addItem(MenuId::layerViewDefault, "Use default view", true,
                    state.view == carta::TrackView::useDefault);
    display.addItem(MenuId::layerViewWaveform, "Waveform", true,
                    state.view == carta::TrackView::waveform);
    display.addItem(MenuId::layerViewSpectrogram, "Spectrogram", true,
                    state.view == carta::TrackView::spectrogram);
    display.addItem(MenuId::layerViewCombined, "Waveform + spectrogram", true,
                    state.view == carta::TrackView::combined);
    menu.addSubMenu("Layer view", display);
    addTrackWaveformZoomMenu(menu, track);

    menu.addSeparator();
    menu.addItem(MenuId::layerSourceInfo, "Source information...");
    menu.addItem(MenuId::layerGainBalance, "Gain and balance...");
    menu.addItem(MenuId::layerVerifySources, "Verify layer source files", !activeSourceCheck);
    menu.addItem(MenuId::layerChannelRouting, "Channel routing...",
                 carta::maxTrackChannels(state) > 0);
    menu.addItem(MenuId::layerSwapStereo, "Swap stereo source channels",
                 carta::maxTrackChannels(state) > 0);
    menu.addItem(MenuId::layerFoldStereoToMono, "Fold stereo routing to mono",
                 carta::maxTrackChannels(state) > 0);
    menu.addItem(MenuId::layerSplitStereo, "Split stereo channels to layers",
                 carta::maxTrackChannels(state) > 0);
    menu.addItem(MenuId::layerCombineChannel, "Combine with next channel layer",
                 track + 1 < static_cast<int>(session.tracks.size()));

    menu.addSeparator();
    menu.addItem(MenuId::layerRename, "Rename layer...");
    menu.addItem(MenuId::layerDuplicate, "Duplicate layer");
    menu.addItem(MenuId::layerRemove, "Remove layer");
    menu.addItem(MenuId::layerMoveUp, "Move layer up", track > 0);
    menu.addItem(MenuId::layerMoveDown, "Move layer down",
                 track + 1 < static_cast<int>(session.tracks.size()));

    const auto revision = editGeneration;
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(
            {event.getScreenX(), event.getScreenY(), 1, 1}),
        [safe = juce::Component::SafePointer<Studio>(this), track, revision](int result) {
            if (safe == nullptr || result == 0) {
                return;
            }
            const juce::ScopedLock guard(safe->lock);
            if (safe->editGeneration != revision || safe->selectedTrack != track ||
                track >= static_cast<int>(safe->session.tracks.size())) {
                safe->status.setText("The layer changed; open its menu again.",
                                     juce::dontSendNotification);
                return;
            }
            {
                const juce::ScopedUnlock unlock(safe->lock);
                safe->menuItemSelected(result, MenuBarIndex::edit);
            }
        });
    return true;
}

bool Studio::showSelectionContextMenu(const juce::MouseEvent& event) {
    const auto area = timeline();
    if (!timeSelection.hasRange() ||
        timeSelection.lastTrack >= static_cast<int>(session.tracks.size()) ||
        !area.contains(event.getPosition()) || event.x < area.getX() + labelWidth) {
        return false;
    }

    const int track = trackAt(event.y);
    if (!trackInTimeScope(track)) {
        return false;
    }

    const auto row = trackArea(track);
    if (event.y < row.getY() + 4 + titleHeight || event.y >= row.getBottom() - 4) {
        return false;
    }

    const int left = timeToX(static_cast<double>(timeSelection.start) / carta::sessionRate);
    const int right =
        std::max(left + 1, timeToX(static_cast<double>(timeSelection.end) / carta::sessionRate));
    if (event.x < left || event.x >= right) {
        return false;
    }

    const auto captured = timeSelection;
    const auto revision = editGeneration;
    const int capturedTrack = selectedTrack;
    const auto capturedTracks = selectedTimelineTracks();
    const bool hasFocusedTrack =
        capturedTrack >= 0 && capturedTrack < static_cast<int>(session.tracks.size());

    juce::PopupMenu menu;
    menu.addItem(MenuId::editCopySelection, "Copy selection");
    menu.addItem(MenuId::editCutSelection, "Cut selection (leave gap)");
    menu.addItem(MenuId::editDeleteSelection, "Delete selection (leave gap)");

    juce::PopupMenu deleteRipple;
    deleteRipple.addItem(MenuId::editRippleDeleteFocused, "Focused layer", hasFocusedTrack);
    deleteRipple.addItem(MenuId::editRippleDeleteSelected, "Selected layers");
    deleteRipple.addItem(MenuId::editRippleDeleteAll, "All layers and markers");
    menu.addSubMenu("Delete and close gap", deleteRipple);

    juce::PopupMenu cutRipple;
    cutRipple.addItem(MenuId::editRippleCutFocused, "Focused layer", hasFocusedTrack);
    cutRipple.addItem(MenuId::editRippleCutSelected, "Selected layers");
    cutRipple.addItem(MenuId::editRippleCutAll, "All layers and markers");
    menu.addSubMenu("Cut and close gap", cutRipple);

    menu.addItem(MenuId::editDuplicateSelectionAfter, "Duplicate selection after end (overlap)");
    menu.addItem(MenuId::editMoveSelection, "Move selection to time and layer...");
    menu.addItem(MenuId::editTrimOutsideSelection, "Trim outside selection (selected layers)");
    menu.addSeparator();
    menu.addItem(MenuId::editSplit, "Split at selection boundaries");
    menu.addItem(MenuId::editSplitSelToNewLayers, "Split selection to new layers");

    juce::PopupMenu insertRipple;
    insertRipple.addItem(MenuId::editInsertGapFocused, "Focused layer", hasFocusedTrack);
    insertRipple.addItem(MenuId::editInsertGapSelected, "Selected layers");
    insertRipple.addItem(MenuId::editInsertGapAll, "All layers and markers");
    menu.addSubMenu("Insert selection-length gap", insertRipple);

    menu.addSeparator();
    menu.addItem(MenuId::transportPlaySelFull, "Play selection (full mix)");
    menu.addItem(MenuId::transportLoopSelFull, "Loop selection (full mix)");
    menu.addItem(MenuId::transportPlaySelLayers, "Play selection (selected layers)");
    menu.addItem(MenuId::transportLoopSelLayers, "Loop selection (selected layers)");

    juce::PopupMenu previewRipple;
    previewRipple.addItem(MenuId::previewRippleFocused, "Focused layer", hasFocusedTrack);
    previewRipple.addItem(MenuId::previewRippleSelected, "Selected layers");
    previewRipple.addItem(MenuId::previewRippleAll, "All layers and markers");
    menu.addSubMenu("Preview closing selection", previewRipple);

    menu.addSeparator();
    menu.addItem(MenuId::fileExportSelWavFull, "Export selection WAV (full mix)");
    menu.addItem(MenuId::fileExportSelWavLayers, "Export selection WAV (selected layers)");
    menu.addItem(MenuId::editZoomToSelection, "Zoom to selection");
    menu.addItem(MenuId::editRememberSelection, "Remember selection");

    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(
            {event.getScreenX(), event.getScreenY(), 1, 1}),
        [safe = juce::Component::SafePointer<Studio>(this), captured, revision, capturedTrack,
         capturedTracks](int result) {
            if (safe == nullptr || result == 0) {
                return;
            }
            const juce::ScopedLock guard(safe->lock);
            const auto& current = safe->timeSelection;
            if (safe->editGeneration != revision ||
                safe->selectedTimelineTracks() != capturedTracks ||
                current.start != captured.start || current.end != captured.end ||
                current.firstTrack != captured.firstTrack ||
                current.lastTrack != captured.lastTrack) {
                safe->status.setText("The selection changed; open its menu again.",
                                     juce::dontSendNotification);
                return;
            }
            if ((result == MenuId::editRippleDeleteFocused ||
                 result == MenuId::editRippleCutFocused || result == MenuId::editInsertGapFocused ||
                 result == MenuId::previewRippleFocused) &&
                safe->selectedTrack != capturedTrack) {
                safe->status.setText("The focused track changed; open the selection menu again.",
                                     juce::dontSendNotification);
                return;
            }
            {
                const juce::ScopedUnlock unlock(safe->lock);
                safe->menuItemSelected(result, MenuBarIndex::edit);
            }
        });
    return true;
}

juce::PopupMenu Studio::selectedClipContextMenu() const {
    juce::PopupMenu group;
    const auto count = timelineClipSelection.get(editGeneration).size();
    const bool selected = count > 0;
    group.addItem(MenuId::selectedClipsCopy, "Copy clips", selected);
    group.addItem(MenuId::selectedClipsCut, "Cut clips", selected);
    group.addItem(MenuId::selectedClipsDelete, "Delete clips", selected);
    group.addItem(MenuId::selectedClipsDuplicate, "Duplicate clips after span", selected);
    group.addItem(MenuId::selectedClipsSplit, "Split clips at playhead", selected);
    group.addItem(MenuId::selectedClipsMove, "Move clips...", selected);
    group.addItem(MenuId::selectedClipsNudgeLeft, "Nudge clips left one sample", selected);
    group.addItem(MenuId::selectedClipsNudgeRight, "Nudge clips right one sample", selected);
    group.addSeparator();
    group.addItem(MenuId::selectedClipsGroup, "Group selected clips", count >= 2);
    group.addItem(MenuId::selectedClipsUngroup, "Ungroup selected clips", selected);
    group.addItem(MenuId::selectedClipsJoin, "Join compatible clips", count >= 2);
    group.addItem(MenuId::selectedClipsGainFades, "Set gain and fades...", selected);
    addFadeShapeMenus(group, selected);
    group.addItem(MenuId::selectedClipsCrossfadePair, "Linear crossfade selected pair", count == 2);
    group.addItem(MenuId::previewCrossfadePair, "Preview crossfade selected pair", count == 2);
    group.addSeparator();
    group.addItem(MenuId::selectedClipsSelectSpan, "Select time spanned by clips", selected);
    group.addItem(MenuId::selectedClipsZoom, "Zoom to clips", selected);
    group.addItem(MenuId::markerAddAtClipStarts, "Add markers at clip starts", selected);
    group.addItem(MenuId::markerAddAtClipEnds, "Add markers at clip ends", selected);
    group.addSeparator();
    group.addItem(MenuId::transportPlaySelectedClips, "Play selected clips", selected);
    group.addItem(MenuId::transportLoopSelectedClips, "Loop selected clips", selected);
    group.addItem(MenuId::fileExportSelectedClipsWav, "Export selected clips WAV...", selected);
    group.addItem(MenuId::layerRenderSelectedClips, "Render selected clips to new track...",
                  selected && !activeExport && pendingImports.empty());
    group.addItem(MenuId::layerConsolidateClips, "Consolidate clips on one track...",
                  selected && !activeExport && pendingImports.empty());
    group.addItem(MenuId::layerConsolidateAllowRouting, "Consolidate allowing routing changes...",
                  selected && !activeExport && pendingImports.empty());
    group.addSeparator();
    group.addItem(MenuId::selectedClipsClear, "Clear clip selection", selected);
    return group;
}

bool Studio::performContextClipEdit(int track, int clip, juce::uint64 revision, bool splitting,
                                    juce::int64 sample) {
    if (editGeneration != revision || selectedTrack != track || selectedClip != clip ||
        !validSelection()) {
        status.setText("The clip changed; open its menu again.", juce::dontSendNotification);
        return false;
    }
    const bool grouped = ensureFocusedPersistentGroup();
    if (editGeneration != revision || selectedTrack != track || selectedClip != clip ||
        !validSelection()) {
        status.setText("The clip changed; open its menu again.", juce::dontSendNotification);
        return false;
    }
    if (!grouped && session.tracks[static_cast<size_t>(track)]
                        .clips[static_cast<size_t>(clip)]
                        .groupId.isNotEmpty()) {
        status.setText("The clip selection changed; open its menu again.",
                       juce::dontSendNotification);
        return false;
    }
    if (grouped) {
        return performTimelineClipAction(
            splitting ? MenuId::selectedClipsSplit : MenuId::selectedClipsDelete, sample);
    }

    juce::String error;
    const bool changed = editFocusedClip(splitting ? 0 : 1, splitting ? sample : 0, error);
    status.setText(changed
                       ? (splitting ? "Split clip at sample " + juce::String(sample) +
                                          "; playhead and time selection stayed in place."
                                    : juce::String("Deleted clip; time selection stayed in place."))
                       : error,
                   juce::dontSendNotification);
    return changed;
}

void Studio::showClipContextMenu(const juce::MouseEvent& event) {
    const auto initialRevision = editGeneration;
#if CARTA_STUDIO_COMPONENT_TEST
    if (clipContextPreparationHook) {
        clipContextPreparationHook();
    }
#endif
    if (initialRevision != editGeneration || dragMode != kDragNone) {
        status.setText("The project changed; open the clip menu again.",
                       juce::dontSendNotification);
        return;
    }
    if (event.y < timeline().getY() + timelineHeaderHeight()) {
        return;
    }
    const int track = trackAt(event.y);
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        return;
    }

    const auto& clips = session.tracks[static_cast<size_t>(track)].clips;
    const auto area = timeline();
    const auto row = trackArea(track);
    const double span = viewLength();
    const auto hits = clipHitsAt(clips, event.getPosition(), row, area, span);
    if (hits.empty()) {
        return;
    }

    int chosen = hits.front();
    bool titleHit = false;
    if (event.y < row.getY() + 4 + titleHeight) {
        for (const auto& region : titleRegions(track, row, area, span)) {
            if (region.bounds.contains(event.x)) {
                chosen = region.clip;
                titleHit = true;
                break;
            }
        }
    }
    if (!titleHit && selectedTrack == track &&
        std::find(hits.begin(), hits.end(), selectedClip) != hits.end()) {
        chosen = selectedClip;
    }
    if (!timelineClipSelection.contains(track, chosen, editGeneration)) {
        timelineClipSelection.clear();
    }
    selectedTrack = track;
    selectedClip = chosen;
    selectedMarker = -1;
    dragMode = kDragNone;
    dragged = false;

    const bool hasPersistentGroup = clips[static_cast<size_t>(chosen)].groupId.isNotEmpty();
    const bool groupReady = ensureFocusedPersistentGroup();
    if (initialRevision != editGeneration || selectedTrack != track || selectedClip != chosen ||
        dragMode != kDragNone || (hasPersistentGroup && !groupReady)) {
        status.setText("The clip selection changed; open its menu again.",
                       juce::dontSendNotification);
        return;
    }

    status.setText("Selected " + carta::clipDisplayName(clips[static_cast<size_t>(chosen)]) + ".",
                   juce::dontSendNotification);
    repaint();

    juce::PopupMenu menu;
    if (hits.size() > 1) {
        juce::PopupMenu overlapping;
        for (size_t i = 0; i < hits.size(); ++i) {
            const auto& clip = clips[static_cast<size_t>(hits[i])];
            juce::int64 start = 0;
            juce::int64 end = 0;
            const auto range = carta::clipSampleRange(clip, start, end)
                                   ? "  [" + juce::String(start) + ", " + juce::String(end) + ")"
                                   : juce::String();
            overlapping.addItem(MenuId::clipContextOverlapBase + static_cast<int>(i),
                                juce::String(hits[i] + 1) + ". " + carta::clipDisplayName(clip) +
                                    range,
                                true, hits[i] == chosen);
        }
        menu.addSubMenu("Select overlapping clip", overlapping);
        menu.addSeparator();
    }

    menu.addItem(MenuId::clipRename, "Rename clip...");
    menu.addItem(MenuId::clipSelectTime, "Select clip time");
    juce::PopupMenu clipTime;
    clipTime.addItem(MenuId::clipSelectVisiblePortion, "Visible portion");
    clipTime.addItem(MenuId::clipSelectBeforePlayhead, "Before playhead");
    clipTime.addItem(MenuId::clipSelectAfterPlayhead, "After playhead");
    menu.addSubMenu("Select clip portion", clipTime);
    menu.addItem(MenuId::clipZoom, "Zoom to clip");
    if (!timelineClipSelection.get(editGeneration).empty()) {
        menu.addSubMenu("Selected clips", selectedClipContextMenu());
    }

    const double pointerFrame = xToTime(event.x) * carta::sessionRate;
    const auto pointerSample =
        std::isfinite(pointerFrame) && pointerFrame >= 0 && pointerFrame < 9007199254740992.0
            ? static_cast<juce::int64>(std::llround(pointerFrame))
            : -1;
    const auto& chosenClip = clips[static_cast<size_t>(chosen)];
    const bool persistentGroup = chosenClip.groupId.isNotEmpty();
    bool canSplit = carta::canSplitClipAtSample(chosenClip, pointerSample);
    if (persistentGroup && !canSplit) {
        for (const auto& ref : timelineClipSelection.get(editGeneration)) {
            if (carta::canSplitClipAtSample(session.tracks[static_cast<size_t>(ref.track)]
                                                .clips[static_cast<size_t>(ref.clip)],
                                            pointerSample)) {
                canSplit = true;
                break;
            }
        }
    }

    menu.addSeparator();
    menu.addItem(MenuId::clipContextSplitAtPointer,
                 juce::String(persistentGroup ? "Split group at pointer (sample "
                                              : "Split at pointer (sample ") +
                     juce::String(pointerSample) + ")",
                 canSplit);
    menu.addItem(MenuId::editSplit,
                 persistentGroup ? "Split group at playhead" : "Split at playhead");
    menu.addItem(MenuId::editDuplicateClip, persistentGroup ? "Duplicate group" : "Duplicate clip");
    menu.addItem(MenuId::clipJoinNext, "Join with next clip");
    menu.addItem(MenuId::clipMoveToSample,
                 persistentGroup ? "Move group using clip start..." : "Move to sample...");
    menu.addItem(MenuId::clipTrimToSamples,
                 persistentGroup ? "Trim group using clip range..." : "Trim to sample range...");
    menu.addItem(MenuId::clipSlip,
                 persistentGroup ? "Slip group source audio..." : "Slip source audio...");

    juce::PopupMenu trimEdge;
    trimEdge.addItem(MenuId::clipNudgeStartEarlier, "Start earlier one sample (F5)");
    trimEdge.addItem(MenuId::clipNudgeStartLater, "Start later one sample (F6)");
    trimEdge.addItem(MenuId::clipNudgeEndEarlier, "End earlier one sample (F7)");
    trimEdge.addItem(MenuId::clipNudgeEndLater, "End later one sample (F8)");
    menu.addSubMenu(timelineClipSelection.get(editGeneration).empty()
                        ? "Adjust clip edge"
                        : "Adjust selected clip edges",
                    trimEdge);

    menu.addSeparator();
    menu.addItem(MenuId::clipGainAndFades, "Gain and fades...");
    addFadeShapeMenus(menu, true);
    menu.addItem(MenuId::clipCrossfadeTrack, "Crossfade overlap on this track");
    menu.addItem(MenuId::clipCrossfadeAcrossTracks, "Crossfade overlap across tracks");
    menu.addSeparator();
    menu.addItem(MenuId::transportPlayClip, "Play clip");
    menu.addItem(MenuId::transportLoopClip, "Loop clip");
    menu.addSeparator();
    menu.addItem(MenuId::editDeleteClip, persistentGroup ? "Delete group" : "Delete clip");

    const auto revision = editGeneration;
    const auto capturedClips = timelineClipSelection.get(editGeneration);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(
            {event.getScreenX(), event.getScreenY(), 1, 1}),
        [safe = juce::Component::SafePointer<Studio>(this), track, chosen, revision, hits,
         pointerSample, capturedClips](int result) {
            if (safe == nullptr || result == 0) {
                return;
            }
            const juce::ScopedLock guard(safe->lock);
            if (safe->editGeneration != revision || safe->selectedTrack != track ||
                safe->selectedClip != chosen || !safe->validSelection()) {
                safe->status.setText("The clip changed; open its menu again.",
                                     juce::dontSendNotification);
                return;
            }
            if (result >= MenuId::clipContextOverlapBase &&
                static_cast<size_t>(result - MenuId::clipContextOverlapBase) < hits.size()) {
                safe->selectedClip =
                    hits[static_cast<size_t>(result - MenuId::clipContextOverlapBase)];
                const auto& clip = safe->session.tracks[static_cast<size_t>(track)]
                                       .clips[static_cast<size_t>(safe->selectedClip)];
                safe->status.setText("Selected " + carta::clipDisplayName(clip) +
                                         ". Open the clip menu for editing commands.",
                                     juce::dontSendNotification);
                safe->menuItemsChanged();
                safe->repaint();
                return;
            }

            const auto& current = safe->timelineClipSelection.get(revision);
            if (current.size() != capturedClips.size() ||
                !std::equal(current.begin(), current.end(), capturedClips.begin(),
                            [](const auto& a, const auto& b) {
                                return a.track == b.track && a.clip == b.clip;
                            })) {
                safe->status.setText("The clip selection changed; open its menu again.",
                                     juce::dontSendNotification);
                return;
            }

            if (result == MenuId::clipContextSplitAtPointer || result == MenuId::editSplit) {
                const double frame = result == MenuId::clipContextSplitAtPointer
                                         ? static_cast<double>(pointerSample)
                                         : safe->position * carta::sessionRate;
                if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
                    safe->status.setText("Invalid split position.", juce::dontSendNotification);
                    return;
                }
                const auto sample = static_cast<juce::int64>(std::llround(frame));
                const juce::ScopedUnlock unlock(safe->lock);
                safe->performContextClipEdit(track, chosen, revision, true, sample);
                return;
            }
            if (result == MenuId::editDeleteClip) {
                const juce::ScopedUnlock unlock(safe->lock);
                safe->performContextClipEdit(track, chosen, revision, false);
                return;
            }
            {
                const juce::ScopedUnlock unlock(safe->lock);
                safe->menuItemSelected(result, MenuBarIndex::edit);
            }
        });
}

// ============================================================================
// Menu Bar Model & ApplicationCommandTarget Integration
// ============================================================================

juce::StringArray Studio::getMenuBarNames() {
    return {"File", "Edit", "Transport", "Markers", "Master", "View", "Layer", "Help"};
}

juce::ApplicationCommandTarget* Studio::getNextCommandTarget() {
    return nullptr;
}

bool Studio::canUseSelectionForDisc(bool appendPart) {
    return timeSelection.hasRange() &&
           (!appendPart || (discWorkspaceVisible && discWorkspace.singleSelectedTrackIndex() >= 0));
}

void Studio::useSelectionForDisc(bool appendPart) {
    if (!canUseSelectionForDisc(appendPart)) {
        status.setText(appendPart ? "Select a timeline time range and one MiniDisc track first."
                                  : "Select a timeline time range first.",
                       juce::dontSendNotification);
        return;
    }

    setDiscWorkspaceVisible(true);
    if (appendPart) {
        const int idx0 = discWorkspace.singleSelectedTrackIndex();
        const auto result =
            ipcCommand({"disc", "track", "part", "add", juce::String(idx0),
                        juce::String(timeSelection.start), juce::String(timeSelection.end)});
        status.setText(static_cast<bool>(result["ok"]) ? "Added timeline selection as MD part."
                                                       : result["error"].toString(),
                       juce::dontSendNotification);
    } else {
        const auto result = ipcCommand({"disc", "track", "add-selection"});
        status.setText(static_cast<bool>(result["ok"])
                           ? "Inserted MiniDisc track from timeline selection."
                           : result["error"].toString(),
                       juce::dontSendNotification);
    }
}

void Studio::getAllCommands(juce::Array<juce::CommandID>& ids) {
    for (const int id :
         {MenuId::fileImportAudio, MenuId::fileOpenProject, MenuId::fileSaveProject,
          MenuId::fileSaveProjectAs, MenuId::editUndo, MenuId::editRedo, MenuId::editCutSelection,
          MenuId::editCopySelection, MenuId::editPasteOverlap, MenuId::editSelectAll,
          MenuId::editSplit, MenuId::editDuplicateClip, MenuId::fileNewProject, MenuId::mdGoToTrack,
          MenuId::mdInsertTrackFromSelection, MenuId::mdAppendPartFromSelection}) {
        ids.add(id);
    }
}

void Studio::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) {
    int key = 0;
    bool shift = false;
    bool enabled = true;
    switch (id) {
    case MenuId::fileImportAudio:
        key = 'i';
        break;
    case MenuId::fileOpenProject:
        key = 'o';
        break;
    case MenuId::fileSaveProject:
        key = 's';
        break;
    case MenuId::fileSaveProjectAs:
        key = 's';
        shift = true;
        break;
    case MenuId::editUndo:
        key = 'z';
        enabled = editHistory.canUndo();
        break;
    case MenuId::editRedo:
        key = 'z';
        shift = true;
        enabled = editHistory.canRedo();
        break;
    case MenuId::editCutSelection:
        key = 'x';
        enabled = timeSelection.hasRange();
        break;
    case MenuId::editCopySelection:
        key = 'c';
        enabled = timeSelection.hasRange();
        break;
    case MenuId::editPasteOverlap:
        key = 'v';
        enabled = !clipboard.empty();
        break;
    case MenuId::editSelectAll:
        key = 'a';
        enabled = !session.tracks.empty() && session.length() > 0;
        break;
    case MenuId::editSplit:
        key = 'b';
        break;
    case MenuId::editDuplicateClip:
        key = 'd';
        break;
    case MenuId::fileNewProject:
        key = 'n';
        break;
    case MenuId::mdGoToTrack:
        key = 'g';
        shift = true;
        enabled = !session.mdDisc.tracks.empty();
        break;
    default:
        enabled = false;
        break;
    case MenuId::mdInsertTrackFromSelection:
        key = 'i';
        shift = true;
        enabled = canUseSelectionForDisc(false);
        break;
    case MenuId::mdAppendPartFromSelection:
        key = 'j';
        shift = true;
        enabled = canUseSelectionForDisc(true);
        break;
    }
    info.setInfo("Carta Studio command", {}, "Editor", 0);
    info.setActive(enabled);
    if (key != 0) {
        info.addDefaultKeypress(key, juce::ModifierKeys::commandModifier |
                                         (shift ? juce::ModifierKeys::shiftModifier : 0));
    }
}

bool Studio::perform(const InvocationInfo& info) {
    if (savePromptActive) {
        return true;
    }
    // JUCE's menu bar invokes the command and then calls menuItemSelected.
    // Leave menu dispatch to the latter so an edit is never performed twice.
    if (info.invocationMethod != InvocationInfo::fromMenu) {
        menuItemSelected(info.commandID, MenuBarIndex::edit);
    }
    return true;
}

juce::PopupMenu Studio::getMenuForIndex(int index, const juce::String&) {
    // Session, selection, history and menu state are message-thread owned.
    // Snapshot only the transport flag shared with the audio callback.
    bool displayPlaying = false;
    if (index == MenuBarIndex::transport) {
        const juce::ScopedLock guard(lock);
        displayPlaying = playing;
    }

    juce::PopupMenu m;
    if (index == MenuBarIndex::file) {
        m.addCommandItem(&commandManager, MenuId::fileNewProject, "New project");
        m.addCommandItem(&commandManager, MenuId::fileOpenProject, "Open project...");
        m.addCommandItem(&commandManager, MenuId::fileSaveProject, "Save project");
        m.addCommandItem(&commandManager, MenuId::fileSaveProjectAs, "Save project as...");

        juce::PopupMenu recoveryMenu;
        recoveryMenu.addItem(MenuId::fileExportPortable, "Export portable project copy...");
        recoveryMenu.addSeparator();
        recoveryMenu.addItem(MenuId::fileSaveRecoveryNow, "Save recovery snapshot now",
                             dirty && !activeRecovery);
        recoveryMenu.addItem(MenuId::fileRecoverAutosave, "Recover autosaved session...");
        recoveryMenu.addSeparator();
        recoveryMenu.addItem(MenuId::fileShowImportIssues, "Import issues...",
                             !importIssues.empty());
        recoveryMenu.addItem(MenuId::fileClearImportIssues, "Clear import issues",
                             !importIssues.empty());
        m.addSubMenu("Project & Recovery", recoveryMenu);

        m.addSeparator();
        m.addCommandItem(&commandManager, MenuId::fileImportAudio, "Import audio...");
        m.addItem(MenuId::fileExportWav, "Export WAV...");

        juce::PopupMenu exportMenu;
        exportMenu.addItem(MenuId::fileExportMdReviewWav, "Export MiniDisc review WAV...",
                           !session.mdDisc.tracks.empty());
        exportMenu.addSeparator();
        exportMenu.addItem(MenuId::fileExportSelWavFull, "Export time selection WAV (full mix)...",
                           timeSelection.hasRange());
        exportMenu.addItem(MenuId::fileExportSelWavLayers,
                           "Export time selection WAV (selected layers)...",
                           timeSelection.hasRange());
        exportMenu.addItem(MenuId::fileExportSelectedClipWav, "Export selected clip WAV...",
                           validSelection());
        exportMenu.addItem(MenuId::fileExportSelectedClipsWav, "Export selected clips WAV...",
                           !timelineClipSelection.get(editGeneration).empty());
        exportMenu.addSeparator();
        exportMenu.addItem(MenuId::fileExportTrackAtPlayhead, "Export track at playhead WAV...",
                           session.length() > 0);
        exportMenu.addItem(MenuId::fileExportAllMasterTracks, "Export all master tracks WAV...",
                           session.length() > 0);
        exportMenu.addItem(MenuId::fileExportManifest, "Export track manifest...");
        m.addSubMenu("Export Special", exportMenu);

        m.addSeparator();
        const bool hasPendingImports =
            std::any_of(pendingImports.begin(), pendingImports.end(),
                        [](const auto& task) { return !task->cancelled.load(); });
        const bool hasCancelable = (activeExport && !activeExport->cancelled.load()) ||
                                   (activeOpen && !activeOpen->cancelled.load()) ||
                                   (activeSave && !activeSave->cancelled.load()) ||
                                   hasPendingImports;
        juce::PopupMenu cancelMenu;
        cancelMenu.addItem(MenuId::fileCancelExport, "Cancel WAV export",
                           activeExport && !activeExport->cancelled.load());
        cancelMenu.addItem(MenuId::fileCancelOpen, "Cancel opening project",
                           activeOpen && !activeOpen->cancelled.load());
        cancelMenu.addItem(MenuId::fileCancelSave, "Cancel saving project",
                           activeSave && !activeSave->cancelled.load());
        cancelMenu.addItem(MenuId::fileCancelPendingImports, "Cancel pending imports",
                           hasPendingImports);
        m.addSubMenu("Cancel Background Tasks", cancelMenu, hasCancelable);
    }

    if (index == MenuBarIndex::edit) {
        const bool hasClips = !timelineClipSelection.get(editGeneration).empty();
        const bool hasRows =
            !selectedTimelineTracks().empty() || timeSelection.hasRange() ||
            (selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size()));
        const bool hasTrackScope =
            timeSelection.hasRange() ||
            (selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size()));
        const bool point = timeSelection.hasPoint() &&
                           timeSelection.firstTrack < static_cast<int>(session.tracks.size());
        const bool clipSelected = validSelection();
        const bool hasTime = timeSelection.hasRange();
        const bool hasMarker =
            selectedMarker >= 0 && selectedMarker < static_cast<int>(session.markers.size());
        const bool canAdjustEdge = clipSelected || hasClips;
        const bool range = timeSelection.hasRange();
        const bool hasScope = range || (selectedTrack >= 0 &&
                                        selectedTrack < static_cast<int>(session.tracks.size()));
        const bool trackSelected =
            selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size());

        m.addCommandItem(&commandManager, MenuId::editUndo,
                         editHistory.canUndo() ? "Undo " + editHistory.undoAction()
                                               : juce::String("Undo"));
        m.addCommandItem(&commandManager, MenuId::editRedo,
                         editHistory.canRedo() ? "Redo " + editHistory.redoAction()
                                               : juce::String("Redo"));

        if (historyCommands.size() > 8192) {
            historyCommands.clear();
        }
        if (nextHistoryCommand > 2000000000) {
            historyCommands.clear();
            nextHistoryCommand = MenuId::historyBase;
        }
        juce::PopupMenu historyRoot;
        bool hasHistoryEntries = false;
        for (const bool redoHistory : {false, true}) {
            const auto targets = editHistory.targets(redoHistory);
            juce::PopupMenu historyMenu;
            for (size_t n = 0; n < targets.size(); ++n) {
                const int command = nextHistoryCommand++;
                historyCommands.emplace(
                    command,
                    HistoryMenuCommand{editGeneration, targets[n].revision, n + 1, redoHistory});
                historyMenu.addItem(command, juce::String(static_cast<int>(n + 1)) + ". " +
                                                 targets[n].action);
            }
            if (!targets.empty()) {
                hasHistoryEntries = true;
            }
            historyRoot.addSubMenu(redoHistory ? "Redo through..." : "Undo through...", historyMenu,
                                   !targets.empty());
        }
        m.addSubMenu("History", historyRoot, hasHistoryEntries);
        m.addSeparator();

        m.addCommandItem(&commandManager, MenuId::editCutSelection,
                         selectedTimelineTracks().empty()
                             ? "Cut selection (leave gap)"
                             : "Cut time on independent layers (leave gap)");
        m.addCommandItem(&commandManager, MenuId::editCopySelection,
                         selectedTimelineTracks().empty() ? "Copy selection"
                                                          : "Copy time on independent layers");
        m.addCommandItem(&commandManager, MenuId::editPasteOverlap, "Paste at playhead (overlap)");
        m.addItem(MenuId::editDeleteSelection,
                  selectedTimelineTracks().empty()
                      ? "Delete selection (leave gap)"
                      : "Delete time on independent layers (leave gap)",
                  timeSelection.hasRange());

        juce::PopupMenu pasteRippleMenu;
        pasteRippleMenu.addItem(MenuId::editPasteReplace, "Paste at playhead (replace destination)",
                                !clipboard.empty());
        pasteRippleMenu.addItem(MenuId::editPasteInsertDest,
                                "Paste and insert time (destination layers)", !clipboard.empty());
        pasteRippleMenu.addItem(MenuId::editPasteInsertAll,
                                "Paste and insert time (all layers and markers)",
                                !clipboard.empty());
        pasteRippleMenu.addSeparator();
        pasteRippleMenu.addItem(MenuId::editDuplicateSelectionAfter,
                                "Duplicate selection after end (overlap) (Cmd/Ctrl+Shift+D)",
                                timeSelection.hasRange());
        pasteRippleMenu.addItem(MenuId::editTrimOutsideSelection,
                                selectedTimelineTracks().empty()
                                    ? "Trim outside selection (selected layers)"
                                    : "Trim outside selection (independent layers)",
                                timeSelection.hasRange());
        pasteRippleMenu.addItem(MenuId::editMoveSelection, "Move selection to time and layer...",
                                timeSelection.hasRange());
        pasteRippleMenu.addItem(MenuId::editSplitSelToNewLayers, "Split selection to new layers",
                                timeSelection.hasRange());
        pasteRippleMenu.addSeparator();

        juce::PopupMenu cutRipple;
        cutRipple.addItem(MenuId::editRippleCutFocused, "Focused layer",
                          timeSelection.hasRange() && selectedTrack >= 0 &&
                              selectedTrack < static_cast<int>(session.tracks.size()));
        cutRipple.addItem(MenuId::editRippleCutSelected, "Selected layers",
                          timeSelection.hasRange());
        cutRipple.addItem(MenuId::editRippleCutAll, "All layers and markers",
                          timeSelection.hasRange());
        pasteRippleMenu.addSubMenu("Cut and close gap", cutRipple, timeSelection.hasRange());

        juce::PopupMenu deleteRipple;
        deleteRipple.addItem(MenuId::editRippleDeleteFocused, "Focused layer",
                             timeSelection.hasRange() && selectedTrack >= 0 &&
                                 selectedTrack < static_cast<int>(session.tracks.size()));
        deleteRipple.addItem(MenuId::editRippleDeleteSelected, "Selected layers (Shift+Delete)",
                             timeSelection.hasRange());
        deleteRipple.addItem(MenuId::editRippleDeleteAll,
                             "All layers and markers (Cmd/Ctrl+Delete)", timeSelection.hasRange());
        pasteRippleMenu.addSubMenu("Delete and close gap", deleteRipple, timeSelection.hasRange());

        juce::PopupMenu insertRipple;
        insertRipple.addItem(MenuId::editInsertGapFocused, "Focused layer",
                             timeSelection.hasRange() && selectedTrack >= 0 &&
                                 selectedTrack < static_cast<int>(session.tracks.size()));
        insertRipple.addItem(MenuId::editInsertGapSelected, "Selected layers",
                             timeSelection.hasRange());
        insertRipple.addItem(MenuId::editInsertGapAll, "All layers and markers",
                             timeSelection.hasRange());
        pasteRippleMenu.addSubMenu("Insert selection-length gap", insertRipple,
                                   timeSelection.hasRange());
        m.addSubMenu("Paste & Ripple", pasteRippleMenu);
        m.addSeparator();

        m.addCommandItem(&commandManager, MenuId::editSelectAll, "Select all audio");
        m.addItem(MenuId::editDeselectAll, "Deselect all (Esc)", hasAnyAudioSelection());

        juce::PopupMenu selectionRoot;
        selectionRoot.addItem(MenuId::editSetTimeSelection, "Set time selection...",
                              !session.tracks.empty());
        selectionRoot.addItem(MenuId::editMarkInAtPlayhead, "Mark selection in at playhead (I)",
                              !session.tracks.empty());
        selectionRoot.addItem(MenuId::editMarkOutAtPlayhead, "Mark selection out at playhead (O)",
                              liveInPresent && liveInGeneration == editGeneration);
        selectionRoot.addSeparator();

        juce::PopupMenu selection;
        selection.addItem(MenuId::editSetSelectionStartLength, "Set selection start and length...");
        selection.addItem(MenuId::editFindZeroCrossingsAny,
                          juce::String::fromUTF8("Find quiet zero crossings (±10 ms)"),
                          timeSelection.hasRange() && !activeLevelAnalysis);
        selection.addItem(
            MenuId::editFindZeroCrossingsCommon,
            juce::String::fromUTF8("Find common output-channel zero crossings (±10 ms)"),
            timeSelection.hasRange() && !activeLevelAnalysis);
        selection.addSeparator();
        selection.addItem(MenuId::editSelectMasterTrackAll, "Master track at playhead (all layers)",
                          session.length() > 0);
        selection.addItem(MenuId::editToBoundaryMasterStart,
                          "Playhead to master start (all layers)", session.length() > 0);
        selection.addItem(MenuId::editToBoundaryMasterEnd, "Playhead to master end (all layers)",
                          session.length() > 0);
        selection.addItem(MenuId::editToBoundaryPrevMarker,
                          "Playhead to previous marker (all layers)", session.length() > 0);
        selection.addItem(MenuId::editToBoundaryNextMarker, "Playhead to next marker (all layers)",
                          session.length() > 0);
        selection.addItem(MenuId::editToBoundaryPrevClip,
                          "Playhead to previous clip edge (all layers)", session.length() > 0);
        selection.addItem(MenuId::editToBoundaryNextClip, "Playhead to next clip edge (all layers)",
                          session.length() > 0);

        juce::PopupMenu scoped;
        scoped.addItem(MenuId::editSelectMasterTrackScoped, "Master track at playhead",
                       hasScope && session.length() > 0);
        scoped.addSeparator();
        scoped.addItem(MenuId::editScopedToMasterStart, "Playhead to master start",
                       hasScope && session.length() > 0);
        scoped.addItem(MenuId::editScopedToMasterEnd, "Playhead to master end",
                       hasScope && session.length() > 0);
        scoped.addItem(MenuId::editScopedToPrevMarker, "Playhead to previous marker",
                       hasScope && session.length() > 0);
        scoped.addItem(MenuId::editScopedToNextMarker, "Playhead to next marker",
                       hasScope && session.length() > 0);
        scoped.addItem(MenuId::editScopedToPrevClip, "Playhead to previous clip edge",
                       hasScope && session.length() > 0);
        scoped.addItem(MenuId::editScopedToNextClip, "Playhead to next clip edge",
                       hasScope && session.length() > 0);
        selection.addSubMenu("Select on selected layers", scoped, hasScope && session.length() > 0);

        selection.addSeparator();
        selection.addItem(MenuId::selExtendLeft, "Extend focus left one sample (Shift+Left)",
                          range || trackSelected);
        selection.addItem(MenuId::selExtendRight, "Extend focus right one sample (Shift+Right)",
                          range || trackSelected);
        selection.addItem(MenuId::selExtendUp, "Extend selection up (Shift+Up)", range);
        selection.addItem(MenuId::selExtendDown, "Extend selection down (Shift+Down)", range);
        selection.addSeparator();
        selection.addItem(MenuId::editRememberSelection, "Remember time selection", range);
        selection.addItem(
            MenuId::editRestoreSelection, "Restore remembered selection",
            selectionMemory.canRestore(static_cast<int>(session.tracks.size()), editGeneration));
        selection.addItem(MenuId::editClearRememberedSelection, "Clear remembered selection",
                          selectionMemory.present);
        selection.addSeparator();
        selection.addItem(MenuId::selStartEarlier,
                          "Start one sample earlier (Cmd/Ctrl+Shift+Alt+Left)", range);
        selection.addItem(MenuId::selStartLater,
                          "Start one sample later (Cmd/Ctrl+Shift+Alt+Right)", range);
        selection.addItem(MenuId::selEndEarlier, "End one sample earlier (Shift+Alt+Left)", range);
        selection.addItem(MenuId::selEndLater, "End one sample later (Shift+Alt+Right)", range);
        selection.addItem(MenuId::selMoveEarlier, "Move one sample earlier (Cmd/Ctrl+Shift+Left)",
                          range);
        selection.addItem(MenuId::selMoveLater, "Move one sample later (Cmd/Ctrl+Shift+Right)",
                          range);
        selectionRoot.addSubMenu("Adjust time selection", selection, !session.tracks.empty());

        juce::PopupMenu pointMenu;
        pointMenu.addItem(MenuId::editPointSetAtPlayhead, "Set point at playhead (E)",
                          selectedTrack >= 0 &&
                              selectedTrack < static_cast<int>(session.tracks.size()));
        pointMenu.addItem(MenuId::editPointNudgeLeft, "Move left one sample (,)", point);
        pointMenu.addItem(MenuId::editPointNudgeRight, "Move right one sample (.)", point);
        pointMenu.addSeparator();

        juce::PopupMenu pointNav;
        pointNav.addItem(MenuId::editPointNavMasterStart, "Master start", point);
        pointNav.addItem(MenuId::editPointNavMasterEnd, "Master end", point);
        pointNav.addSeparator();
        pointNav.addItem(MenuId::editPointNavPrevMarker, "Previous marker (Alt+Page Up)", point);
        pointNav.addItem(MenuId::editPointNavNextMarker, "Next marker (Alt+Page Down)", point);
        pointNav.addSeparator();
        pointNav.addItem(MenuId::editPointNavPrevClipLayer, "Previous clip boundary (Alt+[)",
                         point);
        pointNav.addItem(MenuId::editPointNavNextClipLayer, "Next clip boundary (Alt+])", point);
        pointNav.addItem(MenuId::editPointNavPrevClipAny,
                         "Previous clip boundary on any layer (Shift+Alt+[)", point);
        pointNav.addItem(MenuId::editPointNavNextClipAny,
                         "Next clip boundary on any layer (Shift+Alt+])", point);
        pointMenu.addSubMenu("Move to boundary", pointNav, point);
        pointMenu.addSeparator();

        juce::PopupMenu pointRange;
        pointRange.addItem(MenuId::editPointSelectMasterStart, "Master start", point);
        pointRange.addItem(MenuId::editPointSelectMasterEnd, "Master end", point);
        pointRange.addSeparator();
        pointRange.addItem(MenuId::editPointSelectPrevMarker, "Previous marker", point);
        pointRange.addItem(MenuId::editPointSelectNextMarker, "Next marker", point);
        pointRange.addSeparator();
        pointRange.addItem(MenuId::editPointSelectPrevClipLayer,
                           "Previous clip boundary on this layer", point);
        pointRange.addItem(MenuId::editPointSelectNextClipLayer, "Next clip boundary on this layer",
                           point);
        pointRange.addItem(MenuId::editPointSelectPrevClipAny,
                           "Previous clip boundary on any layer", point);
        pointRange.addItem(MenuId::editPointSelectNextClipAny, "Next clip boundary on any layer",
                           point);
        pointMenu.addSubMenu("Select time to boundary", pointRange, point);
        pointMenu.addItem(MenuId::editPointSelectGapLayer, "Select clip gap on point layer (Alt+G)",
                          point);
        pointMenu.addItem(MenuId::editPointSelectGapIndependent,
                          "Select common clip gap on independently selected layers",
                          point && !selectedTimelineTracks().empty());
        pointMenu.addItem(MenuId::editPointSelectGapAll, "Select common clip gap on all layers",
                          point);
        pointMenu.addSeparator();
        pointMenu.addItem(MenuId::editPointPasteOverlap, "Paste overlap",
                          point && !clipboard.empty());
        pointMenu.addItem(MenuId::editPointPasteReplace, "Paste replacing audio",
                          point && !clipboard.empty());
        pointMenu.addItem(MenuId::editPointPasteInsertDest,
                          "Paste and insert on destination layers", point && !clipboard.empty());
        pointMenu.addItem(MenuId::editPointPasteInsertAll, "Paste and insert on all layers",
                          point && !clipboard.empty());
        selectionRoot.addSubMenu(
            "Edit point", pointMenu,
            point ||
                (selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size())));
        m.addSubMenu("Selection", selectionRoot);

        m.addCommandItem(&commandManager, MenuId::editSplit,
                         timeSelection.hasRange()
                             ? (selectedTimelineTracks().empty()
                                    ? "Split at selection boundaries"
                                    : "Split independent layers at selection boundaries")
                             : (selectedClip < 0 && timeSelection.hasPoint()
                                    ? "Split layer at edit point"
                                    : "Split at playhead"));

        juce::PopupMenu clipMenu;
        clipMenu.addCommandItem(&commandManager, MenuId::editDuplicateClip,
                                "Duplicate selected clip");
        clipMenu.addItem(MenuId::editDeleteClip, "Delete selected clip");
        clipMenu.addItem(MenuId::clipRename,
                         selectedMarker >= 0 ? "Rename selected clip..."
                                             : "Rename selected clip... (F2)",
                         validSelection());
        clipMenu.addItem(MenuId::clipJoinNext, "Join selected clip with next", validSelection());
        juce::PopupMenu splitTracks;
        splitTracks.addItem(MenuId::clipSplitSelectedLayers, "Selected layers", hasTrackScope);
        splitTracks.addItem(MenuId::clipSplitAllLayers, "All layers", !session.tracks.empty());
        clipMenu.addSubMenu("Split clips at playhead", splitTracks,
                            hasTrackScope || !session.tracks.empty());
        clipMenu.addSeparator();
        clipMenu.addItem(MenuId::clipMoveToSample, "Move clip to sample...", validSelection());
        clipMenu.addItem(MenuId::clipTrimToSamples, "Trim clip to sample range...",
                         validSelection());
        clipMenu.addItem(MenuId::clipSlip, "Slip clip source audio...", validSelection());
        clipMenu.addItem(MenuId::clipNudgeLeft, "Nudge clip left one sample (Cmd/Ctrl+Alt+Left)",
                         validSelection());
        clipMenu.addItem(MenuId::clipNudgeRight, "Nudge clip right one sample (Cmd/Ctrl+Alt+Right)",
                         validSelection());

        juce::PopupMenu align;
        align.addItem(MenuId::clipAlignStartPlayhead, "Clip start to playhead", clipSelected);
        align.addItem(MenuId::clipAlignEndPlayhead, "Clip end to playhead", clipSelected);
        align.addSeparator();
        align.addItem(MenuId::clipAlignStartSelStart, "Clip start to selection start",
                      clipSelected && hasTime);
        align.addItem(MenuId::clipAlignStartSelEnd, "Clip start to selection end",
                      clipSelected && hasTime);
        align.addItem(MenuId::clipAlignEndSelStart, "Clip end to selection start",
                      clipSelected && hasTime);
        align.addItem(MenuId::clipAlignEndSelEnd, "Clip end to selection end",
                      clipSelected && hasTime);
        align.addSeparator();
        align.addItem(MenuId::clipAlignStartMarker, "Clip start to selected marker",
                      clipSelected && hasMarker);
        align.addItem(MenuId::clipAlignEndMarker, "Clip end to selected marker",
                      clipSelected && hasMarker);
        align.addSeparator();
        align.addItem(MenuId::clipAlignStartPrevEdge, "Clip start to previous clip edge",
                      clipSelected);
        align.addItem(MenuId::clipAlignStartNextEdge, "Clip start to next clip edge", clipSelected);
        align.addItem(MenuId::clipAlignEndPrevEdge, "Clip end to previous clip edge", clipSelected);
        align.addItem(MenuId::clipAlignEndNextEdge, "Clip end to next clip edge", clipSelected);
        clipMenu.addSubMenu("Align selected clip", align, clipSelected);

        juce::PopupMenu trimEdge;
        trimEdge.addItem(MenuId::clipNudgeStartEarlier, "Start earlier one sample (F5)",
                         canAdjustEdge);
        trimEdge.addItem(MenuId::clipNudgeStartLater, "Start later one sample (F6)", canAdjustEdge);
        trimEdge.addItem(MenuId::clipNudgeEndEarlier, "End earlier one sample (F7)", canAdjustEdge);
        trimEdge.addItem(MenuId::clipNudgeEndLater, "End later one sample (F8)", canAdjustEdge);
        clipMenu.addSubMenu(hasClips ? "Adjust selected clip edges" : "Adjust clip edge", trimEdge,
                            canAdjustEdge);

        juce::PopupMenu clipPortion;
        clipPortion.addItem(MenuId::clipSelectVisiblePortion, "Visible portion", validSelection());
        clipPortion.addItem(MenuId::clipSelectBeforePlayhead, "Before playhead", validSelection());
        clipPortion.addItem(MenuId::clipSelectAfterPlayhead, "After playhead", validSelection());
        clipMenu.addSubMenu("Select portion of focused clip", clipPortion, validSelection());
        clipMenu.addItem(MenuId::clipSelectTime, "Select time occupied by selected clip",
                         validSelection());
        clipMenu.addSeparator();
        clipMenu.addItem(MenuId::clipGainAndFades, "Clip gain and fades...", validSelection());
        addFadeShapeMenus(clipMenu, validSelection() || hasClips);
        clipMenu.addItem(MenuId::clipCrossfadeTrack, "Linear crossfade with overlapping clip",
                         validSelection());
        clipMenu.addItem(MenuId::clipCrossfadeAcrossTracks,
                         "Linear crossfade with clip on another layer", validSelection());
        clipMenu.addItem(MenuId::clipAmplify, "Amplify...", !session.tracks.empty());
        clipMenu.addItem(MenuId::clipNormalize, "Normalize...", !session.tracks.empty());
        m.addSubMenu("Clip", clipMenu, true);

        juce::PopupMenu selectedClipsMenu;
        selectedClipsMenu.addItem(MenuId::selectClipsAllLayers, "Select all clips on all layers",
                                  !session.tracks.empty());
        selectedClipsMenu.addItem(MenuId::selectClipsSelectedLayers,
                                  "Select all clips on selected layers", hasRows);
        selectedClipsMenu.addItem(MenuId::selectClipsInTimeSel,
                                  "Select clips intersecting time selection",
                                  timeSelection.hasRange());
        selectedClipsMenu.addItem(MenuId::selectClipsFocusedSourceAll,
                                  "Select clips using focused source", validSelection());
        selectedClipsMenu.addItem(MenuId::selectClipsFocusedSourceLayers,
                                  "Select focused source on selected layers",
                                  validSelection() && hasRows);
        selectedClipsMenu.addItem(MenuId::selectClipsFocusedSourceTime,
                                  "Select focused source in time selection",
                                  validSelection() && timeSelection.hasRange());
        selectedClipsMenu.addItem(MenuId::clipFindSilence, "Find silence in focused clip...",
                                  validSelection() && !activeLevelAnalysis &&
                                      pendingImports.empty());
        selectedClipsMenu.addItem(MenuId::selectedClipsSelectSpan,
                                  "Select time spanned by selected clips", hasClips);
        selectedClipsMenu.addSeparator();
        selectedClipsMenu.addItem(MenuId::clipToggleSelection, "Toggle focused clip (G)",
                                  validSelection());
        selectedClipsMenu.addSeparator();
        selectedClipsMenu.addItem(MenuId::selectedClipsGroup, "Group selected clips",
                                  timelineClipSelection.get(editGeneration).size() >= 2);
        selectedClipsMenu.addItem(MenuId::selectedClipsUngroup, "Ungroup selected clips",
                                  hasClips || validSelection());
        selectedClipsMenu.addSeparator();
        selectedClipsMenu.addItem(MenuId::selectedClipsCopy, "Copy clips (Cmd/Ctrl+C)", hasClips);
        selectedClipsMenu.addItem(MenuId::selectedClipsCut, "Cut clips (Cmd/Ctrl+X)", hasClips);
        selectedClipsMenu.addItem(MenuId::selectedClipsDelete, "Delete clips (Delete)", hasClips);
        selectedClipsMenu.addItem(MenuId::selectedClipsDuplicate,
                                  "Duplicate clips after span (Cmd/Ctrl+D)", hasClips);
        selectedClipsMenu.addItem(MenuId::selectedClipsSplit,
                                  "Split clips at playhead (Cmd/Ctrl+B)", hasClips);
        selectedClipsMenu.addItem(MenuId::selectedClipsMove, "Move clips...", hasClips);
        selectedClipsMenu.addItem(MenuId::selectedClipsJoin, "Join compatible selected clips",
                                  timelineClipSelection.get(editGeneration).size() >= 2);
        selectedClipsMenu.addItem(MenuId::layerRenderSelectedClips,
                                  "Render selected clips to new layer...",
                                  hasClips && !activeExport && pendingImports.empty());
        selectedClipsMenu.addItem(MenuId::layerConsolidateClips,
                                  "Consolidate clips on one layer...",
                                  hasClips && !activeExport && pendingImports.empty());
        selectedClipsMenu.addItem(MenuId::layerConsolidateAllowRouting,
                                  "Consolidate allowing routing changes...",
                                  hasClips && !activeExport && pendingImports.empty());
        selectedClipsMenu.addItem(MenuId::selectedClipsZoom, "Zoom to selected clips (Shift+Z)",
                                  hasClips);
        selectedClipsMenu.addItem(MenuId::selectedClipsGainFades, "Set gain and fades...",
                                  hasClips);
        addFadeShapeMenus(selectedClipsMenu, hasClips);
        selectedClipsMenu.addItem(MenuId::selectedClipsSlip, "Slip source audio...", hasClips);
        selectedClipsMenu.addItem(MenuId::selectedClipsCrossfadePair,
                                  "Linear crossfade selected pair",
                                  timelineClipSelection.get(editGeneration).size() == 2);

        juce::PopupMenu selectedEdges;
        selectedEdges.addItem(MenuId::clipNudgeStartEarlier, "Start earlier one sample (F5)",
                              hasClips);
        selectedEdges.addItem(MenuId::clipNudgeStartLater, "Start later one sample (F6)", hasClips);
        selectedEdges.addItem(MenuId::clipNudgeEndEarlier, "End earlier one sample (F7)", hasClips);
        selectedEdges.addItem(MenuId::clipNudgeEndLater, "End later one sample (F8)", hasClips);
        selectedEdges.addSeparator();
        selectedEdges.addItem(MenuId::selectedClipsMoveStarts, "Move starts by samples...",
                              hasClips);
        selectedEdges.addItem(MenuId::selectedClipsMoveEnds, "Move ends by samples...", hasClips);
        selectedEdges.addSeparator();
        selectedEdges.addItem(MenuId::selectedClipsTrimStartsPlayhead, "Trim starts using playhead",
                              hasClips);
        selectedEdges.addItem(MenuId::selectedClipsTrimEndsPlayhead, "Trim ends using playhead",
                              hasClips);
        selectedClipsMenu.addSubMenu("Adjust selected clip edges", selectedEdges, hasClips);

        selectedClipsMenu.addItem(MenuId::selectedClipsNudgeLeft,
                                  "Nudge clips left one sample (Cmd/Ctrl+Alt+Left)", hasClips);
        selectedClipsMenu.addItem(MenuId::selectedClipsNudgeRight,
                                  "Nudge clips right one sample (Cmd/Ctrl+Alt+Right)", hasClips);
        selectedClipsMenu.addItem(MenuId::selectedClipsSequence, "Sequence clips by start time",
                                  timelineClipSelection.get(editGeneration).size() >= 2);
        selectedClipsMenu.addItem(MenuId::selectedClipsDistribute,
                                  "Distribute clips with equal gaps",
                                  timelineClipSelection.get(editGeneration).size() >= 3);
        selectedClipsMenu.addSeparator();
        selectedClipsMenu.addItem(MenuId::selectedClipsClear, "Clear clip selection", hasClips);
        m.addSubMenu("Selected clips", selectedClipsMenu, hasClips || !session.tracks.empty());

        juce::PopupMenu snappingMenu;
        snappingMenu.addItem(MenuId::snapOff, "Snap off", true, snapMode == 0);
        snappingMenu.addItem(MenuId::snapGrid, "Snap to grid", true, snapMode == 1);
        snappingMenu.addItem(MenuId::snapGridAndBounds, "Snap to grid and boundaries", true,
                             snapMode == 2);
        snappingMenu.addSeparator();

        juce::PopupMenu snapTargets;
        snapTargets.addItem(MenuId::snapTargetClipEdges, "Clip edges", true, snapOptions.clipEdges);
        snapTargets.addItem(MenuId::snapTargetMarkers, "Markers", true, snapOptions.markers);
        snapTargets.addItem(MenuId::snapTargetSelection, "Time selection edges", true,
                            snapOptions.selection);
        snapTargets.addItem(MenuId::snapTargetPlayhead, "Playhead", true, snapOptions.playhead);
        snapTargets.addItem(MenuId::snapTargetTimelineStart, "Timeline start", true,
                            snapOptions.timelineStart);
        snappingMenu.addSubMenu("Snap boundary targets", snapTargets);

        juce::PopupMenu grid;
        grid.addItem(MenuId::snapGridSamples1, "Project samples", true, snapGridSamples == 1);
        grid.addItem(MenuId::snapGridSamples100Ms, "100 milliseconds", true,
                     snapGridSamples == 4410);
        grid.addItem(MenuId::snapGridSamples1Sec, "1 second", true, snapGridSamples == 44100);
        grid.addSeparator();
        grid.addItem(MenuId::snapGridCustom,
                     "Custom interval... (" + juce::String(snapGridSamples) + " samples)");
        snappingMenu.addSubMenu("Snap grid", grid);
        m.addSubMenu("Snapping", snappingMenu, true);
    }

    if (index == MenuBarIndex::transport) {
        m.addItem(MenuId::transportPlayPause, displayPlaying ? "Pause" : "Play");
        m.addItem(MenuId::transportStop, "Stop and return to start");
        m.addItem(MenuId::transportStopAtPlayhead, "Stop at playhead (K)");
        m.addSeparator();

        juce::PopupMenu playSelMenu;
        playSelMenu.addItem(MenuId::transportPlaySelFull, "Play selection (full mix)",
                            timeSelection.hasRange());
        playSelMenu.addItem(MenuId::transportLoopSelFull, "Loop selection (full mix)",
                            timeSelection.hasRange());
        playSelMenu.addItem(MenuId::transportPlaySelContextFull,
                            "Play selection with context (full mix)", timeSelection.hasRange());
        playSelMenu.addSeparator();
        playSelMenu.addItem(MenuId::transportPlaySelLayers, "Play selection (selected layers)",
                            timeSelection.hasRange());
        playSelMenu.addItem(MenuId::transportLoopSelLayers, "Loop selection (selected layers)",
                            timeSelection.hasRange());
        playSelMenu.addItem(MenuId::transportPlaySelContextLayers,
                            "Play selection with context (selected layers)",
                            timeSelection.hasRange());
        playSelMenu.addSeparator();
        playSelMenu.addItem(MenuId::transportPlayClip, "Play selected clip", validSelection());
        playSelMenu.addItem(MenuId::transportLoopClip, "Loop selected clip", validSelection());
        playSelMenu.addItem(MenuId::transportPlaySelectedClips, "Play selected clips",
                            !timelineClipSelection.get(editGeneration).empty());
        playSelMenu.addItem(MenuId::transportLoopSelectedClips, "Loop selected clips",
                            !timelineClipSelection.get(editGeneration).empty());
        m.addSubMenu("Play & Loop Selection", playSelMenu);

        juce::PopupMenu loopMenu;
        loopMenu.addItem(MenuId::savedLoopSetFromSel, "Set loop region from selection",
                         timeSelection.hasRange());
        loopMenu.addItem(MenuId::savedLoopPlay, "Loop saved region (full mix)",
                         savedLoop.end > savedLoop.start &&
                             savedLoop.end <= carta::projectEndSample(session));
        loopMenu.addItem(MenuId::savedLoopClear, "Clear loop region",
                         savedLoop.end > savedLoop.start);
        m.addSubMenu("Saved Loop Region", loopMenu);

        juce::PopupMenu auditionMenu;
        auditionMenu.addItem(MenuId::auditionPlayhead, "Audition around playhead",
                             session.length() > 0);
        auditionMenu.addItem(MenuId::auditionSelStart, "Audition selection start",
                             timeSelection.hasRange());
        auditionMenu.addItem(MenuId::auditionSelEnd, "Audition selection end",
                             timeSelection.hasRange());
        auditionMenu.addItem(MenuId::auditionEditPoint, "Audition around edit point",
                             timeSelection.hasPoint() &&
                                 timeSelection.firstTrack <
                                     static_cast<int>(session.tracks.size()));
        auditionMenu.addSeparator();

        juce::PopupMenu previewRipple;
        previewRipple.addItem(MenuId::previewRippleFocused, "Focused layer",
                              timeSelection.hasRange() && selectedTrack >= 0 &&
                                  selectedTrack < static_cast<int>(session.tracks.size()));
        previewRipple.addItem(MenuId::previewRippleSelected, "Selected layers",
                              timeSelection.hasRange());
        previewRipple.addItem(MenuId::previewRippleAll, "All layers and markers",
                              timeSelection.hasRange());
        auditionMenu.addSubMenu("Preview closing selection", previewRipple,
                                timeSelection.hasRange());

        juce::PopupMenu crossfade;
        crossfade.addItem(MenuId::previewCrossfadeSameLayer, "On the same layer", validSelection());
        crossfade.addItem(MenuId::previewCrossfadeAcrossLayers, "Across layers", validSelection());
        crossfade.addItem(MenuId::previewCrossfadePair, "Selected pair",
                          timelineClipSelection.get(editGeneration).size() == 2);
        auditionMenu.addSubMenu("Preview linear crossfade", crossfade, validSelection());
        auditionMenu.addSeparator();
        auditionMenu.addItem(MenuId::auditionSetContext, "Set audition context...");
        m.addSubMenu("Audition & Preview", auditionMenu);

        juce::PopupMenu seekMenu;
        seekMenu.addItem(MenuId::seekOneSampleEarlier, "Playhead one sample earlier (Left)",
                         session.length() > 0);
        seekMenu.addItem(MenuId::seekOneSampleLater, "Playhead one sample later (Right)",
                         session.length() > 0);
        seekMenu.addItem(MenuId::seekMasterStart, "Playhead to master start (Home)",
                         session.length() > 0);
        seekMenu.addItem(MenuId::seekMasterEnd, "Playhead to master end (End)",
                         session.length() > 0);
        m.addSubMenu("Seek Playhead", seekMenu, session.length() > 0);
        m.addSeparator();

        m.addItem(MenuId::audioSettings, "Audio output settings...");
        m.addItem(MenuId::audioRescanDevices, "Rescan audio devices");

        juce::PopupMenu diagMenu;
        diagMenu.addItem(MenuId::metersResetPeaks, "Reset held output peaks");
        diagMenu.addItem(MenuId::metersResetEditLockCount, "Reset edit-lock interruption count");
        m.addSubMenu("Diagnostics & Meters", diagMenu);
    }

    if (index == MenuBarIndex::markers) {
        const bool singleMdTrack =
            discWorkspaceVisible && discWorkspace.singleSelectedTrackIndex() >= 0;
        m.addItem(MenuId::markerAddAtPlayhead, "Add track marker at playhead");
        m.addItem(MenuId::editPointAddMarker, "Add track marker at edit point",
                  timeSelection.hasPoint() &&
                      timeSelection.firstTrack < static_cast<int>(session.tracks.size()) &&
                      timeSelection.start < carta::projectEndSample(session));
        m.addItem(MenuId::markerRename, "Rename selected marker... (F2)", selectedMarker >= 0);
        m.addItem(MenuId::markerSetPosition, "Set marker position... (P)", selectedMarker >= 0);
        m.addItem(MenuId::markerDelete, "Delete selected marker", selectedMarker >= 0);
        m.addSeparator();

        juce::PopupMenu mdMenu;
        mdMenu.addItem(MenuId::mdTracksFromClips, "Tracks from Clips", session.length() > 0);
        mdMenu.addCommandItem(&commandManager, MenuId::mdGoToTrack, "Go to MD track...");
        mdMenu.addCommandItem(&commandManager, MenuId::mdInsertTrackFromSelection,
                              "Insert MD track from timeline selection");
        mdMenu.addCommandItem(&commandManager, MenuId::mdAppendPartFromSelection,
                              "Append timeline selection as MD part");
        mdMenu.addSeparator();
        mdMenu.addItem(MenuId::mdPreviewFromSelected, "Preview from selected MD track",
                       singleMdTrack);
        mdMenu.addItem(MenuId::mdEditTrackTitleForms,
                       "Edit selected MD track title forms... (Shift+F2)", singleMdTrack);
        m.addSubMenu("MiniDisc Tracks", mdMenu);

        juce::PopupMenu navListMenu;
        navListMenu.addItem(MenuId::markerPrev, "Previous marker", !session.markers.empty());
        navListMenu.addItem(MenuId::markerNext, "Next marker", !session.markers.empty());
        navListMenu.addItem(MenuId::markerFocusPrev, "Focus previous marker (Shift+Page Up)",
                            !session.markers.empty());
        navListMenu.addItem(MenuId::markerFocusNext, "Focus next marker (Shift+Page Down)",
                            !session.markers.empty());
        navListMenu.addSeparator();
        navListMenu.addItem(MenuId::markerAuditionBoundary, "Audition focused marker boundary (B)",
                            selectedMarker >= 0);
        navListMenu.addSeparator();
        navListMenu.addItem(MenuId::markerTrackList, "Track list...", session.length() > 0);
        navListMenu.addItem(MenuId::markerList, "Marker list and repair...",
                            !session.markers.empty());
        m.addSubMenu("Navigate & Lists", navListMenu);

        juce::PopupMenu editShiftMenu;
        editShiftMenu.addItem(MenuId::markerNudgeLeft, "Move selected marker left one sample (F9)",
                              selectedMarker >= 0);
        editShiftMenu.addItem(MenuId::markerNudgeRight,
                              "Move selected marker right one sample (F10)", selectedMarker >= 0);
        editShiftMenu.addSeparator();
        editShiftMenu.addItem(MenuId::markerShiftAll, "Shift all markers...",
                              !session.markers.empty());
        editShiftMenu.addItem(MenuId::markerShiftFromPlayhead,
                              "Shift markers at or after playhead...", !session.markers.empty());
        editShiftMenu.addItem(MenuId::markerShiftInSelection, "Shift markers in time selection...",
                              !session.markers.empty() && timeSelection.hasRange());
        editShiftMenu.addSeparator();
        editShiftMenu.addItem(MenuId::markerSnapToSamples, "Snap markers to nearest sample",
                              !session.markers.empty());
        editShiftMenu.addItem(MenuId::markerNumberByTrack,
                              "Replace marker names with track numbers", !session.markers.empty());
        editShiftMenu.addItem(MenuId::markerDeleteInSelection, "Delete markers in time selection",
                              timeSelection.hasRange() && !session.markers.empty());
        m.addSubMenu("Edit & Shift Markers", editShiftMenu);

        juce::PopupMenu genMenu;
        genMenu.addItem(MenuId::markerAddAtClipStarts, "Add markers at selected clip starts",
                        validSelection() || !timelineClipSelection.get(editGeneration).empty());
        genMenu.addItem(MenuId::markerAddAtClipEnds, "Add markers at selected clip ends",
                        validSelection() || !timelineClipSelection.get(editGeneration).empty());
        genMenu.addItem(MenuId::markerAddSourceCues, "Add WAV cues from focused clip",
                        validSelection());
        genMenu.addItem(MenuId::markerSuggestQuietGaps, "Suggest markers from quiet gaps...",
                        session.length() > 0 && !activeLevelAnalysis);
        m.addSubMenu("Generate Markers", genMenu);

        m.addSeparator();
        juce::PopupMenu ioMenu;
        ioMenu.addItem(MenuId::markerImport, "Import markers (replace)...", session.length() > 0);
        ioMenu.addItem(MenuId::markerExport, "Export markers...", session.length() > 0);
        ioMenu.addSeparator();
        ioMenu.addItem(MenuId::markerImportAudacity, "Import Audacity point labels (replace)...",
                       session.length() > 0);
        ioMenu.addItem(MenuId::markerExportAudacity, "Export Audacity point labels...",
                       session.length() > 0);
        ioMenu.addSeparator();
        ioMenu.addItem(MenuId::markerCancelExchange, "Cancel marker exchange",
                       activeMarkerExchange && !activeMarkerExchange->cancelled.load());
        m.addSubMenu("Import & Export", ioMenu);
    }

    if (index == MenuBarIndex::master) {
        m.addItem(MenuId::masterStereo, "Stereo", true, masterChannels == 2);
        m.addItem(MenuId::masterMono, "Mono", true, masterChannels == 1);
        m.addItem(MenuId::masterMinimumDuration, "Minimum master duration...");
        m.addSeparator();

        juce::PopupMenu wavFormatMenu;
        wavFormatMenu.addItem(MenuId::wavFmtPcm16, "WAV 16-bit PCM", true,
                              wavEncoding == carta::WavEncoding::pcm16);
        wavFormatMenu.addItem(MenuId::wavFmtPcm24, "WAV 24-bit PCM", true,
                              wavEncoding == carta::WavEncoding::pcm24);
        wavFormatMenu.addItem(MenuId::wavFmtFloat32, "WAV 32-bit float", true,
                              wavEncoding == carta::WavEncoding::float32);
        wavFormatMenu.addSeparator();
        wavFormatMenu.addItem(MenuId::wavToggleDither, "TPDF dither for PCM",
                              wavEncoding != carta::WavEncoding::float32, wavDither);
        wavFormatMenu.addItem(MenuId::wavToggleEmbedMarkers, "Embed markers in WAV", true,
                              wavEmbedMarkers);
        m.addSubMenu("WAV Export Format", wavFormatMenu);

        juce::PopupMenu analysisMenu;
        analysisMenu.addItem(MenuId::analyzeMasterLevels, "Analyze master levels...",
                             session.length() > 0 && !activeLevelAnalysis);
        analysisMenu.addItem(MenuId::analyzeSelLevelsFull, "Analyze selection levels (full mix)...",
                             timeSelection.hasRange() && !activeLevelAnalysis);
        analysisMenu.addItem(MenuId::analyzeSelLevelsLayers,
                             "Analyze selection levels (selected layers)...",
                             timeSelection.hasRange() && !activeLevelAnalysis);
        analysisMenu.addItem(MenuId::analyzeTrackAtPlayhead, "Analyze master track at playhead...",
                             session.length() > 0 && !activeLevelAnalysis);
        analysisMenu.addSeparator();
        analysisMenu.addItem(MenuId::analyzeCancel, "Cancel audio analysis",
                             activeLevelAnalysis && !activeLevelAnalysis->cancelled.load());
        m.addSubMenu("Level Analysis", analysisMenu);
    }

    if (index == MenuBarIndex::view) {
        m.addItem(MenuId::viewToggleMdWorkspace, "Show MiniDisc workspace", true,
                  discWorkspaceVisible);
        m.addItem(MenuId::viewToggleOutputPeaks, "Show output peak meter", true, showOutputPeaks);
        m.addSeparator();
        m.addItem(MenuId::viewModeWaveform, "Waveform", true, !spectrumView);
        m.addItem(MenuId::viewModeSpectrogram, "Spectrogram", true, spectrumView);

        juce::PopupMenu waveMenu;
        juce::PopupMenu amplitude;
        amplitude.addItem(MenuId::viewWaveZoom1x, utf8Str("1× (full scale)"), true,
                          waveformVerticalZoom == 1);
        amplitude.addItem(MenuId::viewWaveZoom2x, utf8Str("2×"), true, waveformVerticalZoom == 2);
        amplitude.addItem(MenuId::viewWaveZoom4x, utf8Str("4×"), true, waveformVerticalZoom == 4);
        amplitude.addItem(MenuId::viewWaveZoom8x, utf8Str("8×"), true, waveformVerticalZoom == 8);
        waveMenu.addSubMenu("Waveform vertical zoom", amplitude);

        juce::PopupMenu waveformScaleMenu;
        waveformScaleMenu.addItem(MenuId::viewWaveScaleLinear, "Linear amplitude", true,
                                  waveformScale == carta::WaveformScale::linear);
        waveformScaleMenu.addItem(MenuId::viewWaveScaleDb, "Logarithmic dB (60 dB range)", true,
                                  waveformScale == carta::WaveformScale::decibels);
        waveMenu.addSubMenu("Waveform scale", waveformScaleMenu);
        waveMenu.addSeparator();
        waveMenu.addItem(MenuId::viewToggleHalfWave, "Show half-wave amplitude", true,
                         waveformHalf);
        waveMenu.addItem(MenuId::viewTogglePeakWarnings, "Show clip peaks over 0 dBFS", true,
                         showClipPeakWarnings);
        waveMenu.addItem(MenuId::viewToggleRms, "Show clip RMS overlay", true, showWaveformRms);
        m.addSubMenu("Waveform Display", waveMenu);

        juce::PopupMenu specMenu;
        juce::PopupMenu resolution;
        resolution.addItem(MenuId::viewSpectrumFft1024, "1024 samples (better time resolution)",
                           true, spectrumFftOrder == 10);
        resolution.addItem(MenuId::viewSpectrumFft2048, "2048 samples", true,
                           spectrumFftOrder == 11);
        resolution.addItem(MenuId::viewSpectrumFft4096, "4096 samples", true,
                           spectrumFftOrder == 12);
        resolution.addItem(MenuId::viewSpectrumFft8192,
                           "8192 samples (better frequency resolution)", true,
                           spectrumFftOrder == 13);
        specMenu.addSubMenu("Spectrogram FFT size", resolution);

        juce::PopupMenu windows;
        windows.addItem(MenuId::viewSpectrumWindowHann, "Hann", true,
                        spectrumWindow == carta::SpectrumWindow::hann);
        windows.addItem(MenuId::viewSpectrumWindowHamming, "Hamming", true,
                        spectrumWindow == carta::SpectrumWindow::hamming);
        windows.addItem(MenuId::viewSpectrumWindowBlackman, "Blackman", true,
                        spectrumWindow == carta::SpectrumWindow::blackman);
        specMenu.addSubMenu("Spectrogram window", windows);

        juce::PopupMenu floors;
        for (int i = 0; i < 5; ++i) {
            const int floor = -40 - i * 20;
            floors.addItem(MenuId::viewSpectrumFloorMinus40 + i,
                           juce::String(floor) + " dB to 0 dB", true, spectrumFloorDb == floor);
        }
        specMenu.addSubMenu("Spectrogram level range", floors);

        juce::PopupMenu frequency;
        frequency.addItem(MenuId::viewSpectrumBandFull, "Full range (20 Hz to Nyquist)", true,
                          spectrumBand == carta::SpectrumBand::full);
        frequency.addItem(MenuId::viewSpectrumBandBass, utf8Str("Bass (20 Hz–1 kHz)"), true,
                          spectrumBand == carta::SpectrumBand::bass);
        frequency.addItem(MenuId::viewSpectrumBandVoice, utf8Str("Voice (100 Hz–8 kHz)"), true,
                          spectrumBand == carta::SpectrumBand::voice);
        frequency.addItem(MenuId::viewSpectrumBandTreble, "Treble (1 kHz to Nyquist)", true,
                          spectrumBand == carta::SpectrumBand::treble);
        frequency.addItem(MenuId::viewSpectrumCustomRange, "Custom frequency range...", true,
                          spectrumBand == carta::SpectrumBand::custom);
        specMenu.addSubMenu("Spectrogram frequency range", frequency);
        m.addSubMenu("Spectrogram Settings", specMenu);
        m.addSeparator();

        m.addItem(MenuId::viewZoomIn, "Zoom in");
        m.addItem(MenuId::viewZoomOut, "Zoom out");
        m.addItem(MenuId::viewZoomFit, "Fit arrangement");

        juce::PopupMenu zoomSubMenu;
        zoomSubMenu.addItem(MenuId::clipZoom, "Zoom to focused clip", validSelection());
        zoomSubMenu.addItem(MenuId::selectedClipsZoom, "Zoom to selected clips (Shift+Z)",
                            !timelineClipSelection.get(editGeneration).empty());
        zoomSubMenu.addItem(MenuId::editZoomToSelection, "Zoom to time selection",
                            timeSelection.hasRange());
        zoomSubMenu.addItem(MenuId::viewZoomMasterTrackAtPlayhead,
                            "Zoom to master track at playhead", session.length() > 0);
        zoomSubMenu.addSeparator();
        zoomSubMenu.addItem(MenuId::viewFitLayersVertically, "Fit layers vertically",
                            !session.tracks.empty());
        zoomSubMenu.addItem(MenuId::viewTogglePrevZoom, "Toggle previous zoom (Z)",
                            zoomMemory.available());
        m.addSubMenu("Zoom", zoomSubMenu);

        juce::PopupMenu navViewMenu;
        juce::PopupMenu follow;
        follow.addItem(MenuId::viewFollowOff, "Off", true, followMode == 0);
        follow.addItem(MenuId::viewFollowPage, "Page", true, followMode == 1);
        follow.addItem(MenuId::viewFollowPinned, "Pinned playhead", true, followMode == 2);
        follow.addSeparator();
        follow.addItem(MenuId::viewResumeFollowing, "Resume following",
                       followMode != 0 && followSuspended);
        navViewMenu.addSubMenu("Follow playhead", follow);

        juce::PopupMenu timeFormat;
        timeFormat.addItem(MenuId::viewTimeFormatAuto, "Automatic", true,
                           timelineTimeFormat == carta::TimelineTimeFormat::automatic);
        timeFormat.addItem(MenuId::viewTimeFormatSamples, "Samples", true,
                           timelineTimeFormat == carta::TimelineTimeFormat::samples);
        timeFormat.addItem(MenuId::viewTimeFormatClock, "Clock", true,
                           timelineTimeFormat == carta::TimelineTimeFormat::clock);
        timeFormat.addItem(MenuId::viewTimeFormatSeconds, "Seconds", true,
                           timelineTimeFormat == carta::TimelineTimeFormat::seconds);
        navViewMenu.addSubMenu("Timeline time units", timeFormat);
        navViewMenu.addSeparator();
        navViewMenu.addItem(MenuId::navClipBoundaryPrevLayer,
                            "Previous clip boundary on selected layer ([)", selectedTrack >= 0);
        navViewMenu.addItem(MenuId::navClipBoundaryNextLayer,
                            "Next clip boundary on selected layer (])", selectedTrack >= 0);
        navViewMenu.addItem(MenuId::navClipBoundaryPrevAny,
                            "Previous clip boundary on any layer (Shift+[)",
                            !session.tracks.empty());
        navViewMenu.addItem(MenuId::navClipBoundaryNextAny,
                            "Next clip boundary on any layer (Shift+])", !session.tracks.empty());
        navViewMenu.addSeparator();
        navViewMenu.addItem(MenuId::viewBrowseOverlaps, "Browse clip overlaps...",
                            !session.tracks.empty());
        m.addSubMenu("Navigate & Time Units", navViewMenu);
    }

    if (index == MenuBarIndex::layer) {
        const bool valid =
            selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size());
        const bool selectedRows =
            !selectedTimelineTracks().empty() ||
            (timeSelection.hasRange()
                 ? timeSelection.lastTrack < static_cast<int>(session.tracks.size())
                 : valid);
        const bool hasIndependent = !selectedTimelineTracks().empty();
        const bool canEditIndependent = hasIndependent && timeSelection.hasRange();
        const auto view = valid ? session.tracks[static_cast<size_t>(selectedTrack)].view
                                : carta::TrackView::useDefault;

        m.addItem(MenuId::layerAddEmpty, "Add empty layer");
        m.addItem(MenuId::layerRename, "Rename layer...", valid);
        m.addItem(MenuId::layerDuplicate, "Duplicate layer", valid);
        m.addItem(MenuId::layerRemove, "Remove layer", valid);
        m.addSeparator();
        m.addItem(MenuId::layerMute, "Mute focused layer (M)", valid,
                  valid && session.tracks[static_cast<size_t>(selectedTrack)].muted);
        m.addItem(MenuId::layerSolo, "Solo focused layer (S)", valid,
                  valid && session.tracks[static_cast<size_t>(selectedTrack)].solo);
        m.addItem(MenuId::layerGainBalance, "Gain and balance...", valid);
        m.addItem(MenuId::layerEditMixList, "Edit or mix multiple layers...",
                  !session.tracks.empty());
        m.addSeparator();

        juce::PopupMenu arrangeMenu;
        arrangeMenu.addItem(MenuId::layerMoveUp, "Move layer up", valid && selectedTrack > 0);
        arrangeMenu.addItem(MenuId::layerMoveDown, "Move layer down",
                            valid && selectedTrack + 1 < static_cast<int>(session.tracks.size()));
        arrangeMenu.addSeparator();
        arrangeMenu.addItem(MenuId::layerToggleCollapse,
                            valid && session.tracks[static_cast<size_t>(selectedTrack)].collapsed
                                ? "Expand layer"
                                : "Collapse layer",
                            valid);
        arrangeMenu.addItem(MenuId::layerCollapseAll, "Collapse all layers",
                            !session.tracks.empty());
        arrangeMenu.addItem(MenuId::layerExpandAll, "Expand all layers", !session.tracks.empty());
        arrangeMenu.addSeparator();
        arrangeMenu.addItem(MenuId::layerSetHeight, "Set layer height...", valid);
        arrangeMenu.addItem(MenuId::layerHeightDecrease, "Decrease layer height (Shift+Alt+Up)",
                            valid);
        arrangeMenu.addItem(MenuId::layerHeightIncrease, "Increase layer height (Shift+Alt+Down)",
                            valid);
        arrangeMenu.addItem(
            MenuId::layerChannelHeights, "Channel heights...",
            valid &&
                (carta::maxTrackChannels(session.tracks[static_cast<size_t>(selectedTrack)]) >= 2 ||
                 !session.tracks[static_cast<size_t>(selectedTrack)].channelHeightWeights.empty()));
        arrangeMenu.addSeparator();

        juce::PopupMenu display;
        display.addItem(MenuId::layerViewDefault, "Use default view", valid,
                        view == carta::TrackView::useDefault);
        display.addItem(MenuId::layerViewWaveform, "Waveform", valid,
                        view == carta::TrackView::waveform);
        display.addItem(MenuId::layerViewSpectrogram, "Spectrogram", valid,
                        view == carta::TrackView::spectrogram);
        display.addItem(MenuId::layerViewCombined, "Waveform + spectrogram", valid,
                        view == carta::TrackView::combined);
        arrangeMenu.addSubMenu("Layer view", display, valid);
        addTrackWaveformZoomMenu(arrangeMenu, selectedTrack);
        m.addSubMenu("Arrange & Appearance", arrangeMenu);

        juce::PopupMenu navLayerMenu;
        navLayerMenu.addItem(MenuId::layerBrowseNavigator, "Browse layers and clips... (F3)",
                             !session.tracks.empty());
        navLayerMenu.addSeparator();
        navLayerMenu.addItem(MenuId::layerSelectPrev, "Select previous layer (Alt+Up)",
                             !session.tracks.empty());
        navLayerMenu.addItem(MenuId::layerSelectNext, "Select next layer (Alt+Down)",
                             !session.tracks.empty());
        navLayerMenu.addItem(MenuId::layerFocusPrev,
                             "Focus previous layer without seeking (Shift+F7)",
                             !session.tracks.empty());
        navLayerMenu.addItem(MenuId::layerFocusNext, "Focus next layer without seeking (Shift+F8)",
                             !session.tracks.empty());
        navLayerMenu.addSeparator();
        navLayerMenu.addItem(MenuId::layerSelectPrevClip,
                             "Select previous clip on layer (Alt+Left)", valid);
        navLayerMenu.addItem(MenuId::layerSelectNextClip, "Select next clip on layer (Alt+Right)",
                             valid);
        navLayerMenu.addItem(MenuId::layerFocusPrevClip,
                             "Focus previous clip without seeking (Shift+F5)", valid);
        navLayerMenu.addItem(MenuId::layerFocusNextClip,
                             "Focus next clip without seeking (Shift+F6)", valid);
        m.addSubMenu("Navigate Layers & Clips", navLayerMenu);

        juce::PopupMenu routingMenu;
        const bool hasChannels =
            valid &&
            carta::maxTrackChannels(session.tracks[static_cast<size_t>(selectedTrack)]) > 0;
        routingMenu.addItem(MenuId::layerChannelRouting, "Channel routing...", hasChannels);
        routingMenu.addItem(
            MenuId::layerClearRouting, "Clear channel routing",
            valid && !session.tracks[static_cast<size_t>(selectedTrack)].routing.empty());
        routingMenu.addSeparator();
        routingMenu.addItem(MenuId::layerSwapStereo, "Swap stereo source channels", hasChannels);
        routingMenu.addItem(MenuId::layerFoldStereoToMono, "Fold stereo routing to mono",
                            hasChannels);
        routingMenu.addItem(MenuId::layerSplitStereo, "Split stereo channels to layers",
                            hasChannels);
        routingMenu.addItem(MenuId::layerCombineChannel, "Combine with next channel layer",
                            valid && selectedTrack + 1 < static_cast<int>(session.tracks.size()));
        m.addSubMenu("Channels & Routing", routingMenu);

        juce::PopupMenu renderMenu;
        renderMenu.addItem(MenuId::layerRenderSelectedTime,
                           "Render selected time and layers to new layer...",
                           timeSelection.hasRange() && !activeExport && pendingImports.empty());
        renderMenu.addItem(
            MenuId::layerRenderSelectedTracks,
            "Render selected layers for full master to new layer...",
            (!selectedTimelineTracks().empty() || timeSelection.hasRange() || valid) &&
                session.length() > 0 && !activeExport && pendingImports.empty());
        renderMenu.addItem(MenuId::layerRenderStereoPair,
                           "Render two mono layers as stereo (upper = left)...",
                           session.tracks.size() >= 2 && !activeExport && pendingImports.empty());
        renderMenu.addItem(
            MenuId::layerRenderSelectedClips, "Render selected clips to new layer...",
            (validSelection() || !timelineClipSelection.get(editGeneration).empty()) &&
                !activeExport && pendingImports.empty());
        m.addSubMenu("Render & Bounce", renderMenu);

        juce::PopupMenu selSourcesMenu;
        juce::PopupMenu rows;
        rows.addItem(MenuId::layerRowsMute, "Mute selected rows", selectedRows);
        rows.addItem(MenuId::layerRowsUnmute, "Unmute selected rows", selectedRows);
        rows.addItem(MenuId::layerRowsSolo, "Solo selected rows", selectedRows);
        rows.addItem(MenuId::layerRowsClearSolo, "Unsolo selected rows", selectedRows);
        rows.addSeparator();
        rows.addItem(MenuId::layerRowsSetGain, "Set gain of selected rows...", selectedRows);
        rows.addItem(MenuId::layerRowsSetGainDb, "Set gain of selected rows in dB...",
                     selectedRows);
        rows.addItem(MenuId::layerRowsScaleGain, "Scale gain of selected rows...", selectedRows);
        rows.addItem(MenuId::layerRowsAdjustGainDb, "Adjust gain of selected rows in dB...",
                     selectedRows);
        rows.addItem(MenuId::layerRowsSetBalance, "Set balance of selected rows...", selectedRows);
        selSourcesMenu.addSubMenu("Selected rows", rows, selectedRows);

        juce::PopupMenu independent;
        independent.addItem(MenuId::layerToggleIndependent, "Toggle focused layer (T)", valid);
        independent.addItem(MenuId::layerSelectAllIndependent, "Select all layers",
                            !session.tracks.empty());
        independent.addItem(MenuId::layerClearIndependent, "Clear layer selection", hasIndependent);
        independent.addSeparator();
        independent.addItem(MenuId::layerIndMute, "Mute", hasIndependent);
        independent.addItem(MenuId::layerIndUnmute, "Unmute", hasIndependent);
        independent.addItem(MenuId::layerIndSolo, "Solo", hasIndependent);
        independent.addItem(MenuId::layerIndClearSolo, "Clear solo", hasIndependent);
        independent.addSeparator();
        independent.addItem(MenuId::layerIndCopyTime, "Copy time", canEditIndependent);
        independent.addItem(MenuId::layerIndCutGap, "Cut time (leave gap)", canEditIndependent);
        independent.addItem(MenuId::layerIndCutClose, "Cut time and close gap", canEditIndependent);
        independent.addItem(MenuId::layerIndDeleteGap, "Delete time (leave gap)",
                            canEditIndependent);
        independent.addItem(MenuId::layerIndDeleteClose, "Delete time and close gap",
                            canEditIndependent);
        independent.addItem(MenuId::layerIndInsertGap, "Insert selection-length gap",
                            canEditIndependent);
        independent.addItem(MenuId::layerIndPreviewClose, "Preview closing gap",
                            canEditIndependent);
        selSourcesMenu.addSubMenu("Independent layers", independent, !session.tracks.empty());
        selSourcesMenu.addSeparator();
        selSourcesMenu.addItem(MenuId::layerSourceInfo, "Source information...", valid);
        selSourcesMenu.addItem(MenuId::layerVerifySources, "Verify layer source files",
                               valid && !activeSourceCheck);
        selSourcesMenu.addItem(MenuId::layerVerifyAllSources, "Verify all source files",
                               !session.tracks.empty() && !activeSourceCheck);
        selSourcesMenu.addItem(MenuId::layerCancelSourceCheck, "Cancel source check or relink",
                               activeSourceCheck && !activeSourceCheck->cancelled.load());
        m.addSubMenu("Selection & Sources", selSourcesMenu);
    }

    if (index == MenuBarIndex::help) {
        m.addItem(MenuId::helpSearchCommands, "Search commands (Cmd/Ctrl+Shift+P)...");
        m.addItem(
            MenuId::helpFocusTimeline,
            "Focus timeline (" +
                juce::KeyPress('0', juce::ModifierKeys::commandModifier, 0).getTextDescription() +
                ")");
        m.addItem(MenuId::helpAnnounceTimeline, "Announce timeline status (F4)");
        m.addItem(MenuId::helpKeyboardShortcuts, "Keyboard shortcuts...");
    }
    return m;
}

void Studio::menuItemSelected(int id, int) {
    if (savePromptActive) {
        status.setText("Finish the current dialog first.", juce::dontSendNotification);
        return;
    }
    if (dragMode != kDragNone && id != MenuId::markerCancelExchange) {
        status.setText("Finish or cancel the current drag first.", juce::dontSendNotification);
        return;
    }
    if (id != MenuId::audioSettings) {
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Studio>(this)] {
            if (safe != nullptr && !safe->savePromptActive &&
                juce::Component::getCurrentlyModalComponent() == nullptr) {
                safe->grabKeyboardFocus();
            }
        });
    }
    if (!preservesSpecialAuditionOnMenuCommand(id)) {
        const juce::ScopedLock guard(lock);
        stopSpecialAudition();
    }

    if (id >= MenuId::historyBase) {
        const auto found = historyCommands.find(id);
        if (found != historyCommands.end()) {
            jumpHistory(found->second);
        } else {
            status.setText("History menu expired; open it again.", juce::dontSendNotification);
        }
        return;
    }

    switch (id) {
    case MenuId::helpSearchCommands:
        showCommandSearch();
        break;
    case MenuId::helpFocusTimeline:
        focusTimeline();
        break;
    case MenuId::fileShowImportIssues:
        showImportIssues();
        break;
    case MenuId::fileClearImportIssues:
        clearImportIssues();
        break;
    case MenuId::previewCrossfadeSameLayer:
    case MenuId::previewCrossfadeAcrossLayers:
        auditionCrossfade(id == MenuId::previewCrossfadeAcrossLayers);
        break;
    case MenuId::editMarkInAtPlayhead:
    case MenuId::editMarkOutAtPlayhead:
        markLiveSelection(id == MenuId::editMarkInAtPlayhead);
        break;
    case MenuId::layerEditMixList:
        showTrackMixList();
        break;
    case MenuId::layerToggleIndependent:
        toggleTimelineTrack(selectedTrack);
        break;
    case MenuId::layerSelectAllIndependent: {
        timelineTracks.clear();
        for (int i = 0; i < static_cast<int>(session.tracks.size()); ++i) {
            timelineTracks.insert(i);
        }
        timelineTrackRevision = editGeneration;
        timelineClipSelection.clear();
        const auto message = describeTimelineTracks();
        status.setText(message, juce::dontSendNotification);
        announceNavigation(message);
        menuItemsChanged();
        repaint();
        break;
    }
    case MenuId::layerClearIndependent:
        timelineTracks.clear();
        status.setText("Independent track selection cleared.", juce::dontSendNotification);
        menuItemsChanged();
        repaint();
        break;
    case MenuId::layerIndMute:
    case MenuId::layerIndUnmute:
    case MenuId::layerIndSolo:
    case MenuId::layerIndClearSolo:
    case MenuId::layerIndCopyTime:
    case MenuId::layerIndCutGap:
    case MenuId::layerIndCutClose:
    case MenuId::layerIndDeleteGap:
    case MenuId::layerIndDeleteClose:
    case MenuId::layerIndInsertGap:
    case MenuId::layerIndPreviewClose:
        performTimelineTrackAction(id);
        break;
    case MenuId::fileImportAudio:
        add.onClick();
        break;
    case MenuId::fileOpenProject:
        load.onClick();
        break;
    case MenuId::fileSaveProject:
        save.onClick();
        break;
    case MenuId::fileExportWav:
        exportButton.onClick();
        break;
    case MenuId::fileExportPortable:
        exportPortableProject();
        break;
    case MenuId::mdGoToTrack:
        promptDiscTrackNumber();
        break;
    case MenuId::mdInsertTrackFromSelection:
        useSelectionForDisc(false);
        break;
    case MenuId::mdAppendPartFromSelection:
        useSelectionForDisc(true);
        break;
    case MenuId::fileExportMdReviewWav:
        exportWav(false, false, false, false, false, true);
        break;
    case MenuId::mdEditTrackTitleForms:
        editSelectedTrackTitleForms();
        break;
    case MenuId::mdTracksFromClips:
        discWorkspace.onCommand({"disc", "sync", "clips"}, editGeneration);
        break;
    case MenuId::editDeselectAll:
        clearAudioSelection(true, false);
        break;
    case MenuId::mdPreviewFromSelected: {
        const int idx0 = discWorkspace.singleSelectedTrackIndex();
        if (idx0 < 0 || idx0 >= static_cast<int>(session.mdDisc.tracks.size())) {
            status.setText("Select one MiniDisc track first.", juce::dontSendNotification);
        } else {
            startDiscPreview(
                editGeneration,
                juce::String::fromUTF8(session.mdDisc.tracks[static_cast<size_t>(idx0)].id.c_str()),
                true);
        }
        break;
    }
    case MenuId::editUndo:
        undo.onClick();
        break;
    case MenuId::editSplit:
        split.onClick();
        break;
    case MenuId::selectedClipsCopy:
    case MenuId::selectedClipsCut:
    case MenuId::selectedClipsDelete:
    case MenuId::selectedClipsDuplicate:
    case MenuId::selectedClipsSplit:
    case MenuId::selectedClipsMove:
    case MenuId::selectedClipsClear:
    case MenuId::selectedClipsJoin:
        performTimelineClipAction(id);
        break;
    case MenuId::selectedClipsGainFades:
        editSelectedClipEnvelopes();
        break;
    case MenuId::selectedClipsSlip:
        editSlipSelectedClips();
        break;
    case MenuId::clipSlip:
        editSlipFocusedClip();
        break;
    case MenuId::selectedClipsMoveStarts:
    case MenuId::selectedClipsMoveEnds:
        editSelectedClipEdges(id == MenuId::selectedClipsMoveEnds);
        break;
    case MenuId::selectedClipsTrimStartsPlayhead:
    case MenuId::selectedClipsTrimEndsPlayhead:
        trimSelectedEdgesToPlayhead(id == MenuId::selectedClipsTrimEndsPlayhead);
        break;
    case MenuId::selectedClipsNudgeLeft:
    case MenuId::selectedClipsNudgeRight:
        nudgeClipGroup(id == MenuId::selectedClipsNudgeLeft ? -1 : 1);
        break;
    case MenuId::clipToggleSelection:
        toggleFocusedClipSelection();
        break;
    case MenuId::selectedClipsGroup:
    case MenuId::selectedClipsUngroup:
        setPersistentClipGrouping(id == MenuId::selectedClipsGroup);
        break;
    case MenuId::selectedClipsSequence:
        sequenceSelectedClips(false);
        break;
    case MenuId::selectedClipsDistribute:
        sequenceSelectedClips(true);
        break;
    case MenuId::markerAddAtClipStarts:
        addClipBoundaryMarkers(false);
        break;
    case MenuId::markerAddAtClipEnds:
        addClipBoundaryMarkers(true);
        break;
    case MenuId::markerDeleteInSelection: {
        int count = 0;
        juce::String error;
        if (!deleteMarkersInSelection(count, error)) {
            status.setText(error, juce::dontSendNotification);
        }
        break;
    }
    case MenuId::markerAddSourceCues:
        addClipSourceCues();
        break;
    case MenuId::selectClipsAllLayers:
    case MenuId::selectClipsSelectedLayers:
    case MenuId::selectClipsInTimeSel:
        selectClipsInScope(id);
        break;
    case MenuId::selectClipsFocusedSourceAll:
        selectSourceClips();
        break;
    case MenuId::viewToggleMdWorkspace:
        setDiscWorkspaceVisible(!discWorkspaceVisible);
        break;
    case MenuId::selectClipsFocusedSourceLayers:
        selectSourceClips(1);
        break;
    case MenuId::selectClipsFocusedSourceTime:
        selectSourceClips(2);
        break;
    case MenuId::clipFindSilence:
        configureClipSilenceSearch();
        break;
    case MenuId::clipSplitSelectedLayers:
    case MenuId::clipSplitAllLayers: {
        juce::String error;
        const bool changed = splitTracksAtPlayhead(id == MenuId::clipSplitAllLayers, error);
        status.setText(
            changed ? "Split clips at the playhead; selection and markers stayed in place." : error,
            juce::dontSendNotification);
        break;
    }
    case MenuId::editDeleteClip:
        remove.onClick();
        break;
    case MenuId::transportPlayPause:
        play.onClick();
        break;
    case MenuId::transportStop:
        stop.onClick();
        break;
    case MenuId::transportStopAtPlayhead:
        stopAtPlayhead();
        break;
    case MenuId::markerAddAtPlayhead:
        marker.onClick();
        break;
    case MenuId::masterStereo:
        mode.setSelectedId(2, juce::sendNotificationSync);
        break;
    case MenuId::masterMono:
        mode.setSelectedId(1, juce::sendNotificationSync);
        break;
    case MenuId::wavFmtPcm16:
    case MenuId::wavFmtPcm24:
    case MenuId::wavFmtFloat32:
        wavEncoding = id == MenuId::wavFmtPcm16   ? carta::WavEncoding::pcm16
                      : id == MenuId::wavFmtPcm24 ? carta::WavEncoding::pcm24
                                                  : carta::WavEncoding::float32;
        menuItemsChanged();
        status.setText(id == MenuId::wavFmtPcm16   ? "WAV export: 16-bit PCM."
                       : id == MenuId::wavFmtPcm24 ? "WAV export: 24-bit PCM."
                                                   : "WAV export: 32-bit float.",
                       juce::dontSendNotification);
        break;
    case MenuId::wavToggleDither:
        wavDither = !wavDither;
        menuItemsChanged();
        status.setText(wavDither ? "TPDF dither enabled for PCM WAV export."
                                 : "PCM WAV export uses no dither.",
                       juce::dontSendNotification);
        break;
    case MenuId::wavToggleEmbedMarkers:
        wavEmbedMarkers = !wavEmbedMarkers;
        menuItemsChanged();
        status.setText(wavEmbedMarkers
                           ? "WAV export will embed explicit markers in the exported range."
                           : "WAV export will not embed markers.",
                       juce::dontSendNotification);
        break;
    case MenuId::markerRename:
        renameMarker();
        break;
    case MenuId::markerDelete: {
        const int index = selectedMarker;
        juce::String error;
        const bool changed = mutateMarkers(
            [&](carta::Session& candidate) {
                return carta::deleteMarkers(candidate, {index}, error);
            },
            [&] { selectedMarker = -1; }, "Delete marker", error);
        const juce::String message = changed ? "Deleted marker." : error;
        status.setText(message, juce::dontSendNotification);
        announceNavigation(message);
        break;
    }
    case MenuId::viewZoomIn:
        zoomAt(2.0);
        break;
    case MenuId::viewZoomOut:
        zoomAt(0.5);
        break;
    case MenuId::viewZoomFit:
        zoomToArrangement();
        break;
    case MenuId::clipZoom:
        zoomToSelected();
        break;
    case MenuId::selectedClipsZoom:
        zoomToSelectedClips();
        break;
    case MenuId::editZoomToSelection:
        zoomToTimeSelection();
        break;
    case MenuId::viewTogglePrevZoom:
        togglePreviousZoom();
        break;
    case MenuId::viewTimeFormatAuto:
    case MenuId::viewTimeFormatSamples:
    case MenuId::viewTimeFormatClock:
    case MenuId::viewTimeFormatSeconds: {
        timelineTimeFormat =
            static_cast<carta::TimelineTimeFormat>(id - MenuId::viewTimeFormatAuto);
        status.setText("Timeline time units: " +
                           juce::String(id == MenuId::viewTimeFormatAuto      ? "automatic"
                                        : id == MenuId::viewTimeFormatSamples ? "samples"
                                        : id == MenuId::viewTimeFormatClock   ? "clock"
                                                                              : "seconds") +
                           ". Editing remains sample-aligned.",
                       juce::dontSendNotification);
        menuItemsChanged();
        repaint();
        break;
    }
    case MenuId::fileSaveRecoveryNow:
        startRecoverySnapshot(true);
        break;
    case MenuId::fileRecoverAutosave:
        recoverProject();
        break;
    case MenuId::auditionPlayhead:
    case MenuId::auditionSelStart:
    case MenuId::auditionSelEnd:
        auditionBoundary(id);
        break;
    case MenuId::auditionSetContext:
        editAuditionContext();
        break;
    case MenuId::previewRippleSelected:
    case MenuId::previewRippleAll:
        auditionRipple(id == MenuId::previewRippleAll);
        break;
    case MenuId::previewRippleFocused:
        auditionRipple(false, true);
        break;
    case MenuId::navClipBoundaryPrevLayer:
    case MenuId::navClipBoundaryNextLayer:
    case MenuId::navClipBoundaryPrevAny:
    case MenuId::navClipBoundaryNextAny:
        navigateClipBoundary(id == MenuId::navClipBoundaryNextLayer ||
                                 id == MenuId::navClipBoundaryNextAny,
                             id >= MenuId::navClipBoundaryPrevAny);
        break;
    case MenuId::viewBrowseOverlaps:
        showOverlapList();
        break;
    case MenuId::editFindZeroCrossingsAny:
        findSelectionZeroCrossings();
        break;
    case MenuId::editFindZeroCrossingsCommon:
        findSelectionZeroCrossings(true);
        break;
    case MenuId::masterMinimumDuration:
        editMinimumMasterDuration();
        break;
    case MenuId::markerSuggestQuietGaps:
        configureQuietGapSearch();
        break;
    case MenuId::clipSelectTime:
        selectClipTime(0);
        break;
    case MenuId::clipSelectVisiblePortion:
    case MenuId::clipSelectBeforePlayhead:
    case MenuId::clipSelectAfterPlayhead:
        selectClipTime(id);
        break;
    case MenuId::clipRename:
        renameClip();
        break;
    case MenuId::transportPlayClip:
    case MenuId::transportLoopClip:
        auditionClip(id == MenuId::transportLoopClip);
        break;
    case MenuId::transportPlaySelectedClips:
    case MenuId::transportLoopSelectedClips:
        auditionClipGroup(id == MenuId::transportLoopSelectedClips);
        break;
    case MenuId::layerSelectPrev:
    case MenuId::layerSelectNext:
        selectAdjacentTrack(id == MenuId::layerSelectNext);
        break;
    case MenuId::layerSelectPrevClip:
    case MenuId::layerSelectNextClip:
        selectAdjacentClip(id == MenuId::layerSelectNextClip);
        break;
    case MenuId::layerFocusPrevClip:
    case MenuId::layerFocusNextClip:
        focusAdjacentClip(id == MenuId::layerFocusNextClip);
        break;
    case MenuId::layerFocusPrev:
    case MenuId::layerFocusNext:
        focusAdjacentTrack(id == MenuId::layerFocusNext);
        break;
    case MenuId::selExtendLeft:
    case MenuId::selExtendRight:
        extendSelection(id == MenuId::selExtendLeft ? -1 : 1);
        break;
    case MenuId::selExtendUp:
    case MenuId::selExtendDown:
        extendSelectionTracks(id == MenuId::selExtendUp ? -1 : 1);
        break;
    case MenuId::editRememberSelection:
    case MenuId::editRestoreSelection:
        rememberSelection(id == MenuId::editRestoreSelection);
        break;
    case MenuId::editClearRememberedSelection:
        clearRememberedSelection();
        break;
    case MenuId::audioSettings:
        showAudioSettings();
        break;
    case MenuId::audioRescanDevices:
        rescanAudioDevices();
        break;
    case MenuId::markerList:
        showMarkerList();
        break;
    case MenuId::markerShiftAll:
    case MenuId::markerShiftFromPlayhead:
    case MenuId::markerShiftInSelection:
        shiftMarkersDialog(id);
        break;
    case MenuId::markerSnapToSamples: {
        juce::String error;
        const bool changed = mutateMarkers(
            [&](carta::Session& candidate) {
                return carta::snapMarkersToSamples(candidate, error);
            },
            [] {}, "Snap markers", error);
        status.setText(error.isNotEmpty() ? error
                       : changed
                           ? "Snapped fractional markers to the nearest sample; undo restores "
                             "their positions."
                           : "Markers unchanged.",
                       juce::dontSendNotification);
        break;
    }
    case MenuId::markerNumberByTrack: {
        juce::String error;
        const bool changed = mutateMarkers(
            [&](carta::Session& candidate) {
                return carta::nameMarkersByTrackNumber(candidate, error);
            },
            [] {}, "Number markers", error);
        status.setText(error.isNotEmpty() ? error
                       : changed ? "Replaced marker names with track numbers. Undo restores the "
                                   "previous names."
                                 : "Marker names already match track numbers.",
                       juce::dontSendNotification);
        break;
    }
    case MenuId::viewFitLayersVertically: {
        bool allVisible = false;
        juce::String error;
        const int height = std::max(0, timeline().getHeight() - timelineHeaderHeight());
        mutateMixState(
            [&](carta::Session& candidate) {
                return carta::fitTrackHeights(candidate, height, allVisible);
            },
            error, "Fit track heights");
        firstTrack = 0;
        repaint();
        status.setText(allVisible ? "Fitted track heights to the timeline."
                                  : "Tracks are at minimum height; scroll to see the remaining "
                                    "tracks.",
                       juce::dontSendNotification);
        break;
    }
    case MenuId::layerSetHeight:
        editTrackHeight();
        break;
    case MenuId::viewFollowOff:
    case MenuId::viewFollowPage:
    case MenuId::viewFollowPinned:
        setFollowMode(id - MenuId::viewFollowOff);
        break;
    case MenuId::viewResumeFollowing:
        resumeFollowing();
        break;
    case MenuId::viewZoomMasterTrackAtPlayhead:
        zoomToMasterTrackAtPlayhead();
        break;
    case MenuId::viewToggleOutputPeaks:
        showOutputPeaks = !showOutputPeaks;
        outputPeakLabel.setVisible(showOutputPeaks);
        resized();
        menuItemsChanged();
        repaint();
        break;
    case MenuId::metersResetEditLockCount:
        resetEditLockInterruptions();
        break;
    case MenuId::metersResetPeaks:
        resetOutputPeaks();
        break;
    case MenuId::seekOneSampleEarlier:
    case MenuId::seekOneSampleLater:
    case MenuId::seekMasterStart:
    case MenuId::seekMasterEnd:
        seekTimeline(id);
        break;
    case MenuId::layerSourceInfo:
        showTrackSources();
        break;
    case MenuId::layerVerifySources:
        verifySelectedSources(false);
        break;
    case MenuId::layerVerifyAllSources:
        verifySelectedSources(true);
        break;
    case MenuId::layerCancelSourceCheck:
        if (activeSourceCheck) {
            activeSourceCheck->cancelled.store(true);
            status.setText("Cancelling source operation...", juce::dontSendNotification);
        }
        break;
    case MenuId::editSetSelectionStartLength:
        editTimeSelection(true);
        break;
    case MenuId::editToBoundaryMasterStart:
    case MenuId::editToBoundaryMasterEnd:
    case MenuId::editToBoundaryPrevMarker:
    case MenuId::editToBoundaryNextMarker:
    case MenuId::editToBoundaryPrevClip:
    case MenuId::editToBoundaryNextClip:
        selectToBoundary(id);
        break;
    case MenuId::editSelectMasterTrackAll:
    case MenuId::editSelectMasterTrackScoped:
        selectMasterTrack(id == MenuId::editSelectMasterTrackScoped);
        break;
    case MenuId::editScopedToMasterStart:
    case MenuId::editScopedToMasterEnd:
    case MenuId::editScopedToPrevMarker:
    case MenuId::editScopedToNextMarker:
    case MenuId::editScopedToPrevClip:
    case MenuId::editScopedToNextClip:
        selectToBoundary(id);
        break;
    case MenuId::fileExportSelectedClipWav:
        exportWav(false, false, false, true);
        break;
    case MenuId::fileExportSelectedClipsWav:
        exportWav(false, false, false, false, true);
        break;
    case MenuId::editTrimOutsideSelection:
        trimSelectionTracks();
        break;
    case MenuId::helpKeyboardShortcuts:
        showKeyboardHelp();
        break;
    case MenuId::helpAnnounceTimeline:
        announceTimeline();
        break;
    case MenuId::fileExportAllMasterTracks:
        exportAllTracks();
        break;
    case MenuId::layerToggleCollapse:
    case MenuId::layerCollapseAll:
    case MenuId::layerExpandAll: {
        juce::String error;
        const int index = selectedTrack;
        const bool changed = mutateMixState(
            [&](carta::Session& candidate) {
                if (id == MenuId::layerToggleCollapse) {
                    if (index < 0 || index >= static_cast<int>(candidate.tracks.size())) {
                        return false;
                    }
                    auto& track = candidate.tracks[static_cast<size_t>(index)];
                    track.collapsed = !track.collapsed;
                    return true;
                }
                bool any = false;
                for (auto& track : candidate.tracks) {
                    if (track.collapsed != (id == MenuId::layerCollapseAll)) {
                        track.collapsed = (id == MenuId::layerCollapseAll);
                        any = true;
                    }
                }
                return any;
            },
            error,
            id == MenuId::layerToggleCollapse ? "Toggle track collapse"
            : id == MenuId::layerCollapseAll  ? "Collapse tracks"
                                              : "Expand tracks");
        if (changed) {
            if (id == MenuId::layerToggleCollapse) {
                revealTrack(selectedTrack);
            } else {
                firstTrack = 0;
            }
            menuItemsChanged();
            repaint();
            status.setText(id == MenuId::layerToggleCollapse ? "Track display updated."
                           : id == MenuId::layerCollapseAll  ? "Tracks collapsed."
                                                             : "Tracks expanded.",
                           juce::dontSendNotification);
        }
        break;
    }
    case MenuId::layerViewDefault:
    case MenuId::layerViewWaveform:
    case MenuId::layerViewSpectrogram:
    case MenuId::layerViewCombined: {
        const auto view = id == MenuId::layerViewCombined
                              ? carta::TrackView::combined
                              : static_cast<carta::TrackView>(id - MenuId::layerViewDefault);
        juce::String error;
        const int index = selectedTrack;
        const bool changed = mutateMixState(
            [&](carta::Session& candidate) {
                if (index < 0 || index >= static_cast<int>(candidate.tracks.size())) {
                    return false;
                }
                auto& track = candidate.tracks[static_cast<size_t>(index)];
                if (track.view == view) {
                    return false;
                }
                track.view = view;
                return true;
            },
            error, "Change track view");
        if (changed) {
            menuItemsChanged();
            status.setText(view == carta::TrackView::useDefault ? "Track uses the default view."
                           : view == carta::TrackView::waveform ? "Track shows waveform."
                           : view == carta::TrackView::spectrogram
                               ? "Track shows spectrogram."
                               : "Track shows waveform and spectrogram.",
                           juce::dontSendNotification);
        }
        break;
    }
    case MenuId::layerWaveZoomDefault:
    case MenuId::layerWaveZoom1x:
    case MenuId::layerWaveZoom2x:
    case MenuId::layerWaveZoom4x:
    case MenuId::layerWaveZoom8x: {
        const int value =
            id == MenuId::layerWaveZoomDefault ? 0 : 1 << (id - MenuId::layerWaveZoom1x);
        juce::String error;
        const int index = selectedTrack;
        const bool changed = mutateMixState(
            [&](carta::Session& candidate) {
                if (index < 0 || index >= static_cast<int>(candidate.tracks.size())) {
                    return false;
                }
                auto& track = candidate.tracks[static_cast<size_t>(index)];
                if (track.waveformZoom == value) {
                    return false;
                }
                track.waveformZoom = value;
                return true;
            },
            error, "Change track waveform zoom");
        if (changed) {
            menuItemsChanged();
            status.setText(value == 0
                               ? "Track waveform follows the default zoom."
                               : "Track waveform zoom: " + juce::String(value) + utf8Str("×."),
                           juce::dontSendNotification);
        }
        break;
    }
    case MenuId::layerChannelHeights:
        editChannelHeights();
        break;
    case MenuId::markerNudgeLeft:
    case MenuId::markerNudgeRight: {
        juce::String error;
        if (!nudgeMarker(selectedMarker, id == MenuId::markerNudgeLeft ? -1 : 1, error)) {
            status.setText(error, juce::dontSendNotification);
            announceNavigation(error);
        }
        break;
    }
    case MenuId::editInsertGapSelected:
    case MenuId::editInsertGapAll:
        insertSelectionGap(id == MenuId::editInsertGapAll);
        break;
    case MenuId::editInsertGapFocused:
        insertSelectionGap(false, true);
        break;
    case MenuId::clipJoinNext: {
        juce::String error;
        const bool changed = joinFocusedClip(error);
        status.setText(changed ? "Joined contiguous source clips." : error,
                       juce::dontSendNotification);
        break;
    }
    case MenuId::editSplitSelToNewLayers: {
        juce::String error;
        const bool changed = splitSelectionToNewTracks(error);
        status.setText(changed ? "Moved selection to new tracks at the same time." : error,
                       juce::dontSendNotification);
        break;
    }
    case MenuId::selStartEarlier:
    case MenuId::selStartLater:
    case MenuId::selEndEarlier:
    case MenuId::selEndLater:
    case MenuId::selMoveEarlier:
    case MenuId::selMoveLater:
        adjustTimeSelection(id <= MenuId::selStartLater ? carta::SelectionPart::start
                            : id <= MenuId::selEndLater ? carta::SelectionPart::end
                                                        : carta::SelectionPart::whole,
                            id % 2 == 1 ? -1 : 1);
        break;
    case MenuId::markerImport:
        importMarkers();
        break;
    case MenuId::markerExport:
        exportMarkers();
        break;
    case MenuId::markerImportAudacity:
        importMarkers(true);
        break;
    case MenuId::markerExportAudacity:
        exportMarkers(true);
        break;
    case MenuId::markerCancelExchange:
        if (activeMarkerExchange) {
            activeMarkerExchange->cancelled.store(true);
            status.setText("Cancelling marker exchange...", juce::dontSendNotification);
            menuItemsChanged();
        }
        break;
    case MenuId::fileExportManifest:
        exportManifest();
        break;
    case MenuId::fileSaveProjectAs:
        saveProject({}, true);
        break;
    case MenuId::fileExportSelWavFull:
        exportWav(true);
        break;
    case MenuId::fileExportSelWavLayers:
        exportWav(true, true);
        break;
    case MenuId::fileExportTrackAtPlayhead:
        exportWav(false, false, true);
        break;
    case MenuId::snapOff:
    case MenuId::snapGrid:
    case MenuId::snapGridAndBounds:
        snapMode = id - MenuId::snapOff;
        syncSnapButtonAppearance();
        menuItemsChanged();
        status.setText(snapMode == 0 ? "Snapping off."
                       : snapMode == 1
                           ? "Snap to grid; hold Shift while dragging to override."
                           : "Snap to grid and boundaries; hold Shift while dragging to override.",
                       juce::dontSendNotification);
        repaint();
        break;
    case MenuId::snapGridSamples1:
    case MenuId::snapGridSamples100Ms:
    case MenuId::snapGridSamples1Sec:
        snapGridSamples = id == MenuId::snapGridSamples1       ? 1
                          : id == MenuId::snapGridSamples100Ms ? 4410
                                                               : 44100;
        status.setText("Snap grid: " +
                           juce::String(id == MenuId::snapGridSamples1       ? "project samples"
                                        : id == MenuId::snapGridSamples100Ms ? "100 milliseconds"
                                                                             : "1 second") +
                           ".",
                       juce::dontSendNotification);
        menuItemsChanged();
        break;
    case MenuId::snapGridCustom:
        editSnapGrid();
        break;
    case MenuId::snapTargetClipEdges:
    case MenuId::snapTargetMarkers:
    case MenuId::snapTargetSelection:
    case MenuId::snapTargetPlayhead:
    case MenuId::snapTargetTimelineStart: {
        bool* option = id == MenuId::snapTargetClipEdges   ? &snapOptions.clipEdges
                       : id == MenuId::snapTargetMarkers   ? &snapOptions.markers
                       : id == MenuId::snapTargetSelection ? &snapOptions.selection
                       : id == MenuId::snapTargetPlayhead  ? &snapOptions.playhead
                                                           : &snapOptions.timelineStart;
        *option = !*option;
        status.setText("Snap boundary targets updated for the next drag. Boundary snapping must be "
                       "enabled.",
                       juce::dontSendNotification);
        menuItemsChanged();
        break;
    }
    case MenuId::clipMoveToSample:
        editClipSamples(false);
        break;
    case MenuId::clipTrimToSamples:
        editClipSamples(true);
        break;
    case MenuId::clipNudgeLeft:
        nudgeClip(-1);
        break;
    case MenuId::clipNudgeRight:
        nudgeClip(1);
        break;
    case MenuId::clipAlignStartPlayhead:
    case MenuId::clipAlignEndPlayhead:
    case MenuId::clipAlignStartSelStart:
    case MenuId::clipAlignStartSelEnd:
    case MenuId::clipAlignEndSelStart:
    case MenuId::clipAlignEndSelEnd:
    case MenuId::clipAlignStartMarker:
    case MenuId::clipAlignEndMarker:
    case MenuId::clipAlignStartPrevEdge:
    case MenuId::clipAlignStartNextEdge:
    case MenuId::clipAlignEndPrevEdge:
    case MenuId::clipAlignEndNextEdge:
        alignSelectedClip(id);
        break;
    case MenuId::clipNudgeStartEarlier:
    case MenuId::clipNudgeStartLater:
    case MenuId::clipNudgeEndEarlier:
    case MenuId::clipNudgeEndLater:
        nudgeSelectedClipEdge(id);
        break;
    case MenuId::clipGainAndFades:
        editClipEnvelope();
        break;
    case MenuId::fadeInLinear:
    case MenuId::fadeInEqualPower:
    case MenuId::fadeInSCurve:
    case MenuId::fadeInExponential:
    case MenuId::fadeOutLinear:
    case MenuId::fadeOutEqualPower:
    case MenuId::fadeOutSCurve:
    case MenuId::fadeOutExponential: {
        constexpr carta::FadeShape shapes[]{carta::FadeShape::linear, carta::FadeShape::equalPower,
                                            carta::FadeShape::sCurve,
                                            carta::FadeShape::exponential};
        const bool fadeIn = id <= MenuId::fadeInExponential;
        const auto shape =
            shapes[(fadeIn ? id - MenuId::fadeInLinear : id - MenuId::fadeOutLinear) & 3];
        juce::String error;
        if (setTargetClipFadeShapes(fadeIn, !fadeIn, shape, error)) {
            status.setText(juce::String(fadeIn ? "Fade in shape: " : "Fade out shape: ") +
                               carta::fadeShapeToString(shape) + ".",
                           juce::dontSendNotification);
        } else if (error.isNotEmpty()) {
            status.setText(error, juce::dontSendNotification);
        }
        break;
    }
    case MenuId::clipAmplify:
        showAmplifyDialog();
        break;
    case MenuId::clipNormalize:
        showNormalizeDialog();
        break;
    case MenuId::clipCrossfadeTrack: {
        juce::String error;
        const bool changed = mutateMixState(
            [&](carta::Session& candidate) {
                return carta::crossfadeOverlap(candidate, selectedTrack, selectedClip, error);
            },
            error, "Crossfade overlap");
        status.setText(changed ? "Created linear crossfade across the overlap." : error,
                       juce::dontSendNotification);
        break;
    }
    case MenuId::clipCrossfadeAcrossTracks: {
        juce::String error;
        const bool changed = mutateMixState(
            [&](carta::Session& candidate) {
                return carta::crossfadeAcrossTracks(candidate, selectedTrack, selectedClip, error);
            },
            error, "Crossfade tracks");
        status.setText(changed ? "Created linear crossfade across tracks." : error,
                       juce::dontSendNotification);
        break;
    }
    case MenuId::fileCancelSave:
        if (activeSave) {
            activeSave->cancelled.store(true);
            status.setText("Cancelling project save...", juce::dontSendNotification);
        }
        break;
    case MenuId::fileCancelOpen:
        if (activeOpen) {
            activeOpen->cancelled.store(true);
            status.setText("Cancelling project opening...", juce::dontSendNotification);
        }
        break;
    case MenuId::fileCancelPendingImports:
        for (const auto& task : pendingImports) {
            task->cancelled.store(true);
        }
        status.setText(utf8Str("Cancelling pending imports…"), juce::dontSendNotification);
        break;
    case MenuId::layerChannelRouting:
        editTrackRouting();
        break;
    case MenuId::layerSwapStereo: {
        juce::String error;
        const bool changed = editStereoTrackRouting(selectedTrack, false, error);
        status.setText(
            changed ? "Swapped stereo source-channel routing. Waveform lanes remain in source "
                      "order."
                    : error,
            juce::dontSendNotification);
        break;
    }
    case MenuId::layerFoldStereoToMono: {
        juce::String error;
        const bool changed = editStereoTrackRouting(selectedTrack, true, error);
        status.setText(
            changed ? "Folded stereo routing to mono before track balance; source lanes remain "
                      "unchanged."
                    : error,
            juce::dontSendNotification);
        break;
    }
    case MenuId::layerSplitStereo: {
        juce::String error;
        const bool changed = editStereoChannelTracks(selectedTrack, false, error);
        status.setText(changed
                           ? "Split stereo source channels into two tracks; source media is shared."
                           : error,
                       juce::dontSendNotification);
        break;
    }
    case MenuId::layerCombineChannel: {
        juce::String error;
        const bool changed = editStereoChannelTracks(selectedTrack, true, error);
        status.setText(changed ? "Combined matching source-channel tracks; source media is shared."
                               : error,
                       juce::dontSendNotification);
        break;
    }
    case MenuId::analyzeMasterLevels:
        analyzeLevels(0);
        break;
    case MenuId::analyzeSelLevelsFull:
        analyzeLevels(1);
        break;
    case MenuId::analyzeSelLevelsLayers:
        analyzeLevels(2);
        break;
    case MenuId::analyzeTrackAtPlayhead:
        analyzeLevels(3);
        break;
    case MenuId::layerBrowseNavigator:
        showTrackClipNavigator();
        break;
    case MenuId::analyzeCancel:
        if (activeLevelAnalysis) {
            activeLevelAnalysis->cancelled.store(true);
            menuItemsChanged();
            status.setText("Cancelling audio analysis...", juce::dontSendNotification);
        }
        break;
    case MenuId::layerClearRouting: {
        const int track = selectedTrack;
        juce::String error;
        const bool changed = mutateMixState(
            [&](carta::Session& candidate) {
                if (track < 0 || track >= static_cast<int>(candidate.tracks.size())) {
                    error = "Select a track first.";
                    return false;
                }
                auto& target = candidate.tracks[static_cast<size_t>(track)];
                if (target.routing.empty()) {
                    error = "Track routing is already automatic.";
                    return false;
                }
                target.routing.clear();
                return true;
            },
            error, "Clear channel routing");
        status.setText(
            changed ? "Channel routing cleared; multichannel audio requires confirmation before "
                      "export."
                    : error,
            juce::dontSendNotification);
        break;
    }
    case MenuId::fileCancelExport:
        if (activeExport) {
            activeExport->cancelled.store(true);
            status.setText(utf8Str("Cancelling WAV export…"), juce::dontSendNotification);
        }
        break;
    case MenuId::markerTrackList:
        showMasterTracks();
        break;
    case MenuId::markerSetPosition:
        editMarkerPosition();
        break;
    case MenuId::markerPrev:
    case MenuId::markerNext:
        navigateMarker(id == MenuId::markerNext);
        break;
    case MenuId::markerFocusPrev:
    case MenuId::markerFocusNext:
        focusAdjacentMarker(id == MenuId::markerFocusNext);
        break;
    case MenuId::markerAuditionBoundary:
        auditionFocusedMarker();
        break;
    case MenuId::savedLoopSetFromSel: {
        juce::String error;
        if (!setSavedLoop(timeSelection.start, timeSelection.end, error)) {
            status.setText(error, juce::dontSendNotification);
        }
        break;
    }
    case MenuId::savedLoopPlay: {
        if (!requireAudioOutput()) {
            break;
        }
        juce::String error;
        if (!startSavedLoop(error)) {
            status.setText(error, juce::dontSendNotification);
        }
        break;
    }
    case MenuId::savedLoopClear:
        clearSavedLoop();
        break;
    case MenuId::editSetTimeSelection:
        editTimeSelection();
        break;
    case MenuId::editSelectAll: {
        const juce::ScopedLock guard(lock);
        if (!session.tracks.empty() && session.length() > 0) {
            const carta::TimeSelection all{0, carta::projectEndSample(session), 0,
                                           static_cast<int>(session.tracks.size()) - 1};
            if (!carta::activateTimeSelection(timeSelection, timelineClipSelection, all,
                                              static_cast<int>(session.tracks.size()))) {
                break;
            }
            timelineTracks.clear();
            keyboardRange.active = false;
            keyboardTracks.active = false;
            if (!selectionPlaybackContinues()) {
                position = 0;
                auditionActive = false;
            }
            menuItemsChanged();
            repaint();
        }
        break;
    }
    case MenuId::layerHeightDecrease:
    case MenuId::layerHeightIncrease:
        nudgeTrackHeight(id == MenuId::layerHeightDecrease ? -1 : 1);
        break;
    case MenuId::selectedClipsSelectSpan:
        selectSelectedClipSpan();
        break;
    case MenuId::selectedClipsCrossfadePair:
        crossfadeSelectedPair();
        break;
    case MenuId::previewCrossfadePair:
        auditionSelectedCrossfade();
        break;
    case MenuId::layerRowsScaleGain:
        editSelectedTrackGain();
        break;
    case MenuId::layerRowsSetGain:
        setSelectedTrackGainDialog();
        break;
    case MenuId::layerRowsSetBalance:
        setSelectedTrackBalanceDialog();
        break;
    case MenuId::layerRowsSetGainDb:
        setSelectedTrackGainDbDialog();
        break;
    case MenuId::layerRowsAdjustGainDb:
        adjustSelectedTrackGainDbDialog();
        break;
    case MenuId::transportPlaySelContextFull:
    case MenuId::transportPlaySelContextLayers:
        if (requireAudioOutput()) {
            startSelectionContextAudition(id == MenuId::transportPlaySelContextLayers);
        }
        break;
    case MenuId::transportPlaySelFull:
    case MenuId::transportLoopSelFull:
    case MenuId::transportPlaySelLayers:
    case MenuId::transportLoopSelLayers:
        if (!requireAudioOutput()) {
            break;
        }
        startSelectionAudition(
            id == MenuId::transportPlaySelLayers || id == MenuId::transportLoopSelLayers,
            id == MenuId::transportLoopSelFull || id == MenuId::transportLoopSelLayers, false);
        break;
    case MenuId::editRedo:
        restoreHistory(true);
        break;
    case MenuId::fileNewProject:
        newProject();
        break;
    case MenuId::editCutSelection:
    case MenuId::editCopySelection:
        copySelection(id == MenuId::editCutSelection);
        break;
    case MenuId::editDuplicateSelectionAfter:
        duplicateTimeSelection();
        break;
    case MenuId::editPasteReplace:
        pasteSelection(1);
        break;
    case MenuId::editPasteInsertDest:
        pasteSelection(2);
        break;
    case MenuId::editPasteInsertAll:
        pasteSelection(3);
        break;
    case MenuId::editPointNudgeLeft:
        nudgeEditPoint(-1);
        break;
    case MenuId::editPointNudgeRight:
        nudgeEditPoint(1);
        break;
    case MenuId::editPointPasteOverlap:
    case MenuId::editPointPasteReplace:
    case MenuId::editPointPasteInsertDest:
    case MenuId::editPointPasteInsertAll:
        pasteSelection(id - MenuId::editPointPasteOverlap, true);
        break;
    case MenuId::editPointSetAtPlayhead:
        setEditPointAtPlayhead();
        break;
    case MenuId::editPointAddMarker:
        addMarkerAtEditPoint("Marker " + juce::String(session.markers.size() + 1));
        break;
    case MenuId::editPointNavMasterStart:
    case MenuId::editPointNavMasterEnd:
    case MenuId::editPointNavPrevMarker:
    case MenuId::editPointNavNextMarker:
    case MenuId::editPointNavPrevClipLayer:
    case MenuId::editPointNavNextClipLayer:
    case MenuId::editPointNavPrevClipAny:
    case MenuId::editPointNavNextClipAny:
        navigateEditPoint(id - MenuId::editPointNavMasterStart);
        break;
    case MenuId::editPointSelectMasterStart:
    case MenuId::editPointSelectMasterEnd:
    case MenuId::editPointSelectPrevMarker:
    case MenuId::editPointSelectNextMarker:
    case MenuId::editPointSelectPrevClipLayer:
    case MenuId::editPointSelectNextClipLayer:
    case MenuId::editPointSelectPrevClipAny:
    case MenuId::editPointSelectNextClipAny:
        selectFromEditPoint(id - MenuId::editPointSelectMasterStart);
        break;
    case MenuId::auditionEditPoint:
        auditionEditPoint();
        break;
    case MenuId::editPointSelectGapLayer:
        selectGapAtEditPoint();
        break;
    case MenuId::editPointSelectGapIndependent:
        selectGapAtEditPoint(1);
        break;
    case MenuId::editPointSelectGapAll:
        selectGapAtEditPoint(2);
        break;
    case MenuId::layerRenderSelectedTime:
        chooseSelectedTrackRender(RenderScope::selectedTime);
        break;
    case MenuId::layerRenderSelectedTracks:
        chooseSelectedTrackRender(RenderScope::tracks);
        break;
    case MenuId::layerRenderSelectedClips:
        chooseSelectedTrackRender(RenderScope::clips);
        break;
    case MenuId::layerConsolidateClips:
        chooseSelectedTrackRender(RenderScope::consolidate);
        break;
    case MenuId::layerConsolidateAllowRouting:
        confirmConsolidationRoutingChange();
        break;
    case MenuId::layerRenderStereoPair:
        chooseSelectedTrackRender(RenderScope::stereoPair);
        break;
    case MenuId::editRippleCutSelected:
        rippleCutSelection(false);
        break;
    case MenuId::editRippleCutAll:
        rippleCutSelection(true);
        break;
    case MenuId::editRippleCutFocused:
        rippleCutSelection(false, true);
        break;
    case MenuId::editMoveSelection:
        moveTimeSelection();
        break;
    case MenuId::editPasteOverlap:
        pasteSelection();
        break;
    case MenuId::editDeleteSelection:
        deleteSelection();
        break;
    case MenuId::editRippleDeleteSelected:
        rippleSelection(false);
        break;
    case MenuId::editRippleDeleteAll:
        rippleSelection(true);
        break;
    case MenuId::editRippleDeleteFocused:
        rippleSelection(false, true);
        break;
    case MenuId::viewModeWaveform:
        spectrumView = false;
        repaint();
        break;
    case MenuId::viewModeSpectrogram:
        spectrumView = true;
        repaint();
        break;
    case MenuId::viewSpectrumFft1024:
    case MenuId::viewSpectrumFft2048:
    case MenuId::viewSpectrumFft4096:
    case MenuId::viewSpectrumFft8192:
        setSpectrumFftOrder(10 + id - MenuId::viewSpectrumFft1024);
        break;
    case MenuId::viewSpectrumBandFull:
    case MenuId::viewSpectrumBandBass:
    case MenuId::viewSpectrumBandVoice:
    case MenuId::viewSpectrumBandTreble:
        setSpectrumPreset(id - MenuId::viewSpectrumBandFull);
        break;
    case MenuId::viewSpectrumWindowHann:
    case MenuId::viewSpectrumWindowHamming:
    case MenuId::viewSpectrumWindowBlackman:
        setSpectrumWindow(static_cast<carta::SpectrumWindow>(id - MenuId::viewSpectrumWindowHann));
        break;
    case MenuId::viewSpectrumFloorMinus40:
    case MenuId::viewSpectrumFloorMinus60:
    case MenuId::viewSpectrumFloorMinus80:
    case MenuId::viewSpectrumFloorMinus100:
    case MenuId::viewSpectrumFloorMinus120:
        setSpectrumFloor(-40 - (id - MenuId::viewSpectrumFloorMinus40) * 20);
        break;
    case MenuId::viewSpectrumCustomRange:
        editCustomSpectrumRange();
        break;
    case MenuId::viewTogglePeakWarnings:
        showClipPeakWarnings = !showClipPeakWarnings;
        status.setText(
            showClipPeakWarnings
                ? "Clip peak indicators on: red is confirmed; amber is a conservative wide-fade "
                  "estimate. Master clipping may differ."
                : "Clip peak indicators off.",
            juce::dontSendNotification);
        menuItemsChanged();
        repaint();
        break;
    case MenuId::viewToggleRms:
        setWaveformRms(!showWaveformRms);
        break;
    case MenuId::viewToggleHalfWave:
        setWaveformHalf(!waveformHalf);
        break;
    case MenuId::viewWaveZoom1x:
    case MenuId::viewWaveZoom2x:
    case MenuId::viewWaveZoom4x:
    case MenuId::viewWaveZoom8x:
        setWaveformZoom(1 << (id - MenuId::viewWaveZoom1x));
        break;
    case MenuId::viewWaveScaleLinear:
    case MenuId::viewWaveScaleDb:
        setWaveformScale(id == MenuId::viewWaveScaleLinear ? carta::WaveformScale::linear
                                                           : carta::WaveformScale::decibels);
        break;
    case MenuId::editDuplicateClip: {
        if (ensureFocusedPersistentGroup()) {
            performTimelineClipAction(MenuId::selectedClipsDuplicate);
            break;
        }
        juce::String error;
        const bool changed = editFocusedClip(2, 0, error);
        status.setText(changed ? "Duplicated clip after its end." : error,
                       juce::dontSendNotification);
        break;
    }
    case MenuId::layerMute:
    case MenuId::layerSolo: {
        juce::String error;
        const bool changed = mutateMixState(
            [this, id](carta::Session& candidate) {
                if (selectedTrack < 0 ||
                    selectedTrack >= static_cast<int>(candidate.tracks.size())) {
                    return false;
                }
                auto& track = candidate.tracks[static_cast<size_t>(selectedTrack)];
                if (id == MenuId::layerMute) {
                    track.muted = !track.muted;
                } else {
                    track.solo = !track.solo;
                }
                return true;
            },
            error, id == MenuId::layerMute ? "Toggle track mute" : "Toggle track solo");
        const auto message =
            changed
                ? "Layer " + juce::String(selectedTrack + 1) + " " +
                      session.tracks[static_cast<size_t>(selectedTrack)].name + ": " +
                      (id == MenuId::layerMute
                           ? (session.tracks[static_cast<size_t>(selectedTrack)].muted ? "muted."
                                                                                       : "unmuted.")
                           : (session.tracks[static_cast<size_t>(selectedTrack)].solo
                                  ? "solo on."
                                  : "solo off."))
            : error.isNotEmpty() ? error
                                 : juce::String("Select a layer first.");
        status.setText(message, juce::dontSendNotification);
        announceNavigation(message);
        break;
    }
    case MenuId::layerRowsMute:
    case MenuId::layerRowsUnmute:
    case MenuId::layerRowsSolo:
    case MenuId::layerRowsClearSolo:
        setSelectedTrackFlag(id);
        break;
    case MenuId::layerGainBalance:
        editTrackMix();
        break;
    case MenuId::layerAddEmpty: {
        juce::String error;
        changeTracks(0, -1, "Layer " + juce::String(static_cast<int>(session.tracks.size() + 1)),
                     error);
        break;
    }
    case MenuId::layerRename:
        renameTrack();
        break;
    case MenuId::layerDuplicate:
    case MenuId::layerRemove: {
        juce::String error;
        changeTracks(id == MenuId::layerDuplicate ? 1 : 2, selectedTrack, {}, error);
        break;
    }
    case MenuId::layerMoveUp:
    case MenuId::layerMoveDown: {
        const int target = selectedTrack + (id == MenuId::layerMoveUp ? -1 : 1);
        if (selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size()) &&
            target >= 0 && target < static_cast<int>(session.tracks.size())) {
            reorderCurrentTrack(selectedTrack, target);
        }
        break;
    }
    default:
        break;
    }
}

// ============================================================================
// Keyboard Shortcuts & Escape Drag/Selection Cancellation
// ============================================================================

bool Studio::keyPressed(const juce::KeyPress& key) {
    if (savePromptActive) {
        return true;
    }

    if (key.getKeyCode() == juce::KeyPress::escapeKey) {
        const juce::ScopedLock guard(lock);
        if (dragMode == kDragMarquee) {
            dragMode = kDragNone;
            dragged = false;
            marqueePreview.clear();
            marqueePreviewValid = false;
            status.setText("Clip selection drag cancelled.", juce::dontSendNotification);
            setMouseCursor(juce::MouseCursor::NormalCursor);
            repaint();
            return true;
        }
        if (dragMode == kDragHeaderControl) {
            if (headerDragTrack >= 0 && headerDragTrack < static_cast<int>(session.tracks.size())) {
                auto& tr = session.tracks[static_cast<size_t>(headerDragTrack)];
                if (headerDragGain) {
                    tr.gain = headerDragStartValue;
                } else {
                    tr.pan = headerDragStartValue;
                }
            }
            dragMode = kDragNone;
            headerDragTrack = -1;
            headerDragMoved = false;
            dragged = false;
            dirty = editHistory.isDirty() || pendingDurability || loopDirty();
            status.setText(headerDragGain ? "Layer gain drag cancelled."
                                          : "Layer balance drag cancelled.",
                           juce::dontSendNotification);
            setMouseCursor(juce::MouseCursor::NormalCursor);
            repaint();
            return true;
        }
        if (dragMode == kDragChannelHeight) {
            if (channelDragTrack >= 0 &&
                channelDragTrack < static_cast<int>(session.tracks.size())) {
                session.tracks[static_cast<size_t>(channelDragTrack)].channelHeightWeights =
                    channelDragOriginal;
            }
            dragMode = kDragNone;
            channelDragTrack = channelDragLane = -1;
            channelDragOriginal.clear();
            channelDragScaled.clear();
            {
                const juce::ScopedUnlock unlock(lock);
                dragBeforeState = {};
            }
            dragged = false;
            dirty = editHistory.isDirty() || pendingDurability || loopDirty();
            status.setText("Channel height drag cancelled.", juce::dontSendNotification);
            setMouseCursor(juce::MouseCursor::NormalCursor);
            repaint();
            return true;
        }
        if (dragMode == kDragTrackHeight) {
            if (resizedTrack >= 0 && resizedTrack < static_cast<int>(session.tracks.size())) {
                session.tracks[static_cast<size_t>(resizedTrack)].height = resizeStartHeight;
            }
            dragMode = kDragNone;
            resizedTrack = -1;
            dragged = false;
            dirty = editHistory.isDirty() || pendingDurability || loopDirty();
            {
                const juce::ScopedUnlock unlock(lock);
                dragBeforeState = {};
            }
            status.setText("Layer resize cancelled.", juce::dontSendNotification);
            setMouseCursor(juce::MouseCursor::NormalCursor);
            repaint();
            return true;
        }
        if (dragMode == kDragTrackReorder) {
            restoreTrackDragSelection();
            dragBeforeState = {};
            dragMode = kDragNone;
            trackDragSource = trackDragSlot = -1;
            trackDragMoved = false;
            status.setText("Layer move cancelled.", juce::dontSendNotification);
            setMouseCursor(juce::MouseCursor::NormalCursor);
            menuItemsChanged();
            repaint();
            return true;
        }
        if (dragMode == kDragMdRibbon) {
            session.mdDisc = std::move(mdDragBeforeDisc);
            dragMode = kDragNone;
            mdDragGesture = MdRibbonGesture::none;
            mdDragTrack0 = mdDragPart0 = mdDragRightTrack0 = mdDragRightPart0 = -1;
            mdDragChanged = false;
            mdDragFocusTrackId = {};
            mdDragActionLabel = {};
            snapGuideVisible = false;
            dragged = false;
            dirty = editHistory.isDirty() || pendingDurability || loopDirty();
            mdDragBeforeDisc = {};
            {
                const juce::ScopedUnlock unlock(lock);
                dragBeforeState = {};
                refreshDiscWorkspace(true);
            }
            status.setText("MD track edit cancelled.", juce::dontSendNotification);
            setMouseCursor(juce::MouseCursor::NormalCursor);
            menuItemsChanged();
            repaint();
            return true;
        }
        if (dragMode == kDragSavedLoop) {
            dragMode = kDragNone;
            loopPointerDragging = false;
            snapGuideVisible = false;
            dragged = false;
            status.setText("Loop boundary drag cancelled.", juce::dontSendNotification);
            setMouseCursor(juce::MouseCursor::NormalCursor);
            repaint();
            return true;
        }
        if (dragMode == kDragGroupMove || dragMode == kDragGroupEdge) {
            timeSelection = selectionBefore;
            if (!(selectionGesturePlayback || selectionPlaybackContinues())) {
                position = selectionBeforePosition;
            }
            selectedTrack = selectionBeforeTrack;
            selectedClip = selectionBeforeClip;
            selectedMarker = selectionBeforeMarker;
            restoreGestureClipSelection();
            dragMode = kDragNone;
            clipPointerDragging = false;
            groupDragCopy = false;
            copyGestureCandidate = false;
            groupDragClips.clear();
            groupEdgePreview.clear();
            groupEdgePlan.clear();
            snapGuideVisible = false;
            dragged = false;
            setMouseCursor(juce::MouseCursor::NormalCursor);
            status.setText("Clip group drag cancelled.", juce::dontSendNotification);
            menuItemsChanged();
            repaint();
            return true;
        }
        if (auditionActive &&
            (cutPreviewActive || groupAuditionActive || auditionRange.tracks.first >= 0)) {
            const bool cut = cutPreviewActive;
            const bool group = groupAuditionActive;
            const bool trackGroup = independentTrackAudition;
            const bool clip = auditionRange.onlyClip >= 0;
            stopSpecialAudition();
            status.setText(cut     ? "Edit preview cancelled; no edit applied."
                           : group ? (trackGroup ? "Selected-track audition stopped."
                                                 : "Selected clips audition stopped.")
                           : clip  ? "Clip audition stopped."
                                   : "Selected-track audition stopped.",
                           juce::dontSendNotification);
            repaint();
            return true;
        }
        if (dragMode == kDragNone && liveInPresent) {
            liveInPresent = false;
            status.setText("Selection in-point cleared.", juce::dontSendNotification);
            menuItemsChanged();
            repaint();
            return true;
        }
        if (dragMode == kDragNone && !timelineClipSelection.get(editGeneration).empty()) {
            timelineClipSelection.clear();
            status.setText("Clip selection cleared.", juce::dontSendNotification);
            menuItemsChanged();
            repaint();
            return true;
        }
        if (dragMode == kDragNone && !selectedTimelineTracks().empty()) {
            timelineTracks.clear();
            status.setText("Independent track selection cleared.", juce::dontSendNotification);
            menuItemsChanged();
            repaint();
            return true;
        }

        const bool markerDrag = dragMode == kDragMarkerOrMasterEnd && !draggingMasterEnd;
        const bool singleClipDrag = dragMode == kDragClipMove || dragMode == kDragClipTrimLeft ||
                                    dragMode == kDragClipTrimRight;
        const bool masterEndDrag = dragMode == kDragMarkerOrMasterEnd && draggingMasterEnd;
        if (markerDrag && dragged) {
            session.markers.swap(dragBefore.markers);
        } else if ((singleClipDrag || masterEndDrag) && dragged) {
            std::swap(session, dragBefore);
        } else if (dragMode >= kDragClipMove && dragMode <= kDragTrackHeight && dragged) {
            session = dragBefore;
        }

        if (dragMode == kDragPlayhead) {
            position = playheadBefore;
        } else if (dragMode >= kDragClipMove && dragMode <= kDragTimeSelectionResize) {
            timeSelection = selectionBefore;
            if (!(selectionGesturePlayback || selectionPlaybackContinues())) {
                position = selectionBeforePosition;
            }
            selectedTrack = selectionBeforeTrack;
            selectedClip = selectionBeforeClip;
            selectedMarker = selectionBeforeMarker;
            if (dragMode == kDragTimeSelection || dragMode == kDragTimeSelectionResize) {
                timelineTracks = selectionBeforeTracks;
            }
            if (selectionClipSnapshotValid) {
                timelineClipSelection = std::move(selectionBeforeClips);
                selectionClipSnapshotValid = false;
            }
            menuItemsChanged();
        } else if (dragMode == kDragNone) {
            clearAudioSelection(true, true);
        } else {
            timeSelection = {};
        }

        dragMode = kDragNone;
        draggingMasterEnd = false;
        selectionPointerDragging = false;
        clipPointerDragging = false;
        playheadPointerDragging = false;
        clipDropTrack = -1;
        resizedTrack = -1;
        dragged = false;
        snapGuideVisible = false;
        dirty = editHistory.isDirty() || pendingDurability || loopDirty();
        if (markerDrag || singleClipDrag || masterEndDrag) {
            const juce::ScopedUnlock unlock(lock);
            dragBefore = {};
            dragBeforeState = {};
        }
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()), viewStart);
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
        return true;
    }

    if (dragMode != kDragNone) {
        return true;
    }

    if (key.getModifiers().isCommandDown() && key.getModifiers().isShiftDown() &&
        !key.getModifiers().isAltDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            'g') {
        promptDiscTrackNumber();
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getModifiers().isShiftDown() &&
        !key.getModifiers().isAltDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            'i') {
        menuItemSelected(MenuId::mdInsertTrackFromSelection, MenuBarIndex::markers);
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getModifiers().isShiftDown() &&
        !key.getModifiers().isAltDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            'j') {
        menuItemSelected(MenuId::mdAppendPartFromSelection, MenuBarIndex::markers);
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getModifiers().isShiftDown() &&
        !key.getModifiers().isAltDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            'p') {
        showCommandSearch();
        return true;
    }
    if (key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown() &&
        !key.getModifiers().isAltDown() && key.getKeyCode() == '0') {
        focusTimeline();
        return true;
    }
    if (selectedMarker >= 0 && !key.getModifiers().isAnyModifierKeyDown() &&
        (key.isKeyCode(juce::KeyPress::deleteKey) || key.isKeyCode(juce::KeyPress::backspaceKey))) {
        menuItemSelected(MenuId::markerDelete, MenuBarIndex::markers);
        return true;
    }

    if (dragMode == kDragNone) {
        const auto mods = key.getModifiers();
        if (!timeSelection.hasRange() && timelineClipSelection.get(editGeneration).empty() &&
            carta::clipGroupShortcut(key.getKeyCode(),
                                     key.isKeyCode(juce::KeyPress::deleteKey) ||
                                         key.isKeyCode(juce::KeyPress::backspaceKey),
                                     !mods.isAnyModifierKeyDown(),
                                     mods.isCommandDown() && !mods.isShiftDown() &&
                                         !mods.isAltDown(),
                                     true) != carta::ClipGroupShortcut::none) {
            ensureFocusedPersistentGroup();
        }
        const auto shortcut = carta::clipGroupShortcut(
            key.getKeyCode(),
            key.isKeyCode(juce::KeyPress::deleteKey) || key.isKeyCode(juce::KeyPress::backspaceKey),
            !mods.isAnyModifierKeyDown(),
            mods.isCommandDown() && !mods.isShiftDown() && !mods.isAltDown(),
            !timelineClipSelection.get(editGeneration).empty());
        switch (shortcut) {
        case carta::ClipGroupShortcut::copy:
            performTimelineClipAction(MenuId::selectedClipsCopy);
            return true;
        case carta::ClipGroupShortcut::cut:
            performTimelineClipAction(MenuId::selectedClipsCut);
            return true;
        case carta::ClipGroupShortcut::remove:
            performTimelineClipAction(MenuId::selectedClipsDelete);
            return true;
        case carta::ClipGroupShortcut::duplicate:
            performTimelineClipAction(MenuId::selectedClipsDuplicate);
            return true;
        case carta::ClipGroupShortcut::split:
            performTimelineClipAction(MenuId::selectedClipsSplit);
            return true;
        case carta::ClipGroupShortcut::none:
            break;
        }
    }

    if (key.getModifiers().isShiftDown() && key.getModifiers().isAltDown() &&
        !key.getModifiers().isCommandDown() && !key.getModifiers().isCtrlDown() &&
        (key.getKeyCode() == juce::KeyPress::upKey ||
         key.getKeyCode() == juce::KeyPress::downKey)) {
        nudgeTrackHeight(key.getKeyCode() == juce::KeyPress::upKey ? -1 : 1);
        return true;
    }
    if (key.getModifiers().isShiftDown() && !key.getModifiers().isAltDown() &&
        !key.getModifiers().isCommandDown() && !key.getModifiers().isCtrlDown() &&
        (key.getKeyCode() == juce::KeyPress::F5Key || key.getKeyCode() == juce::KeyPress::F6Key)) {
        focusAdjacentClip(key.getKeyCode() == juce::KeyPress::F6Key);
        return true;
    }
    if (key.getModifiers().isShiftDown() && !key.getModifiers().isAltDown() &&
        !key.getModifiers().isCommandDown() && !key.getModifiers().isCtrlDown() &&
        (key.getKeyCode() == juce::KeyPress::F7Key || key.getKeyCode() == juce::KeyPress::F8Key)) {
        focusAdjacentTrack(key.getKeyCode() == juce::KeyPress::F8Key);
        return true;
    }
    if (key.getModifiers().isShiftDown() && !key.getModifiers().isCommandDown() &&
        !key.getModifiers().isAltDown() &&
        (key.getKeyCode() == juce::KeyPress::upKey ||
         key.getKeyCode() == juce::KeyPress::downKey)) {
        extendSelectionTracks(key.getKeyCode() == juce::KeyPress::upKey ? -1 : 1);
        return true;
    }
    if (key.getModifiers().isShiftDown() && !key.getModifiers().isCommandDown() &&
        !key.getModifiers().isAltDown() && !key.getModifiers().isCtrlDown() &&
        (key.getKeyCode() == juce::KeyPress::pageUpKey ||
         key.getKeyCode() == juce::KeyPress::pageDownKey)) {
        focusAdjacentMarker(key.getKeyCode() == juce::KeyPress::pageDownKey);
        return true;
    }
    if (key.getModifiers().isShiftDown() && (key.getKeyCode() == juce::KeyPress::leftKey ||
                                             key.getKeyCode() == juce::KeyPress::rightKey)) {
        const bool command = key.getModifiers().isCommandDown();
        const bool alt = key.getModifiers().isAltDown();
        if (command || alt) {
            const auto part = command && alt ? carta::SelectionPart::start
                              : alt          ? carta::SelectionPart::end
                                             : carta::SelectionPart::whole;
            adjustTimeSelection(part, key.getKeyCode() == juce::KeyPress::leftKey ? -1 : 1);
            return true;
        }
        extendSelection(key.getKeyCode() == juce::KeyPress::leftKey ? -1 : 1);
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getModifiers().isAltDown() &&
        !key.getModifiers().isShiftDown()) {
        if (key.getKeyCode() == juce::KeyPress::leftKey) {
            if (!timelineClipSelection.get(editGeneration).empty()) {
                nudgeClipGroup(-1);
            } else {
                nudgeClip(-1);
            }
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::rightKey) {
            if (!timelineClipSelection.get(editGeneration).empty()) {
                nudgeClipGroup(1);
            } else {
                nudgeClip(1);
            }
            return true;
        }
    }
    if (key.getModifiers().isAltDown() && !key.getModifiers().isCommandDown() &&
        !key.getModifiers().isShiftDown()) {
        if (!key.getModifiers().isCtrlDown() &&
            juce::CharacterFunctions::toLowerCase(
                static_cast<juce::juce_wchar>(key.getKeyCode())) == 'g') {
            selectGapAtEditPoint();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::pageUpKey ||
            key.getKeyCode() == juce::KeyPress::pageDownKey) {
            navigateEditPoint(key.getKeyCode() == juce::KeyPress::pageUpKey ? 2 : 3);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::upKey ||
            key.getKeyCode() == juce::KeyPress::downKey) {
            selectAdjacentTrack(key.getKeyCode() == juce::KeyPress::downKey);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::leftKey ||
            key.getKeyCode() == juce::KeyPress::rightKey) {
            selectAdjacentClip(key.getKeyCode() == juce::KeyPress::rightKey);
            return true;
        }
    }
    if (key.getModifiers().isAltDown() && !key.getModifiers().isCommandDown() &&
        !key.getModifiers().isCtrlDown() && (key.getKeyCode() == '[' || key.getKeyCode() == ']')) {
        const bool next = key.getKeyCode() == ']';
        navigateEditPoint((key.getModifiers().isShiftDown() ? 6 : 4) + (next ? 1 : 0));
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::spaceKey &&
        !key.getModifiers().isAnyModifierKeyDown()) {
        play.onClick();
        return true;
    }
    if (!key.getModifiers().isAnyModifierKeyDown()) {
        const auto c =
            juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
        if (c == 'm' || c == 's') {
            menuItemSelected(c == 'm' ? MenuId::layerMute : MenuId::layerSolo, MenuBarIndex::layer);
            return true;
        }
    }
    if (!key.getModifiers().isAnyModifierKeyDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            'k') {
        stopAtPlayhead();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::F2Key && key.getModifiers().isShiftDown() &&
        !key.getModifiers().isAltDown() && !key.getModifiers().isCtrlDown() &&
        !key.getModifiers().isCommandDown()) {
        editSelectedTrackTitleForms();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::F2Key && !key.getModifiers().isAnyModifierKeyDown()) {
        if (selectedMarker >= 0 && selectedMarker < static_cast<int>(session.markers.size())) {
            renameMarker();
        } else {
            renameClip();
        }
        return true;
    }
    if (selectedMarker >= 0 && !key.getModifiers().isAnyModifierKeyDown()) {
        const auto c =
            juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
        if (c == 'p') {
            editMarkerPosition();
            return true;
        }
        if (c == 'b') {
            auditionFocusedMarker();
            return true;
        }
    }
    if (key.getKeyCode() == juce::KeyPress::F3Key && !key.getModifiers().isAnyModifierKeyDown()) {
        showTrackClipNavigator();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::F4Key && !key.getModifiers().isAnyModifierKeyDown()) {
        announceTimeline();
        return true;
    }
    if (!key.getModifiers().isAnyModifierKeyDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            'g') {
        toggleFocusedClipSelection();
        return true;
    }
    if (!key.getModifiers().isAnyModifierKeyDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            't') {
        toggleTimelineTrack(selectedTrack);
        return true;
    }
    if (!key.getModifiers().isAnyModifierKeyDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            'i') {
        markLiveSelection(true);
        return true;
    }
    if (!key.getModifiers().isAnyModifierKeyDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            'o') {
        markLiveSelection(false);
        return true;
    }
    if (!key.getModifiers().isAnyModifierKeyDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            'e') {
        setEditPointAtPlayhead();
        return true;
    }
    if (!key.getModifiers().isAnyModifierKeyDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            'z') {
        togglePreviousZoom();
        return true;
    }
    if (key.getModifiers().isShiftDown() && !key.getModifiers().isCommandDown() &&
        !key.getModifiers().isCtrlDown() && !key.getModifiers().isAltDown() &&
        juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode())) ==
            'z') {
        zoomToSelectedClips();
        return true;
    }
    if (!key.getModifiers().isAnyModifierKeyDown()) {
        if (key.getKeyCode() == ',' || key.getKeyCode() == '.') {
            nudgeEditPoint(key.getKeyCode() == ',' ? -1 : 1);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::leftKey) {
            seekTimeline(MenuId::seekOneSampleEarlier);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::rightKey) {
            seekTimeline(MenuId::seekOneSampleLater);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::homeKey) {
            seekTimeline(MenuId::seekMasterStart);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::endKey) {
            seekTimeline(MenuId::seekMasterEnd);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::pageUpKey ||
            key.getKeyCode() == juce::KeyPress::pageDownKey) {
            navigateMarker(key.getKeyCode() == juce::KeyPress::pageDownKey);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::F5Key) {
            nudgeSelectedClipEdge(MenuId::clipNudgeStartEarlier);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::F6Key) {
            nudgeSelectedClipEdge(MenuId::clipNudgeStartLater);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::F7Key) {
            nudgeSelectedClipEdge(MenuId::clipNudgeEndEarlier);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::F8Key) {
            nudgeSelectedClipEdge(MenuId::clipNudgeEndLater);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::F9Key ||
            key.getKeyCode() == juce::KeyPress::F10Key) {
            juce::String error;
            if (!nudgeMarker(selectedMarker, key.getKeyCode() == juce::KeyPress::F9Key ? -1 : 1,
                             error)) {
                status.setText(error, juce::dontSendNotification);
                announceNavigation(error);
            }
            return true;
        }
    }
    if (!key.getModifiers().isCommandDown() && !key.getModifiers().isAltDown()) {
        const auto c = key.getTextCharacter();
        if (c == '[' || c == '{' || c == ']' || c == '}') {
            navigateClipBoundary(c == ']' || c == '}', key.getModifiers().isShiftDown());
            return true;
        }
    }
    if (key.getTextCharacter() == '+' || key.getTextCharacter() == '=') {
        zoomAt(2.0);
        return true;
    }
    if (key.getTextCharacter() == '-') {
        zoomAt(0.5);
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::deleteKey ||
        key.getKeyCode() == juce::KeyPress::backspaceKey) {
        if (timeSelection.hasRange() && key.getModifiers().isCommandDown()) {
            rippleSelection(true);
        } else if (timeSelection.hasRange() && key.getModifiers().isShiftDown()) {
            rippleSelection(false);
        } else {
            remove.onClick();
        }
        return true;
    }
    if (key.getModifiers().isCommandDown()) {
        switch (juce::CharacterFunctions::toLowerCase(
            static_cast<juce::juce_wchar>(key.getKeyCode()))) {
        case 'n':
            menuItemSelected(MenuId::fileNewProject, MenuBarIndex::edit);
            return true;
        case 'i':
            add.onClick();
            return true;
        case 'o':
            load.onClick();
            return true;
        case 's':
            if (key.getModifiers().isShiftDown()) {
                saveProject({}, true);
            } else {
                save.onClick();
            }
            return true;
        case 'z':
            if (key.getModifiers().isShiftDown()) {
                menuItemSelected(MenuId::editRedo, MenuBarIndex::edit);
            } else {
                undo.onClick();
            }
            return true;
        case 'd':
            menuItemSelected(key.getModifiers().isShiftDown() ? MenuId::editDuplicateSelectionAfter
                                                              : MenuId::editDuplicateClip,
                             MenuBarIndex::edit);
            return true;
        case 'a':
            menuItemSelected(MenuId::editSelectAll, MenuBarIndex::edit);
            return true;
        case 'c':
            copySelection(false);
            return true;
        case 'x':
            copySelection(true);
            return true;
        case 'v':
            pasteSelection();
            return true;
        case 'b':
            split.onClick();
            return true;
        case 'm':
            marker.onClick();
            return true;
        default:
            break;
        }
    }
    return false;
}

// ============================================================================
// Submenu Builders, Command Search & Keyboard Help Dialogs
// ============================================================================

void Studio::addFadeShapeMenus(juce::PopupMenu& target, bool enabled) const {
    const auto targets = activeFadeShapeTargets();
    bool haveCommonIn = false;
    bool haveCommonOut = false;
    carta::FadeShape commonIn = carta::FadeShape::linear;
    carta::FadeShape commonOut = carta::FadeShape::linear;

    if (!targets.empty()) {
        bool first = true;
        for (const auto& ref : targets) {
            if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size()) ||
                ref.clip < 0 ||
                ref.clip >=
                    static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size())) {
                continue;
            }
            const auto& c =
                session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
            if (first) {
                commonIn = c.fadeInShape;
                commonOut = c.fadeOutShape;
                haveCommonIn = haveCommonOut = true;
                first = false;
            } else {
                if (c.fadeInShape != commonIn) {
                    haveCommonIn = false;
                }
                if (c.fadeOutShape != commonOut) {
                    haveCommonOut = false;
                }
            }
        }
    }

    const bool active = enabled && !targets.empty();
    juce::PopupMenu fadeInMenu;
    juce::PopupMenu fadeOutMenu;
    fadeInMenu.addItem(MenuId::fadeInLinear, "Linear", active,
                       haveCommonIn && commonIn == carta::FadeShape::linear);
    fadeInMenu.addItem(MenuId::fadeInEqualPower, "Equal-power", active,
                       haveCommonIn && commonIn == carta::FadeShape::equalPower);
    fadeInMenu.addItem(MenuId::fadeInSCurve, "S-curve", active,
                       haveCommonIn && commonIn == carta::FadeShape::sCurve);
    fadeInMenu.addItem(MenuId::fadeInExponential, "Exponential", active,
                       haveCommonIn && commonIn == carta::FadeShape::exponential);
    fadeOutMenu.addItem(MenuId::fadeOutLinear, "Linear", active,
                        haveCommonOut && commonOut == carta::FadeShape::linear);
    fadeOutMenu.addItem(MenuId::fadeOutEqualPower, "Equal-power", active,
                        haveCommonOut && commonOut == carta::FadeShape::equalPower);
    fadeOutMenu.addItem(MenuId::fadeOutSCurve, "S-curve", active,
                        haveCommonOut && commonOut == carta::FadeShape::sCurve);
    fadeOutMenu.addItem(MenuId::fadeOutExponential, "Exponential", active,
                        haveCommonOut && commonOut == carta::FadeShape::exponential);
    target.addSubMenu("Fade in shape", fadeInMenu, active);
    target.addSubMenu("Fade out shape", fadeOutMenu, active);
}

std::vector<CommandSearchEntry> Studio::commandSearchEntries() {
    std::vector<CommandSearchEntry> entries;
    const auto names = getMenuBarNames();
    for (int i = 0; i < names.size(); ++i) {
        const auto menu = getMenuForIndex(i, names[i]);
        collectSearchCommands(menu, i, names[i], true, entries);
    }
    return entries;
}

void Studio::showCommandSearch() {
    if (dragMode != kDragNone || juce::Component::getCurrentlyModalComponent() != nullptr) {
        status.setText("Finish the current drag or dialog first.", juce::dontSendNotification);
        return;
    }

    auto entries = commandSearchEntries();
    const auto generation = editGeneration;
    const auto selection = timeSelection;
    const int track = selectedTrack;
    const int clip = selectedClip;
    const int markerIndex = selectedMarker;
    const auto tracks = selectedTimelineTracks();
    const auto clips = timelineClipSelection.get(generation);

    auto activate = [safe = juce::Component::SafePointer<Studio>(this), generation, selection,
                     track, clip, markerIndex, tracks, clips](CommandSearchEntry command) {
        juce::MessageManager::callAsync(
            [safe, generation, selection, track, clip, markerIndex, tracks, clips, command] {
                if (safe == nullptr) {
                    return;
                }
                const auto currentClips = safe->timelineClipSelection.get(safe->editGeneration);
                const bool sameClips = clips.size() == currentClips.size() &&
                                       std::equal(clips.begin(), clips.end(), currentClips.begin(),
                                                  [](const auto& a, const auto& b) {
                                                      return a.track == b.track && a.clip == b.clip;
                                                  });
                const auto& current = safe->timeSelection;
                if (safe->dragMode != kDragNone || safe->editGeneration != generation ||
                    safe->selectedTrack != track || safe->selectedClip != clip ||
                    safe->selectedMarker != markerIndex || current.start != selection.start ||
                    current.end != selection.end || current.firstTrack != selection.firstTrack ||
                    current.lastTrack != selection.lastTrack ||
                    safe->selectedTimelineTracks() != tracks || !sameClips) {
                    safe->status.setText("Editing context changed; search commands again.",
                                         juce::dontSendNotification);
                    return;
                }
                const auto fresh = safe->commandSearchEntries();
                const auto found = std::find_if(fresh.begin(), fresh.end(), [&](const auto& item) {
                    return item.id == command.id && item.menu == command.menu &&
                           item.path == command.path && item.enabled;
                });
                if (found == fresh.end()) {
                    safe->status.setText("Command is no longer available; search again.",
                                         juce::dontSendNotification);
                    return;
                }
                safe->menuItemSelected(command.id, command.menu);
            });
    };

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Search commands";
    auto* content = new CommandSearch(std::move(entries), std::move(activate));
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<CommandSearch>(content)] {
        if (safe != nullptr) {
            safe->focusSearch();
        }
    });
}

void Studio::showKeyboardHelp() {
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Keyboard shortcuts";
    auto* content = new KeyboardHelp();
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<KeyboardHelp>(content)] {
        if (safe != nullptr) {
            safe->focusText();
        }
    });
}
