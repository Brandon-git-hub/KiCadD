# KiCadD build integration.
#
# Included from KiCad's top-level CMakeLists.txt (after add_subdirectory( api )) when
# KICADD_DIR is set. Defines:
#   kicadd_proto        - static PIC library with the generated KiCadD protobuf code
#   KICADD_PCB_SRCS     - handler sources to compile into pcbnew_kiface_objects
#   KICADD_INCLUDE_DIRS - include dirs for the above

set( KICADD_PROTO_DIR ${KICADD_DIR}/proto )
set( KICADD_PROTO_SRCS kicadd/board.proto kicadd/render.proto )
set( KICADD_CPP_OUT ${CMAKE_BINARY_DIR}/kicadd/cpp )

set( _kicadd_cpp )
set( _kicadd_hdr )
set( _kicadd_proto_full )

foreach( _p ${KICADD_PROTO_SRCS} )
    string( REGEX REPLACE "\\.proto$" ".pb.cc" _cc ${_p} )
    string( REGEX REPLACE "\\.proto$" ".pb.h"  _h  ${_p} )
    list( APPEND _kicadd_cpp ${KICADD_CPP_OUT}/${_cc} )
    list( APPEND _kicadd_hdr ${KICADD_CPP_OUT}/${_h} )
    list( APPEND _kicadd_proto_full ${KICADD_PROTO_DIR}/${_p} )
endforeach()

add_custom_command(
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KICADD_CPP_OUT}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        --cpp_out=${KICADD_CPP_OUT}
        --proto_path=${KICADD_PROTO_DIR}
        ${KICADD_PROTO_SRCS}
    WORKING_DIRECTORY ${KICADD_PROTO_DIR}
    DEPENDS ${_kicadd_proto_full}
    OUTPUT ${_kicadd_cpp} ${_kicadd_hdr}
    COMMENT "Generating KiCadD protobuf sources"
    )

add_library( kicadd_proto STATIC ${_kicadd_cpp} ${_kicadd_hdr} )
set_target_properties( kicadd_proto PROPERTIES POSITION_INDEPENDENT_CODE ON )
target_include_directories( kicadd_proto PUBLIC ${KICADD_CPP_OUT} ${Protobuf_INCLUDE_DIRS} )
target_link_libraries( kicadd_proto PUBLIC ${Protobuf_LIBRARY} )
# Generated code is not ours to lint.
target_compile_options( kicadd_proto PRIVATE -w )

set( KICADD_PCB_SRCS
    ${KICADD_DIR}/src/pcb/kicadd_handler_pcb.cpp
    ${KICADD_DIR}/src/pcb/kicadd_local_drc.cpp
    ${KICADD_DIR}/src/pcb/kicadd_render.cpp
    ${KICADD_DIR}/src/lcsc/kicadd_lcsc.cpp
    )
set( KICADD_INCLUDE_DIRS ${KICADD_DIR}/src ${KICADD_CPP_OUT} )
# eeschema kiface: LCSC symbols (M4b), reached from pcbnew through KIFACE_KICADD_LCSC_SYMBOL
set( KICADD_SCH_SRCS
    ${KICADD_DIR}/src/sch/kicadd_lcsc_sym.cpp
    )

# Python client stubs (for tests / plugins; no SWIG anywhere).
set( KICADD_PY_OUT ${CMAKE_BINARY_DIR}/kicadd/python )
add_custom_target( kicadd_python
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KICADD_PY_OUT}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
        --python_out=${KICADD_PY_OUT}
        --proto_path=${KICADD_PROTO_DIR}
        ${KICADD_PROTO_SRCS}
    WORKING_DIRECTORY ${KICADD_PROTO_DIR}
    DEPENDS ${_kicadd_proto_full}
    COMMENT "Generating KiCadD Python protobuf stubs"
    )

# MCP server (Phase 2): a standalone IPC client of the daemon. C++ only; HTTP via Boost.Beast
# (Boost is already a KiCad dependency), JSON via KiCad's bundled nlohmann_json.
find_package( Threads REQUIRED )

set( KICADD_WEB_INDEX_CPP ${CMAKE_BINARY_DIR}/kicadd/web_index.cpp )
add_custom_command(
    OUTPUT ${KICADD_WEB_INDEX_CPP}
    COMMAND ${CMAKE_COMMAND} -DIN=${KICADD_DIR}/web/index.html -DOUT=${KICADD_WEB_INDEX_CPP}
            -P ${KICADD_DIR}/cmake/embed_web.cmake
    DEPENDS ${KICADD_DIR}/web/index.html ${KICADD_DIR}/cmake/embed_web.cmake
    COMMENT "Embedding KiCadD web view"
    )

add_executable( kicadd-mcp
    ${KICADD_WEB_INDEX_CPP}
    ${KICADD_DIR}/mcp/web.cpp
    ${KICADD_DIR}/mcp/main.cpp
    ${KICADD_DIR}/mcp/ipc_client.cpp
    ${KICADD_DIR}/mcp/mcp_server.cpp
    ${KICADD_DIR}/mcp/tools.cpp
    ${KICADD_DIR}/mcp/drc.cpp
    ${KICADD_DIR}/mcp/transports.cpp
    )
target_include_directories( kicadd-mcp PRIVATE ${KICADD_DIR}/mcp ${NNG_INCLUDE_DIR} )
target_link_libraries( kicadd-mcp PRIVATE
    kicadd_proto
    kiapi
    nlohmann_json
    Boost::boost
    Threads::Threads
    ${NNG_LIBRARY}
    )
target_compile_features( kicadd-mcp PRIVATE cxx_std_20 )
