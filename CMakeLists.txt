cmake_minimum_required(VERSION 3.20)

project(StarZip VERSION 0.0.2 LANGUAGES CXX)

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
)

target_link_libraries(StarZip PRIVATE Qt6::Widgets)
starzip_link_libarchive(StarZip)

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
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()

if(STARZIP_BUILD_TESTS)
    qt_add_executable(StarZipSelfTest
        tests/selftest.cpp
        ArchiveWorker.h
        ArchiveWorker.cpp
    )
    target_include_directories(StarZipSelfTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(StarZipSelfTest PRIVATE Qt6::Core)
    starzip_link_libarchive(StarZipSelfTest)
endif()

include(GNUInstallDirs)
install(TARGETS StarZip
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(WIN32)
    qt_generate_deploy_app_script(
        TARGET StarZip
        OUTPUT_SCRIPT starzip_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${starzip_deploy_script})
endif()
