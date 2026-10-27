#pragma once

// Single source of the version: the log banner, the DLL's file properties
// (version.rc) and the release zip name (CI) all read it from here.
#define LONGERDAYS_VERSION_MAJOR 0
#define LONGERDAYS_VERSION_MINOR 3
#define LONGERDAYS_VERSION_PATCH 0

#define LONGERDAYS_STR2(x) #x
#define LONGERDAYS_STR(x)  LONGERDAYS_STR2(x)
#define LONGERDAYS_VERSION                       \
    LONGERDAYS_STR(LONGERDAYS_VERSION_MAJOR) "." \
    LONGERDAYS_STR(LONGERDAYS_VERSION_MINOR) "." \
    LONGERDAYS_STR(LONGERDAYS_VERSION_PATCH)
