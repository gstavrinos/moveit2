import launch_testing
import os
import pytest
import unittest
import yaml
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch_ros.actions import Node
from launch_testing.util import KeepAliveProc


def load_file(package_name, file_path):
    package_path = get_package_share_directory(package_name)
    absolute_file_path = os.path.join(package_path, file_path)

    try:
        with open(absolute_file_path, "r") as file:
            return file.read()
    except EnvironmentError:  # parent of IOError, OSError *and* WindowsError where available
        return None


def load_yaml(package_name, file_path):
    package_path = get_package_share_directory(package_name)
    absolute_file_path = os.path.join(package_path, file_path)

    try:
        with open(absolute_file_path, "r") as file:
            return yaml.full_load(file)
    except EnvironmentError:  # parent of IOError, OSError *and* WindowsError where available
        return None


@pytest.mark.rostest
def generate_test_description():

    # Component yaml files are grouped in separate namespaces
    robot_description_config = load_file(
        "moveit_resources_fanuc_description", "urdf/fanuc.urdf"
    )
    robot_description = {"robot_description": robot_description_config}

    robot_description_semantic_config = load_file(
        "moveit_resources_fanuc_moveit_config", "config/fanuc.srdf"
    )
    robot_description_semantic = {
        "robot_description_semantic": robot_description_semantic_config
    }
    kinematics_yaml = load_yaml(
        "moveit_resources_fanuc_moveit_config", "config/kinematics.yaml"
    )
    robot_description_kinematics = {"robot_description_kinematics": kinematics_yaml}
    joint_limits_yaml = {
        "robot_description_planning": load_yaml(
            "moveit_resources_fanuc_moveit_config", "config/joint_limits.yaml"
        )
    }
    test_param = load_yaml("moveit_kinematics", "config/fanuc-lma-test.yaml")

    fanuc_lma = Node(
        package="moveit_kinematics",
        executable="test_kinematics_plugin",
        name="fanuc_lma",
        parameters=[
            robot_description,
            robot_description_semantic,
            robot_description_kinematics,
            joint_limits_yaml,
            test_param,
        ],
        output="screen",
    )

    return (
        LaunchDescription(
            [
                fanuc_lma,
                KeepAliveProc(),
                launch_testing.actions.ReadyToTest(),
            ]
        ),
        {"fanuc_lma": fanuc_lma},
    )


class TestTerminatingProcessStops(unittest.TestCase):
    def test_gtest_run_complete(self, proc_info, fanuc_lma):
        proc_info.assertWaitForShutdown(process=fanuc_lma, timeout=4000.0)


@launch_testing.post_shutdown_test()
class TestOutcome(unittest.TestCase):
    def test_exit_codes(self, proc_info):
        launch_testing.asserts.assertExitCodes(proc_info)
