# Generate C header with embedded UI files as byte arrays
# Usage: cmake -P embed_ui.cmake -DUI_DIR=src/ui -DOUT=src/ui/ui_embedded.h
#
# Text assets are gzipped at build time and served with Content-Encoding: gzip
# (HttpServerModule); the browser inflates natively, so the firmware carries the
# UI ~3.5x smaller with no device-side or front-end decompressor. Standard
# embedded-web-UI practice (WLED, ESPHome, ESP-IDF examples all pre-gzip assets).
# Compression runs on the BUILD HOST via Python's gzip module — stdlib, so any
# Python interpreter works (chosen over a `gzip` binary that Windows toolchains
# may lack). The PNG is already compressed, so it stays raw.
#
# Interpreter resolution — callers pick ONE of:
#   PYTHON_CMD       — a CMake list giving the exact argv prefix (ESP32 path
#                      passes `${Python3_EXECUTABLE}`). Wins if both are set.
#   UV_EXECUTABLE    — path to `uv`; built into `<uv> run python` here (desktop
#                      path). The list is built in-script so the desktop
#                      CMakeLists doesn't have to pass semicolons through
#                      MSBuild / make's command-line splitter.
# If neither is set, we fall back to bare `python3` (POSIX-friendly).
if(DEFINED UV_EXECUTABLE AND NOT DEFINED PYTHON_CMD)
    set(PYTHON_CMD ${UV_EXECUTABLE} run python)
elseif(NOT DEFINED PYTHON_CMD)
    set(PYTHON_CMD python3)
endif()

# Gzip ${UI_DIR}/<name> to ${OUT_DIR}/<name>.gz and HEX-read it into OUT_VAR.
function(gzip_file_hex NAME OUT_VAR)
    set(src "${UI_DIR}/${NAME}")
    # Write the temp .gz next to the generated header (a writable build dir).
    get_filename_component(out_dir "${OUT}" DIRECTORY)
    set(gz "${out_dir}/${NAME}.gz")
    execute_process(
        COMMAND ${PYTHON_CMD} -c "import sys,gzip; open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),9))" "${src}" "${gz}"
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "gzip of ${src} failed (PYTHON_CMD=${PYTHON_CMD} rc=${rc})")
    endif()
    file(READ "${gz}" hex HEX)
    file(REMOVE "${gz}")
    set(${OUT_VAR} "${hex}" PARENT_SCOPE)
endfunction()

gzip_file_hex("index.html" INDEX_HTML)
gzip_file_hex("app.js" APP_JS)
gzip_file_hex("style.css" STYLE_CSS)
gzip_file_hex("install-picker.js" INSTALL_PICKER_JS)
gzip_file_hex("preview3d.js" PREVIEW3D_JS)
file(READ "${UI_DIR}/moonlight-logo.png" LOGO_PNG HEX)

# Convert hex string to C array initializer
function(hex_to_c_array HEX_STR VAR_NAME OUT_VAR)
    string(LENGTH "${HEX_STR}" HEX_LEN)
    set(result "")
    math(EXPR last "${HEX_LEN} - 1")
    foreach(i RANGE 0 ${last} 2)
        string(SUBSTRING "${HEX_STR}" ${i} 2 byte)
        if(result)
            string(APPEND result ",")
        endif()
        string(APPEND result "0x${byte}")
    endforeach()
    set(${OUT_VAR} "${result}" PARENT_SCOPE)
endfunction()

hex_to_c_array("${INDEX_HTML}" "indexHtml" INDEX_ARRAY)
hex_to_c_array("${APP_JS}" "appJs" APP_ARRAY)
hex_to_c_array("${STYLE_CSS}" "styleCss" STYLE_ARRAY)
hex_to_c_array("${INSTALL_PICKER_JS}" "installPickerJs" INSTALL_PICKER_ARRAY)
hex_to_c_array("${PREVIEW3D_JS}" "preview3dJs" PREVIEW3D_ARRAY)
hex_to_c_array("${LOGO_PNG}" "logoPng" LOGO_ARRAY)

string(LENGTH "${INDEX_HTML}" INDEX_HEX_LEN)
string(LENGTH "${APP_JS}" APP_HEX_LEN)
string(LENGTH "${STYLE_CSS}" STYLE_HEX_LEN)
string(LENGTH "${INSTALL_PICKER_JS}" INSTALL_PICKER_HEX_LEN)
string(LENGTH "${PREVIEW3D_JS}" PREVIEW3D_HEX_LEN)
string(LENGTH "${LOGO_PNG}" LOGO_HEX_LEN)
math(EXPR INDEX_LEN "${INDEX_HEX_LEN} / 2")
math(EXPR APP_LEN "${APP_HEX_LEN} / 2")
math(EXPR STYLE_LEN "${STYLE_HEX_LEN} / 2")
math(EXPR INSTALL_PICKER_LEN "${INSTALL_PICKER_HEX_LEN} / 2")
math(EXPR PREVIEW3D_LEN "${PREVIEW3D_HEX_LEN} / 2")
math(EXPR LOGO_LEN "${LOGO_HEX_LEN} / 2")

file(WRITE "${OUT}" "// Auto-generated — do not edit. Rebuild to update.\n")
file(APPEND "${OUT}" "#pragma once\n#include <cstdint>\n#include <cstddef>\nnamespace mm::ui {\n")
file(APPEND "${OUT}" "constexpr uint8_t indexHtml[] = {${INDEX_ARRAY}};\n")
file(APPEND "${OUT}" "constexpr size_t indexHtmlLen = ${INDEX_LEN};\n")
file(APPEND "${OUT}" "constexpr uint8_t appJs[] = {${APP_ARRAY}};\n")
file(APPEND "${OUT}" "constexpr size_t appJsLen = ${APP_LEN};\n")
file(APPEND "${OUT}" "constexpr uint8_t styleCss[] = {${STYLE_ARRAY}};\n")
file(APPEND "${OUT}" "constexpr size_t styleCssLen = ${STYLE_LEN};\n")
file(APPEND "${OUT}" "constexpr uint8_t installPickerJs[] = {${INSTALL_PICKER_ARRAY}};\n")
file(APPEND "${OUT}" "constexpr size_t installPickerJsLen = ${INSTALL_PICKER_LEN};\n")
file(APPEND "${OUT}" "constexpr uint8_t preview3dJs[] = {${PREVIEW3D_ARRAY}};\n")
file(APPEND "${OUT}" "constexpr size_t preview3dJsLen = ${PREVIEW3D_LEN};\n")
file(APPEND "${OUT}" "constexpr uint8_t logoPng[] = {${LOGO_ARRAY}};\n")
file(APPEND "${OUT}" "constexpr size_t logoPngLen = ${LOGO_LEN};\n")
file(APPEND "${OUT}" "} // namespace mm::ui\n")
