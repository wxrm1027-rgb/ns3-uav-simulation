set(source_files
  heterogeneous-nms-main.cc
  framework/heterogeneous-nms-framework.cc
  core/logger.cc
  core/json_config.cc
  core/config_loader.cc
  core/state_manager.cc
  core/structured_log.cc
  core/event_scheduler.cc
  core/sim_event.cc
  protocol/hnmp.cc
  protocol/tlv.cc
  protocol/packet-parse.cc
  app/heterogeneous-node-app.cc
  app/gmc-policy-sender-app.cc
  app/intent-mapper.cc
  app/policy-executor.cc
  trace/policy-state-tracer.cc
)

# Prefix must match ns-3 nested scratch naming so `./ns3 build|run heterogeneous-nms-main`
# resolves to the same CMake target as auto-generated scratches (scratch_<subdir>_<execname>).
build_exec(
  EXECNAME heterogeneous-nms-main
  EXECNAME_PREFIX scratch_heterogeneous-nms_
  SOURCE_FILES "${source_files}"
  LIBRARIES_TO_LINK "${ns3-libs}" "${ns3-contrib-libs}"
  EXECUTABLE_DIRECTORY_PATH ${CMAKE_OUTPUT_DIRECTORY}/scratch/heterogeneous-nms/
)

