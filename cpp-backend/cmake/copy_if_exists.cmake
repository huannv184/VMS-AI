# cpp-backend/cmake/copy_if_exists.cmake
#
# Build-time guarded copy. Invoked via `cmake -P` from a POST_BUILD step:
#
#   COMMAND ${CMAKE_COMMAND}
#       -DSRC=/path/to/source/file
#       -DDST=/path/to/destination/file
#       [-DLABEL="human-readable name"]
#       -P ${CMAKE_SOURCE_DIR}/cmake/copy_if_exists.cmake
#
# Behaviour:
#   - If SRC exists: copy to DST (creating parent dirs as needed).
#     Uses cmake's copy_if_different so the timestamp on DST only bumps when
#     content actually changed (preserves incremental-build laziness).
#   - If SRC is missing: emit a single STATUS line and exit 0. The build
#     continues without failure.
#
# Why this exists (BUG-AI-MODEL-DEPLOY-POSTBUILD-01, 2026-05-19):
#   ai_worker_v2 was unconditionally `copy_if_different`-ing
#   src/ai/inference/models/yolo11m.engine to build/models/ and
#   build/Release/models/ in its POST_BUILD step. The TensorRT .engine is a
#   locally-generated artifact (built on the deploy host from the .onnx) and
#   NEVER lives in the repo. Every CI / fresh-clone build therefore failed
#   the ai_worker_v2 target with MSB3073 errors, leaving operators (and AI
#   agents trying to verify other work) unable to do a clean `cmake --build`
#   without explicitly excluding the target.
#
#   This script lets the build degrade gracefully: the operator who has
#   generated the .engine sees it deployed; the operator who hasn't sees a
#   one-line "skipped, missing" notice and a successful build of every other
#   target.
#
# Why not gate at configure-time:
#   Configure-time `if(EXISTS ...)` would force a reconfigure each time the
#   operator drops the .engine into place — surprising and easy to forget.
#   Build-time gating means the next `cmake --build` after generating the
#   model picks it up automatically.

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "copy_if_exists.cmake requires -DSRC and -DDST")
endif()

if(NOT DEFINED LABEL)
    get_filename_component(LABEL "${SRC}" NAME)
endif()

if(EXISTS "${SRC}")
    get_filename_component(_dst_dir "${DST}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dst_dir}")
    file(COPY_FILE "${SRC}" "${DST}" ONLY_IF_DIFFERENT)
    message(STATUS "Deployed ${LABEL} -> ${DST}")
else()
    message(STATUS "Skip deploy ${LABEL}: ${SRC} not present (build-time artifact, generate before deploy)")
endif()
