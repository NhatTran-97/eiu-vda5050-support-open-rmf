import os
from glob import glob

from setuptools import setup

package_name = "fleet_bringup"

setup(
    name=package_name,
    version="0.1.0",
    packages=[package_name],
    data_files=[
        ("share/ament_index/resource_index/packages", ["resource/" + package_name]),
        ("share/" + package_name, ["package.xml"]),
        (os.path.join("share", package_name, "launch"), glob("launch/*.launch.py")),
    ],
    install_requires=["setuptools"],
    zip_safe=True,
    maintainer="NhatTran",
    maintainer_email="dnnevertogiveup@gmail.com",
    description="Bundled launch for the fleet-management side (RMF core, VDA5050 fleet adapter, mock workcells, UI)",
    license="Apache-2.0",
)
