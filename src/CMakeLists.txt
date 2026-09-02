add_subdirectory(core)

if(TAKT4_BUILD_UI)
  add_subdirectory(ui)

  add_executable(takt4 main.cpp)
  target_link_libraries(takt4 PRIVATE takt4::ui takt4::core takt4::warnings)
endif()
