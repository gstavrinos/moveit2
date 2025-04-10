cmake_minimum_required(VERSION 3.5)
set(MOVEIT_LIB_NAME moveit_kinematics_base)

add_library(${MOVEIT_LIB_NAME} SHARED src/kinematics_base.cpp)
include(GenerateExportHeader)
generate_export_header(${MOVEIT_LIB_NAME})
target_include_directories(${MOVEIT_LIB_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)

# This line is needed to ensure that messages are done being built before this is built
ament_target_dependencies(
  ${MOVEIT_LIB_NAME}
  rclcpp
  urdf
  urdfdom_headers
  srdfdom moveit_msgs
  geometric_shapes
  geometry_msgs
  Boost
)

install(DIRECTORY include/ DESTINATION include)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${MOVEIT_LIB_NAME}_export.h DESTINATION include)
