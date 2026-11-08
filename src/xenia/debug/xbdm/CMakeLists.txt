add_library(xenia-debug-xbdm STATIC)
xe_platform_sources(xenia-debug-xbdm ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-debug-xbdm PUBLIC
  fmt xenia-base xenia-cpu xenia-kernel
)
if(WIN32)
  target_link_libraries(xenia-debug-xbdm PRIVATE ws2_32)
endif()
xe_target_defaults(xenia-debug-xbdm)
