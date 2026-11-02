# Runs PROGRAM twice, the second time with ENV_VAR set, and fails unless both
# succeed with the same standard output.
#   cmake -DPROGRAM=<exe> -DENV_VAR=<name> -P SameOutput.cmake
execute_process(COMMAND "${PROGRAM}" OUTPUT_VARIABLE first RESULT_VARIABLE rc1)
execute_process(COMMAND ${CMAKE_COMMAND} -E env "${ENV_VAR}=1" "${PROGRAM}" OUTPUT_VARIABLE second RESULT_VARIABLE rc2)
if(NOT rc1 EQUAL 0 OR NOT rc2 EQUAL 0)
    message(FATAL_ERROR "a run failed (${rc1}, ${rc2})")
endif()
if(NOT first STREQUAL second)
    message(FATAL_ERROR "outputs differ:\n${first}---\n${second}")
endif()
