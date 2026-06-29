cmake_minimum_required(VERSION 3.22)

project(VegaFlux VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(VEGAFLUX_BUILD_CONFIG "Debug" CACHE STRING "Build configuration used by CTest build smoke")
if(DEFINED CMAKE_BUILD_TYPE)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" VEGAFLUX_CONFIGURED_BUILD_TYPE)
endif()

include(CTest)
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)

add_library(vegaflux_contracts INTERFACE)
target_include_directories(vegaflux_contracts INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/libs/aegis_events/include
)

add_library(aegis_transport
  libs/aegis_transport/src/synthetic.cpp
)
target_include_directories(aegis_transport PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/libs/aegis_transport/include
)

add_library(aegis_events
  libs/aegis_events/src/canonical_jsonl.cpp
)
target_include_directories(aegis_events PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/libs/aegis_events/include
)
target_link_libraries(aegis_events PUBLIC aegis_transport)

add_library(aegis_decode
  libs/aegis_decode/src/synthetic_itch.cpp
)
target_include_directories(aegis_decode PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/libs/aegis_decode/include
)
target_link_libraries(aegis_decode PUBLIC aegis_events aegis_transport)

add_library(aegis_lob
  libs/aegis_lob/src/book.cpp
)
target_include_directories(aegis_lob PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/libs/aegis_lob/include
)

add_library(aegis_replay
  libs/aegis_replay/src/replay.cpp
)
target_include_directories(aegis_replay PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/libs/aegis_replay/include
)
target_link_libraries(aegis_replay PUBLIC aegis_lob)

add_library(vf_features
  libs/vf_features/src/features.cpp
)
target_include_directories(vf_features PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/libs/vf_features/include
)
target_link_libraries(vf_features PUBLIC aegis_replay)

add_library(vf_execution
  libs/vf_execution/src/execution.cpp
)
target_include_directories(vf_execution PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/libs/vf_execution/include
)
target_link_libraries(vf_execution PUBLIC aegis_replay)

add_library(vf_options
  libs/vf_options/src/options.cpp
)
target_include_directories(vf_options PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/libs/vf_options/include
)

add_library(vf_policy
  libs/vf_policy/src/policy.cpp
)
target_include_directories(vf_policy PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/libs/vf_policy/include
)

add_executable(vegaflux_run apps/vegaflux_run/main.cpp)
target_link_libraries(vegaflux_run PRIVATE vegaflux_contracts)

add_executable(aegis_decode_cli apps/aegis_decode_cli/main.cpp)
target_link_libraries(aegis_decode_cli PRIVATE aegis_decode)

add_executable(aegis_replay_cli apps/aegis_replay_cli/main.cpp)
target_link_libraries(aegis_replay_cli PRIVATE aegis_replay)

add_executable(vf_build_dataset_cli apps/vf_build_dataset_cli/main.cpp)
target_link_libraries(vf_build_dataset_cli PRIVATE vf_features)

add_executable(vf_run_execution_cli apps/vf_run_execution_cli/main.cpp)
target_link_libraries(vf_run_execution_cli PRIVATE vf_execution)

add_executable(vf_run_options_cli apps/vf_run_options_cli/main.cpp)
target_link_libraries(vf_run_options_cli PRIVATE vf_options)

add_executable(vf_run_policy_cli apps/vf_run_policy_cli/main.cpp)
target_link_libraries(vf_run_policy_cli PRIVATE vf_policy)

add_executable(contracts_smoke tests/unit/contracts_smoke.cpp)
target_link_libraries(contracts_smoke PRIVATE vegaflux_contracts)

add_executable(decoder_tests tests/unit/decoder_tests.cpp)
target_link_libraries(decoder_tests PRIVATE aegis_decode)

add_executable(replay_tests tests/unit/replay_tests.cpp)
target_link_libraries(replay_tests PRIVATE aegis_replay)

add_executable(feature_tests tests/unit/feature_tests.cpp)
target_link_libraries(feature_tests PRIVATE vf_features)

add_executable(execution_tests tests/unit/execution_tests.cpp)
target_link_libraries(execution_tests PRIVATE vf_execution)

add_executable(options_tests tests/unit/options_tests.cpp)
target_link_libraries(options_tests PRIVATE vf_options)

add_executable(policy_tests tests/unit/policy_tests.cpp)
target_link_libraries(policy_tests PRIVATE vf_policy)

add_executable(decoder_benchmark benchmarks/decoder_benchmark.cpp)
target_link_libraries(decoder_benchmark PRIVATE aegis_decode)

add_executable(replay_benchmark benchmarks/replay_benchmark.cpp)
target_link_libraries(replay_benchmark PRIVATE aegis_replay)

add_executable(feature_benchmark benchmarks/feature_benchmark.cpp)
target_link_libraries(feature_benchmark PRIVATE vf_features)

