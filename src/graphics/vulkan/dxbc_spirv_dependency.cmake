# Runtime library, pinned including the SPIR-V headers gitlink.
include(FetchContent)
find_program(MESON_EXECUTABLE meson REQUIRED)
find_program(NINJA_EXECUTABLE ninja REQUIRED)
FetchContent_Declare(dxbc_spirv
    GIT_REPOSITORY https://github.com/doitsujin/dxbc-spirv.git
    GIT_TAG 687a30131e50533fedd9e785a1bf3aea463504f2
    GIT_SHALLOW FALSE
    GIT_SUBMODULES submodules/spirv_headers
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(dxbc_spirv)

# Meson must use the target compiler, not whichever compiler is on PATH. A
# supplied machine file also carries SDK/sysroot/CPU options for cross builds.
set(REXGLUE_DXBC_SPIRV_MACHINE_FILE "" CACHE FILEPATH "Meson target native/cross file")
set(_dxbc_spirv_machine "${CMAKE_CURRENT_BINARY_DIR}/dxbc-spirv-native.ini")
if(REXGLUE_DXBC_SPIRV_MACHINE_FILE)
    set(_dxbc_spirv_machine "${REXGLUE_DXBC_SPIRV_MACHINE_FILE}")
elseif(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Cross Vulkan builds require REXGLUE_DXBC_SPIRV_MACHINE_FILE for the target toolchain")
else()
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _dxbc_spirv_config)
    separate_arguments(_dxbc_spirv_cpp_flags NATIVE_COMMAND
        "${CMAKE_CXX_COMPILER_ARG1} ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_dxbc_spirv_config}}")
    if(CMAKE_CXX_COMPILER_TARGET)
        list(APPEND _dxbc_spirv_cpp_flags "--target=${CMAKE_CXX_COMPILER_TARGET}")
    endif()
    if(CMAKE_SYSROOT)
        list(APPEND _dxbc_spirv_cpp_flags "--sysroot=${CMAKE_SYSROOT}")
    endif()
    set(_dxbc_spirv_cpp_args "")
    foreach(_arg IN LISTS _dxbc_spirv_cpp_flags)
        string(REPLACE "\\" "\\\\" _arg "${_arg}")
        string(REPLACE "'" "\\'" _arg "${_arg}")
        string(APPEND _dxbc_spirv_cpp_args "'${_arg}',")
    endforeach()
    file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" _dxbc_spirv_cpp)
    file(TO_CMAKE_PATH "${CMAKE_AR}" _dxbc_spirv_ar)
    file(WRITE "${_dxbc_spirv_machine}"
        "[binaries]\ncpp = '${_dxbc_spirv_cpp}'\nar = '${_dxbc_spirv_ar}'\n[built-in options]\ncpp_args = [${_dxbc_spirv_cpp_args}]\n")
endif()
if(CMAKE_CROSSCOMPILING)
    set(_dxbc_spirv_machine_option --cross-file)
else()
    set(_dxbc_spirv_machine_option --native-file)
endif()
set(_dxbc_spirv_build_dir "${CMAKE_BINARY_DIR}/dxbc-spirv-build")
set(_dxbc_spirv_archive "${_dxbc_spirv_build_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}dxbc_spv${CMAKE_STATIC_LIBRARY_SUFFIX}")
if(EXISTS "${_dxbc_spirv_build_dir}/meson-private/coredata.dat")
    set(_dxbc_spirv_setup_mode --reconfigure)
else()
    set(_dxbc_spirv_setup_mode)
endif()
execute_process(
    COMMAND "${MESON_EXECUTABLE}" setup ${_dxbc_spirv_setup_mode}
            "${_dxbc_spirv_build_dir}" "${dxbc_spirv_SOURCE_DIR}"
            ${_dxbc_spirv_machine_option} "${_dxbc_spirv_machine}"
            --backend=ninja --buildtype=release -Ddefault_library=static -Db_staticpic=true
            -Denable_tests=false -Denable_tools=false -Denable_sm3=false
            -Denable_sm5=true -Denable_spirv=true
    RESULT_VARIABLE _dxbc_spirv_setup_result
)
if(NOT _dxbc_spirv_setup_result EQUAL 0)
    message(FATAL_ERROR "Failed to configure pinned dxbc-spirv library")
endif()
add_custom_target(dxbc-spirv-library
    COMMAND "${MESON_EXECUTABLE}" compile -C "${_dxbc_spirv_build_dir}" -j 2
    BYPRODUCTS "${_dxbc_spirv_archive}"
    VERBATIM)
add_library(dxbc_spirv_runtime STATIC IMPORTED GLOBAL)
set_target_properties(dxbc_spirv_runtime PROPERTIES
    IMPORTED_LOCATION "${_dxbc_spirv_archive}"
    INTERFACE_INCLUDE_DIRECTORIES "${dxbc_spirv_SOURCE_DIR}")
add_dependencies(dxbc_spirv_runtime dxbc-spirv-library)
