# Object-tree hygiene: every exported .o must map to a live source file.
#
# 2026-10-22 production SEGV regression guard. CMake incremental builds never
# delete .o files whose sources were removed or moved by a refactor; those
# orphan objects used to ride along in the exported object tree, link.sh
# collected them, and their frozen strong symbols (old layouts, same mangled
# names) silently overrode the current header-inline weak symbols. The old
# LocalFileStorageProvider constructor thus initialized a new-layout object and
# both production servers segfaulted during bootstrap. build.sh now prunes
# orphans at export time; this test fails if any orphan ever reaches an
# exported object tree again.
if(NOT DEFINED PR_SOURCE_ROOT)
    get_filename_component(PR_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()

set(_scanned 0)
file(GLOB artifact_roots
    "${PR_SOURCE_ROOT}/artifacts/*/Debug/obj/CMakeFiles"
    "${PR_SOURCE_ROOT}/artifacts/*/Release/obj/CMakeFiles"
    "${PR_SOURCE_ROOT}/artifacts/*/SafeRelease/obj/CMakeFiles"
    "${PR_SOURCE_ROOT}/artifacts/*/Asan/obj/CMakeFiles")
foreach(root IN LISTS artifact_roots)
    file(GLOB_RECURSE objects RELATIVE "${root}" "${root}/*.o")
    foreach(obj IN LISTS objects)
        # Path layout: <target>.dir/<source-path>.o
        string(FIND "${obj}" ".dir/" _dir_sep)
        if(_dir_sep EQUAL -1)
            continue()
        endif()
        math(EXPR _dir_sep "${_dir_sep} + 5")
        string(SUBSTRING "${obj}" ${_dir_sep} -1 source)
        string(REGEX REPLACE "\\.o$" "" source "${source}")
        set(_scanned 1)
        if(NOT EXISTS "${PR_SOURCE_ROOT}/${source}")
            message(FATAL_ERROR
                "Orphan object in exported tree: ${root}/${obj}\n"
                "Source missing from the checkout: ${source}\n"
                "This object would be linked by deploy.sh and its stale strong "
                "symbols can override current inline (weak) symbols (ODR). "
                "Re-run build.sh object-only to prune, or delete the orphan.")
        endif()
    endforeach()
endforeach()
if(_scanned EQUAL 0)
    message(STATUS "No exported object tree present; object-tree hygiene vacuously holds")
else()
    message(STATUS "Exported object trees contain no orphan objects")
endif()
