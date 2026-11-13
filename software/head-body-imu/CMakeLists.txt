cmake_minimum_required(VERSION 3.16)
project(MicroduckHeadBodyEstimator VERSION 1.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

add_library(microduck_head_body SHARED source/head_body_estimator.cpp source/head_calibration.cpp)
target_compile_features(microduck_head_body PUBLIC cxx_std_17)
set_target_properties(microduck_head_body PROPERTIES
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)
target_include_directories(microduck_head_body PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/source>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/microduck-head-body>
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(microduck_head_body PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(UNIX)
    target_link_libraries(microduck_head_body PRIVATE m)
endif()

if(BUILD_TESTING)
    add_executable(estimator_tests source/tests/estimator_tests.cpp)
    target_link_libraries(estimator_tests PRIVATE microduck_head_body)
    add_test(NAME estimator_tests COMMAND estimator_tests)
    add_executable(calibration_tests source/tests/calibration_tests.cpp)
    target_link_libraries(calibration_tests PRIVATE microduck_head_body)
    add_test(NAME calibration_tests COMMAND calibration_tests)
endif()

install(TARGETS microduck_head_body LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES source/head_body_estimator.h source/head_calibration.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/microduck-head-body)
