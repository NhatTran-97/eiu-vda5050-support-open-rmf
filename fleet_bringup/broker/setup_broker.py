#!/usr/bin/env python3
"""Generate a VDA5050 mosquitto setup: TLS certificates, user accounts and topic ACL."""

import argparse
import os
import secrets
import shutil
import stat
import subprocess
import sys

import yaml

MASTER_USER = 'fleet_master'
VIEWER_USER = 'fleet_ui'
MASTER_WRITES = ('order', 'instantActions')
AGV_WRITES = ('state', 'visualization', 'connection', 'factsheet')


def agvs_from_configs(paths):
    """Return {(interface, manufacturer, serial)} of the robots in fleet adapter configs."""
    agvs = set()
    for path in paths:
        with open(path) as f:
            vda = (yaml.safe_load(f) or {}).get('vda5050') or {}
        interface = vda.get('interface_name')
        if not interface:
            sys.exit(f'{path}: vda5050.interface_name is missing')
        for name, robot in (vda.get('robots') or {}).items():
            if not robot or not robot.get('manufacturer') or not robot.get('serial'):
                sys.exit(f'{path}: vda5050.robots.{name} needs manufacturer and serial')
            agvs.add((interface, str(robot['manufacturer']), str(robot['serial'])))
    return agvs


def agv_user(manufacturer, serial):
    """Broker user name of one AGV."""
    return f'{manufacturer}_{serial}'


def acl_text(agvs):
    """ACL: the master writes orders and reads AGV topics, each AGV only its own topics."""
    interfaces = sorted({interface for interface, _, _ in agvs})
    lines = [f'user {MASTER_USER}']
    for interface in interfaces:
        lines += [f'topic write {interface}/v2/+/+/{leaf}' for leaf in MASTER_WRITES]
        lines += [f'topic read {interface}/v2/+/+/{leaf}' for leaf in AGV_WRITES]
    lines += ['', f'user {VIEWER_USER}']
    for interface in interfaces:
        lines += [f'topic read {interface}/v2/+/+/{leaf}' for leaf in AGV_WRITES]
    for interface, manufacturer, serial in sorted(agvs):
        base = f'{interface}/v2/{manufacturer}/{serial}'
        lines += ['', f'user {agv_user(manufacturer, serial)}']
        lines += [f'topic read {base}/{leaf}' for leaf in MASTER_WRITES]
        lines += [f'topic write {base}/{leaf}' for leaf in AGV_WRITES]
    return '\n'.join(lines) + '\n'


def run(*command):
    """Run a command and stop on failure."""
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f'{" ".join(command)} failed:\n{result.stderr}')


def make_certificates(certs, hosts, days):
    """Create the CA once, then a server certificate for the given host names and addresses."""
    ca_key, ca_crt = os.path.join(certs, 'ca.key'), os.path.join(certs, 'ca.crt')
    if not os.path.exists(ca_crt):
        run('openssl', 'req', '-x509', '-newkey', 'rsa:3072', '-nodes', '-keyout', ca_key, '-out', ca_crt,
            '-days', str(days), '-subj', '/CN=VDA5050 broker CA')
    alt_names = ','.join(('IP:' if host.replace('.', '').isdigit() or ':' in host else 'DNS:') + host for host in hosts)
    extensions = os.path.join(certs, 'server.ext')
    with open(extensions, 'w') as f:
        f.write(f'subjectAltName={alt_names}\nextendedKeyUsage=serverAuth\n')
    server_key, server_csr = os.path.join(certs, 'server.key'), os.path.join(certs, 'server.csr')
    run('openssl', 'req', '-newkey', 'rsa:3072', '-nodes', '-keyout', server_key, '-out', server_csr,
        '-subj', f'/CN={hosts[0]}')
    run('openssl', 'x509', '-req', '-in', server_csr, '-CA', ca_crt, '-CAkey', ca_key, '-CAcreateserial',
        '-out', os.path.join(certs, 'server.crt'), '-days', str(days), '-extfile', extensions)
    os.remove(server_csr)
    os.remove(extensions)
    for key in (ca_key, server_key):
        os.chmod(key, stat.S_IRUSR | stat.S_IWUSR)


def read_credentials(path):
    """Existing user:password lines, so a rerun keeps every account's password."""
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        return dict(line.rstrip('\n').split(':', 1) for line in f if ':' in line)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, help='output directory')
    parser.add_argument('--config', nargs='*', default=[], help='fleet adapter config files with vda5050.robots')
    parser.add_argument('--agv', nargs='*', default=[], metavar='INTERFACE/MANUFACTURER/SERIAL',
                        help='AGVs that no config lists, e.g. added at runtime')
    parser.add_argument('--host', nargs='+', required=True, help='broker host names and IP addresses')
    parser.add_argument('--port', type=int, default=8883)
    parser.add_argument('--days', type=int, default=825, help='certificate validity')
    args = parser.parse_args()

    if not shutil.which('mosquitto_passwd'):
        sys.exit('mosquitto_passwd not found (package mosquitto)')
    agvs = agvs_from_configs(args.config)
    for entry in args.agv:
        parts = entry.split('/')
        if len(parts) != 3 or not all(parts):
            sys.exit(f'--agv {entry}: expected INTERFACE/MANUFACTURER/SERIAL')
        agvs.add(tuple(parts))
    if not agvs:
        sys.exit('no AGV given: use --config or --agv')

    out = os.path.abspath(args.out)
    certs = os.path.join(out, 'certs')
    os.makedirs(certs, exist_ok=True)
    make_certificates(certs, args.host, args.days)

    credentials_file = os.path.join(out, 'credentials.txt')
    credentials = read_credentials(credentials_file)
    users = [MASTER_USER, VIEWER_USER] + sorted({agv_user(m, s) for _, m, s in agvs})
    for user in users:
        credentials.setdefault(user, secrets.token_urlsafe(18))
    with open(credentials_file, 'w') as f:
        f.writelines(f'{user}:{credentials[user]}\n' for user in users)
    os.chmod(credentials_file, stat.S_IRUSR | stat.S_IWUSR)

    passwd = os.path.join(out, 'passwd')
    open(passwd, 'w').close()
    for user in users:
        run('mosquitto_passwd', '-b', passwd, user, credentials[user])
    os.chmod(passwd, stat.S_IRUSR | stat.S_IWUSR)

    with open(os.path.join(out, 'acl'), 'w') as f:
        f.write(acl_text(agvs))
    with open(os.path.join(out, 'mosquitto.conf'), 'w') as f:
        f.write(f'''listener {args.port}
cafile {certs}/ca.crt
certfile {certs}/server.crt
keyfile {certs}/server.key
tls_version tlsv1.2
allow_anonymous false
password_file {passwd}
acl_file {out}/acl
persistence false
''')
    print(f'{out}: mosquitto.conf, acl, passwd, certs/ca.crt, credentials.txt ({len(users)} users)')


if __name__ == '__main__':
    main()
