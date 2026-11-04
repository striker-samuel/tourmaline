# SPDX-License-Identifier: MIT
# Requires Tourmaline's root CMake, as an explicitly opt-in target.
if(NOT TARGET engine)
  message(FATAL_ERROR "RecompDiagnostic requires Tourmaline's engine target")
endif()
add_executable(RecompDiagnostic diagnostic_host.cpp)
target_compile_features(RecompDiagnostic PRIVATE cxx_std_20)
tourmaline_host_setup(RecompDiagnostic)

# The host reads identity/asset bytes ONLY via the apphost ProjectRuntime.
target_link_libraries(RecompDiagnostic PRIVATE tourmaline_apphost)
target_compile_definitions(RecompDiagnostic PRIVATE
  TOUR_RECOMP_SOURCE_ROOT="\"${TOURMALINE_SOURCE_DIR}\"")
