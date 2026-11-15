# Compiler options shared by every first-party target: strict warnings and reproducible floating point.
#
# Strict warnings: every first-party target. The physics core already builds clean
# with these flags; keep it that way (QSIG_WARNINGS_AS_ERRORS=ON in CI).
option(QSIG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(qsig_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(QSIG_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
            -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
        if(QSIG_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Reproducible floating point. Without this, GCC on ARM64 and Clang everywhere fuse a*b+c into
# FMA instructions; the last-bit difference was enough to flip the patch case of the validation
# bench to a non-physical branch (Zin = -0.43 - j8.3 ohm instead of 9.51 - j42.4 ohm).
function(qsig_set_fp_model target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PUBLIC -ffp-contract=off)
    elseif(MSVC)
        target_compile_options(${target} PUBLIC /fp:precise)
    endif()
endfunction()
