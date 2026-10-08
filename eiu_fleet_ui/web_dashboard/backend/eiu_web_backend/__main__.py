"""Command line: run the server and manage accounts.

  python -m eiu_web_backend serve
  python -m eiu_web_backend create-user --email a@eiu.edu.vn --name "Full Name" --role admin
  python -m eiu_web_backend create-user --email o@eiu.edu.vn --name "Full Name" --services delivery,patrol
  python -m eiu_web_backend set-password --email a@eiu.edu.vn
  python -m eiu_web_backend disable-user --email a@eiu.edu.vn
  python -m eiu_web_backend list-users
"""

import argparse
import getpass
import logging
import sys
import uuid

from sqlalchemy import select

from . import security
from .db import Database, User, user_by_email
from .settings import load_or_exit
from .site import Site, SiteError


def _password(args, minimum: int) -> str:
    password = args.password or getpass.getpass("Password: ")
    if len(password) < minimum:
        sys.exit(f"password must have at least {minimum} characters")
    if not args.password and getpass.getpass("Repeat password: ") != password:
        sys.exit("passwords do not match")
    return password


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(prog="eiu_web_backend", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--settings", help="settings file (default: config/settings.yaml or EIU_WEB_SETTINGS)")
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("serve")
    create = sub.add_parser("create-user")
    create.add_argument("--email", required=True)
    create.add_argument("--name", required=True)
    create.add_argument("--role", choices=security.ROLES, default="operator")
    create.add_argument("--services", default="", help="operator: comma-separated service ids")
    create.add_argument("--zones", default="", help="operator: comma-separated zone ids; empty = every zone")
    create.add_argument("--department", default="")
    create.add_argument("--locale", choices=("vi", "en"), default="vi")
    create.add_argument("--password", help="omit to type it in")
    pw = sub.add_parser("set-password")
    pw.add_argument("--email", required=True)
    pw.add_argument("--password", help="omit to type it in")
    disable = sub.add_parser("disable-user")
    disable.add_argument("--email", required=True)
    sub.add_parser("list-users")
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    settings = load_or_exit(args.settings)
    db = Database.sqlite(settings.path("server", "database"))
    minimum = settings.get("auth", "min_password_length")

    if args.command in (None, "serve"):
        serve(settings, db)
        return

    with db.session() as s:
        if args.command == "create-user":
            if user_by_email(s, args.email):
                sys.exit(f"{args.email} exists")
            s.add(User(id=str(uuid.uuid4()), email=args.email.strip().lower(), full_name=args.name,
                       role=args.role, locale=args.locale, department=args.department,
                       allowed_services=[x for x in args.services.split(",") if x],
                       allowed_zones=[x for x in args.zones.split(",") if x],
                       permissions=list(security.DEFAULT_OPERATOR),
                       password_hash=security.hash_password(_password(args, minimum))))
            db.audit(s, None, "user.create", args.email, f"cli role={args.role}")
        elif args.command == "set-password":
            user = user_by_email(s, args.email) or sys.exit(f"no user {args.email}")
            user.password_hash = security.hash_password(_password(args, minimum))
            security.end_sessions(s, user.id)
            db.audit(s, None, "user.password", args.email, "cli")
        elif args.command == "disable-user":
            user = user_by_email(s, args.email) or sys.exit(f"no user {args.email}")
            user.active = False
            security.end_sessions(s, user.id)
            db.audit(s, None, "user.disable", args.email, "cli")
        elif args.command == "list-users":
            for u in s.scalars(select(User).order_by(User.email)).all():
                services = "*" if u.role == "admin" else ",".join(u.allowed_services or []) or "-"
                print(f"{u.email:32} {u.role:8} {'active' if u.active else 'disabled':8} {services:24} {u.full_name}")
        s.commit()
    if args.command != "list-users":
        print("done")


def serve(settings, db) -> None:
    import uvicorn

    from .app import create_app
    from .rmf.client import GatewayClient

    log = logging.getLogger("eiu_web")
    try:
        site = Site.load(settings)
    except (SiteError, OSError, KeyError) as e:
        sys.exit(f"site catalog: {e}")
    with db.session() as s:
        if not security.any_user(s):
            log.warning("no accounts yet: python -m eiu_web_backend create-user --email ... --role admin")
    gateway = None
    if settings.get("rmf", "enabled"):
        gateway = GatewayClient(settings.get("redis", "url"), settings.get("redis", "prefix"),
                                settings.get("redis", "cursor_key"))
        log.info("Open-RMF through eiu_rmf_gateway at %s (prefix %s)", settings.get("redis", "url"),
                 settings.get("redis", "prefix"))
    app = create_app(settings, site, db, gateway)
    uvicorn.run(app, host=settings.get("server", "host"), port=settings.get("server", "port"),
                log_level="info", proxy_headers=True)


if __name__ == "__main__":
    main()
