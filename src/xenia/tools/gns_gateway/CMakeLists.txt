# GNS gateway sidecar — gives a dedicated title server a GNS endpoint by
# bridging GNS <-> loopback UDP. Reuses the kernel's GNS transport + signaling
# sources directly (compiled into this standalone executable); see
# docs/gns_integration.md Phase 7.

add_executable(gns-gateway
  gns_gateway_main.cc
  ${PROJECT_SOURCE_DIR}/src/xenia/kernel/gns_transport.cc
  ${PROJECT_SOURCE_DIR}/src/xenia/kernel/gns_signaling.cc
)

target_compile_definitions(gns-gateway PRIVATE XE_GNS_ENABLED)

target_link_libraries(gns-gateway PRIVATE
  xenia-base
  GameNetworkingSockets
)

# libcurl WebSocket client for signaling. Windows uses the vendored Schannel
# build; Linux uses system libcurl (OpenSSL TLS) -- which must be recent enough
# to provide the WebSocket API (curl_ws_*, libcurl >= 7.86 with websockets
# enabled).
if(WIN32)
  target_link_libraries(gns-gateway PRIVATE libcurl)
else()
  find_package(CURL REQUIRED)
  target_link_libraries(gns-gateway PRIVATE CURL::libcurl)
endif()

xe_target_defaults(gns-gateway)
