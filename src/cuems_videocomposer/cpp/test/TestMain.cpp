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
#ifdef HAVE_MTCRECEIVER
extern bool test_Integration_MIDISyncSource_NoJumpSnapBias();
#endif

extern bool test_MTCDecoder();

extern bool test_SMPTEUtils_Overflow24h();

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
#ifdef HAVE_MTCRECEIVER
    TestFramework::instance().addTest("Integration_MIDISyncSource_NoJumpSnapBias", test_Integration_MIDISyncSource_NoJumpSnapBias);
#endif

    TestFramework::instance().addTest("MTCDecoder", test_MTCDecoder);
    TestFramework::instance().addTest("SMPTEUtils_Overflow24h", test_SMPTEUtils_Overflow24h);

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

    return TestFramework::instance().runAll();
}

