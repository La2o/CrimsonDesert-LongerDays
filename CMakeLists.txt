cmake_minimum_required(VERSION 3.20)
project(CrimsonDesertLongerDays LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Host unit tests for the game-independent logic (any OS):
#   cmake -S . -B build-tests -DLONGERDAYS_TESTS=ON && cmake --build build-tests && ctest --test-dir build-tests
option(LONGERDAYS_TESTS "Build the host unit tests instead of the mod" OFF)
if(LONGERDAYS_TESTS)
    enable_testing()
    add_executable(longerdays_tests tests/tests.cpp src/schedule.cpp src/rate_meter.cpp)
    add_test(NAME longerdays_tests COMMAND longerdays_tests)
    return()
endif()

# Builds CrimsonDesertLongerDays.asi, loaded into the game by an ASI loader
# (Ultimate ASI Loader's winmm.dll / dinput8.dll, or a mod manager's loader).
if(NOT WIN32)
    message(FATAL_ERROR "The mod targets Windows (MSVC, or MinGW-w64 cross-compile). "
                        "On other hosts, configure with -DLONGERDAYS_TESTS=ON to run the tests.")
endif()

enable_language(RC)

# Static CRT so players don't need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
)
FetchContent_MakeAvailable(minhook)

add_library(CrimsonDesertLongerDays SHARED
    src/dllmain.cpp
    src/longer_days.cpp
    src/schedule.cpp
    src/rate_meter.cpp
    src/config.cpp
    src/scanner.cpp
    src/log.cpp
    src/version.rc
)
target_link_libraries(CrimsonDesertLongerDays PRIVATE minhook)
target_compile_definitions(CrimsonDesertLongerDays PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

if(MINGW)
    target_link_options(CrimsonDesertLongerDays PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# An .asi is just a DLL with a different extension.
set_target_properties(CrimsonDesertLongerDays PROPERTIES PREFIX "" SUFFIX ".asi")

install(TARGETS CrimsonDesertLongerDays RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES CrimsonDesertLongerDays.ini DESTINATION .)