add_executable(execution_benchmark benchmarks/execution_benchmark.cpp)
target_link_libraries(execution_benchmark PRIVATE vf_execution)

add_executable(options_benchmark benchmarks/options_benchmark.cpp)
target_link_libraries(options_benchmark PRIVATE vf_options)

add_executable(policy_benchmark benchmarks/policy_benchmark.cpp)
target_link_libraries(policy_benchmark PRIVATE vf_policy)

if(BUILD_TESTING)
  set(_py_env "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python")

  add_test(
    NAME build_project
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config ${VEGAFLUX_BUILD_CONFIG}
  )
  set_tests_properties(build_project PROPERTIES FIXTURES_SETUP project_built)

  add_test(
    NAME contracts_smoke
    COMMAND ${CMAKE_COMMAND}
      -DBINARY_DIR=${CMAKE_BINARY_DIR}
      -DCONFIG=${VEGAFLUX_BUILD_CONFIG}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_cpp_smoke.cmake
  )
  set_tests_properties(contracts_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME fixture_check
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m vegaflux_py.fixtures --check --root ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_test(
    NAME python_smoke
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m vegaflux_py.smoke --root ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_test(
    NAME schema_compatibility
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m vegaflux_py.schema_compat
      --baseline ${CMAKE_CURRENT_SOURCE_DIR}/data_contracts/schemas/canonical_market.v0.1.proto
      --current ${CMAKE_CURRENT_SOURCE_DIR}/proto/canonical_market.proto
  )

  add_test(
    NAME schema_negative_reused_field
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m vegaflux_py.schema_compat
      --baseline ${CMAKE_CURRENT_SOURCE_DIR}/data_contracts/schemas/canonical_market.v0.1.proto
      --current ${CMAKE_CURRENT_SOURCE_DIR}/data_contracts/schemas/canonical_market.reused_field_negative.proto
  )
  set_tests_properties(schema_negative_reused_field PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME schema_policy_negative
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m vegaflux_py.schema_compat
      --baseline ${CMAKE_CURRENT_SOURCE_DIR}/data_contracts/schemas/canonical_market.v0.1.proto
      --current ${CMAKE_CURRENT_SOURCE_DIR}/data_contracts/schemas/canonical_market.policy_negative.proto
  )
  set_tests_properties(schema_policy_negative PROPERTIES WILL_FAIL TRUE)

  add_test(
    NAME manifest_component_schema
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m vegaflux_py.schema_contracts
      ${CMAKE_CURRENT_SOURCE_DIR}/data_contracts/schemas/canonical_market.v0.1.proto
      ${CMAKE_CURRENT_SOURCE_DIR}/proto/canonical_market.proto
  )

  add_test(
    NAME manifest_smoke
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m vegaflux_py.manifest
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --component foundation
      --build-dir ${CMAKE_BINARY_DIR}
      --dataset ${CMAKE_CURRENT_SOURCE_DIR}/data_contracts/fixtures/normalized_events.golden.jsonl
      --config ${CMAKE_CURRENT_SOURCE_DIR}/configs/experiments/manifest_smoke.json
      --output ${CMAKE_BINARY_DIR}/run_manifest.json
      --seed 424242
  )

  add_test(
    NAME format_check
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m vegaflux_py.repo_checks
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --mode format
  )

  add_test(
    NAME lint_check
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m vegaflux_py.repo_checks
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --mode lint
  )

  add_test(
    NAME static_check
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m vegaflux_py.repo_checks
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --mode static
  )

  add_test(
    NAME foundation_benchmark_smoke
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m vegaflux_py.benchmark_smoke
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --dataset ${CMAKE_CURRENT_SOURCE_DIR}/data_contracts/fixtures/normalized_events.golden.jsonl
      --output ${CMAKE_BINARY_DIR}/foundation_benchmark_summary.json
      --seed 424242
  )

  add_test(
    NAME decoder_fixture_check
    COMMAND $<TARGET_FILE:aegis_decode_cli>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --check-fixtures
  )
  set_tests_properties(decoder_fixture_check PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME decoder_cli_smoke
    COMMAND $<TARGET_FILE:aegis_decode_cli>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --input data_contracts/fixtures/synthetic_itch.bin
      --output ${CMAKE_BINARY_DIR}/decoder_decoded.normalized.jsonl
      --quarantine ${CMAKE_BINARY_DIR}/decoder_quarantine.jsonl
  )
  set_tests_properties(decoder_cli_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME decoder_tests
    COMMAND $<TARGET_FILE:decoder_tests> ${CMAKE_CURRENT_SOURCE_DIR}
  )
  set_tests_properties(decoder_tests PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME decoder_benchmark_smoke
    COMMAND $<TARGET_FILE:decoder_benchmark>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --output ${CMAKE_BINARY_DIR}/decoder_benchmark_summary.json
  )
  set_tests_properties(decoder_benchmark_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME replay_tests
    COMMAND $<TARGET_FILE:replay_tests> ${CMAKE_CURRENT_SOURCE_DIR}
  )
  set_tests_properties(replay_tests PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME replay_cli_snapshot_smoke
    COMMAND $<TARGET_FILE:aegis_replay_cli>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --input artifacts/decoder/decoded.normalized.jsonl
      --depth 2
      --snapshot-output ${CMAKE_BINARY_DIR}/replay_demo_snapshot.json
      --checksum-output ${CMAKE_BINARY_DIR}/replay_checksum.json
  )
  set_tests_properties(replay_cli_snapshot_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME replay_benchmark_smoke
    COMMAND $<TARGET_FILE:replay_benchmark>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --output ${CMAKE_BINARY_DIR}/replay_benchmark_summary.json
  )
  set_tests_properties(replay_benchmark_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME feature_tests
    COMMAND $<TARGET_FILE:feature_tests> ${CMAKE_CURRENT_SOURCE_DIR}
  )
  set_tests_properties(feature_tests PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME feature_dataset_cli_smoke
    COMMAND $<TARGET_FILE:vf_build_dataset_cli>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --manifest configs/experiments/features_smoke.json
      --features-output ${CMAKE_BINARY_DIR}/features.jsonl
      --metadata-output ${CMAKE_BINARY_DIR}/dataset_metadata.json
  )
  set_tests_properties(feature_dataset_cli_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME feature_benchmark_smoke
    COMMAND $<TARGET_FILE:feature_benchmark>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --output ${CMAKE_BINARY_DIR}/feature_benchmark_summary.json
  )
  set_tests_properties(feature_benchmark_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME execution_tests
    COMMAND $<TARGET_FILE:execution_tests>
  )
  set_tests_properties(execution_tests PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME execution_cli_smoke
    COMMAND $<TARGET_FILE:vf_run_execution_cli>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --manifest configs/experiments/execution_smoke.json
      --fills-output ${CMAKE_BINARY_DIR}/fills.jsonl
      --metadata-output ${CMAKE_BINARY_DIR}/simulation_metadata.json
  )
  set_tests_properties(execution_cli_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME execution_benchmark_smoke
    COMMAND $<TARGET_FILE:execution_benchmark>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --output ${CMAKE_BINARY_DIR}/execution_benchmark_summary.json
  )
  set_tests_properties(execution_benchmark_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME options_tests
    COMMAND $<TARGET_FILE:options_tests> ${CMAKE_CURRENT_SOURCE_DIR}
  )
  set_tests_properties(options_tests PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME options_cli_smoke
    COMMAND $<TARGET_FILE:vf_run_options_cli>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --manifest configs/experiments/options_smoke.json
      --filtered-quotes-output ${CMAKE_BINARY_DIR}/filtered_quotes.jsonl
      --diagnostics-output ${CMAKE_BINARY_DIR}/surface_diagnostics.json
  )
  set_tests_properties(options_cli_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME options_benchmark_smoke
    COMMAND $<TARGET_FILE:options_benchmark>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --output ${CMAKE_BINARY_DIR}/options_benchmark_summary.json
  )
  set_tests_properties(options_benchmark_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME policy_tests
    COMMAND $<TARGET_FILE:policy_tests> ${CMAKE_CURRENT_SOURCE_DIR}
  )
  set_tests_properties(policy_tests PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME policy_cli_smoke
    COMMAND $<TARGET_FILE:vf_run_policy_cli>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --manifest configs/experiments/policy_smoke.json
      --quote-decisions-output ${CMAKE_BINARY_DIR}/quote_decisions.jsonl
      --pnl-attribution-output ${CMAKE_BINARY_DIR}/pnl_attribution.json
  )
  set_tests_properties(policy_cli_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME policy_benchmark_smoke
    COMMAND $<TARGET_FILE:policy_benchmark>
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --output ${CMAKE_BINARY_DIR}/policy_benchmark_summary.json
  )
  set_tests_properties(policy_benchmark_smoke PROPERTIES FIXTURES_REQUIRED project_built)

  add_test(
    NAME report_regeneration
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/scripts/generate_report.py
  )

  add_test(
    NAME report_stats_json_valid
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m json.tool
      ${CMAKE_CURRENT_SOURCE_DIR}/artifacts/report/stats_summary.json
  )

  add_test(
    NAME report_manifest_json_valid
    COMMAND ${CMAKE_COMMAND} -E env ${_py_env}
      ${Python3_EXECUTABLE} -m json.tool
      ${CMAKE_CURRENT_SOURCE_DIR}/artifacts/report/free_data_manifest.json
  )
endif()
