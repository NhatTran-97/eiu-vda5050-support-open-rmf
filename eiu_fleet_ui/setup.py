from setuptools import setup, find_packages
import os
from glob import glob

package_name = 'eiu_fleet_ui'

setup(
    name=package_name,
    version='0.2.0',
    packages=find_packages(),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
        *[
            (os.path.join('share', package_name, dirpath),
             [os.path.join(dirpath, f) for f in filenames])
            for dirpath, _, filenames in os.walk('qml')
            if filenames
        ],
        *[
            (os.path.join('share', package_name, dirpath),
             [os.path.join(dirpath, f) for f in filenames])
            for dirpath, _, filenames in os.walk('maps')
            if filenames
        ],
        *[
            (os.path.join('share', package_name, dirpath),
             [os.path.join(dirpath, f) for f in filenames])
            for dirpath, _, filenames in os.walk('icons')
            if filenames
        ],
    ],
    install_requires=['setuptools'],
    zip_safe=True,
    maintainer='NhatTran',
    maintainer_email='dnnevertogiveup@gmail.com',
    description='EIU Fleet Management UI',
    license='Apache-2.0',
    entry_points={
        'console_scripts': [
            'eiu_fleet_ui = eiu_fleet_ui.main:main',
        ],
    },
)
