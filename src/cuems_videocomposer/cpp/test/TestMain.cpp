/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "TestFramework.h"

// Forward declarations of test functions
extern bool test_LayerManager_AddLayer();
extern bool test_LayerManager_RemoveLayer();
extern bool test_LayerManager_ZOrder();
extern bool test_LayerManager_DuplicateLayer();
extern bool test_LayerManager_Reorder();
extern bool test_LayerManager_ResolveLiveCueId();
extern bool test_LayerManager_ResolveAbsentDigitUuidIsNotAnIntegerId();
extern bool test_LayerManager_ResolveIntegerId();
extern bool test_LayerManager_ResolveMalformedOrMissing();
extern bool test_LayerManager_ResolveCueIdWinsOverIntegerId();
extern bool test_LayerManager_ResolveAfterRemovalIsGone();

extern bool test_VideoLayer_PlayPause();
extern bool test_VideoLayer_Seek();
extern bool test_VideoLayer_TimeOffset();
extern bool test_VideoLayer_TimeScale();
extern bool test_VideoLayer_Wraparound();
extern bool test_VideoLayer_Reverse();
extern bool test_VideoLayer_SyncUpdate();

extern bool test_ConfigurationManager_Defaults();
extern bool test_ConfigurationManager_SetGet();
extern bool test_ConfigurationManager_Override();
extern bool test_ConfigurationManager_NonExistent();

extern bool test_Integration_LayerManagerWithMultipleLayers();
extern bool test_Integration_VideoLayerTimeScaling();
extern bool test_Integration_LayerProperties();
extern bool test_Integration_MIDISyncSource_DisplayLatencyAtomicity();
extern bool test_Integration_MIDISyncSource_GetDisplayLatencyMs();
extern bool test_Integration_FramerateConverter_DelegatesDisplayLatency();
extern bool test_Integration_FramerateConverter_SameRateFollowsTimeNotRawFrame();
#ifdef HAVE_MTCRECEIVER
extern bool test_Integration_MIDISyncSource_NoJumpSnapBias();
extern bool test_Integration_MIDISyncSource_RateWindowImmuneToPollCadence();
#endif

extern bool test_MTCDecoder();

// Day alignment of engine layer offsets (1amfwm7fbqy, 24h MTC wrap)
extern bool test_DayAlign_FrameMsConversionAndSign();
extern bool test_DayAlign_KZeroIsByteIdentical();
extern bool test_DayAlign_TwelveHourTies();
extern bool test_DayAlign_LatchPendingLatestWinsAndClear();
extern bool test_DayAlign_UnresolvedWarningOncePerValue();
extern bool test_DayAlign_ImplausiblePosition();
extern bool test_DayAlign_LogLines();
extern bool test_DayAlign_Int64ExtremesNoOverflow();
#if defined(HAVE_MTCRECEIVER) && defined(MTCRECV_TESTING)
extern bool test_DayAlign_RealResolverWaitsForAuthoritativeHead();
extern bool test_DayAlign_LayerAfterRestartShowsTheRightFrame();
extern bool test_DayAlign_LayerAheadOfEngineShowsTheRightFrame();
extern bool test_DayAlign_LayerAlignedIsUnchanged();
extern bool test_DayAlign_LaterWriterReplacesPending();
extern bool test_DayAlign_NoReceiverDriverAppliesAsReceived();
extern bool test_DayAlign_ResolvedOnReceiptWhenAuthoritative();
extern bool test_DayAlign_PendingLayerStillPollsOncePerUpdate();
extern bool test_DayAlign_LoopLayerAtNonZeroK();
extern bool test_DayAlign_LayerInt64Extremes();
#endif

extern bool test_SMPTEUtils_Overflow24h();
extern bool test_LayerPathId_IntegerIds();
extern bool test_LayerPathId_UuidsAreNotIntegerIds();
extern bool test_LayerPathId_Malformed();

// ExitReporter tests (F1, 869en65tm) - most of these fork, because the paths
// under test are exit() and fatal signals.
extern bool test_ExitReporter_SilentBeforeRunning();
extern bool test_ExitReporter_CleanShutdownIsInfo();
extern bool test_ExitReporter_DirtyExitIsErrorWithCensus();
extern bool test_ExitReporter_PeaksSurviveClose();
extern bool test_ExitReporter_FatalSignalRecordAndReraise();
extern bool test_ExitReporter_ReportsOnlyOnce();
extern bool test_ExitReporter_CensusLine();

// RecoveryPolicy tests (G3, 869en65tm) - the arithmetic that bounds a fault
// run. Pure and thread-free; the hardware drill is the integration proof.
extern bool test_RecoveryPolicy_LimpingYieldsAccumulateToCap();
extern bool test_RecoveryPolicy_GoodRunResetsTheLadder();
extern bool test_RecoveryPolicy_JustBelowThresholdDoesNotDecay();
extern bool test_RecoveryPolicy_DeclarationConsumesNothing();
extern bool test_RecoveryPolicy_DecayRescuesAtTheCap();
extern bool test_RecoveryPolicy_OneWakeIsOneEpisode();
extern bool test_RecoveryPolicy_ZeroYieldFirstWakeAttempts();

