cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../components/kvm_common")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 組件在工程目錄之外（../components），IDF 默認只把工程目錄從 __FILE__ 裡去掉；這裡把整個倉庫的路徑換成 "."，
# 編出來的固件裡不留本機的絕對路徑
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
idf_build_set_property(COMPILE_OPTIONS "-fmacro-prefix-map=${REPO_ROOT}=." APPEND)
project(bridge)
