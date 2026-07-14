list(APPEND CMAKE_MODULE_PATH
    ${CMAKE_SOURCE_DIR}/CMakeModules)
include(GetGitRevisionDescription)

git_describe(GIT_VERSION --tags --always --match "cdda-*")

if(GIT_BINARY)
    set(_git_executable "${GIT_BINARY}")
else()
    find_program(_git_executable NAMES git)
endif()

if(_git_executable)
    execute_process(COMMAND "${_git_executable}" -c core.safecrlf=false diff --quiet
        RESULT_VARIABLE _version_dirty_result
        ERROR_QUIET
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    if("${_version_dirty_result}" STREQUAL "1")
        string(APPEND GIT_VERSION "-dirty")
    endif()
endif()

function(_is_valid_multiplayer_build_id _value _result)
    set(_sha "${_value}")
    if("${_sha}" MATCHES "-dirty$")
        string(REGEX REPLACE "-dirty$" "" _sha "${_sha}")
    endif()

    string(LENGTH "${_sha}" _sha_length)
    set(_valid FALSE)
    if(_sha_length EQUAL 40 AND "${_sha}" MATCHES "^[0-9a-f]+$")
        if("${_value}" STREQUAL "${_sha}" OR
           "${_value}" STREQUAL "${_sha}-dirty")
            set(_valid TRUE)
        endif()
    endif()

    set(${_result} ${_valid} PARENT_SCOPE)
endfunction()

set(_multiplayer_build_id "")
if(NOT "${MULTIPLAYER_BUILD_ID}" STREQUAL "")
    _is_valid_multiplayer_build_id("${MULTIPLAYER_BUILD_ID}" _override_is_valid)
    if(NOT _override_is_valid)
        message(FATAL_ERROR
                "MULTIPLAYER_BUILD_ID must be 40 lowercase hexadecimal characters, optionally followed by -dirty")
    endif()
    set(_multiplayer_build_id "${MULTIPLAYER_BUILD_ID}")
elseif(_git_executable)
    execute_process(COMMAND "${_git_executable}" rev-parse HEAD
        RESULT_VARIABLE _build_id_result
        OUTPUT_VARIABLE _build_id_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    if("${_build_id_result}" STREQUAL "0")
        _is_valid_multiplayer_build_id("${_build_id_sha}" _build_id_sha_is_valid)
        if(NOT _build_id_sha_is_valid)
            message(FATAL_ERROR
                    "git rev-parse HEAD returned an invalid multiplayer build ID: ${_build_id_sha}")
        endif()

        execute_process(COMMAND "${_git_executable}" diff --quiet HEAD
            RESULT_VARIABLE _build_id_dirty_result
            ERROR_QUIET
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        if("${_build_id_dirty_result}" STREQUAL "0")
            set(_multiplayer_build_id "${_build_id_sha}")
        elseif("${_build_id_dirty_result}" STREQUAL "1")
            set(_multiplayer_build_id "${_build_id_sha}-dirty")
        else()
            message(FATAL_ERROR
                    "git diff --quiet HEAD failed with exit code ${_build_id_dirty_result}")
        endif()
    endif()
endif()

set(_version_header_path "${CMAKE_SOURCE_DIR}/src/version.h")
set(_old_version_header "")
if(EXISTS "${_version_header_path}")
    file(READ "${_version_header_path}" _old_version_header)
endif()

set(_version_value "")
if(NOT "${GIT_VERSION}" MATCHES "GIT-NOTFOUND" AND GIT_VERSION)
    string(REPLACE "-NOTFOUND" "" _version_value "${GIT_VERSION}")
else()
    string(REGEX MATCH "#define VERSION \"[^\"]*\"" _existing_version_define
           "${_old_version_header}")
    if(_existing_version_define)
        string(REGEX REPLACE "^#define VERSION \"([^\"]*)\"$" "\\1"
               _version_value "${_existing_version_define}")
    endif()
endif()

set(_new_version_header "// NOLINT(cata-header-guard)\n")
if(NOT "${_version_value}" STREQUAL "")
    string(APPEND _new_version_header "#define VERSION \"${_version_value}\"\n")
endif()
string(APPEND _new_version_header
       "#define MULTIPLAYER_BUILD_ID \"${_multiplayer_build_id}\"\n")

if(NOT "${_old_version_header}" STREQUAL "${_new_version_header}")
    file(WRITE "${_version_header_path}" "${_new_version_header}")
endif()

message(NOTICE "${GIT_VERSION}")

if("${GIT_VERSION}" MATCHES "GIT-NOTFOUND")
    return()
endif()

# get_git_head_revision() does not work with worktrees in Windows
execute_process(COMMAND "${_git_executable}" rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _sha1
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(TIMESTAMP _timestamp %Y-%m-%d-%H%M)
file(WRITE ${CMAKE_SOURCE_DIR}/VERSION.txt "\
build type: Release\n\
build number: ${_timestamp}\n\
commit sha: ${_sha1}\n\
commit url: https://github.com/CleverRaven/Cataclysm-DDA/commit/${_sha1}"
)
