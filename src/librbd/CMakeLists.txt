set(librbd_types_srcs
  JournalTypes.cc
  WatchNotifyTypes.cc
  )
add_library(librbd_types OBJECT ${librbd_types_srcs})

set(librbd_internal_srcs
  AioCompletion.cc
  AioImageRequest.cc
  AioImageRequestWQ.cc
  AioObjectRequest.cc
  AsyncObjectThrottle.cc
  AsyncOperation.cc
  AsyncRequest.cc
  CopyupRequest.cc
  DiffIterate.cc
  ImageCtx.cc
  ImageWatcher.cc
  internal.cc
  Journal.cc
  JournalReplay.cc
  LibrbdAdminSocketHook.cc
  LibrbdWriteback.cc
  ObjectMap.cc
  object_map/InvalidateRequest.cc
  object_map/Request.cc
  object_map/ResizeRequest.cc
  object_map/SnapshotCreateRequest.cc
  object_map/SnapshotRemoveRequest.cc
  object_map/SnapshotRollbackRequest.cc
  object_map/UpdateRequest.cc
  operation/FlattenRequest.cc
  operation/RebuildObjectMapRequest.cc
  operation/RenameRequest.cc
  operation/Request.cc
  operation/ResizeRequest.cc
  operation/SnapshotCreateRequest.cc
  operation/SnapshotProtectRequest.cc
  operation/SnapshotRemoveRequest.cc
  operation/SnapshotRenameRequest.cc
  operation/SnapshotRollbackRequest.cc
  operation/SnapshotUnprotectRequest.cc
  operation/TrimRequest.cc
  )
add_library(librbd_internal OBJECT ${librbd_internal_srcs})

set(librbd_api_srcs librbd.cc)
add_library(librbd_api OBJECT ${librbd_api_srcs})

set(librbd_srcs
  $<TARGET_OBJECTS:librbd_api>
  $<TARGET_OBJECTS:librbd_internal>
  $<TARGET_OBJECTS:librbd_types>
  $<TARGET_OBJECTS:osdc_rbd_objs>
  $<TARGET_OBJECTS:common_util_obj>
  )
add_library(librbd ${CEPH_SHARED} ${librbd_srcs})
target_link_libraries(librbd journal librados common
  cls_lock_client cls_rbd_client cls_journal_client)
if(ENABLE_SHARED)
  set_target_properties(librbd PROPERTIES VERSION "1.0.0" SOVERSION "1"
    OUTPUT_NAME rbd)
endif(ENABLE_SHARED)
install(TARGETS librbd DESTINATION lib)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/include/rbd DESTINATION include)

if(UNIX)
  set_target_properties(librbd PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    CMAKE_SHARED_LINKER_FLAGS "--exclude-libs=ALL")
endif(UNIX)
