cmake_minimum_required(VERSION 3.21)
project(MolForma VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY FALSE)
find_package(Eigen3 REQUIRED NO_MODULE)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENBABEL REQUIRED IMPORTED_TARGET openbabel-3)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
find_package(rdkit CONFIG QUIET)
add_library(molforma_core STATIC
    src/model/document.cpp
    src/model/geometry.cpp
    src/editing/chemistry.cpp
    src/editing/coordinates.cpp
    src/editing/coordination.cpp
    src/editing/library.cpp
    src/editing/operations.cpp
    src/editing/peptide.cpp
    src/editing/stereochemistry.cpp
    src/nwchem/history.cpp
    src/nwchem/nwchem.cpp
    src/nwchem/nwchem_advanced.cpp
    src/nwchem/nwchem_execution.cpp
    src/nwchem/nwchem_import.cpp
    src/nwchem/nwchem_output.cpp
    src/results/molden_orbitals.cpp
    src/results/results.cpp
    assets/resources.qrc)
target_include_directories(molforma_core PUBLIC src)
target_compile_definitions(molforma_core PUBLIC MOLFORMA_VERSION="${PROJECT_VERSION}")
find_package(Threads REQUIRED)
target_link_libraries(molforma_core PUBLIC Qt6::Widgets Eigen3::Eigen PkgConfig::OPENBABEL Threads::Threads)
if(TARGET RDKit::CIPLabeler)
    target_link_libraries(molforma_core PRIVATE RDKit::CIPLabeler RDKit::GraphMol RDKit::SubstructMatch)
    target_compile_definitions(molforma_core PUBLIC MOLFORMA_HAS_RDKIT=1)
