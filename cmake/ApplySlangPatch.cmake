# Applies every patches/slang-*.patch to slang's FetchContent-vendored checkout,
# as CMake's PATCH_COMMAND for the `slang` FetchContent_Declare in the top-level
# CMakeLists.txt. Runs with the working directory set to slang's SOURCE_DIR (that
# is how ExternalProject/FetchContent invoke PATCH_COMMAND), so every `git apply`
# below targets the right tree with no path passed in.
#
# The set is globbed and sorted rather than listed, so adding a patch is one file
# and no build-system edit. Patches must therefore touch disjoint hunks; they are
# applied independently, in name order.
#
# Idempotent by construction rather than by assumption: CMake reruns a
# FetchContent dependency's patch step whenever the step's recorded command
# hash changes (for example, editing this script or a patch file), not only
# on a fully clean fetch -- and on that rerun the checkout already carries the
# patches from the previous run. `git apply --reverse --check` on a clean git
# checkout is the standard idiom for "is this patch already applied?": it
# succeeds silently (exit 0) exactly when reversing it would be a no-op error,
# i.e. the patch is present, so plain `git apply` only runs when it is not.
file(GLOB patch_files "${PATCH_DIR}/slang-*.patch")
list(SORT patch_files)
if(patch_files STREQUAL "")
  message(FATAL_ERROR "No slang-*.patch found in ${PATCH_DIR}")
endif()

foreach(patch_file IN LISTS patch_files)
  execute_process(
    COMMAND git apply --reverse --check "${patch_file}"
    RESULT_VARIABLE already_applied
    OUTPUT_QUIET
    ERROR_QUIET
  )
  if(NOT already_applied EQUAL 0)
    execute_process(
      COMMAND git apply "${patch_file}"
      RESULT_VARIABLE apply_result
    )
    if(NOT apply_result EQUAL 0)
      message(FATAL_ERROR "Failed to apply ${patch_file} to slang's vendored source")
    endif()
  endif()
endforeach()
