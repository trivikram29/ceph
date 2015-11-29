set(librbd_replay_types_srcs ActionTypes.cc)
add_library(librbd_replay_types OBJECT ${librbd_replay_types_srcs})

set(librbd_replay_srcs
  actions.cc
  BufferReader.cc
  ImageNameMap.cc
  PendingIO.cc
  rbd_loc.cc
  Replayer.cc
  )
add_library(librbd_replay OBJECT ${librbd_replay_srcs})

set(rbd_replay_srcs
  rbd-replay.cc
  $<TARGET_OBJECTS:librbd_replay>
  $<TARGET_OBJECTS:librbd_replay_types>
#  $<TARGET_OBJECTS:parse_secret_objs>
  )
add_executable(rbd-replay ${rbd_replay_srcs})
target_link_libraries(rbd-replay librbd librados global)
install(TARGETS rbd-replay DESTINATION bin)

set(librbd_replay_ios_srcs ios.cc)
add_library(librbd_replay_ios OBJECT ${librbd_replay_ios_srcs})

if(WITH_BABELTRACE)
set(rbd_replay_prep_srcs
  rbd-replay-prep.cc
  $<TARGET_OBJECTS:librbd_replay>
  $<TARGET_OBJECTS:librbd_replay_ios>
  $<TARGET_OBJECTS:librbd_replay_types>
  )
add_executable(rbd-replay-prep ${rbd_replay_prep_srcs})
target_link_libraries(rbd-replay-prep librbd librados global
  babeltrace babeltrace-ctf)
install(TARGETS rbd-replay-prep DESTINATION bin)
endif(WITH_BABELTRACE)
