cmake_minimum_required (VERSION 2.4)

if(COMMAND cmake_policy)
cmake_policy(VERSION 2.6)
endif(COMMAND cmake_policy)

project (PearPC)

set(CMAKE_BUILD_TYPE PearPCDefaultBuildType)

# - Try to find precompiled headers support for GCC 3.4 and 4.x
# Once done this will define:
#
# Variable:
#   PCHSupport_FOUND
#
# Macro:
#   ADD_PRECOMPILED_HEADER

IF(CMAKE_COMPILER_IS_GNUCXX)
    EXEC_PROGRAM(
    	${CMAKE_CXX_COMPILER} 
        ARGS 			--version 
        OUTPUT_VARIABLE _compiler_output)
    STRING(REGEX REPLACE ".* ([0-9]\\.[0-9]\\.[0-9]) .*" "\\1" 
           gcc_compiler_version ${_compiler_output})
    #MESSAGE("GCC Version: ${gcc_compiler_version}")
    IF(gcc_compiler_version MATCHES "4\\.[0-9]\\.[0-9]")
        SET(PCHSupport_FOUND TRUE)
    ELSE(gcc_compiler_version MATCHES "4\\.[0-9]\\.[0-9]")
        IF(gcc_compiler_version MATCHES "3\\.4\\.[0-9]")
            SET(PCHSupport_FOUND TRUE)
        ENDIF(gcc_compiler_version MATCHES "3\\.4\\.[0-9]")
    ENDIF(gcc_compiler_version MATCHES "4\\.[0-9]\\.[0-9]")
ENDIF(CMAKE_COMPILER_IS_GNUCXX)

MACRO(ADD_PRECOMPILED_HEADER _targetName _header _source)
IF(MSVC)
	STRING(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" _headerName "${_header}")
	SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /Yu\"${_headerName}\"")
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Yu\"${_headerName}\"")
	GET_FILENAME_COMPONENT(_sourceFile ${_source} NAME)
	set_source_files_properties("${_sourceFile}"
		COMPILE_FLAGS "/Yc\"${_headerName}\""
	)
ENDIF(MSVC)
IF(CMAKE_COMPILER_IS_GNUCXX AND NOT XCODE)
	IF(NOT CMAKE_BUILD_TYPE)
		MESSAGE(FATAL_ERROR 
			"This is the ADD_PRECOMPILED_HEADER macro. " 
			"You must set CMAKE_BUILD_TYPE!"
		)
	ENDIF(NOT CMAKE_BUILD_TYPE)

	GET_FILENAME_COMPONENT(_name ${_header} NAME)
	GET_FILENAME_COMPONENT(_path ${_header} PATH)
	SET(_outdir "${CMAKE_CURRENT_BINARY_DIR}/${_name}.gch")
	SET(_output "${_outdir}/${CMAKE_BUILD_TYPE}.c++")

	ADD_CUSTOM_COMMAND(
		OUTPUT ${_outdir}
		COMMAND mkdir -p ${_outdir} # TODO: {CMAKE_COMMAND} -E ...
	)
	#MAKE_DIRECTORY(${_outdir})
    
	STRING(TOUPPER "CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE}" _flags_var_name)
	SET(_compile_FLAGS ${${_flags_var_name}})
	GET_DIRECTORY_PROPERTY(_directory_flags INCLUDE_DIRECTORIES)

	FOREACH(item ${_directory_flags})
		LIST(APPEND _compile_FLAGS "-I${item}")
	ENDFOREACH(item)

	GET_DIRECTORY_PROPERTY(_directory_flags DEFINITIONS)
	LIST(APPEND _compile_FLAGS ${_directory_flags})
	LIST(APPEND _compile_FLAGS ${CMAKE_CXX_FLAGS} )

	SEPARATE_ARGUMENTS(_compile_FLAGS)
	#MESSAGE("_compiler_FLAGS: ${_compiler_FLAGS}")
	#message(${_compile_FLAGS} )
	#message("COMMAND ${CMAKE_CXX_COMPILER} ${_compile_FLAGS} -x c++-header -o ${_output} ${_header}")

	ADD_CUSTOM_COMMAND(
		OUTPUT ${_output} 	
		COMMAND ${CMAKE_CXX_COMPILER}
				${_compile_FLAGS}
				-x c++-header
				-o ${_output} 
				${_header}
		DEPENDS ${_header} ${_outdir}
	)
   	ADD_CUSTOM_TARGET(${_targetName}_gch 
		DEPENDS	${_output} 	
	)
	ADD_DEPENDENCIES(${_targetName} ${_targetName}_gch)
	ADD_DEPENDENCIES(${_targetName}_gch BuildNumber)
	SET_TARGET_PROPERTIES(${_targetName} 
		PROPERTIES	
		COMPILE_FLAGS "-include ${_header} -Winvalid-pch"
	)
