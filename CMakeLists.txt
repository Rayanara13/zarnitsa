cmake_minimum_required(VERSION 3.13)
project(zarnitsa VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ZARNITSA_TESTS "Собирать тесты" ON)
option(ZARNITSA_GUI "Собирать окно (Qt Widgets)" ON)

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network)
if(ZARNITSA_GUI)
  find_package(Qt5 5.15 REQUIRED COMPONENTS Gui Widgets)
endif()

# Общий для рабочих мест Xeon без AVX: никаких -march=native.
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

set(CORE_SOURCES
  src/core/Types.cpp
  src/core/Target.cpp
  src/core/Settings.cpp
  src/core/Stats.cpp
  src/core/Monitor.cpp
  src/core/EventLog.cpp
  src/core/Config.cpp
  src/probes/ProbeDispatcher.cpp
  src/probes/Resolver.cpp
  src/probes/PingOutput.cpp
  src/probes/IcmpEngine.cpp
  src/probes/NetProbers.cpp
  src/probes/Probe.h
  src/icmp/icmp_packet.cpp
)
file(GLOB INTEGRATION_SOURCES CONFIGURE_DEPENDS src/integrations/*.cpp src/integrations/*.h)
file(GLOB API_SOURCES CONFIGURE_DEPENDS src/api/*.cpp src/api/*.h)
file(GLOB SERVICE_SOURCES CONFIGURE_DEPENDS src/service/*.cpp src/service/*.h)
file(GLOB CORE_HEADERS CONFIGURE_DEPENDS src/core/*.h src/probes/*.h)

add_library(zarnitsa_core STATIC ${CORE_SOURCES} ${CORE_HEADERS} ${INTEGRATION_SOURCES} ${API_SOURCES} ${SERVICE_SOURCES})
target_include_directories(zarnitsa_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(zarnitsa_core PUBLIC ZARNITSA_VERSION="${PROJECT_VERSION}" QT_DEPRECATED_WARNINGS)
target_link_libraries(zarnitsa_core PUBLIC Qt5::Core Qt5::Network)

# Помощник ICMP: без Qt и статически — его подписывают и выдают cap_net_raw отдельно.
add_executable(zarnitsa-icmp src/icmp/helper_main.cpp src/icmp/icmp_packet.cpp)
target_include_directories(zarnitsa-icmp PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_options(zarnitsa-icmp PRIVATE -static)

if(ZARNITSA_GUI)
  file(GLOB UI_SOURCES CONFIGURE_DEPENDS src/ui/*.cpp src/ui/*.h)
  add_library(zarnitsa_ui STATIC ${UI_SOURCES} resources/zarnitsa.qrc)
  target_link_libraries(zarnitsa_ui PUBLIC zarnitsa_core Qt5::Gui Qt5::Widgets)

  file(GLOB APP_SOURCES CONFIGURE_DEPENDS src/app/*.cpp src/app/*.h)
  add_executable(zarnitsa ${APP_SOURCES})
  target_link_libraries(zarnitsa PRIVATE zarnitsa_ui)
  install(TARGETS zarnitsa zarnitsa-icmp RUNTIME DESTINATION bin)
endif()

if(ZARNITSA_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
