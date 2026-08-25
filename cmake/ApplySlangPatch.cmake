# Applies patches/slang-class-forward-ref.patch to slang's FetchContent-vendored
# checkout, as CMake's PATCH_COMMAND for the `slang` FetchContent_Declare in the
# top-level CMakeLists.txt. Runs with the working directory set to slang's
# SOURCE_DIR (that is how ExternalProject/FetchContent invoke PATCH_COMMAND), so
# every `git apply` below targets the right tree with no path passed in.
#
# Idempotent by construction rather than by assumption: CMake reruns a
# FetchContent dependency's patch step whenever the step's recorded command
# hash changes (for example, editing this script or the patch file), not only
# on a fully clean fetch -- and on that rerun the checkout already carries the
# patch from the previous run. `git apply --reverse --check` on a clean git
# checkout is the standard idiom for "is this patch already applied?": it
# succeeds silently (exit 0) exactly when reversing it would be a no-op error,
# i.e. the patch is present, so plain `git apply` only runs when it is not.
execute_process(
  COMMAND git apply --reverse --check "${PATCH_FILE}"
  RESULT_VARIABLE already_applied
  OUTPUT_QUIET
  ERROR_QUIET
)
if(NOT already_applied EQUAL 0)
  execute_process(
    COMMAND git apply "${PATCH_FILE}"
    RESULT_VARIABLE apply_result
  )
  if(NOT apply_result EQUAL 0)
    message(FATAL_ERROR "Failed to apply ${PATCH_FILE} to slang's vendored source")
  endif()
endif()
