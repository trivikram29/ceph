# Specify the C++ standard version for the -std= flag.
#
# USE_CXX_STANDARD(VERSIONS versions...
#                  [WITHOUT_EXTENSIONS]
#                  [REQUIRED])
#
# Checks compiler support for the given VERSIONS, using the first successful
# entry (see CXX_STANDARD).
#
# If the optional flag WITHOUT_EXTENSIONS is given, compiler specific
# extensions are disabled. This generally means -std=c++## instead of
# -std=gnu++## (see CXX_EXTENSIONS).
#
# If none of the given VERSIONS are supported and the REQUIRED flag is given,
# the function fails with a fatal error.
#
# Output variables:
#   CMAKE_CXX_STANDARD
#   CMAKE_CXX_EXTENSIONS
#
# Example usage to require at least c++11:
#   use_cxx_standard(VERSIONS 14 11 REQUIRED)
#

include(CMakeParseArguments)

function(USE_CXX_STANDARD)
	if(DEFINED CMAKE_CXX_STANDARD) # use cached value
		return()
	endif()

	cmake_parse_arguments(USE_CXX_STANDARD
		"WITHOUT_EXTENSIONS;REQUIRED"
		""
		"VERSIONS" ${ARGN})

	if(USE_CXX_STANDARD_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "Unrecognized arguments in USE_CXX_STANDARD: ${USE_CXX_STANDARD_UNPARSED_ARGUMENTS}")
	endif()

	if(NOT USE_CXX_STANDARD_VERSIONS)
		message(FATAL_ERROR "Missing VERSIONS arguments in USE_CXX_STANDARD.")
	endif()

	if(USE_CXX_STANDARD_WITHOUT_EXTENSIONS)
		set(EXTENSIONS OFF)
	else()
		set(EXTENSIONS ON)
	endif()

	foreach(VERSION IN LISTS USE_CXX_STANDARD_VERSIONS)
		check_cxx_standard(VERSION_SUPPORTED ${VERSION} ${EXTENSIONS})
		if(VERSION_SUPPORTED)
			set(CMAKE_CXX_STANDARD ${VERSION} CACHE INTERNAL "")
			set(CMAKE_CXX_EXTENSIONS ${EXTENSIONS} CACHE INTERNAL "")
			return()
		endif()
	endforeach()

	if(USE_CXX_STANDARD_REQUIRED)
		list(GET USE_CXX_STANDARD_VERSIONS -1 MIN_REQUIRED_VERSION)
		message(FATAL_ERROR "Compiler does not support the minimum required C++ version ${MIN_REQUIRED_VERSION}.")
	endif()
endfunction()

# Check compiler support for the given values of CXX_STANDARD/CXX_EXTENSIONS
macro(CHECK_CXX_STANDARD VARIABLE VERSION EXTENSIONS)
	set(CHECK_CXX_STANDARD_MESSAGE "Checking CXX${VERSION} support")
	message(STATUS ${CHECK_CXX_STANDARD_MESSAGE})
	try_compile(${VARIABLE}
		${CMAKE_BINARY_DIR}
		${CMAKE_ROOT}/Modules/DummyCXXFile.cxx
		CMAKE_FLAGS -DCMAKE_CXX_STANDARD=${VERSION} -DCMAKE_CXX_EXTENSIONS=${EXTENSIONS}
		OUTPUT_VARIABLE OUTPUT)
	if(${VARIABLE})
		message(STATUS "${CHECK_CXX_STANDARD_MESSAGE} - yes")
		file(APPEND ${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeOutput.log
			"${CHECK_CXX_STANDARD_MESSAGE} passed with "
			"the following output:\n${OUTPUT}\n\n")
	else()
		message(STATUS "${CHECK_CXX_STANDARD_MESSAGE} - no")
		file(APPEND ${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeError.log
			"${CHECK_CXX_STANDARD_MESSAGE} failed with "
			"the following output:\n${OUTPUT}\n\n")
	endif()
endmacro()
