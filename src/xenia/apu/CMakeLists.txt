add_library(xenia-apu STATIC)
xe_platform_sources(xenia-apu ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(xenia-apu PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/FFmpeg
  ${PROJECT_SOURCE_DIR}/third_party/ffmpeg-xenia
)
target_link_libraries(xenia-apu PUBLIC libavcodec libavutil libavformat xenia-base)
# libopus: netplay voice side-channel codec (voice_codec.cc). The `opus` target
# propagates its own include dirs.
target_link_libraries(xenia-apu PUBLIC opus)
# librnnoise: RNN noise suppression + speech-probability VAD (voice_denoise.cc).
target_link_libraries(xenia-apu PUBLIC rnnoise)
if(WIN32)
  # WinMM waveOut for the netplay voice-chat host output (voice_output.cc).
  target_link_libraries(xenia-apu PUBLIC winmm)
endif()
xe_target_defaults(xenia-apu)