// HangGuard tests (G12, 869en65tm) - the cap arithmetic, the two counters
// behind it, and the fail-closed rule that keeps a broken header from sailing
// past the boundary.
extern bool test_HangGuard_HdIsExemptAtTheBoundary();
extern bool test_HangGuard_UhdAndDciAreFourKClass();
extern bool test_HangGuard_MissingMetadataFailsClosed();
extern bool test_HangGuard_AboveEnvelopeIsFlaggedNotRefused();
extern bool test_HangGuard_AdmitsUpToCapThenRefuses();
extern bool test_HangGuard_ExemptSessionsAreNeverRefused();
extern bool test_HangGuard_MonitorOnlyProfileNeverRefuses();
extern bool test_HangGuard_DisarmStopsRefusalButKeepsCounting();
extern bool test_HangGuard_DefaultOffKeepsCapAndRefusesNothing();
extern bool test_HangGuard_RepeatedRevealDoesNotDoubleCount();
extern bool test_HangGuard_ReleaseIsIdempotentAndSafeWhenUnreserved();
extern bool test_HangGuard_RefusedRevealReservesNothing();
extern bool test_HangGuard_ArmedCountCountsOnceAndReleasesOnce();
extern bool test_HangGuard_ArmedCountIgnoresExemptLoads();
extern bool test_HangGuard_ArmedCountOverCapDoesNotRefuse();
extern bool test_HangGuard_AdvisoryLatchTracksTheCrossing();
extern bool test_HangGuard_NoAdvisoryWithoutACap();
extern bool test_HangGuard_MonitorStateCarriesBothCountsSeparately();
extern bool test_HangGuard_OnlyMeasuredProfilesAreArmed();
extern bool test_HangGuard_LegacyRamKeyedNamesAreAliases();
extern bool test_HangGuard_PicassoArmsIndependentlyOfRam();
extern bool test_HangGuard_DetectAlwaysYieldsAUsableProfile();
extern bool test_HangGuard_DeferredAdvisoryClearIsNotLost();
extern bool test_HangGuard_PollIsANoOpWhenTheLogIsCurrent();
extern bool test_HangGuard_TransferMovesTheSlotWithoutChangingCounts();
extern bool test_HangGuard_TransferFromUnreservedIsANoOp();

extern bool test_NdiPreview_LiveLayerPullsWithoutMtc();
extern bool test_NdiPreview_LivePullNeverBlocks();
extern bool test_NdiPreview_ReadLatestFrameZeroWaitIsNonBlocking();
extern bool test_NdiPreview_FileLayerUnchangedWithoutSync();
extern bool test_NdiPreview_AutoUnloadSkipsUnknownLength();
extern bool test_NdiPreview_FitOutputTest3Regions();

// Per-output color (programme phase 3)
extern bool test_CubeLut_ParsesHeaderAndData();
extern bool test_CubeLut_RedVariesFastest();
extern bool test_CubeLut_DomainMinMax();
extern bool test_CubeLut_InputRangeMapsToDomain();
extern bool test_CubeLut_RejectsDomainAndInputRangeDisagree();
extern bool test_CubeLut_RejectsDomainMaxNotAboveMin();
extern bool test_CubeLut_RejectsSizeOutOfRange();
extern bool test_CubeLut_RejectsShortOrLongData();
extern bool test_CubeLut_RejectsFourTokenLine();
extern bool test_CubeLut_Rejects1D();
extern bool test_CubeLut_RejectsUnknownKeyword();
extern bool test_CubeLut_RejectsNonFinite();
extern bool test_CubeLut_AcceptsBomTabsScientificCrlf();
extern bool test_CubeLut_ClampsOutOfRangeAndCounts();
extern bool test_CubeLut_RefusesNonRegularFile();
extern bool test_CubeLut_RejectsDotDotRelativePath();
extern bool test_CubeLut_SizeCap16MiB();
extern bool test_CubeLut_ParseTime65();
extern bool test_OutputColor_DefaultsAreIdentity();
extern bool test_OutputColor_ClampsMatchLayerHandlers();
extern bool test_OutputColor_HueFoldNeverLoops();
extern bool test_OutputColor_FromCharsWholeToken();
extern bool test_OutputColor_ReferenceMatchesLayerSemantics();
extern bool test_OutputColor_LutCoordHalfTexel();
extern bool test_OutputBlitShader_ProgramATextUnchanged();
extern bool test_OutputBlitShader_ProgramBEmbedsSharedGlsl();
extern bool test_VideoShaders_ColorFunctionsPinned();
extern bool test_ColorMatrix_MatchesReference();
extern bool test_ColorMatrix_KnownValues();
extern bool test_ColorMatrix_IdentityIsIdentity();
extern bool test_ColorMatrix_HueZeroSkipsRotation();
extern bool test_ColorMatrix_ColumnMajor();
extern bool test_ColorGlsl_MultiplyOrder();
extern bool test_ColorGlsl_UniformNamesDeclared();
extern bool test_VideoShaders_ColorFragmentsUseMatrix();
extern bool test_OutputRegion_CopyCarriesLut();
extern bool test_DisplayConfig_ColorKeysRoundTrip();
extern bool test_DisplayConfig_PartialColorKeys();
extern bool test_DisplayConfig_ValueCrlfAndTrailingSpaceStripped();
extern bool test_DisplayConfig_BadColorValueIsLoudAndIgnored();
extern bool test_DisplayConfig_OutOfRangeClampedWithWarning();
extern bool test_DisplayConfig_NonFiniteRefused();
extern bool test_DisplayConfig_UnknownKeysStillIgnored();
extern bool test_DisplayConfig_SpacedKeyIsUnknown();
extern bool test_DisplayConfig_ColorWithAutoLayout();
extern bool test_DisplayConfig_ColorWithoutCanvasRegionWarns();
extern bool test_DisplayConfig_LutPathRules();
extern bool test_ShowLock_GateOnTempPath();
extern bool test_ShowLock_RefusalLineNamesBothCauses();
extern bool test_OutputColorCommands_ParsesAndClamps();
extern bool test_OutputColorCommands_RejectsPathsAndDotDot();
extern bool test_OutputColorCommands_PasteReadyLine();
extern bool test_OutputColorCommands_DumpAllowedUnderLock();
extern bool test_OutputColorCommands_LutNoneClearsOnlyLut();
extern bool test_OutputColorCommands_StatusToLogLine();
extern bool test_OutputList_ColorLineNeverMatchesRegionRegex();
extern bool test_NdiPreview_FitOutputNativeMatchesEngine();
extern bool test_NdiPreview_FitOutputNarrowCanvasAndFallback();
extern bool test_NdiPreview_FitOutputRefitsOnFormatChange();
extern bool test_NdiPreview_JournalContract();

