# ESP-IDF 项目配置
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 固件版本号: 写入镜像 esp_app_desc_t.version,
# Web 控制台 {{VERSION}} 占位符与 OTA 页面前端解析的均为此值。
# (须在 project() 之前设置; 不设置时 IDF 会回退为 git describe 或编译日期)
set(PROJECT_VER "1.3.1")

project(esp32_c3_usb_watchdog)

# ==============================================================================
# 分区表 vs Flash 大小 一致性校验
#
# 背景: 改分区表时很容易忘了同步改 menuconfig 里的 Flash size, 结果是
#   "Partitions tables occupies 4.0MB ... does not fit in configured flash size 2MB"
# 这个报错在编译到一半才蹦出来, 且不说怎么改。这里在 configure 阶段就拦下,
# 并直接给出对应的修复步骤。
#
# 校验在 project() 之后执行 (此时 sdkconfig 已被 IDF 加载)。
# ==============================================================================
function(_wd_compute_partition_end csv out_var)
    set(_max_end 0)
    set(_cursor 32768)      # 分区表从 0x8000 开始
    set(_have_data FALSE)

    file(STRINGS "${csv}" _lines)
    foreach(_line IN LISTS _lines)
        string(REGEX REPLACE "#.*$" "" _line "${_line}")
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "")
            continue()
        endif()
        string(REPLACE "," ";" _parts "${_line}")
        list(LENGTH _parts _n)
        if(_n LESS 5)
            continue()
        endif()
        list(GET _parts 3 _off)
        list(GET _parts 4 _size)
        string(STRIP "${_off}" _off)
        string(STRIP "${_size}" _size)
        if(_size STREQUAL "")
            continue()
        endif()
        if(_off STREQUAL "")
            set(_off "${_cursor}")
        endif()
        # 计算 end = off + size (支持 0x 十六进制)
        math(EXPR _end "${_off} + ${_size}")
        if(_end GREATER _max_end)
            set(_max_end ${_end})
        endif()
        set(_cursor ${_end})
        set(_have_data TRUE)
    endforeach()

    if(_have_data)
        set(${out_var} ${_max_end} PARENT_SCOPE)
    else()
        set(${out_var} 0 PARENT_SCOPE)
    endif()
endfunction()

function(_wd_get_config_flash_size out_var)
    # 优先用 IDF 已解析好的变量 (最可靠)
    set(_size_mb 0)
    if(DEFINED CONFIG_ESPTOOLPY_FLASHSIZE)
        # 形如 "4MB" / "2MB"
        string(REGEX MATCH "^([0-9]+)MB$" _m "${CONFIG_ESPTOOLPY_FLASHSIZE}")
        if(_m)
            set(_size_mb "${CMAKE_MATCH_1}")
        endif()
    endif()

    # 兜底: 直接扫 sdkconfig 文件里的 CONFIG_ESPTOOLPY_FLASHSIZE_xxMB=y
    if(_size_mb EQUAL 0)
        set(_sdk "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig")
        if(EXISTS "${_sdk}")
            file(STRINGS "${_sdk}" _cfg REGEX "^CONFIG_ESPTOOLPY_FLASHSIZE_[0-9]+MB=y$")
            foreach(_c IN LISTS _cfg)
                string(REGEX MATCH "FLASHSIZE_([0-9]+)MB" _mm "${_c}")
                if(_mm)
                    set(_size_mb "${CMAKE_MATCH_1}")
                endif()
            endforeach()
        endif()
    endif()

    set(${out_var} ${_size_mb} PARENT_SCOPE)
endfunction()

# --- 执行校验 ---
set(_wd_csv "${CMAKE_CURRENT_SOURCE_DIR}/partitions.csv")
if(EXISTS "${_wd_csv}")
    _wd_compute_partition_end("${_wd_csv}" _wd_end)
    _wd_get_config_flash_size(_wd_mb)

    if(_wd_end GREATER 0 AND _wd_mb GREATER 0)
        math(EXPR _wd_flash_bytes "${_wd_mb} * 1024 * 1024")
        math(EXPR _wd_need_kb "${_wd_end} / 1024")
        math(EXPR _wd_have_kb "${_wd_flash_bytes} / 1024")

        if(_wd_end GREATER _wd_flash_bytes)
            message(FATAL_ERROR
"\n"
"==========================================================================\n"
" 分区表与 Flash 大小不匹配\n"
"==========================================================================\n"
"   partitions.csv 需要 : ${_wd_need_kb} KB (${_wd_end} 字节)\n"
"   sdkconfig 配置的 Flash : ${_wd_mb} MB (${_wd_have_kb} KB)\n"
"\n"
" 分区表放不下! 二者必须配套 —— 请二选一:\n"
"\n"
"   [A] 用 4MB (默认, 推荐; 你的芯片实测就是 4MB)\n"
"       1. cp partitions.csv 保持为 4MB 版本 (仓库默认即是)\n"
"       2. sdkconfig 里确认: CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y\n"
"\n"
"   [B] 用 2MB (仅当模块确实是 2MB)\n"
"       1. cp partitions_2mb.csv partitions.csv\n"
"       2. sdkconfig 里改成: CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y\n"
"\n"
" 改完后务必执行:\n"
"       idf.py fullclean && idf.py build\n"
"==========================================================================\n")
        else()
            math(EXPR _wd_free_kb "(${_wd_flash_bytes} - ${_wd_end}) / 1024")
            message(STATUS "分区表校验通过: 占用 ${_wd_need_kb} KB / Flash ${_wd_mb} MB, 剩余 ${_wd_free_kb} KB")
        endif()
    else()
        message(STATUS "分区表校验: 信息不足 (end=${_wd_end}, flash=${_wd_mb}MB), 跳过")
    endif()
endif()
