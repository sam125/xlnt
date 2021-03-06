cmake_minimum_required(VERSION 3.2)
project(xlnt.test)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT COMBINED_PROJECT)
    add_subdirectory(${LIBRARY_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/source)
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/../third-party")

# Tests uses the private interface of xlnt (source/detail) so it needs to explicitly link zlib and cryptopp
# as they are not part of xlnt's default link interface
find_package(ZLIB REQUIRED)
find_package(CryptoPP REQUIRED)

file(GLOB CELL_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../source/cell/tests/test_*.hpp)
file(GLOB CHARTS_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../source/charts/tests/test_*.hpp)
file(GLOB CHARTSHEET_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../source/chartsheet/tests/test_*.hpp)
file(GLOB DRAWING_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../source/drawing/tests/test_*.hpp)
file(GLOB FORMULA_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../source/formula/tests/test_*.hpp)
file(GLOB PACKAGING_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../source/packaging/tests/test_*.hpp)
file(GLOB STYLES_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../source/styles/tests/test_*.hpp)
file(GLOB UTILS_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../source/utils/tests/test_*.hpp)
file(GLOB WORKBOOK_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../source/workbook/tests/test_*.hpp)
file(GLOB WORKSHEET_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/../source/worksheet/tests/test_*.hpp)

set(TESTS ${CELL_TESTS} ${CHARTS_TESTS} ${CHARTSHEET_TESTS} ${DRAWING_TESTS} 
    ${FORMULA_TESTS} ${PACKAGING_TESTS} ${STYLES_TESTS} ${UTILS_TESTS} 
	${WORKBOOK_TESTS} ${WORKSHEET_TESTS})

file(GLOB TEST_HELPERS_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/helpers/*.hpp)
file(GLOB TEST_HELPERS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/helpers/*.cpp)

set(TEST_HELPERS ${TEST_HELPERS_HEADERS} ${TEST_HELPERS_SOURCES})

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
set(RUNNER "${CMAKE_CURRENT_BINARY_DIR}/runner-autogen.cpp")
set_source_files_properties(${RUNNER} PROPERTIES GENERATED TRUE)

if(COVERAGE)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-arcs -ftest-coverage")
endif()

set(CXXTEST_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third-party/cxxtest)
set(CXXTEST_INCLUDE_DIR ${CXXTEST_ROOT_DIR})
set(CXXTEST_PYTHON_TESTGEN_EXECUTABLE ${CXXTEST_ROOT_DIR}/bin/cxxtestgen)

add_executable(xlnt.test ${TEST_HELPERS} ${TESTS} ${RUNNER})
target_link_libraries(xlnt.test
    PRIVATE xlnt
    PRIVATE libstudxml
    PRIVATE ${ZLIB_LIBRARIES}
    PRIVATE ${CRYPTOPP_LIBRARIES})
target_include_directories(xlnt.test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../source
    PRIVATE ${ZLIB_INCLUDE_DIR}
    PRIVATE ${CRYPTOPP_INCLUDE_DIR}
    PRIVATE ${CXXTEST_INCLUDE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third-party/libstudxml)
set(XLNT_TEST_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
target_compile_definitions(xlnt.test PRIVATE XLNT_TEST_DATA_DIR=${XLNT_TEST_DATA_DIR})

source_group(helpers FILES ${TEST_HELPERS})
source_group(xlnt\\detail FILES ${XLNT_ZIP})
source_group(tests\\cell FILES ${CELL_TESTS})
source_group(tests\\charts FILES ${CHARTS_TESTS})
source_group(tests\\chartsheet FILES ${CHARTSHEET_TESTS})
source_group(tests\\drawing FILES ${DRAWING_TESTS})
source_group(tests\\formula FILES ${FORMULA_TESTS})
source_group(tests\\packaging FILES ${PACKAGING_TESTS})
source_group(tests\\serialization FILES ${SERIALIZATION_TESTS})
source_group(tests\\styles FILES ${STYLES_TESTS})
source_group(tests\\utils FILES ${UTILS_TESTS})
source_group(tests\\workbook FILES ${WORKBOOK_TESTS})
source_group(tests\\worksheet FILES ${WORKSHEET_TESTS})
source_group(runner FILES ${RUNNER})

if(MSVC)
    set_target_properties(xlnt.test PROPERTIES COMPILE_FLAGS "/wd\"4251\" /wd\"4275\" /wd\"4068\"")
endif()

find_package(PythonInterp REQUIRED)

add_custom_command(OUTPUT ${RUNNER}
    COMMAND ${PYTHON_EXECUTABLE} ${CXXTEST_PYTHON_TESTGEN_EXECUTABLE} --runner=ErrorPrinter -o ${RUNNER} ${TESTS}
    DEPENDS ${TESTS}
    COMMENT "Generating test runner ${RUNNER}")

if(NOT STATIC)
    add_custom_command(TARGET xlnt.test POST_BUILD 
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:xlnt>
        $<TARGET_FILE_DIR:xlnt.test>)
endif()

add_custom_target(generate-test-runner DEPENDS ${RUNNER})
add_dependencies(xlnt.test generate-test-runner)