extern bool test_NdiPreview_AddressForm();
extern bool test_NdiPreview_TransportDecision();
extern bool test_NdiPreview_JournalContractRelay();
extern bool test_NdiPreview_ExtraIps();
extern bool test_NdiPreview_JournalContractExtraIps();
#ifdef HAVE_CONTRACT_TESTS
extern bool test_Contract_ModeFromTimingAndIds();
extern bool test_Contract_HelloIgnoresVersion();
extern bool test_Contract_Errors();
extern bool test_Contract_HardwareShape();
extern bool test_Contract_DuplicateModesCollapse();
extern bool test_Contract_StateShowLock();
extern bool test_Contract_CachedHardwareMatches();
extern bool test_Contract_ServerRoundTripAndBindFailure();
extern bool test_Contract_ServerLifecycleStress();
extern bool test_Contract_OversizedRequestDoesNotKillServer();
extern bool test_Contract_PubEventsReachSubscriber();
#endif
extern bool test_PresentationTiming_CaptureDisabled_NoOp();
extern bool test_PresentationTiming_FifoPairing();
extern bool test_PresentationTiming_FifoPairing_UsesKernelUst();
extern bool test_PresentationTiming_DiscardPendingSubmit();
extern bool test_PresentationTiming_StatisticsMedianAndP95();
extern bool test_PresentationTiming_ResetClearsState();
extern bool test_PresentationTiming_ConcurrentSubmitFlip();
extern bool test_PresentationTiming_SkippedVsyncsFromMscDelta();
extern bool test_PresentationTiming_SustainedUnderrateDetected();
extern bool test_PresentationTiming_NoUnderrateAtFullRate();

using namespace videocomposer::test;

