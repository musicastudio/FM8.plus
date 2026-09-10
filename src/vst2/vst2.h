// Minimal VST 2.4 ABI definitions, written from the public interface description.
// Only what FM8.plus needs: AEffect, the host callback, events, time info, opcodes.
#pragma once
#include <cstdint>

struct AEffect;
typedef intptr_t (*AudioMasterCallback)(AEffect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
typedef intptr_t (*AEffectDispatcherProc)(AEffect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
typedef void (*AEffectProcessProc)(AEffect*, float** inputs, float** outputs, int32_t sampleFrames);
typedef void (*AEffectProcessDoubleProc)(AEffect*, double** inputs, double** outputs, int32_t sampleFrames);
typedef void (*AEffectSetParameterProc)(AEffect*, int32_t index, float value);
typedef float (*AEffectGetParameterProc)(AEffect*, int32_t index);

constexpr int32_t kEffectMagic = 0x56737450;  // 'VstP'

struct AEffect {
    int32_t magic;
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process;  // deprecated
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t resvd1;
    intptr_t resvd2;
    int32_t initialDelay;
    int32_t realQualities;  // deprecated
    int32_t offQualities;   // deprecated
    float ioRatio;          // deprecated
    void* object;
    void* user;
    int32_t uniqueID;
    int32_t version;
    AEffectProcessProc processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;
    char future[56];
};

enum AEffectFlags {
    effFlagsHasEditor = 1 << 0,
    effFlagsCanReplacing = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth = 1 << 8,
    effFlagsNoSoundInStop = 1 << 9,
    effFlagsCanDoubleReplacing = 1 << 12,
};

enum AEffectOpcodes {
    effOpen = 0, effClose, effSetProgram, effGetProgram, effSetProgramName, effGetProgramName,
    effGetParamLabel, effGetParamDisplay, effGetParamName, effGetVu, effSetSampleRate,
    effSetBlockSize, effMainsChanged, effEditGetRect, effEditOpen, effEditClose, effEditDraw,
    effEditMouse, effEditKey, effEditIdle, effEditTop, effEditSleep, effIdentify, effGetChunk,
    effSetChunk, effProcessEvents, effCanBeAutomated, effString2Parameter,
    effGetNumProgramCategories, effGetProgramNameIndexed, effCopyProgram, effConnectInput,
    effConnectOutput, effGetInputProperties, effGetOutputProperties, effGetPlugCategory,
    effGetCurrentPosition, effGetDestinationBuffer, effOfflineNotify, effOfflinePrepare,
    effOfflineRun, effProcessVarIo, effSetSpeakerArrangement, effSetBlockSizeAndSampleRate,
    effSetBypass, effGetEffectName, effGetErrorText, effGetVendorString, effGetProductString,
    effGetVendorVersion, effVendorSpecific, effCanDo, effGetTailSize, effIdle, effGetIcon,
    effSetViewPosition, effGetParameterProperties, effKeysRequired, effGetVstVersion,
    effEditKeyDown, effEditKeyUp, effSetEditKnobMode, effGetMidiProgramName,
    effGetCurrentMidiProgram, effGetMidiProgramCategory, effHasMidiProgramsChanged,
    effGetMidiKeyName, effBeginSetProgram, effEndSetProgram, effGetSpeakerArrangement,
    effShellGetNextPlugin, effStartProcess, effStopProcess, effSetTotalSampleToProcess,
    effSetPanLaw, effBeginLoadBank, effBeginLoadProgram, effSetProcessPrecision,
    effGetNumMidiInputChannels, effGetNumMidiOutputChannels,
};

enum AudioMasterOpcodes {
    audioMasterAutomate = 0, audioMasterVersion, audioMasterCurrentId, audioMasterIdle,
    audioMasterPinConnected, audioMasterUnused5, audioMasterWantMidi, audioMasterGetTime,
    audioMasterProcessEvents, audioMasterSetTime, audioMasterTempoAt,
    audioMasterGetNumAutomatableParameters, audioMasterGetParameterQuantization,
    audioMasterIOChanged, audioMasterNeedIdle, audioMasterSizeWindow, audioMasterGetSampleRate,
    audioMasterGetBlockSize, audioMasterGetInputLatency, audioMasterGetOutputLatency,
    audioMasterGetPreviousPlug, audioMasterGetNextPlug, audioMasterWillReplaceOrAccumulate,
    audioMasterGetCurrentProcessLevel, audioMasterGetAutomationState, audioMasterOfflineStart,
    audioMasterOfflineRead, audioMasterOfflineWrite, audioMasterOfflineGetCurrentPass,
    audioMasterOfflineGetCurrentMetaPass, audioMasterSetOutputSampleRate,
    audioMasterGetOutputSpeakerArrangement, audioMasterGetVendorString,
    audioMasterGetProductString, audioMasterGetVendorVersion, audioMasterVendorSpecific,
    audioMasterSetIcon, audioMasterCanDo, audioMasterGetLanguage, audioMasterOpenWindow,
    audioMasterCloseWindow, audioMasterGetDirectory, audioMasterUpdateDisplay,
    audioMasterBeginEdit, audioMasterEndEdit, audioMasterOpenFileSelector,
    audioMasterCloseFileSelector, audioMasterEditFile, audioMasterGetChunkFile,
    audioMasterGetInputSpeakerArrangement,
};

enum VstEventTypes { kVstMidiType = 1, kVstSysExType = 6 };

struct VstEvent {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    char data[16];
};

struct VstMidiEvent {
    int32_t type;        // kVstMidiType
    int32_t byteSize;    // sizeof(VstMidiEvent)
    int32_t deltaFrames; // sample offset inside the block
    int32_t flags;       // kVstMidiEventIsRealtime = 1
    int32_t noteLength;
    int32_t noteOffset;
    char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
};

struct VstEvents {
    int32_t numEvents;
    intptr_t reserved;
    VstEvent* events[2];  // variable length in practice
};

struct VstTimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};

enum VstTimeInfoFlags {
    kVstTransportChanged = 1, kVstTransportPlaying = 1 << 1, kVstTransportCycleActive = 1 << 2,
    kVstTransportRecording = 1 << 3, kVstAutomationWriting = 1 << 6, kVstAutomationReading = 1 << 7,
    kVstNanosValid = 1 << 8, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10,
    kVstBarsValid = 1 << 11, kVstCyclePosValid = 1 << 12, kVstTimeSigValid = 1 << 13,
    kVstSmpteValid = 1 << 14, kVstClockValid = 1 << 15,
};

struct ERect { int16_t top, left, bottom, right; };
