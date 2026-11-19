#pragma once
#include <juce_core/juce_core.h>
int runExportCrashTest();
int runExportCrashChild(const juce::File& target, bool beforeCommit, bool noReplace);
int runProjectCrashTest();
int runProjectCrashChild(const juce::File& target, const juce::File& source,
                         const juce::String& phase, const juce::String& mode);
