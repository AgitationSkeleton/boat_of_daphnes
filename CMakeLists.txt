cmake_minimum_required(VERSION 3.25)
project(twwrecomp LANGUAGES C CXX VERSION 0.1.0)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (UNIX AND NOT APPLE)
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    set(CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif ()
find_package(Threads REQUIRED)

# The recompiler: main.dol + REL modules -> C++ (the SA2B pipeline's, docs/PLAN.md).
add_library(tww_recompiler STATIC
    src/recompiler/image.cpp
    src/recompiler/analyze.cpp
    src/recompiler/emit.cpp
    src/recompiler/build.cpp)
target_include_directories(tww_recompiler PUBLIC src)

add_executable(recompile tools/recomp/recompile.cpp)
target_link_libraries(recompile PRIVATE tww_recompiler)

# ---- Aurora: GX renderer (WebGPU/Dawn), window, controllers (extern/aurora, MIT) ---------------
# Local changes to Aurora are kept as patches (patches/aurora); applied to a pristine checkout only.
file(GLOB AURORA_PATCHES ${CMAKE_SOURCE_DIR}/patches/aurora/*.patch)
list(SORT AURORA_PATCHES)
set(_aurora ${CMAKE_SOURCE_DIR}/extern/aurora)
execute_process(COMMAND git diff --quiet HEAD WORKING_DIRECTORY ${_aurora} RESULT_VARIABLE _dirty)
if (_dirty EQUAL 0)
    execute_process(COMMAND git config core.autocrlf false WORKING_DIRECTORY ${_aurora})
    execute_process(COMMAND git rm -r -q --cached . WORKING_DIRECTORY ${_aurora} OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND git reset -q --hard WORKING_DIRECTORY ${_aurora})
    execute_process(COMMAND git checkout-index -f -a WORKING_DIRECTORY ${_aurora})
    foreach (_patch ${AURORA_PATCHES})
        execute_process(COMMAND git apply ${_patch} WORKING_DIRECTORY ${_aurora} RESULT_VARIABLE _result)
        if (NOT _result EQUAL 0)
            message(FATAL_ERROR "Failed to apply Aurora patch ${_patch}")
        endif ()
        message(STATUS "Applied Aurora patch ${_patch}")
    endforeach ()
else ()
    message(STATUS "Aurora has local changes: taken as the patched tree (patches/aurora), left as is")
endif ()
# Aurora takes zlib from find_package, then pkg-config, else builds its own. A devkitPro/MSYS2 pkg-config on PATH would
# hand it MSYS's zlib (not linkable here), so on Windows pkg-config is not consulted and nothing under those is searched.
if (WIN32)
    set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)
    list(APPEND CMAKE_IGNORE_PREFIX_PATH C:/devkitPro C:/msys64 C:/msys2 C:/MinGW)
endif ()
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

# Disc images: .iso/.gcm, .ciso, .rvz, .wia (src/disc; RVZ/WIA need Aurora's zstd).
add_library(tww_disc STATIC src/disc/discimage.cpp)
target_include_directories(tww_disc PUBLIC src)
target_link_libraries(tww_disc PRIVATE zstd::libzstd)

# ---- Boat of Daphnes' menus and windows (src/bod: its windows/src and host headers, GPL-3.0-or-later,
# by AgitationSkeleton), built as it builds them; its host API is provided by src/runtime/bod_*.cpp. ---
set(BOD_DIR ${CMAKE_SOURCE_DIR}/src/bod)
# Archive reading for mods (.zip, .rar, .7z): its vendored libarchive (third_party/libarchive, BSD-2) over
# Aurora's zlib and its liblzma (Dolphin's Externals/liblzma), as its windows/CMakeLists.txt builds them.
function(dolphin_disable_warnings target)
    target_compile_options(${target} PRIVATE -w)
endfunction()
add_subdirectory(${BOD_DIR}/third_party/liblzma liblzma_build EXCLUDE_FROM_ALL)
get_target_property(_lzma_sources lzma SOURCES)
list(FILTER _lzma_sources EXCLUDE REGEX "tuklib_(mbstr_fw|mbstr_width|exit|open_stdxxx|progname)[.]c$")
set_property(TARGET lzma PROPERTY SOURCES ${_lzma_sources})
add_subdirectory(${BOD_DIR}/third_party/libarchive libarchive_build EXCLUDE_FROM_ALL)

# The Tingle Tuner's Game Boy Advance (Boat of Daphnes' windows/CMakeLists.txt): mGBA (MPL-2.0), vendored in
# src/bod/third_party/mgba, as the library only (no frontends, no zlib/lzma/png). Only the GBA core is wanted.
set(LIBMGBA_ONLY ON)
set(USE_LZMA OFF)
set(M_CORE_GB OFF)
set(M_CORE_GBA ON)
add_subdirectory(${BOD_DIR}/third_party/mgba mgba_build EXCLUDE_FROM_ALL)
target_compile_definitions(mgba PUBLIC ENABLE_VFS ENABLE_DIRECTORIES M_CORE_GBA)
target_compile_options(mgba PRIVATE -w)
# Its crc32 from Aurora's zlib (as Dolphin builds it, HAVE_CRC32), not its own: the two clash at link.
target_compile_definitions(mgba PRIVATE HAVE_CRC32)
if (TARGET ZLIB::ZLIB)
    target_link_libraries(mgba ZLIB::ZLIB)
elseif (TARGET zlib)
    target_link_libraries(mgba zlib)
endif ()
# mGBA links libm by name unless the compiler is MSVC; this clang is not (the C runtime has the math functions).
get_target_property(_mgba_libs mgba LINK_LIBRARIES)
list(REMOVE_ITEM _mgba_libs m)
set_property(TARGET mgba PROPERTY LINK_LIBRARIES "${_mgba_libs}")
get_target_property(_mgba_ilibs mgba INTERFACE_LINK_LIBRARIES)
list(REMOVE_ITEM _mgba_ilibs m)
set_property(TARGET mgba PROPERTY INTERFACE_LINK_LIBRARIES "${_mgba_ilibs}")
# emibios (LGPL-3.0-or-later, src/bod/third_party/emibios): the free GBA BIOS, compiled in as a C array.
file(READ ${BOD_DIR}/third_party/emibios/gba_bios.bin _emibios_hex HEX)
string(LENGTH "${_emibios_hex}" _emibios_hex_len)
math(EXPR _emibios_size "${_emibios_hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _emibios_c "${_emibios_hex}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/emibios_data.c.tmp
    "/* generated from src/bod/third_party/emibios/gba_bios.bin */
const unsigned char bluewake_emibios[] = {${_emibios_c}};
const unsigned bluewake_emibios_size = ${_emibios_size}u;
")
file(COPY_FILE ${CMAKE_CURRENT_BINARY_DIR}/emibios_data.c.tmp ${CMAKE_CURRENT_BINARY_DIR}/emibios_data.c ONLY_IF_DIFFERENT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BOD_DIR}/third_party/emibios/gba_bios.bin)
set(BOD_UI_SOURCES
    win_settings.cpp input_editor.cpp gba_window.cpp dev_windows.cpp ship_menu.cpp ship_menu_classic.cpp
    cosmetics_editor.cpp link_colors.cpp model_colors.cpp gx_texture.cpp png_decode.cpp anchor.cpp
    anchor_client.cpp anchor_content.cpp item_found.cpp notifications.cpp item_tracker.cpp mods.cpp rarc.cpp
    hud_colors.cpp randomizer_menu.cpp rando_pack.cpp rando_saves.cpp rando_flow.cpp fs_art.cpp fs_blo.cpp
    fs_font.cpp anchor_carry.cpp check_tracker.cpp input_viewer.cpp player_parts.cpp content_validate.cpp
    voice_bank.cpp gb_browser.cpp)
# What differs per operating system (platform/bod_platform.h): the platform layer, the crash reporter, the GameCube adapter.
if (WIN32)
    list(APPEND BOD_UI_SOURCES win_crash.cpp adapter_driver.cpp platform/platform_win32.cpp)
else ()
    list(APPEND BOD_UI_SOURCES linux_crash.cpp platform/adapter_driver_linux.cpp platform/platform_posix.cpp platform/net_posix.cpp)
endif ()
list(TRANSFORM BOD_UI_SOURCES PREPEND ${BOD_DIR}/windows/src/)
set(BOD_HOST_SOURCES dev_tools.c game_state.c cosmetics.c fast_load.c quick_doors.c treasure_glow.c forest_water.c
    gc_adapter.c gc_adapter_proto.c pad_wire.c haptics.c
    mod_archive.c mods_host.c widescreen.c ui_pad.c team_sync.c puppet.c gba_link.c
    voice_pitch.c audio_volume.c autosave.c puppet_pvp.c puppet_carry.c bunny_hood.c chart_reveal.c text_swap.c
    text_speed.c jabun_guard.c switch_sync.c)
list(TRANSFORM BOD_HOST_SOURCES PREPEND ${BOD_DIR}/runtime/host/src/)
if (WIN32)
    set(BOD_COMPAT_SOURCES ${BOD_DIR}/windows/compat/bw_posix_compat.c)
endif ()
add_library(bod_ui STATIC ${BOD_UI_SOURCES} ${BOD_COMPAT_SOURCES} ${BOD_HOST_SOURCES} ${BOD_DIR}/runtime/host/src/ship/cvar.cpp
    ${BOD_DIR}/shim/bod_stubs.c ${BOD_DIR}/shim/rando_bridge.c ${BOD_DIR}/shim/guest_coro_sync.c ${CMAKE_CURRENT_BINARY_DIR}/emibios_data.c)
target_include_directories(bod_ui PUBLIC ${BOD_DIR}/runtime/host/src ${BOD_DIR}/include ${BOD_DIR}/windows/src)
if (WIN32)
    # (the POSIX headers a Windows build lacks: unistd.h, pthread.h, dirent.h ...)
    target_include_directories(bod_ui PUBLIC ${BOD_DIR}/windows/compat/include ${BOD_DIR}/windows/compat)
endif ()
if (WIN32)
    target_compile_options(bod_ui PRIVATE /FI${BOD_DIR}/windows/compat/bw_posix_compat.h /FI${BOD_DIR}/shim/bod_compat.h
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc> -Wno-unused-function -Wno-unused-variable)
    target_compile_definitions(bod_ui PRIVATE BLUEWAKE_WINDOWS=1 BLUEWAKE_APP_NAME="Boat-Of-Daphnes"
        BLUEWAKE_ENABLE_DEVELOPER_TRACING=0 BLUEWAKE_EDGE_CENSUS=0 _CRT_SECURE_NO_WARNINGS NOMINMAX)
else ()
    # Linux has the POSIX calls the sources use (bw_posix_compat.c is the Windows shim for them: not built here).
    target_compile_options(bod_ui PRIVATE -include ${BOD_DIR}/shim/bod_compat.h -Wno-unused-function -Wno-unused-variable
        # (a function that only calls itself is always a mistake here: a wrapper that forgot its real call hung the game)
        $<$<COMPILE_LANGUAGE:C,CXX>:-Werror=infinite-recursion>)
    target_compile_definitions(bod_ui PRIVATE BLUEWAKE_LINUX=1 BLUEWAKE_APP_NAME="Boat-Of-Daphnes"
        BLUEWAKE_ENABLE_DEVELOPER_TRACING=0 BLUEWAKE_EDGE_CENSUS=0 _GNU_SOURCE)
endif ()
# (gba_link.c: windows.h without the old winsock.h, which clashes with the winsock2.h mGBA's headers include)
set_source_files_properties(${BOD_DIR}/runtime/host/src/gba_link.c PROPERTIES COMPILE_DEFINITIONS WIN32_LEAN_AND_MEAN)
target_link_libraries(bod_ui PRIVATE aurora::core aurora::gx aurora::pad imgui xxhash bluewake_archive mgba)
if (WIN32)
    target_link_libraries(bod_ui PRIVATE winmm bcrypt comctl32 comdlg32 winhttp setupapi shell32 ole32)
else ()
    # The platform layer's sockets and TLS (Anchor, the model browser's downloads) use the system's OpenSSL.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(bod_ui PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS} m)
endif ()


# ---- The program (tww): the runtime. It loads tww_game.dll, the game's code built from the disc ---
set(TWW_ICON_INC ${CMAKE_BINARY_DIR}/generated/app_icon.inc)
if (NOT WIN32)
    # The window's icon on Linux (src/runtime/appicon.h): the PNG compiled in, as the GBA BIOS above. Windows takes it from
    # the program's resources.
    file(READ ${BOD_DIR}/windows/resources/BlueWake.png _icon_hex HEX)
    string(LENGTH "${_icon_hex}" _icon_hex_len)
    math(EXPR _icon_size "${_icon_hex_len} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _icon_c "${_icon_hex}")
    file(WRITE ${TWW_ICON_INC}.tmp "/* generated from src/bod/windows/resources/BlueWake.png */
static const unsigned char kAppIconPng[] = {${_icon_c}};
static const unsigned long kAppIconPngSize = ${_icon_size}ul;
")
    file(COPY_FILE ${TWW_ICON_INC}.tmp ${TWW_ICON_INC} ONLY_IF_DIFFERENT)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BOD_DIR}/windows/resources/BlueWake.png)
endif ()
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS src/runtime/*.cpp)
# First-run setup (src/setup): the game's code built from the player's disc inside the program, with the files it
# compiles the module with embedded.
set(_setup_embed ${CMAKE_BINARY_DIR}/generated/setup_files.inc)
add_custom_command(OUTPUT ${_setup_embed}
    COMMAND ${CMAKE_COMMAND} -DOUT=${_setup_embed} -DROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tools/setup/embed.cmake
    DEPENDS tools/setup/embed.cmake src/runtime/ctx.h src/runtime/modules.h src/runtime/abi.h src/game/glue.cpp config/GZLE01/names.txt)
add_executable(tww ${RUNTIME_SOURCES} src/setup/firstrun.cpp src/setup/setup_ui.cpp ${_setup_embed})
# boat-of-daphnes.exe: what the player clicks (its import library, boat-of-daphnes.lib, is what the game modules link against)
set_target_properties(tww PROPERTIES OUTPUT_NAME boat-of-daphnes)
target_include_directories(tww PRIVATE src ${CMAKE_BINARY_DIR}/generated)
if (MSVC)
    target_compile_options(tww PRIVATE /EHsc)
endif ()
set_target_properties(tww PROPERTIES ENABLE_EXPORTS ON)
if (MSVC)
    target_link_options(tww PRIVATE /ERRORLIMIT:0)
endif ()
target_link_libraries(tww PRIVATE aurora::core aurora::gx aurora::vi aurora::pad aurora::mtx tww_disc tww_recompiler bod_ui imgui Threads::Threads)
target_include_directories(tww PRIVATE ${CMAKE_SOURCE_DIR}/extern/aurora/lib)
if (UNIX)
    target_link_libraries(tww PRIVATE ${CMAKE_DL_LIBS})
endif ()
if (UNIX AND NOT APPLE)
    # The release asks nothing of the player's libstdc++ (the game's modules are freestanding and never see it).
    option(BOD_STATIC_STDLIB "Link libstdc++ and libgcc into the program" OFF)
    if (BOD_STATIC_STDLIB)
        target_link_options(tww PRIVATE -static-libstdc++ -static-libgcc)
    endif ()
endif ()
if (WIN32)
    add_custom_command(TARGET tww POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:tww> $<TARGET_FILE_DIR:tww>
        COMMAND_EXPAND_LISTS)
    file(GLOB _dawn_dx_dlls "${CMAKE_BINARY_DIR}/_deps/dawn_prebuilt-src/bin/dx*.dll")
    if (_dawn_dx_dlls)
        add_custom_command(TARGET tww POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dawn_dx_dlls} $<TARGET_FILE_DIR:tww>)
    endif ()
endif ()

# ---- Developer build of the game module (tww_game.dll) from the recompiler's output -----------
# Generate with: recompile --dol orig/main.dol --rels orig/rel --names config/GZLE01/names.txt
#                --out build/gen      (nothing generated is ever committed)
set(TWW_GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Recompiler output for the developer build")
file(GLOB TWW_GEN_SOURCES CONFIGURE_DEPENDS ${TWW_GEN_DIR}/*.cpp)
if (TWW_GEN_SOURCES)
    add_library(tww_game SHARED ${TWW_GEN_SOURCES} src/game/glue.cpp)
    target_include_directories(tww_game PRIVATE src ${TWW_GEN_DIR})
    target_compile_definitions(tww_game PRIVATE SA2B_GAME_DLL)
    if (WIN32)
        target_compile_options(tww_game PRIVATE /O2 /clang:-g0 /clang:-ffreestanding /GS- /Zl /GR- /EHs-c-
            -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
        target_link_options(tww_game PRIVATE /NOENTRY /NODEFAULTLIB)
        target_link_libraries(tww_game PRIVATE tww)
        set_target_properties(tww_game PROPERTIES MSVC_RUNTIME_LIBRARY "")
    else ()
        target_compile_options(tww_game PRIVATE -O2 -g0 -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics
            -fvisibility=hidden -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
        target_link_options(tww_game PRIVATE -nostdlib -Wl,-Bsymbolic)
        set_target_properties(tww_game PROPERTIES PREFIX "")
        add_dependencies(tww_game tww)
    endif ()
else ()
    message(STATUS "No recompiler output in ${TWW_GEN_DIR}; skipping the developer build of tww_game.dll")
endif ()

# ---- Developer build of the randomizer's game module (tww_game_rando.dll) ----------------------
# Boat of Daphnes' randomizer variant on this pipeline: wwrando's patched code (seed independent: its
# reference run, tools/rando/patch_game.py --iso-out) recompiled as a second module, which a randomizer
# session loads. Generate with: tools/extract.py build/rando/reference.iso build/rando/orig, then
# recompile --dol build/rando/orig/main.dol --rels build/rando/orig/rel --names config/GZLE01/names.txt
# --out build/gen_rando.
set(TWW_GEN_RANDO_DIR "${CMAKE_SOURCE_DIR}/build/gen_rando" CACHE PATH "Recompiler output of the randomizer variant")
file(GLOB TWW_GEN_RANDO_SOURCES CONFIGURE_DEPENDS ${TWW_GEN_RANDO_DIR}/*.cpp)
if (TWW_GEN_RANDO_SOURCES)
    add_library(tww_game_rando SHARED ${TWW_GEN_RANDO_SOURCES} src/game/glue.cpp)
    target_include_directories(tww_game_rando PRIVATE src ${TWW_GEN_RANDO_DIR})
    # (its overrides enter the runtime by their own name: the runtime tells the two modules' slot numbers apart)
    target_compile_definitions(tww_game_rando PRIVATE SA2B_GAME_DLL runOverride=runOverrideRando)
    if (WIN32)
        target_compile_options(tww_game_rando PRIVATE /O2 /clang:-g0 /clang:-ffreestanding /GS- /Zl /GR- /EHs-c-
            -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
        target_link_options(tww_game_rando PRIVATE /NOENTRY /NODEFAULTLIB)
        target_link_libraries(tww_game_rando PRIVATE tww)
        set_target_properties(tww_game_rando PROPERTIES MSVC_RUNTIME_LIBRARY "")
    else ()
        target_compile_options(tww_game_rando PRIVATE -O2 -g0 -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics
            -fvisibility=hidden -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
        target_link_options(tww_game_rando PRIVATE -nostdlib -Wl,-Bsymbolic)
        set_target_properties(tww_game_rando PROPERTIES PREFIX "")
        add_dependencies(tww_game_rando tww)
    endif ()
endif ()
