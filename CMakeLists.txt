cmake_minimum_required(VERSION 3.21)
project(MolForma VERSION 1.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY FALSE)
find_package(Eigen3 REQUIRED NO_MODULE)
find_package(ZLIB REQUIRED)
if(MSVC)
    # Open Babel's MSVC build installs a CMake package but no pkg-config file (docs/installation/windows.md).
    find_package(OpenBabel3 3 CONFIG REQUIRED)
    set(MOLFORMA_OPENBABEL openbabel)
    get_target_property(openbabel_dll openbabel IMPORTED_LOCATION_RELEASE)
    get_filename_component(openbabel_bin "${openbabel_dll}" DIRECTORY)
    message(STATUS "Found OpenBabel3, version ${OpenBabel3_VERSION}, in ${openbabel_bin}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPENBABEL REQUIRED IMPORTED_TARGET openbabel-3)
    set(MOLFORMA_OPENBABEL PkgConfig::OPENBABEL)
endif()
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
find_package(rdkit CONFIG QUIET)
# Keep dependency directions visible in both the target graph and source includes.
set(MOLFORMA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
include(cmake/CheckLayers.cmake)
add_library(molforma_project STATIC src/project/atom_references.cpp)
target_include_directories(molforma_project PUBLIC src)
target_link_libraries(molforma_project PUBLIC Qt6::Core)
add_library(molforma_foundation STATIC
    src/foundation/files.cpp
    src/foundation/structure_file.cpp
    src/foundation/i18n.cpp
    assets/resources.qrc)
target_include_directories(molforma_foundation PUBLIC src)
target_link_libraries(molforma_foundation PUBLIC Qt6::Core PRIVATE ZLIB::ZLIB)
add_library(molforma_model STATIC
    src/model/bond_inference.cpp
    src/model/document.cpp
    src/model/elements.cpp
    src/model/geometry.cpp
    src/model/topology.cpp
    src/model/xyz.cpp)
target_include_directories(molforma_model PUBLIC src)
target_compile_definitions(molforma_model PUBLIC MOLFORMA_VERSION="${PROJECT_VERSION}")
target_link_libraries(molforma_model PUBLIC molforma_project molforma_foundation Qt6::Core Eigen3::Eigen)

add_library(molforma_editing STATIC
    src/editing/atom_properties.cpp
    src/editing/chemistry.cpp
    src/editing/chemistry_job.cpp
    src/editing/editor_session.cpp
    src/editing/coordinates.cpp
    src/editing/coordination.cpp
    src/editing/library.cpp
    src/editing/operations.cpp
    src/editing/peptide.cpp
    src/editing/site.cpp
    src/editing/solvation.cpp
    src/editing/stereochemistry.cpp
    src/editing/xtb.cpp)
# Qt Gui only for the undo stack of EditorSession (QUndoStack); the layers below use Qt Core alone.
target_link_libraries(molforma_editing PUBLIC molforma_model Qt6::Gui PRIVATE ${MOLFORMA_OPENBABEL})
if(TARGET RDKit::CIPLabeler)
    target_link_libraries(molforma_editing PRIVATE RDKit::CIPLabeler RDKit::GraphMol RDKit::SubstructMatch)
    target_compile_definitions(molforma_editing PUBLIC MOLFORMA_HAS_RDKIT=1)
endif()
if(MSVC)
    # These definitions apply only to the targets using Open Babel's native API.
    target_compile_definitions(molforma_editing PRIVATE USING_DYNAMIC_LIBS
        MOLFORMA_OPENBABEL_DATADIR="${openbabel_bin}/data")
endif()

find_package(Threads REQUIRED)
add_library(molforma_results STATIC
    src/results/field_analysis.cpp
    src/results/energy_profile.cpp
    src/results/molden_orbitals.cpp
    src/results/result_files.cpp
    src/results/result_loader.cpp
    src/results/result_presentation.cpp
    src/results/result_properties.cpp
    src/results/result_summary.cpp
    src/results/spectrum.cpp
    src/results/cube_reader.cpp
    src/results/molden_reader.cpp
    src/results/nwchem_log.cpp
    src/results/result_cache.cpp
    src/results/result_codec.cpp
    src/results/result_parsing.cpp
    src/results/result_path.cpp
    src/results/result_surface.cpp
    src/results/result_worker.cpp
    src/results/xyz_reader.cpp
    src/results/results.cpp)
target_link_libraries(molforma_results PUBLIC molforma_model PRIVATE Threads::Threads)

add_library(molforma_nwchem STATIC
    src/nwchem/conformer_set.cpp
    src/nwchem/execution_environment.cpp
    src/nwchem/execution_plan.cpp
    src/nwchem/execution_settings.cpp
    src/nwchem/job_script.cpp
    src/nwchem/result_files.cpp
    src/nwchem/history.cpp
    src/nwchem/history_import.cpp
    src/nwchem/nwchem.cpp
    src/nwchem/nwchem_catalog.cpp
    src/nwchem/nwchem_paths.cpp
    src/nwchem/nwchem_plan.cpp
    src/nwchem/nwchem_input.cpp
    src/nwchem/nwchem_validation.cpp
    src/nwchem/nwchem_advanced.cpp
    src/nwchem/nwchem_geometry.cpp
    src/nwchem/nwchem_import.cpp
    src/nwchem/nwchem_output.cpp
    src/nwchem/nwchem_queue.cpp
    src/nwchem/nwchem_restart.cpp
    src/nwchem/nwchem_run.cpp
    src/nwchem/remote_host.cpp
    src/nwchem/remote_probe.cpp
    src/nwchem/remote_run.cpp
    src/nwchem/remote_shell.cpp
    src/nwchem/remote_supervisor.cpp
    src/nwchem/run_diagnostics.cpp
    src/nwchem/run_process.cpp
    src/nwchem/run_record.cpp
    src/nwchem/run_session.cpp
    src/nwchem/run_state.cpp
    src/nwchem/run_supervisor.cpp)
target_link_libraries(molforma_nwchem PUBLIC molforma_model PRIVATE molforma_editing molforma_results ${MOLFORMA_OPENBABEL})
if(MSVC)
    target_compile_definitions(molforma_nwchem PRIVATE USING_DYNAMIC_LIBS)
endif()

add_library(molforma_io STATIC
    src/io/babel_io.cpp
    src/io/crystal.cpp
    src/io/document_storage.cpp
    src/io/file_download.cpp
    src/io/mmcif.cpp
    src/io/pdb.cpp
    src/io/pdb_download.cpp
    src/io/pubchem.cpp
    src/io/structure_io.cpp)
# Open Babel for the structure files read and written through its API (babel_io.cpp).
target_link_libraries(molforma_io PUBLIC molforma_model
    PRIVATE molforma_nwchem molforma_editing molforma_results Qt6::Network ${MOLFORMA_OPENBABEL})
if(MSVC)
    target_compile_definitions(molforma_io PRIVATE USING_DYNAMIC_LIBS)
endif()

# Preserve the convenient aggregate for applications/tests without hiding the internal graph.
add_library(molforma_core INTERFACE)
target_link_libraries(molforma_core INTERFACE molforma_io molforma_nwchem molforma_editing molforma_results molforma_model)
foreach(target molforma_foundation molforma_project molforma_model molforma_editing molforma_results molforma_nwchem molforma_io)
    target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
endforeach()
add_library(molforma_ui STATIC
    src/ui/conformer_set_dialog.cpp
    src/ui/energy_profile_dialog.cpp
    src/ui/figure_export.cpp
    src/ui/calculation_panel.cpp
    src/ui/calculation_summary.cpp
    src/ui/chrome_layouts.cpp
    src/ui/coordinates_panel.cpp
    src/ui/diagnostics_list.cpp
    src/ui/editor_chrome.cpp
    src/ui/editor_tool.cpp
    src/ui/molecule_panel.cpp
    src/ui/molecule_properties.cpp
    src/ui/candidate_controls.cpp
    src/ui/execution_form.cpp
    src/ui/execution_panel.cpp
    src/ui/help_dialogs.cpp
    src/ui/history_panel.cpp
    src/ui/input_highlighter.cpp
    src/ui/input_preview_panel.cpp
    src/ui/line_icons.cpp
    src/ui/main_window.cpp
    src/ui/mcp_gui.cpp
    src/ui/molecule_view.cpp
    src/ui/pdb_download.cpp
    src/ui/periodic_table.cpp
    src/ui/preferences_dialog.cpp
    src/ui/result_charts.cpp
    src/ui/property_geometry_view.cpp
    src/ui/pubchem.cpp
    src/ui/remote_hosts_dialog.cpp
    src/ui/spectroscopy_view.cpp
    src/ui/results_panel.cpp
    src/ui/sequence_track.cpp
    src/ui/settings_widgets.cpp
    src/ui/structure_import.cpp
    src/ui/workspace_controller.cpp
    src/ui/run_progress.cpp
    src/ui/template_delegate.cpp
    src/ui/theme.cpp
    src/ui/translations.cpp
    src/ui/view_controls.cpp
    src/ui/xtb_dialog.cpp
    docs/manual.qrc)
# McpGui shares tool definitions, result views and the access policy with the headless server.
target_link_libraries(molforma_ui PUBLIC molforma_core molforma_mcp Qt6::Widgets PRIVATE ${MOLFORMA_OPENBABEL})
if(MSVC)
    # Both renderers use Open Babel van der Waals radii.
    target_compile_definitions(molforma_ui PRIVATE USING_DYNAMIC_LIBS)
endif()
target_compile_options(molforma_ui PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
# Optional: figures of charts in SVG beside PNG and PDF (Qt SVG).
find_package(Qt6 QUIET COMPONENTS Svg)
if(TARGET Qt6::Svg)
    target_link_libraries(molforma_ui PRIVATE Qt6::Svg)
    target_compile_definitions(molforma_ui PUBLIC MOLFORMA_HAS_QTSVG=1)
endif()
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
    src/mcp/document_adapter.cpp
    src/mcp/document_tools.cpp
    src/mcp/execution_trust.cpp
    src/mcp/headless.cpp
    src/mcp/json_schema.cpp
    src/mcp/policy.cpp
    src/mcp/prompts.cpp
    src/mcp/server.cpp
    src/mcp/stdio_transport.cpp
    src/mcp/tools.cpp)
target_link_libraries(molforma_mcp PUBLIC molforma_core)
target_compile_options(molforma_mcp PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
add_executable(molforma src/main.cpp)
# On Windows a GUI application without a console window; MCP clients still pass pipes as its standard streams.
set_target_properties(molforma PROPERTIES WIN32_EXECUTABLE ON)
target_link_libraries(molforma PRIVATE molforma_ui molforma_mcp)
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_test(NAME source_layers COMMAND ${CMAKE_COMMAND}
        "-DMOLFORMA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckLayers.cmake")
    add_executable(test_mcp tests/test_mcp.cpp)
    target_link_libraries(test_mcp PRIVATE molforma_ui molforma_mcp Qt6::Test)
    add_dependencies(test_mcp molforma)
    add_test(NAME mcp COMMAND test_mcp)
    set_tests_properties(mcp PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    add_executable(test_services tests/test_services.cpp)
    target_link_libraries(test_services PRIVATE molforma_core Qt6::Test)
    add_test(NAME services COMMAND test_services)
    # A stand-in for the xtb program (test_services).
    add_executable(fake_xtb tests/fake_xtb.cpp)
    target_link_libraries(fake_xtb PRIVATE Qt6::Core)
    add_dependencies(test_services fake_xtb)
    target_compile_definitions(test_services PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE molforma_core Qt6::Test ZLIB::ZLIB)
    target_compile_definitions(test_core PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
                                                 RECIPES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs/recipes/samples")
    add_test(NAME core COMMAND test_core)
    add_executable(test_editor tests/test_editor.cpp)
    target_link_libraries(test_editor PRIVATE molforma_ui Qt6::Test)
    add_dependencies(test_editor fake_xtb)
    add_test(NAME editor COMMAND test_editor)
    add_executable(test_rendering tests/test_rendering.cpp)
    target_link_libraries(test_rendering PRIVATE molforma_ui Qt6::Test)
    if(MOLFORMA_ENABLE_VTK AND VTK_FOUND AND VTK_QT_VERSION STREQUAL "6")
        target_link_libraries(test_rendering PRIVATE ${VTK_LIBRARIES} Qt6::OpenGLWidgets)
    endif()
    add_test(NAME rendering COMMAND test_rendering)
    set_tests_properties(rendering PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    add_executable(test_coordinates tests/test_coordinates.cpp)
    target_link_libraries(test_coordinates PRIVATE molforma_ui Qt6::Test ZLIB::ZLIB)
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
    # A stand-in for ssh and scp (test_execution: runs on a remote destination).
    add_executable(fake_ssh tests/fake_ssh.cpp)
    target_link_libraries(fake_ssh PRIVATE Qt6::Core)
    add_executable(test_execution tests/test_execution.cpp)
    target_link_libraries(test_execution PRIVATE molforma_ui Qt6::Test)
    add_dependencies(test_execution fake_nwchem fake_ssh molforma)
    add_test(NAME execution COMMAND test_execution)
    add_executable(test_input_preview tests/test_input_preview.cpp)
    target_link_libraries(test_input_preview PRIVATE molforma_ui Qt6::Test)
    add_test(NAME input_preview COMMAND test_input_preview)
    add_executable(test_results tests/test_results.cpp)
    target_link_libraries(test_results PRIVATE molforma_ui Qt6::Test)
    target_compile_definitions(test_results PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME results COMMAND test_results)
    add_executable(test_history tests/test_history.cpp)
    target_link_libraries(test_history PRIVATE molforma_ui Qt6::Test)
    add_dependencies(test_history fake_nwchem molforma)
    add_test(NAME history COMMAND test_history)
    add_executable(test_output tests/test_output.cpp)
    target_link_libraries(test_output PRIVATE molforma_ui Qt6::Test)
    add_dependencies(test_output fake_nwchem)
    add_test(NAME output COMMAND test_output)
    add_executable(test_translations tests/test_translations.cpp)
    target_link_libraries(test_translations PRIVATE molforma_ui Qt6::Test)
    target_compile_definitions(test_translations PRIVATE SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME translations COMMAND test_translations)
    add_executable(test_molden tests/test_molden.cpp)
    target_link_libraries(test_molden PRIVATE molforma_ui Qt6::Test)
    add_test(NAME molden COMMAND test_molden)
    set_tests_properties(core services editor coordinates calculation advanced execution input_preview results history output molden translations PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        # The offscreen platform finds no fonts on Windows by itself; without them every text is a box.
        file(TO_CMAKE_PATH "$ENV{WINDIR}/Fonts" windows_fonts)
        set_tests_properties(mcp core services editor rendering coordinates calculation advanced execution input_preview results
            history output molden translations PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_FONTDIR=${windows_fonts}"
            # The copy of fake_nwchem in a temporary folder (test_execution) has no Qt beside it.
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/bin")
    endif()
    set_tests_properties(core services editor coordinates calculation advanced execution input_preview results history output molden translations PROPERTIES TIMEOUT 120)
    # Remote runs wait for the looks of their supervisors (every 10 seconds at the least).
    set_tests_properties(execution PROPERTIES TIMEOUT 240)
endif()
if(APPLE)
    # macOS: MolForma.app, which holds the libraries it uses (Qt, Open Babel with its format plugins and
    # data, RDKit, VTK), so that it runs without Homebrew; "cpack -G DragNDrop" puts it into a disk
    # image (docs/installation/macos.md). The build folder keeps a plain executable for the tests.
    set(bundle MolForma.app/Contents)
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(MOLFORMA_MACOS_MINIMUM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        # Homebrew's libraries are built for the macOS they were installed on.
        execute_process(COMMAND sw_vers -productVersion OUTPUT_VARIABLE macos_version
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX REPLACE "^([0-9]+).*" "\\1.0" MOLFORMA_MACOS_MINIMUM "${macos_version}")
    endif()
    configure_file(packaging/macos/Info.plist.in Info.plist @ONLY)
    install(TARGETS molforma RUNTIME DESTINATION ${bundle}/MacOS)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" DESTINATION ${bundle})
    install(FILES packaging/macos/molforma.icns LICENSE NOTICE README.md README.en.md DESTINATION ${bundle}/Resources)
    # Open Babel finds its format plugins (copied by bundle.cmake) and data through BABEL_LIBDIR and
    # BABEL_DATADIR, which main() points into the bundle.
    pkg_get_variable(openbabel_libdir openbabel-3 libdir)
    pkg_get_variable(openbabel_prefix openbabel-3 prefix)
    set(openbabel_plugins "${openbabel_libdir}/openbabel/${OPENBABEL_VERSION}")
    set(openbabel_data "${openbabel_prefix}/share/openbabel/${OPENBABEL_VERSION}")
    if(NOT EXISTS "${openbabel_plugins}" OR NOT EXISTS "${openbabel_data}")
        message(WARNING "Open Babel plugins or data not found (${openbabel_plugins}, ${openbabel_data})")
    endif()
    install(DIRECTORY "${openbabel_data}/" DESTINATION ${bundle}/Resources/openbabel)
    # Qt's Japanese texts for its standard dialogs, where qt.conf of macdeployqt makes Qt look for them.
    set(qt_translation "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}/qtbase_ja.qm")
    if(EXISTS "${qt_translation}")
        install(FILES "${qt_translation}" DESTINATION ${bundle}/translations)
    endif()
    # Copies Qt and the other libraries into the bundle and signs it (packaging/macos/bundle.cmake).
    find_program(MACDEPLOYQT macdeployqt HINTS "${QT6_INSTALL_PREFIX}/bin" REQUIRED)
    set(library_search "${QT6_INSTALL_PREFIX}/lib" "${openbabel_libdir}")
    foreach(package_dir IN ITEMS "${rdkit_DIR}" "${VTK_DIR}")
        if(package_dir)
            get_filename_component(package_lib "${package_dir}/../.." ABSOLUTE)
            list(APPEND library_search "${package_lib}")
        endif()
    endforeach()
    install(CODE "
        set(APP \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/MolForma.app\")
        set(MACDEPLOYQT \"${MACDEPLOYQT}\")
        set(SEARCH \"${library_search}\")
        set(OPENBABEL_PLUGINS \"${openbabel_plugins}\")
        include(\"${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/bundle.cmake\")")
    set(CPACK_PACKAGE_FILE_NAME "MolForma-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_DMG_VOLUME_NAME MolForma)
else()
    install(TARGETS molforma RUNTIME DESTINATION bin)
    install(FILES LICENSE NOTICE README.md README.en.md DESTINATION share/molforma)
    install(FILES docs/features.md docs/mcp.md DESTINATION share/molforma/docs)
    install(DIRECTORY docs/features DESTINATION share/molforma/docs)
    install(FILES docs/en/features.md docs/en/mcp.md DESTINATION share/molforma/docs/en)
    install(DIRECTORY docs/en/features DESTINATION share/molforma/docs/en)
endif()
if(MSVC)
    # Windows: a folder that runs on its own (docs/installation/windows.md). bin gets Qt (windeployqt),
    # Open Babel with its format plugins (*.obf, loaded from beside its DLL), its data folder and the
    # DLLs they link.
    file(GLOB openbabel_plugins "${openbabel_bin}/*.obf")
    install(FILES ${openbabel_plugins} DESTINATION bin)
    install(DIRECTORY "${openbabel_bin}/data" DESTINATION bin)
    set(dll_directories "${openbabel_bin}")
    foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND dll_directories "${prefix}/bin")
    endforeach()
    install(CODE "
        set(plugins \"${openbabel_plugins}\")
        set(directories \"${dll_directories}\")
        file(GET_RUNTIME_DEPENDENCIES EXECUTABLES \"$<TARGET_FILE:molforma>\" MODULES \${plugins}
            DIRECTORIES \${directories} PRE_EXCLUDE_REGEXES \"^api-ms-\" \"^ext-ms-\" \"^Qt6\"
            POST_EXCLUDE_REGEXES \"[Ss]ystem32\" RESOLVED_DEPENDENCIES_VAR dlls
            CONFLICTING_DEPENDENCIES_PREFIX conflict)
        # A DLL copied beside the executable after linking is found there too; take the original.
        foreach(name IN LISTS conflict_FILENAMES)
            foreach(path IN LISTS conflict_\${name})
                get_filename_component(directory \"\${path}\" DIRECTORY)
                if(NOT directory STREQUAL \"$<TARGET_FILE_DIR:molforma>\")
                    list(APPEND dlls \"\${path}\")
                    break()
                endif()
            endforeach()
        endforeach()
        file(INSTALL \${dlls} DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin\")")
    # NWChem and mpirun of WSL, for "3 Run settings" (packaging/windows).
    install(FILES packaging/windows/nwchem.cmd packaging/windows/mpirun.cmd DESTINATION nwchem-wsl)
    qt_generate_deploy_app_script(TARGET molforma OUTPUT_SCRIPT molforma_deploy NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT "${molforma_deploy}")
    # The executables of the build folder start without PATH too: after linking, each gets the DLLs it
    # loads (Qt's as well), Open Babel's plugins and a qt.conf beside it.
    set(runtime_targets molforma)
    if(BUILD_TESTING)
        list(APPEND runtime_targets fake_nwchem fake_ssh fake_xtb test_mcp test_core test_services test_editor test_rendering test_coordinates
            test_calculation test_advanced test_execution test_input_preview test_results test_history test_output
            test_translations test_molden)
    endif()
    foreach(target IN LISTS runtime_targets)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} "-DEXECUTABLE=$<TARGET_FILE:${target}>" "-DPLUGINS=${openbabel_plugins}"
                "-DDIRECTORIES=${QT6_INSTALL_PREFIX}/bin;${dll_directories}" "-DQT_PREFIX=${QT6_INSTALL_PREFIX}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/copy_runtime.cmake"
            VERBATIM)
    endforeach()
elseif(NOT APPLE)
    # Desktop integration (freedesktop.org): launcher, icon and file types.
    install(FILES packaging/molforma.desktop DESTINATION share/applications)
    install(FILES packaging/molforma.svg DESTINATION share/icons/hicolor/scalable/apps)
    install(FILES packaging/molforma-256.png DESTINATION share/icons/hicolor/256x256/apps RENAME molforma.png)
    install(FILES packaging/molforma-mime.xml DESTINATION share/mime/packages RENAME molforma.xml)
endif()
# "cpack -G DEB" in the build folder makes a Debian/Ubuntu package of the installed files
# (docs/installation/ubuntu.md). dpkg-shlibdeps adds the libraries the executable links;
# Depends adds the Qt platform plugins it loads at run time.
set(CPACK_PACKAGE_NAME molforma)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Molecular editor, input builder and result viewer for NWChem")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://hnakada123.github.io/MolForma/")
set(CPACK_PACKAGE_CONTACT "hnakada123")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION science)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-translations-l10n, fonts-noto-cjk")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "nwchem")
include(CPack)
