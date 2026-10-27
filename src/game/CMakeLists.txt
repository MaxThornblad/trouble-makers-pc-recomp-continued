# Host executable build.
#
# Builds `troublemakers`, the host executable that wires the translated game
# (mm_recompiled) to the librecomp/ultramodern runtime and launches it.
#
# The root CMakeLists.txt already: provides RUNTIME_INCLUDES, finds SDL2, adds
# the N64ModernRuntime subdirectory, and add_subdirectory()s this directory
# when this file exists. So this file only declares the troublemakers target.
#
# Link strategy mirrors the proven-working mm_runtime_probe (docs/README.md):
# mm_recompiled is whole-archive linked because the recompiled game is
# data-driven (recomp_overlays.inl's FuncEntry tables hold function pointers
# that no normal call graph reaches) — without --whole-archive the linker
# would strip nearly all of the game. librecomp + ultramodern are also
# whole-archive linked to match the probe's clean link under --no-undefined.
# The two OS wrappers the game needs (rmonPrintf_recomp, __osGetCause_recomp)
# are provided by os_stubs.cpp in this target.

add_executable(troublemakers
    main.cpp
    app_dirs.cpp
    session_log.cpp
    steam_deck.cpp
    register_overlays.cpp
    os_stubs.cpp
    cheats.cpp
    debug_menu.cpp
    gem_hints.cpp
    actors.cpp
    custom_actors.cpp
    grab_blocks.cpp
    dev_harness.cpp
    inspector.cpp
    crash_handler.cpp
    spawn_catalog.cpp
    enemy_bars.cpp
    gem_compass.cpp
    rank_timer.cpp
    rumble.cpp
    savestate.cpp
    replay.cpp
    far_coop.cpp
    player2.cpp
    player2_draw.cpp
    player2_camera.cpp
    netplay.cpp
    steam_net.cpp
    net_enet.cpp
    widescreen.cpp
)

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE MM_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT MM_GIT_COMMIT)
    set(MM_GIT_COMMIT "unknown")
endif()
target_compile_definitions(troublemakers PRIVATE
    MM_GIT_COMMIT="${MM_GIT_COMMIT}"
    MM_BUILD_TYPE="$<CONFIG>"
    MM_PROJECT_VERSION="${MM_PROJECT_VERSION}")

target_include_directories(troublemakers PRIVATE
    ${RUNTIME_INCLUDES}                 # recomp.h, librecomp/*.hpp, ultramodern/*.hpp
    ${CMAKE_SOURCE_DIR}/RecompiledFuncs # funcs.h, recomp_overlays.inl (siblings of the .c TUs)
    ${CMAKE_SOURCE_DIR}/include         # recomp_config.h (game shim)
)

# Component libraries are reached through the callback structs main() builds,
# so normal linking suffices for them — only the game + runtime need
# whole-archive (data-driven function tables, see above). The LINK_LIBRARY
# generator expression emits --whole-archive / /WHOLEARCHIVE / -force_load
# per linker (needs CMake 3.24; set at the top-level CMakeLists).
# Netplay transport (netplay.cpp): ENet, reliable ordered UDP, built from source.
include(FetchContent)
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG v1.3.18
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(enet)
target_include_directories(troublemakers PRIVATE ${enet_SOURCE_DIR}/include)
target_link_libraries(troublemakers PRIVATE enet)
# Steam transport (steam_net.cpp) loads the Steam library at run time.
target_link_libraries(troublemakers PRIVATE ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(troublemakers PRIVATE ws2_32 winmm)
endif()

target_link_libraries(troublemakers PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,mm_recompiled,librecomp,ultramodern>"
    mm_rsp
    mm_audio_input
    mm_diagnostics
    ${SDL2_LIBRARIES}
)

# The RT64 renderer is optional (MM_BUILD_GRAPHICS gates it; see
# src/graphics/CMakeLists.txt). When absent, main.cpp falls back to its stub
# renderer, which aborts the gfx thread cleanly at startup.
if(TARGET mm_graphics)
    target_link_libraries(troublemakers PRIVATE mm_graphics)
    target_compile_definitions(troublemakers PRIVATE MM_HAS_GRAPHICS)

    # Launcher splash screen (launcher.cpp): Dear ImGui + nativefiledialog-
    # extended, both vendored and compiled inside RT64 (rt64.a provides the
    # imgui core and the SDL2 platform backend; nfd is its own target). Only
    # the SDL_Renderer backend TU is missing from rt64, so compile it here.
    target_sources(troublemakers PRIVATE
        launcher.cpp
        ${CMAKE_SOURCE_DIR}/assets/launcher/splash-background.jpg
        ${CMAKE_SOURCE_DIR}/assets/launcher/title-logo.png
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/imgui/backends/imgui_impl_sdlrenderer2.cpp
    )
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/assets/launcher/splash-background.jpg
        ${CMAKE_SOURCE_DIR}/assets/launcher/title-logo.png
        PROPERTIES HEADER_FILE_ONLY TRUE)
    target_include_directories(troublemakers PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/imgui
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/imgui                                      # imgui_impl_sdl2_custom.h
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include
    )
    target_link_libraries(troublemakers PRIVATE nfd)
    add_custom_command(TARGET troublemakers POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/launcher"
            "$<TARGET_FILE_DIR:troublemakers>/assets/launcher"
        COMMENT "Copying launcher artwork")
endif()

# Enforce no unresolved symbols (matches the probe; surfaces any new gap).
# GNU-ld flag; MSVC's link.exe already hard-errors on undefined symbols.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(troublemakers PRIVATE -Wl,--no-undefined)
endif()

if(WIN32)
    # rt64's interface carries bare "SDL2"/"SDL2main" names (its vendored
    # win32 deps); resolve them against the fetched SDL2 libs, exactly as
    # the reference does on its app target.
    target_link_directories(troublemakers PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)

    # Ship the runtime DLLs next to the exe: SDL2 (fetched in the top-level
    # CMakeLists) and RT64's bundled DXC shader compiler. Same set the
    # reference copies.
    add_custom_command(TARGET troublemakers POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_PROPERTY:SDL2::SDL2,IMPORTED_LOCATION>"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:troublemakers>)
    # GUI subsystem for release so no console window flashes; keep the
    # console in Debug for logs. mainCRTStartup keeps our plain main().
    if(MINGW)
        # GNU-ld spelling of the GUI subsystem; link the GCC runtime
        # statically so the exe needs no MinGW DLLs.
        target_link_options(troublemakers PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:-mwindows>
            -static -static-libgcc -static-libstdc++)
    else()
    set_target_properties(troublemakers PROPERTIES
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_MINSIZEREL "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
    endif()
endif()

if(BUILD_TESTING)
    add_executable(mm_presentation_tests tests/presentation_test.cpp)
    target_include_directories(mm_presentation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME mm_presentation COMMAND mm_presentation_tests)

    find_package(Threads REQUIRED)
    add_executable(mm_session_log_tests
        tests/session_log_test.cpp
        session_log.cpp
    )
    target_include_directories(mm_session_log_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mm_session_log_tests PRIVATE SDL2::SDL2 Threads::Threads)
    add_test(NAME mm_session_log COMMAND mm_session_log_tests)
endif()
