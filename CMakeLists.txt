cmake_minimum_required(VERSION 3.20)

project(StarZip VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(STARZIP_BUILD_TESTS "构建 ArchiveWorker 核心逻辑自检程序" OFF)

find_package(Qt6 6.3 REQUIRED COMPONENTS Widgets)
find_package(LibArchive REQUIRED)

qt_standard_project_setup()

# 兼容 CMake 内置 FindLibArchive 与 vcpkg/发行版提供的 LibArchive config 包。
function(starzip_link_libarchive target)
    if(TARGET LibArchive::LibArchive)
        target_link_libraries(${target} PRIVATE LibArchive::LibArchive)
    elseif(TARGET LibArchive::archive)
        target_link_libraries(${target} PRIVATE LibArchive::archive)
    elseif(TARGET archive)
        target_link_libraries(${target} PRIVATE archive)
    else()
        if(LIBARCHIVE_INCLUDE_DIRS)
            target_include_directories(${target} PRIVATE ${LIBARCHIVE_INCLUDE_DIRS})
        elseif(LIBARCHIVE_INCLUDE_DIR)
            target_include_directories(${target} PRIVATE ${LIBARCHIVE_INCLUDE_DIR})
        endif()
        if(LIBARCHIVE_LIBRARIES)
            target_link_libraries(${target} PRIVATE ${LIBARCHIVE_LIBRARIES})
        elseif(LIBARCHIVE_LIBRARY)
            target_link_libraries(${target} PRIVATE ${LIBARCHIVE_LIBRARY})
        else()
            message(FATAL_ERROR "找到 LibArchive 头文件，但没有可链接的导入目标/库变量。")
        endif()
    endif()
endfunction()

qt_add_executable(StarZip
    WIN32
    main.cpp
    MainWindow.h
    MainWindow.cpp
    ArchiveWorker.h
    ArchiveWorker.cpp
    ExternalEngine.h
    ExternalEngine.cpp
    ShellIntegration.h
    ShellIntegration.cpp
    ZPack.h
    ZPack.cpp
)

target_link_libraries(StarZip PRIVATE Qt6::Widgets)
starzip_link_libarchive(StarZip)

if(WIN32)
    target_link_libraries(StarZip PRIVATE shell32)
endif()

set(STARZIP_7ZIP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/7zip")

if(MSVC)
    target_compile_options(StarZip PRIVATE /W4 /permissive-)
else()
    target_compile_options(StarZip PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    # 将直接链接的导入库运行时 DLL（如 libarchive.dll）复制到可执行文件旁。
    add_custom_command(TARGET StarZip POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:StarZip>
                $<TARGET_FILE_DIR:StarZip>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STARZIP_7ZIP_DIR}/7z.exe"
                "${STARZIP_7ZIP_DIR}/7z.dll"
                "${STARZIP_7ZIP_DIR}/License.txt"
                $<TARGET_FILE_DIR:StarZip>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()

if(STARZIP_BUILD_TESTS)
    qt_add_executable(StarZipSelfTest
        tests/selftest.cpp
        ArchiveWorker.h
        ArchiveWorker.cpp
        ExternalEngine.h
        ExternalEngine.cpp
        ShellIntegration.h
        ShellIntegration.cpp
        ZPack.h
        ZPack.cpp
    )
    target_include_directories(StarZipSelfTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(StarZipSelfTest PRIVATE Qt6::Core)
    if(WIN32)
        target_link_libraries(StarZipSelfTest PRIVATE shell32)
    endif()
    starzip_link_libarchive(StarZipSelfTest)

    qt_add_executable(ZPackTool
        tests/zpack_cli.cpp
        ZPack.h
        ZPack.cpp
    )
    target_include_directories(ZPackTool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ZPackTool PRIVATE Qt6::Core)
endif()

include(GNUInstallDirs)
install(TARGETS StarZip
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(WIN32)
    # 安装时从构建目录复制本程序直接/间接依赖的 DLL（libarchive、lzma、zlib 等）。
    install(CODE "
        file(GLOB _starzip_runtime_dlls \"${CMAKE_BINARY_DIR}/*.dll\")
        if(_starzip_runtime_dlls)
            file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\"
                 TYPE FILE FILES \${_starzip_runtime_dlls})
        endif()
    ")
    install(FILES
        "${STARZIP_7ZIP_DIR}/7z.exe"
        "${STARZIP_7ZIP_DIR}/7z.dll"
        "${STARZIP_7ZIP_DIR}/License.txt"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(WIN32)
    qt_generate_deploy_app_script(
        TARGET StarZip
        OUTPUT_SCRIPT starzip_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${starzip_deploy_script})
endif()
