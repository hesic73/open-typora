cmake_minimum_required(VERSION 3.21)
project(OpenTypora VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg Network PrintSupport Test)

# MicroTeX draws LaTeX math with Qt; tinyxml2 reads its font descriptions.
file(GLOB_RECURSE MICROTEX_SOURCES CONFIGURE_DEPENDS third_party/microtex/src/*.cpp)
list(FILTER MICROTEX_SOURCES EXCLUDE REGEX "/platform/")
add_library(microtex STATIC ${MICROTEX_SOURCES} third_party/microtex/src/platform/qt/graphic_qt.cpp
                            third_party/tinyxml2/tinyxml2.cpp)
target_include_directories(microtex SYSTEM PUBLIC third_party/microtex/src third_party/tinyxml2)
target_compile_definitions(microtex PUBLIC BUILD_QT)
target_compile_options(microtex PRIVATE -w)
target_link_libraries(microtex PUBLIC Qt6::Gui)
install(DIRECTORY third_party/microtex/res/ DESTINATION share/open-typora/microtex)

add_library(ot_warnings INTERFACE)
target_compile_options(ot_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)

# The editor and everything around it, as a library so that tests can drive it.
file(GLOB OT_SOURCES CONFIGURE_DEPENDS
  src/render/*.cpp src/render/*.h
  src/editor/*.cpp src/editor/*.h
  src/app/*.cpp src/app/*.h)
add_library(ot_lib STATIC ${OT_SOURCES})
target_include_directories(ot_lib PUBLIC src src/render src/editor src/app)
target_link_libraries(ot_lib PUBLIC Qt6::Widgets Qt6::Svg Qt6::Network Qt6::PrintSupport PRIVATE microtex ot_warnings)
# Where the math fonts are while developing; an installed copy is found next to the program.
target_compile_definitions(ot_lib PRIVATE OT_MICROTEX_SOURCE_RES="${CMAKE_SOURCE_DIR}/third_party/microtex/res")

add_executable(open-typora src/main.cpp resources/resources.qrc)
target_link_libraries(open-typora PRIVATE ot_lib ot_warnings)
set_target_properties(open-typora PROPERTIES INSTALL_RPATH_USE_LINK_PATH ON)
install(TARGETS open-typora RUNTIME DESTINATION bin)
install(FILES resources/open-typora.desktop DESTINATION share/applications)
install(FILES resources/open-typora.svg DESTINATION share/icons/hicolor/scalable/apps)

enable_testing()
add_subdirectory(tests)
