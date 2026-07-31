#ifndef _BS_LOOP_H_
#define _BS_LOOP_H_

#include "runner.h"

// ===[ COMMAND LINE ARGUMENTS ]===

typedef struct { const char* name; YoYoOperatingSystem value; } OsTypeNameEntry;

static const OsTypeNameEntry OS_TYPE_NAMES[] = {
    {"unknown",       OS_UNKNOWN},
    {"windows",       OS_WINDOWS},
    {"win32",         OS_WINDOWS},
    {"macosx",        OS_MACOSX},
    {"macos",         OS_MACOSX},
    {"psp",           OS_PSP},
    {"ios",           OS_IOS},
    {"android",       OS_ANDROID},
    {"symbian",       OS_SYMBIAN},
    {"linux",         OS_LINUX},
    {"winphone",      OS_WINPHONE},
    {"tizen",         OS_TIZEN},
    {"win8native",    OS_WIN8NATIVE},
    {"wiiu",          OS_WIIU},
    {"3ds",           OS_3DS},
    {"psvita",        OS_PSVITA},
    {"bb10",          OS_BB10},
    {"ps4",           OS_PS4},
    {"xboxone",       OS_XBOXONE},
    {"ps3",           OS_PS3},
    {"xbox360",       OS_XBOX360},
    {"uwp",           OS_UWP},
    {"amazon",        OS_AMAZON},
    {"switch",        OS_SWITCH},
};
#define OS_TYPE_NAMES_COUNT (sizeof(OS_TYPE_NAMES)/sizeof(OS_TYPE_NAMES[0]))

typedef struct {
    int key;
    // We need this dummy value, think that the ds_map is like a Java HashMap NOT a HashSet
    // (Which is funny, because in Java HashSets are backed by HashMaps lol)
    bool value;
} FrameSetEntry;

typedef struct {
    const char* dataWinPath;
    const char* saveFolder; // null = default to the directory containing dataWinPath
    const char* screenshotPattern;
    FrameSetEntry* screenshotFrames;
    const char* screenshotSurfacesPattern;
    FrameSetEntry* screenshotSurfacesFrames;
    FrameSetEntry* dumpFrames;
    FrameSetEntry* dumpJsonFrames;
    const char* dumpJsonFilePattern;
    StringBooleanEntry* varReadsToBeTraced;
    StringBooleanEntry* varWritesToBeTraced;
    StringBooleanEntry* functionCallsToBeTraced;
    StringBooleanEntry* alarmsToBeTraced;
    StringBooleanEntry* instanceLifecyclesToBeTraced;
    StringBooleanEntry* eventsToBeTraced;
    StringBooleanEntry* collisionsToBeTraced;
    StringBooleanEntry* opcodesToBeTraced;
    StringBooleanEntry* stackToBeTraced;
    StringBooleanEntry* disassemble;
    StringBooleanEntry* tilesToBeTraced;
    bool alwaysLogUnknownFunctions;
    bool alwaysLogStubbedFunctions;
    bool headless;
    bool traceFrames;
    bool printRooms;
    bool printObjects;
    bool printShaders;
    bool printDeclaredFunctions;
    bool printUnknownFunctions;
    int exitAtFrame;
    int traceBytecodeAfterFrame;
    double speedMultiplier;
    double fastForwardSpeed;
    int seed;
    bool hasSeed;
    bool debug;
    bool traceEventInherited;
    const char* recordInputsPath;
    const char* playbackInputsPath;
    const char* renderer;
    YoYoOperatingSystem osType;
    int32_t windowWidth, windowHeight; // 0 = auto (gen8 default, or the console-native size for console os-types)
    float widescreenAspect; // "widescreen hack" target aspect ratio (width/height), 0 = disabled
    char** gameArgs; // stb_ds array of owned strings, gameArgs[0] = runner executable path
    bool lazyRooms;
    StringBooleanEntry* eagerRooms; // stb_ds string-keyed set of room names
    bool lazyTextures;
    bool lazyAudio;
    DataWinLoadType loadType;
    int profilerFramesBetween; // 0 = disabled
#ifdef ENABLE_VM_OPCODE_PROFILER
    bool opcodeProfiler;
#endif
} CommandLineArgs;

char** extractRunnerArguments(char* rawArguments);
int loop(CommandLineArgs args, const char *argv0);

#endif