int main() {
    // Register all tests
    TestFramework::instance().addTest("LayerManager_AddLayer", test_LayerManager_AddLayer);
    TestFramework::instance().addTest("LayerManager_RemoveLayer", test_LayerManager_RemoveLayer);
    TestFramework::instance().addTest("LayerManager_ZOrder", test_LayerManager_ZOrder);
    TestFramework::instance().addTest("LayerManager_DuplicateLayer", test_LayerManager_DuplicateLayer);
    TestFramework::instance().addTest("LayerManager_Reorder", test_LayerManager_Reorder);
    TestFramework::instance().addTest("LayerManager_ResolveLiveCueId", test_LayerManager_ResolveLiveCueId);
    TestFramework::instance().addTest("LayerManager_ResolveAbsentDigitUuidIsNotAnIntegerId", test_LayerManager_ResolveAbsentDigitUuidIsNotAnIntegerId);
    TestFramework::instance().addTest("LayerManager_ResolveIntegerId", test_LayerManager_ResolveIntegerId);
    TestFramework::instance().addTest("LayerManager_ResolveMalformedOrMissing", test_LayerManager_ResolveMalformedOrMissing);
    TestFramework::instance().addTest("LayerManager_ResolveCueIdWinsOverIntegerId", test_LayerManager_ResolveCueIdWinsOverIntegerId);
    TestFramework::instance().addTest("LayerManager_ResolveAfterRemovalIsGone", test_LayerManager_ResolveAfterRemovalIsGone);
    
    TestFramework::instance().addTest("VideoLayer_PlayPause", test_VideoLayer_PlayPause);
    TestFramework::instance().addTest("VideoLayer_Seek", test_VideoLayer_Seek);
    TestFramework::instance().addTest("VideoLayer_TimeOffset", test_VideoLayer_TimeOffset);
    TestFramework::instance().addTest("VideoLayer_TimeScale", test_VideoLayer_TimeScale);
    TestFramework::instance().addTest("VideoLayer_Wraparound", test_VideoLayer_Wraparound);
    TestFramework::instance().addTest("VideoLayer_Reverse", test_VideoLayer_Reverse);
    TestFramework::instance().addTest("VideoLayer_SyncUpdate", test_VideoLayer_SyncUpdate);
    
    TestFramework::instance().addTest("ConfigurationManager_Defaults", test_ConfigurationManager_Defaults);
    TestFramework::instance().addTest("ConfigurationManager_SetGet", test_ConfigurationManager_SetGet);
    TestFramework::instance().addTest("ConfigurationManager_Override", test_ConfigurationManager_Override);
    TestFramework::instance().addTest("ConfigurationManager_NonExistent", test_ConfigurationManager_NonExistent);
    
    TestFramework::instance().addTest("Integration_LayerManagerWithMultipleLayers", test_Integration_LayerManagerWithMultipleLayers);
    TestFramework::instance().addTest("Integration_VideoLayerTimeScaling", test_Integration_VideoLayerTimeScaling);
    TestFramework::instance().addTest("Integration_LayerProperties", test_Integration_LayerProperties);
    TestFramework::instance().addTest("Integration_MIDISyncSource_DisplayLatencyAtomicity", test_Integration_MIDISyncSource_DisplayLatencyAtomicity);
    TestFramework::instance().addTest("Integration_MIDISyncSource_GetDisplayLatencyMs", test_Integration_MIDISyncSource_GetDisplayLatencyMs);
    TestFramework::instance().addTest("Integration_FramerateConverter_DelegatesDisplayLatency", test_Integration_FramerateConverter_DelegatesDisplayLatency);
    TestFramework::instance().addTest("Integration_FramerateConverter_SameRateFollowsTimeNotRawFrame", test_Integration_FramerateConverter_SameRateFollowsTimeNotRawFrame);
#ifdef HAVE_MTCRECEIVER
    TestFramework::instance().addTest("Integration_MIDISyncSource_NoJumpSnapBias", test_Integration_MIDISyncSource_NoJumpSnapBias);
    TestFramework::instance().addTest("Integration_MIDISyncSource_RateWindowImmuneToPollCadence", test_Integration_MIDISyncSource_RateWindowImmuneToPollCadence);
#endif

    TestFramework::instance().addTest("MTCDecoder", test_MTCDecoder);

    TestFramework::instance().addTest("DayAlign_FrameMsConversionAndSign", test_DayAlign_FrameMsConversionAndSign);
    TestFramework::instance().addTest("DayAlign_KZeroIsByteIdentical", test_DayAlign_KZeroIsByteIdentical);
    TestFramework::instance().addTest("DayAlign_TwelveHourTies", test_DayAlign_TwelveHourTies);
    TestFramework::instance().addTest("DayAlign_LatchPendingLatestWinsAndClear", test_DayAlign_LatchPendingLatestWinsAndClear);
    TestFramework::instance().addTest("DayAlign_UnresolvedWarningOncePerValue", test_DayAlign_UnresolvedWarningOncePerValue);
    TestFramework::instance().addTest("DayAlign_ImplausiblePosition", test_DayAlign_ImplausiblePosition);
    TestFramework::instance().addTest("DayAlign_LogLines", test_DayAlign_LogLines);
    TestFramework::instance().addTest("DayAlign_Int64ExtremesNoOverflow", test_DayAlign_Int64ExtremesNoOverflow);
#if defined(HAVE_MTCRECEIVER) && defined(MTCRECV_TESTING)
    TestFramework::instance().addTest("DayAlign_RealResolverWaitsForAuthoritativeHead", test_DayAlign_RealResolverWaitsForAuthoritativeHead);
    TestFramework::instance().addTest("DayAlign_LayerAfterRestartShowsTheRightFrame", test_DayAlign_LayerAfterRestartShowsTheRightFrame);
    TestFramework::instance().addTest("DayAlign_LayerAheadOfEngineShowsTheRightFrame", test_DayAlign_LayerAheadOfEngineShowsTheRightFrame);
    TestFramework::instance().addTest("DayAlign_LayerAlignedIsUnchanged", test_DayAlign_LayerAlignedIsUnchanged);
    TestFramework::instance().addTest("DayAlign_LaterWriterReplacesPending", test_DayAlign_LaterWriterReplacesPending);
    TestFramework::instance().addTest("DayAlign_NoReceiverDriverAppliesAsReceived", test_DayAlign_NoReceiverDriverAppliesAsReceived);
    TestFramework::instance().addTest("DayAlign_ResolvedOnReceiptWhenAuthoritative", test_DayAlign_ResolvedOnReceiptWhenAuthoritative);
    TestFramework::instance().addTest("DayAlign_PendingLayerStillPollsOncePerUpdate", test_DayAlign_PendingLayerStillPollsOncePerUpdate);
    TestFramework::instance().addTest("DayAlign_LoopLayerAtNonZeroK", test_DayAlign_LoopLayerAtNonZeroK);
    TestFramework::instance().addTest("DayAlign_LayerInt64Extremes", test_DayAlign_LayerInt64Extremes);
#endif
    TestFramework::instance().addTest("SMPTEUtils_Overflow24h", test_SMPTEUtils_Overflow24h);
    TestFramework::instance().addTest("LayerPathId_IntegerIds", test_LayerPathId_IntegerIds);
    TestFramework::instance().addTest("LayerPathId_UuidsAreNotIntegerIds", test_LayerPathId_UuidsAreNotIntegerIds);
    TestFramework::instance().addTest("LayerPathId_Malformed", test_LayerPathId_Malformed);

    TestFramework::instance().addTest("ExitReporter_SilentBeforeRunning", test_ExitReporter_SilentBeforeRunning);
    TestFramework::instance().addTest("ExitReporter_CleanShutdownIsInfo", test_ExitReporter_CleanShutdownIsInfo);
    TestFramework::instance().addTest("ExitReporter_DirtyExitIsErrorWithCensus", test_ExitReporter_DirtyExitIsErrorWithCensus);
    TestFramework::instance().addTest("ExitReporter_PeaksSurviveClose", test_ExitReporter_PeaksSurviveClose);
    TestFramework::instance().addTest("ExitReporter_FatalSignalRecordAndReraise", test_ExitReporter_FatalSignalRecordAndReraise);
    TestFramework::instance().addTest("ExitReporter_ReportsOnlyOnce", test_ExitReporter_ReportsOnlyOnce);
    TestFramework::instance().addTest("ExitReporter_CensusLine", test_ExitReporter_CensusLine);

    TestFramework::instance().addTest("RecoveryPolicy_LimpingYieldsAccumulateToCap", test_RecoveryPolicy_LimpingYieldsAccumulateToCap);
    TestFramework::instance().addTest("RecoveryPolicy_GoodRunResetsTheLadder", test_RecoveryPolicy_GoodRunResetsTheLadder);
    TestFramework::instance().addTest("RecoveryPolicy_JustBelowThresholdDoesNotDecay", test_RecoveryPolicy_JustBelowThresholdDoesNotDecay);
    TestFramework::instance().addTest("RecoveryPolicy_DeclarationConsumesNothing", test_RecoveryPolicy_DeclarationConsumesNothing);
    TestFramework::instance().addTest("RecoveryPolicy_DecayRescuesAtTheCap", test_RecoveryPolicy_DecayRescuesAtTheCap);
    TestFramework::instance().addTest("RecoveryPolicy_OneWakeIsOneEpisode", test_RecoveryPolicy_OneWakeIsOneEpisode);
    TestFramework::instance().addTest("RecoveryPolicy_ZeroYieldFirstWakeAttempts", test_RecoveryPolicy_ZeroYieldFirstWakeAttempts);

    TestFramework::instance().addTest("HangGuard_HdIsExemptAtTheBoundary", test_HangGuard_HdIsExemptAtTheBoundary);
    TestFramework::instance().addTest("HangGuard_UhdAndDciAreFourKClass", test_HangGuard_UhdAndDciAreFourKClass);
    TestFramework::instance().addTest("HangGuard_MissingMetadataFailsClosed", test_HangGuard_MissingMetadataFailsClosed);
    TestFramework::instance().addTest("HangGuard_AboveEnvelopeIsFlaggedNotRefused", test_HangGuard_AboveEnvelopeIsFlaggedNotRefused);
    TestFramework::instance().addTest("HangGuard_AdmitsUpToCapThenRefuses", test_HangGuard_AdmitsUpToCapThenRefuses);
    TestFramework::instance().addTest("HangGuard_ExemptSessionsAreNeverRefused", test_HangGuard_ExemptSessionsAreNeverRefused);
    TestFramework::instance().addTest("HangGuard_MonitorOnlyProfileNeverRefuses", test_HangGuard_MonitorOnlyProfileNeverRefuses);
    TestFramework::instance().addTest("HangGuard_DisarmStopsRefusalButKeepsCounting", test_HangGuard_DisarmStopsRefusalButKeepsCounting);
    TestFramework::instance().addTest("HangGuard_DefaultOffKeepsCapAndRefusesNothing", test_HangGuard_DefaultOffKeepsCapAndRefusesNothing);
    TestFramework::instance().addTest("HangGuard_RepeatedRevealDoesNotDoubleCount", test_HangGuard_RepeatedRevealDoesNotDoubleCount);
    TestFramework::instance().addTest("HangGuard_ReleaseIsIdempotentAndSafeWhenUnreserved", test_HangGuard_ReleaseIsIdempotentAndSafeWhenUnreserved);
    TestFramework::instance().addTest("HangGuard_RefusedRevealReservesNothing", test_HangGuard_RefusedRevealReservesNothing);
    TestFramework::instance().addTest("HangGuard_ArmedCountCountsOnceAndReleasesOnce", test_HangGuard_ArmedCountCountsOnceAndReleasesOnce);
    TestFramework::instance().addTest("HangGuard_ArmedCountIgnoresExemptLoads", test_HangGuard_ArmedCountIgnoresExemptLoads);
    TestFramework::instance().addTest("HangGuard_ArmedCountOverCapDoesNotRefuse", test_HangGuard_ArmedCountOverCapDoesNotRefuse);
    TestFramework::instance().addTest("HangGuard_AdvisoryLatchTracksTheCrossing", test_HangGuard_AdvisoryLatchTracksTheCrossing);
    TestFramework::instance().addTest("HangGuard_NoAdvisoryWithoutACap", test_HangGuard_NoAdvisoryWithoutACap);
    TestFramework::instance().addTest("HangGuard_MonitorStateCarriesBothCountsSeparately", test_HangGuard_MonitorStateCarriesBothCountsSeparately);
    TestFramework::instance().addTest("HangGuard_OnlyMeasuredProfilesAreArmed", test_HangGuard_OnlyMeasuredProfilesAreArmed);
    TestFramework::instance().addTest("HangGuard_LegacyRamKeyedNamesAreAliases", test_HangGuard_LegacyRamKeyedNamesAreAliases);
    TestFramework::instance().addTest("HangGuard_PicassoArmsIndependentlyOfRam", test_HangGuard_PicassoArmsIndependentlyOfRam);
    TestFramework::instance().addTest("HangGuard_DetectAlwaysYieldsAUsableProfile", test_HangGuard_DetectAlwaysYieldsAUsableProfile);
    TestFramework::instance().addTest("HangGuard_DeferredAdvisoryClearIsNotLost", test_HangGuard_DeferredAdvisoryClearIsNotLost);
    TestFramework::instance().addTest("HangGuard_PollIsANoOpWhenTheLogIsCurrent", test_HangGuard_PollIsANoOpWhenTheLogIsCurrent);
    TestFramework::instance().addTest("HangGuard_TransferMovesTheSlotWithoutChangingCounts", test_HangGuard_TransferMovesTheSlotWithoutChangingCounts);
    TestFramework::instance().addTest("HangGuard_TransferFromUnreservedIsANoOp", test_HangGuard_TransferFromUnreservedIsANoOp);

#ifdef HAVE_CONTRACT_TESTS
    TestFramework::instance().addTest("Contract_ModeFromTimingAndIds", test_Contract_ModeFromTimingAndIds);
    TestFramework::instance().addTest("Contract_HelloIgnoresVersion", test_Contract_HelloIgnoresVersion);
    TestFramework::instance().addTest("Contract_Errors", test_Contract_Errors);
    TestFramework::instance().addTest("Contract_HardwareShape", test_Contract_HardwareShape);
    TestFramework::instance().addTest("Contract_DuplicateModesCollapse", test_Contract_DuplicateModesCollapse);
    TestFramework::instance().addTest("Contract_StateShowLock", test_Contract_StateShowLock);
    TestFramework::instance().addTest("Contract_CachedHardwareMatches", test_Contract_CachedHardwareMatches);
    TestFramework::instance().addTest("Contract_ServerRoundTripAndBindFailure", test_Contract_ServerRoundTripAndBindFailure);
    TestFramework::instance().addTest("Contract_ServerLifecycleStress", test_Contract_ServerLifecycleStress);
    TestFramework::instance().addTest("Contract_OversizedRequestDoesNotKillServer", test_Contract_OversizedRequestDoesNotKillServer);
    TestFramework::instance().addTest("Contract_PubEventsReachSubscriber", test_Contract_PubEventsReachSubscriber);
#endif
    TestFramework::instance().addTest("PresentationTiming_CaptureDisabled_NoOp", test_PresentationTiming_CaptureDisabled_NoOp);
    TestFramework::instance().addTest("PresentationTiming_FifoPairing", test_PresentationTiming_FifoPairing);
    TestFramework::instance().addTest("PresentationTiming_FifoPairing_UsesKernelUst", test_PresentationTiming_FifoPairing_UsesKernelUst);
    TestFramework::instance().addTest("PresentationTiming_DiscardPendingSubmit", test_PresentationTiming_DiscardPendingSubmit);
    TestFramework::instance().addTest("PresentationTiming_StatisticsMedianAndP95", test_PresentationTiming_StatisticsMedianAndP95);
    TestFramework::instance().addTest("PresentationTiming_ResetClearsState", test_PresentationTiming_ResetClearsState);
    TestFramework::instance().addTest("PresentationTiming_ConcurrentSubmitFlip", test_PresentationTiming_ConcurrentSubmitFlip);
    TestFramework::instance().addTest("PresentationTiming_SkippedVsyncsFromMscDelta", test_PresentationTiming_SkippedVsyncsFromMscDelta);
    TestFramework::instance().addTest("PresentationTiming_SustainedUnderrateDetected", test_PresentationTiming_SustainedUnderrateDetected);
    TestFramework::instance().addTest("PresentationTiming_NoUnderrateAtFullRate", test_PresentationTiming_NoUnderrateAtFullRate);

    TestFramework::instance().addTest("NdiPreview_LiveLayerPullsWithoutMtc", test_NdiPreview_LiveLayerPullsWithoutMtc);
    TestFramework::instance().addTest("NdiPreview_LivePullNeverBlocks", test_NdiPreview_LivePullNeverBlocks);
    TestFramework::instance().addTest("NdiPreview_ReadLatestFrameZeroWaitIsNonBlocking", test_NdiPreview_ReadLatestFrameZeroWaitIsNonBlocking);
    TestFramework::instance().addTest("NdiPreview_FileLayerUnchangedWithoutSync", test_NdiPreview_FileLayerUnchangedWithoutSync);
    TestFramework::instance().addTest("NdiPreview_AutoUnloadSkipsUnknownLength", test_NdiPreview_AutoUnloadSkipsUnknownLength);
    TestFramework::instance().addTest("NdiPreview_FitOutputTest3Regions", test_NdiPreview_FitOutputTest3Regions);
    TestFramework::instance().addTest("NdiPreview_FitOutputNativeMatchesEngine", test_NdiPreview_FitOutputNativeMatchesEngine);
    TestFramework::instance().addTest("NdiPreview_FitOutputNarrowCanvasAndFallback", test_NdiPreview_FitOutputNarrowCanvasAndFallback);
    TestFramework::instance().addTest("NdiPreview_FitOutputRefitsOnFormatChange", test_NdiPreview_FitOutputRefitsOnFormatChange);
    TestFramework::instance().addTest("NdiPreview_JournalContract", test_NdiPreview_JournalContract);

    TestFramework::instance().addTest("NdiPreview_AddressForm", test_NdiPreview_AddressForm);
    TestFramework::instance().addTest("NdiPreview_TransportDecision", test_NdiPreview_TransportDecision);
    TestFramework::instance().addTest("NdiPreview_JournalContractRelay", test_NdiPreview_JournalContractRelay);

    TestFramework::instance().addTest("CubeLut_ParsesHeaderAndData", test_CubeLut_ParsesHeaderAndData);
    TestFramework::instance().addTest("CubeLut_RedVariesFastest", test_CubeLut_RedVariesFastest);
    TestFramework::instance().addTest("CubeLut_DomainMinMax", test_CubeLut_DomainMinMax);
    TestFramework::instance().addTest("CubeLut_InputRangeMapsToDomain", test_CubeLut_InputRangeMapsToDomain);
    TestFramework::instance().addTest("CubeLut_RejectsDomainAndInputRangeDisagree", test_CubeLut_RejectsDomainAndInputRangeDisagree);
    TestFramework::instance().addTest("CubeLut_RejectsDomainMaxNotAboveMin", test_CubeLut_RejectsDomainMaxNotAboveMin);
    TestFramework::instance().addTest("CubeLut_RejectsSizeOutOfRange", test_CubeLut_RejectsSizeOutOfRange);
    TestFramework::instance().addTest("CubeLut_RejectsShortOrLongData", test_CubeLut_RejectsShortOrLongData);
    TestFramework::instance().addTest("CubeLut_RejectsFourTokenLine", test_CubeLut_RejectsFourTokenLine);
    TestFramework::instance().addTest("CubeLut_Rejects1D", test_CubeLut_Rejects1D);
    TestFramework::instance().addTest("CubeLut_RejectsUnknownKeyword", test_CubeLut_RejectsUnknownKeyword);
    TestFramework::instance().addTest("CubeLut_RejectsNonFinite", test_CubeLut_RejectsNonFinite);
    TestFramework::instance().addTest("CubeLut_AcceptsBomTabsScientificCrlf", test_CubeLut_AcceptsBomTabsScientificCrlf);
    TestFramework::instance().addTest("CubeLut_ClampsOutOfRangeAndCounts", test_CubeLut_ClampsOutOfRangeAndCounts);
    TestFramework::instance().addTest("CubeLut_RefusesNonRegularFile", test_CubeLut_RefusesNonRegularFile);
    TestFramework::instance().addTest("CubeLut_RejectsDotDotRelativePath", test_CubeLut_RejectsDotDotRelativePath);
    TestFramework::instance().addTest("CubeLut_SizeCap16MiB", test_CubeLut_SizeCap16MiB);
    TestFramework::instance().addTest("CubeLut_ParseTime65", test_CubeLut_ParseTime65);
    TestFramework::instance().addTest("OutputColor_DefaultsAreIdentity", test_OutputColor_DefaultsAreIdentity);
    TestFramework::instance().addTest("OutputColor_ClampsMatchLayerHandlers", test_OutputColor_ClampsMatchLayerHandlers);
    TestFramework::instance().addTest("OutputColor_HueFoldNeverLoops", test_OutputColor_HueFoldNeverLoops);
    TestFramework::instance().addTest("OutputColor_FromCharsWholeToken", test_OutputColor_FromCharsWholeToken);
    TestFramework::instance().addTest("OutputColor_ReferenceMatchesLayerSemantics", test_OutputColor_ReferenceMatchesLayerSemantics);
    TestFramework::instance().addTest("OutputColor_LutCoordHalfTexel", test_OutputColor_LutCoordHalfTexel);
    TestFramework::instance().addTest("OutputBlitShader_ProgramATextUnchanged", test_OutputBlitShader_ProgramATextUnchanged);
    TestFramework::instance().addTest("OutputBlitShader_ProgramBEmbedsSharedGlsl", test_OutputBlitShader_ProgramBEmbedsSharedGlsl);
    TestFramework::instance().addTest("VideoShaders_ColorFunctionsPinned", test_VideoShaders_ColorFunctionsPinned);
    TestFramework::instance().addTest("ColorMatrix_MatchesReference", test_ColorMatrix_MatchesReference);
    TestFramework::instance().addTest("ColorMatrix_KnownValues", test_ColorMatrix_KnownValues);
    TestFramework::instance().addTest("ColorMatrix_IdentityIsIdentity", test_ColorMatrix_IdentityIsIdentity);
    TestFramework::instance().addTest("ColorMatrix_HueZeroSkipsRotation", test_ColorMatrix_HueZeroSkipsRotation);
    TestFramework::instance().addTest("ColorMatrix_ColumnMajor", test_ColorMatrix_ColumnMajor);
    TestFramework::instance().addTest("ColorGlsl_MultiplyOrder", test_ColorGlsl_MultiplyOrder);
    TestFramework::instance().addTest("ColorGlsl_UniformNamesDeclared", test_ColorGlsl_UniformNamesDeclared);
    TestFramework::instance().addTest("VideoShaders_ColorFragmentsUseMatrix", test_VideoShaders_ColorFragmentsUseMatrix);
    TestFramework::instance().addTest("OutputRegion_CopyCarriesLut", test_OutputRegion_CopyCarriesLut);
    TestFramework::instance().addTest("DisplayConfig_ColorKeysRoundTrip", test_DisplayConfig_ColorKeysRoundTrip);
    TestFramework::instance().addTest("DisplayConfig_PartialColorKeys", test_DisplayConfig_PartialColorKeys);
    TestFramework::instance().addTest("DisplayConfig_ValueCrlfAndTrailingSpaceStripped", test_DisplayConfig_ValueCrlfAndTrailingSpaceStripped);
    TestFramework::instance().addTest("DisplayConfig_BadColorValueIsLoudAndIgnored", test_DisplayConfig_BadColorValueIsLoudAndIgnored);
    TestFramework::instance().addTest("DisplayConfig_OutOfRangeClampedWithWarning", test_DisplayConfig_OutOfRangeClampedWithWarning);
    TestFramework::instance().addTest("DisplayConfig_NonFiniteRefused", test_DisplayConfig_NonFiniteRefused);
    TestFramework::instance().addTest("DisplayConfig_UnknownKeysStillIgnored", test_DisplayConfig_UnknownKeysStillIgnored);
    TestFramework::instance().addTest("DisplayConfig_SpacedKeyIsUnknown", test_DisplayConfig_SpacedKeyIsUnknown);
    TestFramework::instance().addTest("DisplayConfig_ColorWithAutoLayout", test_DisplayConfig_ColorWithAutoLayout);
    TestFramework::instance().addTest("DisplayConfig_ColorWithoutCanvasRegionWarns", test_DisplayConfig_ColorWithoutCanvasRegionWarns);
    TestFramework::instance().addTest("DisplayConfig_LutPathRules", test_DisplayConfig_LutPathRules);
    TestFramework::instance().addTest("ShowLock_GateOnTempPath", test_ShowLock_GateOnTempPath);
    TestFramework::instance().addTest("ShowLock_RefusalLineNamesBothCauses", test_ShowLock_RefusalLineNamesBothCauses);
    TestFramework::instance().addTest("OutputColorCommands_ParsesAndClamps", test_OutputColorCommands_ParsesAndClamps);
    TestFramework::instance().addTest("OutputColorCommands_RejectsPathsAndDotDot", test_OutputColorCommands_RejectsPathsAndDotDot);
    TestFramework::instance().addTest("OutputColorCommands_PasteReadyLine", test_OutputColorCommands_PasteReadyLine);
    TestFramework::instance().addTest("OutputColorCommands_DumpAllowedUnderLock", test_OutputColorCommands_DumpAllowedUnderLock);
    TestFramework::instance().addTest("OutputColorCommands_LutNoneClearsOnlyLut", test_OutputColorCommands_LutNoneClearsOnlyLut);
    TestFramework::instance().addTest("OutputColorCommands_StatusToLogLine", test_OutputColorCommands_StatusToLogLine);
    TestFramework::instance().addTest("OutputList_ColorLineNeverMatchesRegionRegex", test_OutputList_ColorLineNeverMatchesRegionRegex);
    TestFramework::instance().addTest("NdiPreview_ExtraIps", test_NdiPreview_ExtraIps);
    TestFramework::instance().addTest("NdiPreview_JournalContractExtraIps", test_NdiPreview_JournalContractExtraIps);
    return TestFramework::instance().runAll();
}

