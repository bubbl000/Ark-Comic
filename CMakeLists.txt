cmake_minimum_required(VERSION 3.20)
project(ArkComic VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")

# 源码
file(GLOB_RECURSE SOURCES
    src/*.cpp
    src/*.h
)

add_definitions(-DNOMINMAX -DWIN32_LEAN_AND_MEAN)

# 启用 NVJPEG 硬件解码（动态加载 nvjpeg64_12.dll + cudart64_12.dll，无 N 卡时自动回退 CPU）
add_definitions(-DHAS_NVJPEG)

# sq即 sqlite3 + miniz（CBZ/ZIP 解压）官方单文件（编进项目）
add_executable(ArkComic WIN32
    ${SOURCES}
    third_party/sqlite3/sqlite3.c
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_zip.c
    resources/app.rc
)

# ── 运行时 DLL 自动拷贝（POST_BUILD）──
# 这些 DLL 是运行时动态加载（LoadLibrary），不链接 .lib：
#   turbojpeg.dll    - JPEG CPU 解码加速（libjpeg-turbo）
#   nvjpeg64_12.dll  - N 卡 JPEG 硬解（可选，无 N 卡自动回退）
#   cudart64_12.dll  - CUDA 运行时（nvjpeg 依赖）
# 统一放 third_party/bin/，构建时自动拷到 build/，避免每次手动复制
# （third_party/bin 不入 git 仓库；DLL 不存在时跳过拷贝，构建后手动放置即可）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bin/turbojpeg.dll")
    add_custom_command(TARGET ArkComic POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bin/turbojpeg.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bin/nvjpeg64_12.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bin/cudart64_12.dll"
            "$<TARGET_FILE_DIR:ArkComic>"
        COMMENT "拷贝运行时 DLL 到 build 目录")
else()
    message(WARNING "third_party/bin/turbojpeg.dll 不存在——跳过 DLL 拷贝（构建后请手动放置运行时 DLL）")
endif()

if(MSVC)
    # /Zi + /DEBUG：Release 亦生成 PDB
    target_compile_options(ArkComic PRIVATE /utf-8 /Zi)
    target_link_options(ArkComic PRIVATE /DEBUG)
endif()

target_include_directories(ArkComic PRIVATE
    src
    third_party/sqlite3
    third_party/nlohmann
    third_party/miniz
)

target_link_libraries(ArkComic PRIVATE
    d2d1
    dwrite
    dxgi
    windowscodecs
    shlwapi
    comctl32
    gdi32
    kernel32
    user32
    dwmapi
    uuid
    psapi
    comdlg32
    shell32
    ole32
    oleaut32
)

install(TARGETS ArkComic RUNTIME DESTINATION .)