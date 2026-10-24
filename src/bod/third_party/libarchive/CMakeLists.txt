# libarchive for BlueWake: the read side of .zip, .7z, .rar and RAR5 as a static library, for the
# mod archive wrapper (runtime/host/src/mod_archive.c). Not libarchive's own build: its tools, tests,
# write support, disk extraction and the formats mods never use are left out; the configuration is
# the hand-adapted config_bluewake.h. See VENDORED.md.
#
# The caller supplies the libraries libarchive links: ZLIB::ZLIB (zlib or zlib-ng in compat mode) and
# LibLZMA::LibLZMA (RecompCore's Externals/liblzma). Windows' bcrypt does the digests and decryption.
if(NOT TARGET ZLIB::ZLIB OR NOT TARGET LibLZMA::LibLZMA)
    message(FATAL_ERROR "libarchive needs the ZLIB::ZLIB and LibLZMA::LibLZMA targets")
endif()

set(_la ${CMAKE_CURRENT_LIST_DIR}/libarchive)
set(_la_sources
    archive_acl.c archive_check_magic.c archive_cmdline.c archive_cryptor.c archive_digest.c
    archive_entry.c archive_entry_copy_stat.c archive_entry_sparse.c archive_entry_stat.c
    archive_entry_strmode.c archive_entry_xattr.c archive_hmac.c archive_options.c
    archive_pack_dev.c archive_parse_date.c archive_pathmatch.c archive_ppmd7.c archive_ppmd8.c
    archive_random.c archive_rb.c archive_read.c archive_read_add_passphrase.c
    archive_read_append_filter.c archive_read_open_filename.c archive_read_open_memory.c
    archive_read_set_format.c archive_read_set_options.c archive_read_support_filter_none.c
    archive_read_support_format_7zip.c archive_read_support_format_rar.c
    archive_read_support_format_rar5.c archive_read_support_format_zip.c
    archive_string.c archive_string_sprintf.c archive_time.c archive_util.c
    archive_version_details.c archive_virtual.c archive_windows.c
    archive_blake2s_ref.c archive_blake2sp_ref.c)
list(TRANSFORM _la_sources PREPEND ${_la}/)

add_library(bluewake_archive STATIC ${_la_sources})
set_target_properties(bluewake_archive PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_include_directories(bluewake_archive PUBLIC ${_la} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
# The Windows configuration uses CNG (bcrypt); the Linux one uses OpenSSL's libcrypto for the digests and encrypted entries.
if(WIN32)
    set(_la_config config_bluewake.h)
else()
    set(_la_config config_bluewake_linux.h)
endif()
target_compile_definitions(bluewake_archive
    PUBLIC LIBARCHIVE_STATIC
    PRIVATE PLATFORM_CONFIG_H="${_la_config}")
# Third-party code: no warnings of ours, and none of the host's force-included headers.
target_compile_options(bluewake_archive PRIVATE -w)
if(WIN32)
    target_link_libraries(bluewake_archive PUBLIC ZLIB::ZLIB LibLZMA::LibLZMA PRIVATE bcrypt)
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(bluewake_archive PUBLIC ZLIB::ZLIB LibLZMA::LibLZMA PRIVATE OpenSSL::Crypto)
endif()
