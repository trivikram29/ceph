set(rbd_srcs
  rbd.cc
  ArgumentTypes.cc
  IndentStream.cc
  OptionPrinter.cc
  Shell.cc
  Utils.cc
  action/BenchWrite.cc
  action/Children.cc
  action/Clone.cc
  action/Copy.cc
  action/Create.cc
  action/Diff.cc
  action/DiskUsage.cc
  action/Export.cc
  action/ExportDiff.cc
  action/Feature.cc
  action/Flatten.cc
  action/ImageMeta.cc
  action/Import.cc
  action/ImportDiff.cc
  action/Info.cc
  action/Kernel.cc
  action/List.cc
  action/Lock.cc
  action/MergeDiff.cc
  action/ObjectMap.cc
  action/Remove.cc
  action/Rename.cc
  action/Resize.cc
  action/Snap.cc
  action/Status.cc
  action/Watch.cc
  $<TARGET_OBJECTS:krbd>
  $<TARGET_OBJECTS:common_util_obj>
  $<TARGET_OBJECTS:parse_secret_objs>
  $<TARGET_OBJECTS:heap_profiler_objs>
  )
add_executable(rbd ${rbd_srcs})
set_target_properties(rbd PROPERTIES OUTPUT_NAME rbd)
target_link_libraries(rbd librbd librados global common keyutils udev
  boost_regex boost_program_options
  ${BLKID_LIBRARIES} ${CMAKE_DL_LIBS} ${TCMALLOC_LIBS})
install(TARGETS rbd DESTINATION bin)
