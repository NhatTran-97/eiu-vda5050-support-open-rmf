from glob import glob

from setuptools import find_packages, setup

package_name = 'eiu_rmf_gateway'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml', 'README.md']),
        ('share/' + package_name + '/config', glob('config/*.yaml')),
        ('share/' + package_name + '/launch', glob('launch/*.py')),
        ('share/' + package_name + '/docs', glob('docs/*.md')),
    ],
    install_requires=['setuptools'],
    zip_safe=True,
    maintainer='NhatTran',
    maintainer_email='dnnevertogiveup@gmail.com',
    description='Gateway between Open-RMF (ROS 2) and applications over Redis',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'gateway = eiu_rmf_gateway.main:main',
        ],
    },
)
