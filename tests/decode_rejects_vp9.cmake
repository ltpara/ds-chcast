# VP9 has no Annex B framing. chast-decode must refuse a raw VP9 file instead
# of feeding it to the H.264/HEVC splitter, and it must say why. This guards
# the tool's only raw-stream entry point; the wire path carries access units.

execute_process(
    COMMAND "${DECODE}" --in "${INPUT}" --codec vp9
    RESULT_VARIABLE result
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)

if(result EQUAL 0)
    message(FATAL_ERROR "chast-decode accepted a raw VP9 stream")
endif()
if(NOT "${out}${err}" MATCHES "has no Annex B framing")
    message(FATAL_ERROR "chast-decode did not explain the VP9 rejection: ${out}${err}")
endif()
message(STATUS "chast-decode rejected raw VP9: ${err}")
