# slopcap_delay_import(<dll> <out-var>): make a MinGW delay-load import library for a vendor DLL, so an executable
# linked against it starts even when the DLL is absent; the DLL is loaded on the first call into it (or found already
# loaded: the GUI extracts the embedded SDK DLLs and loads them first). Callers check that the DLL loads before
# calling into it (see camera/open_camera.cpp).
# The .def comes from the DLL's own export table (objdump), the library from dlltool --output-delaylib.
function(slopcap_delay_import dll out_var)
  get_filename_component(name ${dll} NAME_WE)
  get_filename_component(dllname ${dll} NAME)
  set(dir ${CMAKE_BINARY_DIR}/delayimp)
  file(MAKE_DIRECTORY ${dir})
  find_program(SLOPCAP_OBJDUMP NAMES x86_64-w64-mingw32-objdump REQUIRED)
  find_program(SLOPCAP_DLLTOOL NAMES x86_64-w64-mingw32-dlltool REQUIRED)
  execute_process(COMMAND ${SLOPCAP_OBJDUMP} -p ${dll} OUTPUT_VARIABLE dump COMMAND_ERROR_IS_FATAL ANY)
  # name rows look like "	[  21] +base[  22]  0015 ASIGetNumOfConnectedCameras" (4-digit hint, then the name);
  # the address rows next to them have an 8-digit RVA instead
  set(hex "[0-9a-f]")
  string(REGEX MATCHALL "\t\\[ *[0-9]+\\] \\+base\\[ *[0-9]+\\] +${hex}${hex}${hex}${hex} [A-Za-z_][A-Za-z0-9_@?$]*" rows "${dump}")
  set(def "LIBRARY ${dllname}\nEXPORTS\n")
  foreach(row IN LISTS rows)
    string(REGEX REPLACE "^.* " "" sym "${row}")
    # qhyccd.dll also exports the pthreads-win32 it was built with; those would clash with MinGW's own winpthreads
    if(sym MATCHES "^(pthread|ptw32|sem_|sched_)")
      continue()
    endif()
    string(APPEND def "  ${sym}\n")
  endforeach()
  list(LENGTH rows count)
  if(count EQUAL 0)
    message(FATAL_ERROR "no exports found in ${dll}")
  endif()
  file(WRITE ${dir}/${name}.def "${def}")
  set(lib ${dir}/lib${name}.delay.a)
  execute_process(COMMAND ${SLOPCAP_DLLTOOL} --input-def ${dir}/${name}.def --dllname ${dllname} --output-delaylib ${lib}
                  COMMAND_ERROR_IS_FATAL ANY)
  message(STATUS "${dllname}: ${count} exports, delay-loaded via ${lib}")
  set(${out_var} ${lib} PARENT_SCOPE)
endfunction()
