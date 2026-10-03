# ESPAsyncWebServer -- vendored, because the fork's WebManager.cpp includes <ESPAsyncWebServer.h>
# and there is no registry component for it.
#
# See kryonos_async_tcp/CMakeLists.txt for why this is a copy: it is mathieucarbou/ESPAsyncWebServer
# 3.1.5, the exact version platformio.ini pins, lifted from PlatformIO's package cache.
#
# WHAT WAS CHANGED
#   Nothing in the src/ tree. The vendored sources are byte-identical to upstream.
#
# LICENCE: LGPL-3.0 (see LICENSE, vendored alongside the sources).

# port/SHA1Builder.cpp is compiled explicitly rather than globbed with the rest: it is the WebSocket
# handshake's digest helper, it lives in a subdirectory, and `file(GLOB ...)` without a subdirectory
# pattern would silently skip it -- leaving a link error that reads like a missing library rather
# than a missing source.
set(ASYNC_WS_SRCS
    "AsyncEventSource.cpp"
    "AsyncWebSocket.cpp"
    "WebAuthentication.cpp"
    "WebHandlers.cpp"
    "WebRequest.cpp"
    "WebResponses.cpp"
    "WebServer.cpp"
    "port/SHA1Builder.cpp"
)

idf_component_register(
    SRCS ${ASYNC_WS_SRCS}
    INCLUDE_DIRS "." "port"
    REQUIRES
        arduino-esp32
        kryonos_async_tcp
        # AsyncJson.h is part of this library's public surface and includes <ArduinoJson.h> behind
        # ARDUINOJSON_VERSION_MAJOR >= 6. WebManager.cpp does not currently use AsyncJson, but the
        # header ships in the same include directory as the one it does use, so the dependency has
        # to be satisfied for the directory to be usable at all.
        bblanchon__arduinojson
)
