#!/usr/bin/env python3
import sys

SENSORS_PLUGIN = '''    <plugin filename="gz-sim-sensors-system" name="gz::sim::systems::Sensors">
      <render_engine>ogre2</render_engine>
    </plugin>
    <plugin filename="gz-sim-imu-system" name="gz::sim::systems::Imu">
    </plugin>
'''

def patch_world(world_file):
    with open(world_file, 'r') as f:
        content = f.read()
    if 'gz-sim-sensors-system' in content:
        print(f'Already patched: {world_file}')
        return
    content = content.replace(
        '<plugin filename="libdoor.so"',
        SENSORS_PLUGIN + '    <plugin filename="libdoor.so"'
    )
    with open(world_file, 'w') as f:
        f.write(content)
    print(f'Patched: {world_file}')

if __name__ == '__main__':
    patch_world(sys.argv[1])
