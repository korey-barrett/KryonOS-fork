# AsyncTCP -- vendored, because ESPAsyncWebServer cannot exist without it and it is not published
# to the ESP-IDF component registry.
#
# WHY THIS IS A COPY RATHER THAN A MANAGED COMPONENT
#   The registry has no AsyncTCP, and `idf_component.yml` can only pull from the registry, a git URL
#   or a local path. A git URL would be a network dependency at configure time and would float;
#   these two libraries are the ones the fork already pins by exact version in platformio.ini
#   (mathieucarbou/ESPAsyncWebServer @ 3.1.5, which requires mathieucarbou/AsyncTCP), so the copy
#   here is that same 3.3.2 source, taken from PlatformIO's own package cache rather than retyped.
#   Same bytes, same version, no network, and reproducible.
#
# WHAT WAS CHANGED
#   Nothing in AsyncTCP.cpp or AsyncTCP.h. The vendored sources are byte-identical to upstream;
#   only this file is ours.
#
# LICENCE: LGPL-3.0 (see LICENSE, vendored alongside the sources).

idf_component_register(
    SRCS "AsyncTCP.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        arduino-esp32
        # lwip is named explicitly rather than relied on transitively: AsyncTCP talks to the raw
        # TCP PCB layer (tcp.h, tcpip.h) and to the netif list, which arduino-esp32 does not
        # re-export. Naming it keeps those headers resolvable if the Arduino core's own REQUIRES
        # ever narrows.
        lwip
)
