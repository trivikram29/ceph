if(WITH_RBD)
  add_subdirectory(rbd)
endif(WITH_RBD)

set(ceph_conf_srcs ceph_conf.cc)
add_executable(ceph-conf ${ceph_conf_srcs})
target_link_libraries(ceph-conf global)
install(TARGETS ceph-conf DESTINATION bin)

set(monmaptool_srcs monmaptool.cc)
add_executable(monmaptool ${monmaptool_srcs})
target_link_libraries(monmaptool global)
install(TARGETS monmaptool DESTINATION bin)

set(osdomaptool_srcs osdmaptool.cc)
add_executable(osdmaptool ${osdomaptool_srcs})
target_link_libraries(osdmaptool global ${CMAKE_DL_LIBS})
install(TARGETS osdmaptool DESTINATION bin)

set(ceph_authtool_srcs ceph_authtool.cc)
add_executable(ceph-authtool ${ceph_authtool_srcs})
target_link_libraries(ceph-authtool global ${EXTRALIBS} ${CRYPTO_LIBS})
install(TARGETS ceph-authtool DESTINATION bin)
