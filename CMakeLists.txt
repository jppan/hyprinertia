cmake_minimum_required(VERSION 3.27)

project(
  hyprinertia
  DESCRIPTION "Pointer inertia plugin for Hyprland"
  VERSION 0.4)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)

file(GLOB_RECURSE SRC "src/*.cpp")

add_library(hyprinertia SHARED ${SRC})

find_package(PkgConfig REQUIRED)
pkg_check_modules(
  deps
  REQUIRED
  IMPORTED_TARGET
  hyprland
  libdrm
  libinput
  libudev
  pangocairo
  pixman-1
  wayland-server
  xkbcommon)

target_link_libraries(hyprinertia PRIVATE rt PkgConfig::deps)
install(TARGETS hyprinertia)