ENDIF(CMAKE_COMPILER_IS_GNUCXX AND NOT XCODE)
ENDMACRO(ADD_PRECOMPILED_HEADER)

#
# BROKEN: Doesn't properly detect non-Apple GCC compilers.
#
# if(APPLE)
# 	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -arch i386 -arch x86_64 -arch ppc -arch ppc64")
# 	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -arch i386 -arch x86_64 -arch ppc -arch ppc64")
# 	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -arch i386 -arch x86_64 -arch ppc -arch ppc64")
# endif(APPLE)

if(CMAKE_COMPILER_IS_GNUCXX)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -std=gnu++98 -fno-common -fno-strict-aliasing -Wall -Wundef -Wno-long-long")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -std=gnu99 -fno-common -fno-strict-aliasing -Wall -Wundef -Wno-long-long")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/opt/local/lib -lX11")
	if(NOT CYGWIN)
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb")
	endif(NOT CYGWIN)
endif(CMAKE_COMPILER_IS_GNUCXX)

if(CYGWIN OR APPLE)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DASM_FUNCTION_PREFIX=_")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DASM_FUNCTION_PREFIX=_")
endif(CYGWIN OR APPLE)

# settings for Microsoft Visual C
IF(MSVC)
   IF(NOT WITH_SHARED_LIBS)
     IF(CMAKE_GENERATOR STREQUAL "Visual Studio 7" OR CMAKE_GENERATOR STREQUAL "Visual Studio 7 .NET 2003")
       SET(CMAKE_C_FLAGS "/nologo /W3 /Gy /Gm /G6 /GS- /Zi")
       SET(CMAKE_C_FLAGS_DEBUG "/MTd /Zi /Od")
       SET(CMAKE_C_FLAGS_RELEASE "/MT /O2 /Ot")
     ENDIF(CMAKE_GENERATOR STREQUAL "Visual Studio 7" OR CMAKE_GENERATOR STREQUAL "Visual Studio 7 .NET 2003")
     IF(CMAKE_GENERATOR STREQUAL "Visual Studio 8 2005" OR CMAKE_GENERATOR STREQUAL "Visual Studio 9 2008")
       SET(CMAKE_C_FLAGS "/nologo /W3 /Gy /Gm /GS- /Zi /D_CRT_SECURE_NO_DEPRECATE /D_CRT_NONSTDC_NO_DEPRECATE")
       SET(CMAKE_C_FLAGS_DEBUG "/MTd /Od")
       SET(CMAKE_C_FLAGS_RELEASE "/MT /O2 /Ot")
     ENDIF(CMAKE_GENERATOR STREQUAL "Visual Studio 8 2005" OR CMAKE_GENERATOR STREQUAL "Visual Studio 9 2008")
   ENDIF(NOT WITH_SHARED_LIBS)
ENDIF(MSVC)

include_directories (
	${PearPC_BINARY_DIR}
	${PearPC_BINARY_DIR}/src
	${PearPC_SOURCE_DIR}/src
	${PearPC_SOURCE_DIR}/contrib/crisscross/source
	.)

if(MSVC)
	include_directories(${PearPC_SOURCE_DIR}/contrib/windows C:/SDKs/WpdPack/Include)
endif(MSVC)

add_subdirectory (src)
add_subdirectory (contrib/crisscross)
