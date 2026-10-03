# WebSockets (links2004) -- vendored, because Runtime/WebSocketClient.h includes <WebSocketsClient.h>
# and there is no registry component for it.
#
# Version 2.4.x, the one platformio.ini pins, lifted from PlatformIO's package cache. See
# kryonos_async_tcp/CMakeLists.txt for the reasoning about copying rather than referencing.
#
# WHAT WAS CHANGED
#   One include block and one statement in WebSockets.cpp, both forced by arduino-esp32 4.x: the
#   SHAParallelEngine this library uses for the handshake digest (sha/sha_parallel_engine.h and its
#   esp_sha() entry point) was removed from the core. The replacement is PSA Crypto's SHA-1 -- the
#   same underlying engine, reached through the API that survived. See the comment at the call site.
#   Nothing else differs from upstream.
#
# LICENCE: LGPL-2.1 (see LICENSE, vendored alongside the sources).

set(WEBSOCKETS_SRCS
    "WebSockets.cpp"
    "WebSocketsClient.cpp"
    "WebSocketsServer.cpp"
    "SocketIOclient.cpp"
    # The vendored SHA-1 and base64 fallbacks. WebSockets.cpp reaches libb64 unconditionally (its
    # CORE_HAS_LIBB64 branch picks between two headers, both of which are these sources) and libsha1
    # is the non-ESP fallback path, kept compiled so the tree matches upstream rather than carrying a
    # partially-pruned copy.
    "libb64/cdecode.c"
    "libb64/cencode.c"
    "libsha1/libsha1.c"
)

idf_component_register(
    SRCS ${WEBSOCKETS_SRCS}
    # "." makes <WebSocketsClient.h> resolvable to WebSocketClient.h. The two subdirectories are on
    # the path because the library includes them as "libb64/cencode_inc.h" and
    # "libsha1/libsha1.h" -- that is, relative to the source root, not to the subdirectory.
    INCLUDE_DIRS "." "libb64" "libsha1"
    REQUIRES
        arduino-esp32
        mbedtls
)
