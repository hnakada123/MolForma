# SPDX-License-Identifier: GPL-2.0-or-later
# Run after linking an executable of an MSVC build (CMakeLists.txt) so that it starts from the build folder
# without PATH: copies beside it the DLLs that it and Open Babel's format plugins load, the plugins
# themselves (Open Babel looks for them beside its DLL), and writes a qt.conf that sends Qt to its own
# folder for its plugins.
#   cmake -DEXECUTABLE=<exe> -DPLUGINS=<*.obf> -DDIRECTORIES=<folders with DLLs> -DQT_PREFIX=<Qt> -P copy_runtime.cmake
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
get_filename_component(folder "${EXECUTABLE}" DIRECTORY)
file(GET_RUNTIME_DEPENDENCIES EXECUTABLES "${EXECUTABLE}" MODULES ${PLUGINS} DIRECTORIES ${DIRECTORIES}
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-" POST_EXCLUDE_REGEXES "[Ss]ystem32"
    RESOLVED_DEPENDENCIES_VAR dlls CONFLICTING_DEPENDENCIES_PREFIX conflict)
# A DLL copied by an earlier build is found beside the executable too; take the original.
foreach(name IN LISTS conflict_FILENAMES)
    foreach(path IN LISTS conflict_${name})
        get_filename_component(directory "${path}" DIRECTORY)
        if(NOT directory STREQUAL folder)
            list(APPEND dlls "${path}")
            break()
        endif()
    endforeach()
endforeach()
set(copies)
foreach(path IN LISTS dlls PLUGINS)
    get_filename_component(directory "${path}" DIRECTORY)
    if(NOT directory STREQUAL folder)
        list(APPEND copies "${path}")
    endif()
endforeach()
file(COPY ${copies} DESTINATION "${folder}")
set(conf "[Paths]\nPrefix = ${QT_PREFIX}\n")
if(EXISTS "${folder}/qt.conf")
    file(READ "${folder}/qt.conf" old)
endif()
if(NOT old STREQUAL conf)
    file(WRITE "${folder}/qt.conf" "${conf}")
endif()
