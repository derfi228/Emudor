# ─── copy_runtime_dlls.cmake ─────────────────────────────────────────────────
# Вызывается как POST_BUILD step из CMakeLists.txt.
# Находит все MinGW-зависимости exe через file(GET_RUNTIME_DEPENDENCIES)
# и копирует их рядом с бинарником.
#
# Переменные, ожидаемые при вызове (-D):
#   EXE_PATH   — полный путь к emudor.exe (уже слинкован)
#   DEST_DIR   — куда копировать DLL (обычно = каталог exe)
#   MINGW_BIN  — путь к MinGW bin (C:/msys64/mingw64/bin)
# ─────────────────────────────────────────────────────────────────────────────

if(NOT EXISTS "${EXE_PATH}")
    message(WARNING "copy_runtime_dlls: exe not found: ${EXE_PATH}")
    return()
endif()

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES          "${EXE_PATH}"
    RESOLVED_DEPENDENCIES_VAR   _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    DIRECTORIES
        "${MINGW_BIN}"
        "${DEST_DIR}"
    # Системные DLL — пропускаем
    PRE_EXCLUDE_REGEXES
        "^api-ms-win"
        "^ext-ms-win"
        "^hvsifiletrust"
        "^pdm\\.dll$"
    POST_EXCLUDE_REGEXES
        "[Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\/\\\\]"
        "[Ss][Yy][Ss][Tt][Ee][Mm]32[\\/\\\\]"
        "[Ss][Yy][Ss][Ww][Oo][Ww]64[\\/\\\\]"
)

if(_unresolved)
    message(STATUS "copy_runtime_dlls: unresolved DLLs (ok if system): ${_unresolved}")
endif()

set(_copied 0)
foreach(_dll ${_resolved})
    get_filename_component(_fname "${_dll}" NAME)
    set(_dst "${DEST_DIR}/${_fname}")
    if(NOT EXISTS "${_dst}")
        file(COPY "${_dll}" DESTINATION "${DEST_DIR}")
        math(EXPR _copied "${_copied} + 1")
    endif()
endforeach()

if(_copied GREATER 0)
    message(STATUS "copy_runtime_dlls: copied ${_copied} DLL(s) to ${DEST_DIR}")
endif()
