# Writes build_id.h from git, and only when the id changed.
# radio_devices_docs/open_hub/arch/build-and-generation.md
execute_process(COMMAND git -C "${SRC}" describe --tags --always --dirty
                OUTPUT_VARIABLE ID OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE RC)
if(NOT RC EQUAL 0 OR ID STREQUAL "")
    # No git, no tree, no answer: say so rather than emit a plausible id.
    set(ID "unknown")
endif()
set(NEW "#pragma once\n#define BUILD_ID \"${ID}\"\n")
set(OLD "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" OLD)
endif()
if(NOT OLD STREQUAL NEW)
    file(WRITE "${OUT}" "${NEW}")
endif()
