# cmake -DIN=index.html -DOUT=web_index.cpp -P embed_web.cmake
# Embeds IN as kicadd::mcp::KICADD_WEB_INDEX_HTML (NUL-terminated char array).
file( READ "${IN}" _hex HEX )
string( REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}" )
file( WRITE "${OUT}"
    "// Generated from ${IN}; do not edit.\n"
    "namespace kicadd::mcp {\n"
    "static const unsigned char kIndex[] = { ${_bytes} 0x00 };\n"
    "extern const char* const KICADD_WEB_INDEX_HTML;\n"
    "const char* const KICADD_WEB_INDEX_HTML = reinterpret_cast<const char*>( kIndex );\n"
    "}\n" )
