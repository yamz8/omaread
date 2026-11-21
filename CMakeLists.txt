cmake_minimum_required(VERSION 3.21)
project(omaread LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 DBus Concurrent Pdf Test)
# EPUBs and comic archives are ZIP, RAR, 7z or tar files.
find_package(LibArchive REQUIRED)

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB omaread_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM omaread_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(omaread_core STATIC ${omaread_sources})
target_include_directories(omaread_core PUBLIC src)
target_link_libraries(omaread_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus Qt6::Concurrent Qt6::Pdf
  LibArchive::LibArchive)
target_compile_options(omaread_core PUBLIC -Wall -Wextra)

add_executable(omaread src/main.cpp src/resources.qrc)
target_link_libraries(omaread PRIVATE omaread_core)

# The tests load the real Main.qml, so they carry the resources too.
enable_testing()
add_executable(omaread_tests tests/omaread_tests.cpp src/resources.qrc)
target_link_libraries(omaread_tests PRIVATE omaread_core Qt6::Test)
add_test(NAME omaread_tests COMMAND omaread_tests)
set_tests_properties(omaread_tests PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# `cmake --install build --prefix ~/.local` puts the app in the launcher
# before it is packaged; a package installs the same files under /usr.
include(GNUInstallDirs)
install(TARGETS omaread RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES share/omaread.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES share/omaread.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/omaread)
