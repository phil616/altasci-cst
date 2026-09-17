# Portable verification harness; this does not build or package the Windows application.
cmake_minimum_required(VERSION 3.25)
project(CSTPortableTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
enable_testing()
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Test)
add_library(cst_domain STATIC ../src/domain/Configuration.cpp ../src/domain/Runtime.cpp)
target_include_directories(cst_domain PUBLIC ../src)
target_link_libraries(cst_domain PUBLIC Qt6::Core Qt6::Network)
if(MSVC)
  target_compile_options(cst_domain PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
else()
  target_compile_options(cst_domain PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_executable(cst-unit-domain unit/DomainTests.cpp)
target_link_libraries(cst-unit-domain PRIVATE cst_domain Qt6::Test)
target_compile_definitions(cst-unit-domain PRIVATE CST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME domain COMMAND cst-unit-domain)
add_library(cst_application STATIC
  ../src/application/ProjectConfigService.cpp
  ../src/application/ReadinessService.cpp
  ../src/application/PortReclaimService.cpp
  ../src/application/SourceSyncService.cpp
  ../src/application/TaskSupervisor.cpp
  ../src/application/OperationQueue.cpp
  ../src/application/ProjectRuntimeService.cpp
  ../src/application/LogService.cpp
  ../src/application/DiagnosticExportService.cpp
  ../src/infrastructure/common/SystemClock.cpp)
target_link_libraries(cst_application PUBLIC cst_domain)
if(MSVC)
  target_compile_options(cst_application PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
else()
  target_compile_options(cst_application PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_executable(cst-unit-config unit/ConfigTests.cpp)
target_link_libraries(cst-unit-config PRIVATE cst_application Qt6::Test)
target_compile_definitions(cst-unit-config PRIVATE CST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME config COMMAND cst-unit-config)
add_executable(cst-unit-services unit/ServiceTests.cpp)
target_link_libraries(cst-unit-services PRIVATE cst_application Qt6::Test)
add_test(NAME services COMMAND cst-unit-services)
add_library(cst_ui STATIC
  ../src/ui/MainWindow.cpp
  ../src/ui/widgets/SchemaEditor.cpp
  ../src/ui/pages/user/UserPage.cpp
  ../src/ui/pages/admin/AdminPage.cpp)
target_link_libraries(cst_ui PUBLIC cst_application Qt6::Widgets)
add_executable(cst-unit-ui unit/UiTests.cpp)
target_link_libraries(cst-unit-ui PRIVATE cst_ui Qt6::Test)
target_compile_definitions(cst-unit-ui PRIVATE CST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
foreach(scale 1.25 1.5 2)
  add_test(NAME ui.${scale} COMMAND cst-unit-ui)
  set_tests_properties(ui.${scale} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale}" TIMEOUT 30)
endforeach()
add_executable(cst-integration-git integration/GitSyncTests.cpp)
target_link_libraries(cst-integration-git PRIVATE cst_application Qt6::Test)
add_test(NAME integration.git COMMAND cst-integration-git)
set_tests_properties(integration.git PROPERTIES TIMEOUT 120)
add_executable(cst-unit-log unit/LogTests.cpp)
target_link_libraries(cst-unit-log PRIVATE cst_application Qt6::Test)
add_test(NAME log COMMAND cst-unit-log)
add_executable(cst-test-port-owner fixtures/PortOwner.cpp)
target_link_libraries(cst-test-port-owner PRIVATE Qt6::Core Qt6::Network)
