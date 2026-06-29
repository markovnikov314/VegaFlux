set(_candidates
  "${BINARY_DIR}/${CONFIG}/contracts_smoke.exe"
  "${BINARY_DIR}/${CONFIG}/contracts_smoke"
  "${BINARY_DIR}/contracts_smoke.exe"
  "${BINARY_DIR}/contracts_smoke"
  "${BINARY_DIR}/tests/unit/${CONFIG}/contracts_smoke.exe"
  "${BINARY_DIR}/tests/unit/${CONFIG}/contracts_smoke"
)

foreach(_candidate IN LISTS _candidates)
  if(EXISTS "${_candidate}")
    execute_process(COMMAND "${_candidate}" RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
      message(FATAL_ERROR "contracts_smoke failed with ${_result}")
    endif()
    return()
  endif()
endforeach()

message(FATAL_ERROR "contracts_smoke executable not found under ${BINARY_DIR}")
