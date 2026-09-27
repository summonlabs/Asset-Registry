# Copyright 2026 Summon Software Labs
# SPDX-License-Identifier: Apache-2.0
#
# Drives the inspection CLI and the consumer example across a real store: register
# from a document, export canonically, and then answer questions from the exported
# document in a separate process. This proves the exported document is the
# interchange contract rather than an internal detail.

if(NOT DEFINED CLI OR NOT DEFINED CONSUMER OR NOT DEFINED WORK)
    message(FATAL_ERROR "export_round_trip.cmake requires CLI, CONSUMER, and WORK")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

set(STORE "${WORK}/store")
set(RECORD "${WORK}/record.json")
set(EXPORT "${WORK}/export.json")

file(WRITE "${RECORD}" "{\"asset_id\":\"11111111-1111-4111-8111-111111111111\",\"asset_class\":\"server\",\"serial_identity\":{\"manufacturer\":\"Example Compute\",\"serial\":\"CLI-0001\"},\"metadata\":{\"display_name\":\"cli-example-server\",\"owner\":\"org.example.platform\",\"site\":\"site-alpha.hall-1\",\"notes\":\"registered by the inspection CLI\",\"labels\":[{\"key\":\"tier\",\"value\":\"gold\"}]},\"state\":{\"lifecycle\":\"planned\",\"installation\":\"unknown\"},\"references\":[{\"kind\":\"capability\",\"evidence\":\"verified\",\"target\":\"asi:accelerator.scheduling\"}]}\n")

execute_process(COMMAND "${CLI}" register "${STORE}" "${RECORD}" --writer cli-example
    RESULT_VARIABLE register_status OUTPUT_VARIABLE register_output ERROR_VARIABLE register_error)
if(NOT register_status EQUAL 0)
    message(FATAL_ERROR "register failed (${register_status}): ${register_output} ${register_error}")
endif()

execute_process(COMMAND "${CLI}" verify "${STORE}"
    RESULT_VARIABLE verify_status OUTPUT_VARIABLE verify_output ERROR_VARIABLE verify_error)
if(NOT verify_status EQUAL 0)
    message(FATAL_ERROR "verify failed (${verify_status}): ${verify_output} ${verify_error}")
endif()

execute_process(COMMAND "${CLI}" export "${STORE}" --out "${EXPORT}"
    RESULT_VARIABLE export_status OUTPUT_VARIABLE export_output ERROR_VARIABLE export_error)
if(NOT export_status EQUAL 0)
    message(FATAL_ERROR "export failed (${export_status}): ${export_output} ${export_error}")
endif()

if(NOT EXISTS "${EXPORT}")
    message(FATAL_ERROR "export produced no document")
endif()

execute_process(COMMAND "${CONSUMER}" "${EXPORT}"
    RESULT_VARIABLE consumer_status OUTPUT_VARIABLE consumer_output ERROR_VARIABLE consumer_error)
if(NOT consumer_status EQUAL 0)
    message(FATAL_ERROR "consumer failed (${consumer_status}): ${consumer_output} ${consumer_error}")
endif()

string(FIND "${consumer_output}" "assets:              1" assets_position)
if(assets_position EQUAL -1)
    message(FATAL_ERROR "consumer did not report exactly one asset:\n${consumer_output}")
endif()

string(FIND "${consumer_output}" "conflict audit: clean" clean_position)
if(clean_position EQUAL -1)
    message(FATAL_ERROR "consumer reported an unclean audit:\n${consumer_output}")
endif()

# The store directory must be exactly the documented layout, with no residue.
file(GLOB store_entries RELATIVE "${STORE}" "${STORE}/*")
list(SORT store_entries)
set(expected_entries "CURRENT" "generations" "lock" "meta" "tmp")
if(NOT store_entries STREQUAL expected_entries)
    message(FATAL_ERROR "unexpected store layout: ${store_entries}")
endif()

# Exporting twice must produce byte-identical documents.
execute_process(COMMAND "${CLI}" export "${STORE}" --out "${WORK}/export-again.json"
    RESULT_VARIABLE second_status OUTPUT_QUIET ERROR_VARIABLE second_error)
if(NOT second_status EQUAL 0)
    message(FATAL_ERROR "second export failed: ${second_error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${EXPORT}" "${WORK}/export-again.json"
    RESULT_VARIABLE compare_status)
if(NOT compare_status EQUAL 0)
    message(FATAL_ERROR "two exports of one snapshot are not byte-identical")
endif()

message(STATUS "export round trip validated: ${EXPORT}")