endif()
target_compile_options(molforma_core PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
add_library(molforma_ui STATIC
    src/ui/advanced_panel.cpp
    src/ui/calculation_panel.cpp
    src/ui/coordinates_panel.cpp
    src/ui/diagnostics_list.cpp
    src/ui/editor_panels.cpp
    src/ui/execution_panel.cpp
    src/ui/history_panel.cpp
    src/ui/input_preview_panel.cpp
    src/ui/main_window.cpp
    src/ui/mcp_gui.cpp
    src/ui/molecule_view.cpp
    src/ui/periodic_table.cpp
    src/ui/result_charts.cpp
    src/ui/results_panel.cpp
    src/ui/template_delegate.cpp
    docs/manual.qrc)
# McpGui shares tool definitions, result views and the access policy with the headless server.
target_link_libraries(molforma_ui PUBLIC molforma_core molforma_mcp)
target_compile_options(molforma_ui PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
option(MOLFORMA_ENABLE_VTK "Use native VTK for accelerated molecular rendering when available" ON)
if(MOLFORMA_ENABLE_VTK)
    find_package(VTK 9.1 QUIET COMPONENTS CommonCore CommonDataModel FiltersSources
        RenderingCore RenderingOpenGL2 GUISupportQt InteractionStyle)
    # Some VTK packages report FOUND with missing private development dependencies.
    # Inspect only the requested VTK target graph, not unrelated optional VTK modules.
    set(vtk_pending ${VTK_LIBRARIES})
    set(vtk_checked)
    while(vtk_pending)
        list(POP_FRONT vtk_pending vtk_target)
        if(TARGET ${vtk_target} AND NOT vtk_target IN_LIST vtk_checked)
            list(APPEND vtk_checked ${vtk_target})
            get_target_property(vtk_links ${vtk_target} INTERFACE_LINK_LIBRARIES)
            string(REGEX MATCHALL "[A-Za-z0-9_]+::[A-Za-z0-9_]+" vtk_dependencies "${vtk_links}")
            foreach(dependency IN LISTS vtk_dependencies)
                if(NOT TARGET ${dependency})
                    set(VTK_FOUND FALSE)
                    message(STATUS "VTK development dependency unavailable: ${dependency}")
                elseif(dependency MATCHES "^VTK::")
                    list(APPEND vtk_pending ${dependency})
                endif()
            endforeach()
        endif()
    endwhile()
    if(VTK_FOUND AND VTK_QT_VERSION STREQUAL "6")
        find_package(Qt6 REQUIRED COMPONENTS OpenGLWidgets)
        target_sources(molforma_ui PRIVATE src/ui/vtk_molecule_renderer.cpp)
        target_link_libraries(molforma_ui PRIVATE ${VTK_LIBRARIES} Qt6::OpenGLWidgets)
        target_compile_definitions(molforma_ui PUBLIC MOLFORMA_HAS_VTK=1)
        vtk_module_autoinit(TARGETS molforma_ui MODULES ${VTK_LIBRARIES})
        message(STATUS "MolForma rendering: VTK ${VTK_VERSION} (Qt6), with QPainter fallback")
    else()
        message(STATUS "MolForma rendering: QPainter (Qt6-enabled VTK development package not found)")
    endif()
else()
    message(STATUS "MolForma rendering: QPainter (VTK disabled)")
endif()
add_library(molforma_mcp STATIC
    src/mcp/headless.cpp
    src/mcp/policy.cpp
    src/mcp/server.cpp
    src/mcp/stdio_transport.cpp
    src/mcp/tools.cpp)
target_link_libraries(molforma_mcp PUBLIC molforma_core)
target_compile_options(molforma_mcp PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
add_executable(molforma src/main.cpp)
target_link_libraries(molforma PRIVATE molforma_ui molforma_mcp)
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(test_mcp tests/test_mcp.cpp)
    target_link_libraries(test_mcp PRIVATE molforma_mcp molforma_ui Qt6::Test)
    add_dependencies(test_mcp molforma)
    add_test(NAME mcp COMMAND test_mcp)
    set_tests_properties(mcp PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE molforma_core Qt6::Test)
    target_compile_definitions(test_core PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME core COMMAND test_core)
    add_executable(test_editor tests/test_editor.cpp)
    target_link_libraries(test_editor PRIVATE molforma_ui Qt6::Test)
    add_test(NAME editor COMMAND test_editor)
    add_executable(test_rendering tests/test_rendering.cpp)
    target_link_libraries(test_rendering PRIVATE molforma_ui Qt6::Test)
    if(MOLFORMA_ENABLE_VTK AND VTK_FOUND AND VTK_QT_VERSION STREQUAL "6")
        target_link_libraries(test_rendering PRIVATE ${VTK_LIBRARIES} Qt6::OpenGLWidgets)
    endif()
    add_test(NAME rendering COMMAND test_rendering)
    set_tests_properties(rendering PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    add_executable(test_coordinates tests/test_coordinates.cpp)
    target_link_libraries(test_coordinates PRIVATE molforma_ui Qt6::Test)
    add_test(NAME coordinates COMMAND test_coordinates)
    add_executable(test_calculation tests/test_calculation.cpp)
    target_link_libraries(test_calculation PRIVATE molforma_ui Qt6::Test)
    add_test(NAME calculation COMMAND test_calculation)
    add_executable(test_advanced tests/test_advanced.cpp)
    target_link_libraries(test_advanced PRIVATE molforma_ui Qt6::Test)
    add_test(NAME advanced COMMAND test_advanced)
    add_executable(fake_nwchem tests/fake_nwchem.cpp)
    target_link_libraries(fake_nwchem PRIVATE Qt6::Core)
    add_dependencies(test_mcp fake_nwchem)
    add_executable(test_execution tests/test_execution.cpp)
    target_link_libraries(test_execution PRIVATE molforma_ui Qt6::Test)
    add_dependencies(test_execution fake_nwchem)
    add_test(NAME execution COMMAND test_execution)
    add_executable(test_input_preview tests/test_input_preview.cpp)
    target_link_libraries(test_input_preview PRIVATE molforma_ui Qt6::Test)
    add_test(NAME input_preview COMMAND test_input_preview)
    add_executable(test_results tests/test_results.cpp)
    target_link_libraries(test_results PRIVATE molforma_ui Qt6::Test)
    add_test(NAME results COMMAND test_results)
    add_executable(test_history tests/test_history.cpp)
    target_link_libraries(test_history PRIVATE molforma_ui Qt6::Test)
    add_dependencies(test_history fake_nwchem)
    add_test(NAME history COMMAND test_history)
    add_executable(test_output tests/test_output.cpp)
    target_link_libraries(test_output PRIVATE molforma_ui Qt6::Test)
    add_dependencies(test_output fake_nwchem)
    add_test(NAME output COMMAND test_output)
    add_executable(test_molden tests/test_molden.cpp)
    target_link_libraries(test_molden PRIVATE molforma_ui Qt6::Test)
    add_test(NAME molden COMMAND test_molden)
    set_tests_properties(core editor coordinates calculation advanced execution input_preview results history output molden PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(core editor coordinates calculation advanced execution input_preview results history output molden PROPERTIES TIMEOUT 120)
endif()
install(TARGETS molforma RUNTIME DESTINATION bin)
install(FILES LICENSE NOTICE README.md DESTINATION share/molforma)
install(FILES docs/features.md docs/mcp.md DESTINATION share/molforma/docs)
# Desktop integration (freedesktop.org): launcher, icon and file types.
install(FILES packaging/molforma.desktop DESTINATION share/applications)
install(FILES packaging/molforma.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES packaging/molforma-256.png DESTINATION share/icons/hicolor/256x256/apps RENAME molforma.png)
install(FILES packaging/molforma-mime.xml DESTINATION share/mime/packages RENAME molforma.xml)
