# cmake -DIN=index.html -DOUT=web_index.cpp [-DVAR=KICADD_WEB_INDEX_HTML] -P embed_web.cmake
# Embeds IN as kicadd::mcp::<VAR> (NUL-terminated char array).
if( NOT VAR )
    set( VAR KICADD_WEB_INDEX_HTML )
endif()
file( READ "${IN}" _hex HEX )
string( REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}" )
file( WRITE "${OUT}"
    "// Generated from ${IN}; do not edit.\n"
    "namespace kicadd::mcp {\n"
    "static const unsigned char kData[] = { ${_bytes} 0x00 };\n"
    "extern const char* const ${VAR};\n"
    "const char* const ${VAR} = reinterpret_cast<const char*>( kData );\n"
    "}\n" )
