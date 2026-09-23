# Run the node-based web UI tests, or say plainly that they were skipped.
if(NODE STREQUAL "NODE_EXECUTABLE-NOTFOUND" OR NODE STREQUAL "")
    message(STATUS "web_tests SKIPPED: node not found on PATH")
    return()
endif()
execute_process(COMMAND ${NODE} ${TEST} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "web_tests failed")
endif()
