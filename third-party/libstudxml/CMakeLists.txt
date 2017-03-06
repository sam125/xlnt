cmake_minimum_required(VERSION 3.2)
project(libstudxml)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIBSTUDXML_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LIBSTUDXML_INCLUDE_DIR ${LIBSTUDXML_ROOT_DIR})

set(LIBSTUDXML
    ${LIBSTUDXML_ROOT_DIR}/xml/parser.cxx
    ${LIBSTUDXML_ROOT_DIR}/xml/qname.cxx 
    ${LIBSTUDXML_ROOT_DIR}/xml/serializer.cxx
    ${LIBSTUDXML_ROOT_DIR}/xml/value-traits.cxx
    ${LIBSTUDXML_ROOT_DIR}/xml/content
    ${LIBSTUDXML_ROOT_DIR}/xml/exception
    ${LIBSTUDXML_ROOT_DIR}/xml/forward
    ${LIBSTUDXML_ROOT_DIR}/xml/parser
    ${LIBSTUDXML_ROOT_DIR}/xml/qname
    ${LIBSTUDXML_ROOT_DIR}/xml/serializer
    ${LIBSTUDXML_ROOT_DIR}/xml/value-traits)

set(GENX
    ${LIBSTUDXML_ROOT_DIR}/xml/details/genx/char-props.c
    ${LIBSTUDXML_ROOT_DIR}/xml/details/genx/genx.c
    ${LIBSTUDXML_ROOT_DIR}/xml/details/genx/genx.h)

if(BUILD_DEPS)
    set(EXPAT
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/xmlparse.c
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/xmlrole.c
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/xmltok.c
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/ascii.h
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/asciitab.h
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/config.h
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/expat_external.h
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/expat.h
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/iasciitab.h
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/internal.h
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/latin1tab.h
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/nametab.h
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/utf8tab.h
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/xmlrole.h
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/xmltok_impl.h
        ${LIBSTUDXML_ROOT_DIR}/xml/details/expat/xmltok.h)
else()
    set(EXPAT "")    
endif()

add_library(libstudxml STATIC ${LIBSTUDXML} ${GENX} ${EXPAT})
target_compile_definitions(libstudxml PUBLIC LIBSTUDXML_STATIC_LIB=1)
target_include_directories(libstudxml PUBLIC ${LIBSTUDXML_ROOT_DIR})

if(NOT BUILD_DEPS)
    find_package(EXPAT REQUIRED)
    target_compile_definitions(libstudxml PRIVATE LIBSTUDXML_EXTERNAL_EXPAT=1)
    target_link_libraries(libstudxml PRIVATE expat)
endif()

if(NOT STATIC)
    set_target_properties(libstudxml PROPERTIES POSITION_INDEPENDENT_CODE 1)
endif()