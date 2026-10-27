"""Entrypoint: ``python3 -m bellhost [--config config.yaml]`` from host/."""

from __future__ import annotations

import argparse
import asyncio

from . import config, registry, logs as logs_mod, bridge, web
from aiohttp import web as aiohttp_web


async def main(cfg_path: str) -> None:
    cfg = config.load(cfg_path)
    reg = registry.Registry(cfg)
    logs = logs_mod.Logs(config.storage_dir(cfg))
    br = bridge.Bridge(cfg, reg, logs)
    app = web.build_app(br, cfg, reg, logs)
    await br.start()
    runner = aiohttp_web.AppRunner(app)
    await runner.setup()
    site = aiohttp_web.TCPSite(runner, host="0.0.0.0", port=cfg.web_port)
    await site.start()
    print(f"bell host up: module port {cfg.module_port}, "
          f"web port {cfg.web_port}")
    await asyncio.Event().wait()


def run() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="config.yaml")
    args = ap.parse_args()
    try:
        asyncio.run(main(args.config))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    run()
