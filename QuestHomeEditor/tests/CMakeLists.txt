if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(questhomeeditor_zip_safety_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    enable_testing()
endif()
add_executable(zip_safety_tests zip_safety_tests.cpp)
target_include_directories(zip_safety_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME zip_safety_tests COMMAND zip_safety_tests)

add_executable(cook_preflight_tests cook_preflight_tests.cpp)
target_include_directories(cook_preflight_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME cook_preflight_tests COMMAND cook_preflight_tests)

add_executable(archive_read_limits_tests archive_read_limits_tests.cpp)
target_include_directories(archive_read_limits_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME archive_read_limits_tests COMMAND archive_read_limits_tests)

add_executable(gltf_material_rules_tests gltf_material_rules_tests.cpp)
target_include_directories(gltf_material_rules_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME gltf_material_rules_tests COMMAND gltf_material_rules_tests)

add_executable(cook_quality_tests cook_quality_tests.cpp)
target_include_directories(cook_quality_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME cook_quality_tests COMMAND cook_quality_tests)

add_executable(scenery_alpha_policy_tests scenery_alpha_policy_tests.cpp)
target_include_directories(scenery_alpha_policy_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME scenery_alpha_policy_tests COMMAND scenery_alpha_policy_tests)

add_executable(quest_display_policy_tests quest_display_policy_tests.cpp)
target_include_directories(quest_display_policy_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME quest_display_policy_tests COMMAND quest_display_policy_tests)

add_executable(cook_ui_policy_tests cook_ui_policy_tests.cpp)
target_include_directories(cook_ui_policy_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME cook_ui_policy_tests COMMAND cook_ui_policy_tests)

add_executable(home_library_policy_tests home_library_policy_tests.cpp)
target_include_directories(home_library_policy_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME home_library_policy_tests COMMAND home_library_policy_tests)

add_executable(install_safety_policy_tests install_safety_policy_tests.cpp)
target_include_directories(install_safety_policy_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME install_safety_policy_tests COMMAND install_safety_policy_tests)

add_executable(runtime_health_policy_tests runtime_health_policy_tests.cpp)
target_include_directories(runtime_health_policy_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME runtime_health_policy_tests COMMAND runtime_health_policy_tests)

add_executable(standalone_haven_policy_tests standalone_haven_policy_tests.cpp)
target_include_directories(standalone_haven_policy_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME standalone_haven_policy_tests COMMAND standalone_haven_policy_tests)

add_executable(navigation_floor_policy_tests navigation_floor_policy_tests.cpp)
target_include_directories(navigation_floor_policy_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
add_test(NAME navigation_floor_policy_tests COMMAND navigation_floor_policy_tests)
