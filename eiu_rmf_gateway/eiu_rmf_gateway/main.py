"""Run the gateway: the ROS node on its own thread, Redis commands and flushes on asyncio."""

import asyncio
import logging
import os
import threading
from pathlib import Path

import rclpy
import redis
from rclpy.executors import ExternalShutdownException, SingleThreadedExecutor

from . import contract, nav_graph
from .core import Gateway, Recorder
from .ros_io import RosIO
from .task_events import serve_task_events

log = logging.getLogger('eiu_rmf_gateway')


async def run(node: RosIO, gateway: Gateway):
    port = int(node.param('task_events_port'))
    server = None
    if port:
        server = await serve_task_events(gateway.recorder, node.param('task_events_host'), port)
        log.info('task events socket on ws://%s:%d', node.param('task_events_host'), port)

    async def commands():
        block_ms = int(node.param('command_block_ms'))
        while True:
            try:
                await asyncio.to_thread(gateway.handle_commands, block_ms)
            except redis.RedisError as e:
                log.warning('redis (commands): %s', e)
                await asyncio.sleep(1.0)

    async def graph_watch():
        path = node.param('nav_graph_path')
        last = None
        while path:
            try:
                stat = Path(path).stat().st_mtime_ns
                if stat != last:
                    last = stat
                    gateway.recorder.set_value('nav_graph', nav_graph.read(Path(path)))
            except OSError as e:
                log.warning('nav graph %s: %s', path, e)
            await asyncio.sleep(2.0)

    async def flushes():
        period = float(node.param('flush_period_s'))
        while True:
            try:
                await asyncio.to_thread(gateway.flush)
            except redis.RedisError as e:
                log.warning('redis (flush): %s', e)
                await asyncio.sleep(1.0)
            await asyncio.sleep(period)

    async def ros_alive():
        while rclpy.ok():
            await asyncio.sleep(0.2)
        log.info('ROS 2 shut down; stopping the gateway')

    tasks = [asyncio.create_task(commands()), asyncio.create_task(flushes()), asyncio.create_task(graph_watch())]
    try:
        await ros_alive()
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if server:
            server.close()


def spin(executor):
    try:
        executor.spin()
    except ExternalShutdownException:
        pass


def make_execute(node: RosIO, recorder: Recorder):
    """Command runner: the nav graph file here, everything else on the ROS side."""
    def execute(cmd_type: str, cmd_id: str, body: dict):
        if cmd_type == 'nav_graph_save':
            path = node.param('nav_graph_path')
            if not path:
                return False, 'no_nav_graph', 'nav_graph_path is not set'
            ok, error, detail = nav_graph.save(Path(path), body['yaml'], body['base_sha256'])
            if ok:
                recorder.set_value('nav_graph', nav_graph.read(Path(path)))
                log.info('nav graph %s saved (%s)', path, detail[:12])
            return ok, error, detail
        return node.execute(cmd_type, cmd_id, body)
    return execute


def main(args=None):
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(name)s: %(message)s')
    rclpy.init(args=args)
    recorder = Recorder()
    node = RosIO(recorder)
    client = redis.Redis.from_url(node.param('redis_url'), decode_responses=True)
    gateway = Gateway(
        client, contract.Keys(node.param('redis_prefix')), recorder, make_execute(node, recorder),
        events_maxlen=int(node.param('events_maxlen')), command_max_age_s=float(node.param('command_max_age_s')),
        heartbeat_ttl_s=float(node.param('heartbeat_ttl_s')),
        info={'ros_domain_id': os.environ.get('ROS_DOMAIN_ID', '0'), 'task_events': bool(node.param('task_events_port')),
              'nav_graph_path': node.param('nav_graph_path')})
    while True:
        try:
            gateway.ensure_group()
            break
        except redis.RedisError as e:
            log.warning('waiting for redis at %s: %s', node.param('redis_url'), e)
            threading.Event().wait(2.0)

    executor = SingleThreadedExecutor()
    executor.add_node(node)
    threading.Thread(target=spin, args=(executor,), name='ros', daemon=True).start()
    log.info('gateway online: redis %s, prefix %s, ROS_DOMAIN_ID=%s', node.param('redis_url'),
             node.param('redis_prefix'), os.environ.get('ROS_DOMAIN_ID', '0'))
    try:
        asyncio.run(run(node, gateway))
    except (KeyboardInterrupt, ExternalShutdownException):
        pass
    finally:
        executor.shutdown()
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()
        try:
            client.delete(gateway.keys.gateway)
        except redis.RedisError:
            pass


if __name__ == '__main__':
    main()
